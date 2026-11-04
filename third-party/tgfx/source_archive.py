"""Verify explicitly receipted codeload source archives without inventing Git state."""

import hashlib
import json
import os
from collections import deque
from pathlib import Path, PurePosixPath
import tarfile


def contained_source_path(source, path):
    """Resolve relative links one component at a time, never stepping outside source."""
    pending = deque(path.relative_to(source).parts)
    current = source
    followed = 0
    while pending:
        component = pending.popleft()
        if component in {"", "."}:
            continue
        if component == "..":
            if current == source:
                raise ValueError(f"Source symlink escapes its root: {path}")
            current = current.parent
            continue
        candidate = current / component
        if candidate.is_symlink():
            link = os.readlink(candidate)
            if Path(link).is_absolute():
                raise ValueError(f"Absolute source symlink is unsupported: {candidate}")
            followed += 1
            if followed > 40:
                raise ValueError(f"Source symlink chain is cyclic or too long: {path}")
            pending.extendleft(reversed(Path(link).parts))
        else:
            current = candidate
    return current


def verified_symlink_text(source, original):
    """Return link text after checking its parent and entire relative target chain."""
    parent = contained_source_path(source, original.parent)
    actual = parent / original.name
    if not actual.is_symlink():
        raise ValueError(f"Expected source symlink: {original}")
    link = os.readlink(actual)
    contained_source_path(source, original)
    return link


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_receipts(paths):
    """Accept the parent's name/commit/archiveSHA256 list plus an explicit archive path.

    Without `archive`, a list entry refers to NAME.tar.gz beside its receipt.
    No hash is inferred from the source checkout or downloaded from the network.
    """
    result = {}
    for path in paths:
        path = path.resolve(strict=True)
        data = json.loads(path.read_text())
        entries = data if isinstance(data, list) else data["dependencies"]
        for original in entries:
            entry = dict(original)
            name = entry["name"]
            if name in result:
                raise ValueError(f"Duplicate source-archive receipt for {name}")
            archive = Path(entry.get("archive", name + ".tar.gz"))
            if not archive.is_absolute():
                archive = path.parent / archive
            entry["archive"] = str(archive.resolve(strict=True))
            entry["receipt"] = str(path)
            entry["receipt_sha256"] = file_sha256(path)
            digest = entry.get("archiveSHA256", entry.get("archive_sha256", ""))
            if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError(f"{name}: receipt requires an explicit archive SHA256")
            entry["archiveSHA256"] = digest
            result[name] = entry
    return result


def snapshot_archive(source, destination, expected, key, receipt):
    """Check every archived source member against the extracted tree, then stage it.

    Staging only verified members discards build outputs and other unreceipted
    files. Nested dependencies are independently staged from their own receipts.
    """
    source = source.resolve(strict=True)
    archive = Path(receipt["archive"]).resolve(strict=True)
    if receipt["commit"] != expected:
        raise ValueError(f"{key}: archive receipt commit does not match TGFX DEPS {expected}")
    if file_sha256(archive) != receipt["archiveSHA256"]:
        raise ValueError(f"{key}: source tarball SHA256 mismatch: {archive}")
    destination.mkdir(parents=True, exist_ok=True)
    copied = {}
    with tarfile.open(archive, "r:*") as stream:
        members = stream.getmembers()
        roots = {PurePosixPath(m.name).parts[0] for m in members if PurePosixPath(m.name).parts}
        if len(roots) != 1:
            raise ValueError(f"{key}: expected one codeload archive root")
        prefix = roots.pop()
        if prefix != receipt.get("strip_prefix", prefix) or not prefix.endswith("-" + expected):
            raise ValueError(f"{key}: archive root must name the complete pinned commit: {prefix}")
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"{key}: unsafe archive member {member.name}")
            relative = Path(*path.parts[1:])
            if relative == Path("."):
                continue
            original, target = source / relative, destination / relative
            if member.isdir():
                continue
            if relative.as_posix() in copied:
                raise ValueError(f"{key}: duplicate archive member {relative}")
            if relative.suffix in {".a", ".so", ".dylib", ".dll", ".lib", ".o", ".obj"}:
                raise ValueError(f"{key}: refusing binary source input {relative}")
            contained_source_path(source, original.parent)
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym():
                if verified_symlink_text(source, original) != member.linkname:
                    raise ValueError(f"{key}: extracted symlink differs from tar member: {relative}")
                target.symlink_to(member.linkname)
                copied[relative.as_posix()] = "symlink:" + member.linkname
            elif member.isfile():
                if not original.is_file() or original.is_symlink():
                    raise ValueError(f"{key}: extracted source missing/not a regular file: {relative}")
                payload = stream.extractfile(member)
                digest = hashlib.sha256()
                with target.open("wb") as staged:
                    for block in iter(lambda: payload.read(1024 * 1024), b""):
                        digest.update(block)
                        staged.write(block)
                payload.close()
                if digest.hexdigest() != file_sha256(original):
                    raise ValueError(f"{key}: extracted source differs from tar member: {relative}")
                target.chmod(member.mode & 0o777)
                copied[relative.as_posix()] = digest.hexdigest()
            else:
                raise ValueError(f"{key}: unsupported archive member type: {relative}")
    if not copied:
        raise ValueError(f"{key}: empty source archive")
    return {
        "source": str(source), "input_kind": "source_archive",
        "commit": expected, "archive": str(archive),
        "archive_sha256": receipt["archiveSHA256"],
        "archive_root": prefix, "receipt": receipt["receipt"],
        "receipt_sha256": receipt["receipt_sha256"],
        "verified_member_count": len(copied),
        "verified_files_sha256": hashlib.sha256(json.dumps(copied, sort_keys=True).encode()).hexdigest(),
        "provenance": "Extracted files matched explicit pinned codeload tar members; no Git state synthesized.",
    }
