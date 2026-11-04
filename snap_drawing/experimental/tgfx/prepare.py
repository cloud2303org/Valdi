#!/usr/bin/env python3
"""Bind local, independently built dependencies to the standalone Bazel experiment."""

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

TGFX_COMMIT = "4cf8991d5a3a010c9314538dd2fea937593b206b"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bind(target, source):
    source = source.resolve(strict=True)
    if target.is_symlink():
        target.unlink()
    elif target.exists():
        raise RuntimeError(f"Refusing to replace a non-symlink dependency: {target}")
    target.symlink_to(source, target_is_directory=source.is_dir())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tgfx-source", required=True, type=Path)
    parser.add_argument("--tgfx-library", required=True, type=Path)
    parser.add_argument("--fmt-source", required=True, type=Path)
    args = parser.parse_args()
    source = args.tgfx_source.resolve(strict=True)
    library = args.tgfx_library.resolve(strict=True)
    fmt = args.fmt_source.resolve(strict=True)
    commit = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    if commit != TGFX_COMMIT:
        raise RuntimeError(f"Expected TGFX {TGFX_COMMIT}; got {commit}")
    if library.read_bytes()[:8] != b"!<arch>\n":
        raise RuntimeError("TGFX dependency is not a regular static archive")
    if not (fmt / "include/fmt/format.h").is_file():
        raise RuntimeError("Expected the real fmt source directory")
    root = Path(__file__).resolve().parent
    native_changes = subprocess.check_output(
        ["git", "-C", str(source), "diff", "HEAD"], text=True
    )
    expected_patch = (root / "patches/tgfx-opengl-shader-sync.patch").read_text()
    if native_changes != expected_patch:
        raise RuntimeError("TGFX CMake changes differ from the recorded OpenGL patch")
    local = root / ".local"
    (local / "tgfx").mkdir(parents=True, exist_ok=True)
    bind(local / "tgfx/include", source / "include")
    bind(local / "tgfx/tgfx.a", library)
    bind(local / "fmt", fmt)
    # File symlinks expose the actual sources without importing nested Bazel packages.
    framework = root.parents[2]
    files = re.findall(r'"((?:snap_drawing|libs)/[^"]+\.(?:cpp|hpp))"',
                       (root / "framework.BUILD.bazel").read_text())
    for name in files:
        target = local / "framework" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        bind(target, framework / name)
    receipt = {
        "tgfxCommit": commit,
        "tgfxSource": str(source),
        "tgfxLibrary": str(library),
        "tgfxLibrarySHA256": digest(library),
        "tgfxCMakeSHA256": digest(source / "CMakeLists.txt"),
        "tgfxBuildPatchSHA256": digest(root / "patches/tgfx-opengl-shader-sync.patch"),
        "fmtSource": str(fmt),
        "fmtHeaderSHA256": digest(fmt / "include/fmt/format.h"),
    }
    (local / "inputs.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
