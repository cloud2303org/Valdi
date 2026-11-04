#!/usr/bin/env python3
"""Read-only, backend-neutral host stage check. No compiler, receipt, or device I/O."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    """Same tree format as full/app/bridge/artifact.mjs; do not follow symlinks."""
    path = Path(path)
    if path.is_file() and not path.is_symlink():
        with path.open("rb") as source:
            value = hashlib.sha256()
            for block in iter(lambda: source.read(1024 * 1024), b""):
                value.update(block)
            return value.hexdigest()
    require(path.is_dir() and not path.is_symlink(), f"Expected artifact: {path}")
    entries = []

    def walk(directory, prefix=""):
        for child in sorted(directory.iterdir()):
            name = prefix + child.name
            if child.is_symlink():
                entries.append((name, "link:" + os.readlink(child)))
            elif child.is_dir():
                walk(child, name + "/")
            elif child.is_file():
                entries.append((name, digest(child)))
            else:
                raise ValueError(f"Unsupported artifact entry: {child}")

    walk(path)
    stream = "".join(f"{name}\0{value}\n" for name, value in sorted(entries))
    return hashlib.sha256(stream.encode()).hexdigest()


def nonempty(path):
    require(path.is_file() and path.stat().st_size > 0, f"Missing/empty file: {path}")


def validate(stage, platform, backend=None):
    nonempty(stage / "provenance.json")
    provenance = json.loads((stage / "provenance.json").read_text())
    require(isinstance(provenance, dict), "provenance.json must be an object")
    require(provenance.get("backend") in ("skia", "tgfx"), "backend must be skia or tgfx")
    if backend:
        require(provenance["backend"] == backend, "Requested backend differs from stage provenance")
    require(str(provenance.get("config", "")).lower() == "release", "Stage config must be Release")
    expected = provenance.get("runtimeSha256", "")
    require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected),
            "runtimeSha256 must be a lowercase SHA-256")
    runtime = stage / "runtime" / (
        "valdi_runtime.aar" if platform == "android" else "ValdiRuntime.xcframework")
    require(digest(runtime) == expected, "Runtime hash mismatch")
    modules = stage / "package/modules"
    for module in ("drawing", "render_benchmark"):
        nonempty(modules / f"{module}.valdimodule")
    # Maps are optional; providers recognize them when supplied.
    resources = stage / f"generated/resources/{platform}"
    require(resources.is_dir(), f"Missing generated resources directory: {resources}")
    package_provenance = stage / "package/provenance.json"
    if package_provenance.exists():
        require(json.loads(package_provenance.read_text()) == provenance,
                "package/provenance.json differs from stage/provenance.json")
    generated = stage / f"generated/{platform}"
    require(generated.is_dir(), f"Missing generated source directory: {generated}")
    if platform == "android":
        with zipfile.ZipFile(runtime) as archive:
            require("classes.jar" in archive.namelist()
                    and archive.getinfo("classes.jar").file_size > 0, "Runtime AAR has no classes.jar")
            native = "jni/arm64-v8a/libvaldi_runtime.so"
            require(native in archive.namelist() and archive.getinfo(native).file_size > 0,
                    "Runtime AAR must contain its hashed arm64-v8a libvaldi_runtime.so")
        require(not (stage / "jniLibs/arm64-v8a/libvaldi_runtime.so").exists(),
                "Do not override the AAR's hashed runtime through jniLibs")
        sources = "\n".join(p.read_text() for p in sorted(generated.rglob("*.kt")))
        require("package com.snap.modules.render_benchmark" in sources
                and "class BenchmarkContext" in sources
                and "nowMs" in sources and "report" in sources,
                "Inspect emitted Kotlin BenchmarkContext(report, nowMs)")
    else:
        nonempty(runtime / "Info.plist")
        info = plistlib.loads((runtime / "Info.plist").read_bytes())
        require(bool(info.get("AvailableLibraries")), "XCFramework has no AvailableLibraries")
        for library in info["AvailableLibraries"]:
            framework = runtime / library["LibraryIdentifier"] / library["LibraryPath"]
            if framework.suffix == ".framework":
                nonempty(framework / framework.stem)
            else:
                nonempty(framework)
        header = generated / "RenderBenchmarkTypes/RenderBenchmarkTypes.h"
        nonempty(header)
        source = header.read_text()
        require("RenderBenchmarkBenchmarkContext" in source and "initWithReport:" in source
                and "nowMs:" in source, "Inspect emitted Objective-C context initializer")
        require((resources / "render_benchmark.bundle").is_dir(),
                "Missing render_benchmark.bundle resources")
    # Optional parent-owned manifest. Every listed path must be relative and
    # contained in the stage; directory hashes use the same tree format above.
    manifest = stage / "host-manifest.json"
    if manifest.exists():
        hashes = json.loads(manifest.read_text()).get("sha256")
        require(isinstance(hashes, dict) and bool(hashes), "host-manifest.json needs sha256 entries")
        for relative, expected_hash in hashes.items():
            require(isinstance(relative, str) and not Path(relative).is_absolute(),
                    "Manifest paths must be relative")
            entry = stage / relative
            require(entry.resolve().is_relative_to(stage.resolve()),
                    f"Manifest path escapes stage: {relative}")
            require(digest(entry) == expected_hash, f"Manifest hash mismatch: {relative}")
    print(f"Stage files verified: backend={provenance['backend']} config={provenance['config']} "
          f"runtimeSha256={expected}. No runtime/device acceptance claimed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("android", "ios"))
    parser.add_argument("--backend", choices=("skia", "tgfx"))
    args = parser.parse_args()
    validate(args.stage.resolve(), args.platform, args.backend)


if __name__ == "__main__":
    main()
