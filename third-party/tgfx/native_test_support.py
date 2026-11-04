"""Prepare internal test headers and exact native preprocessor definitions."""

import hashlib
from pathlib import Path
import re
import shlex
import shutil

from source_archive import contained_source_path


HEADER_SUFFIXES = {".h", ".hpp", ".inl", ".inc"}
COMPILE_ANCHORS = [
    "src/core/utils/Task.cpp",
    "src/gpu/DrawingManager.cpp",
    "src/gpu/ProxyProvider.cpp",
    "src/gpu/VertexProvider.cpp",
    "src/gpu/tasks/GPUBufferUploadTask.cpp",
]
ABI_HEADERS = [
    "include/tgfx/core/Task.h",
    "src/core/utils/TaskGroup.h",
    "src/gpu/DrawingManager.h",
    "src/gpu/ProxyProvider.h",
    "src/gpu/VertexProvider.h",
    "src/gpu/tasks/GPUBufferUploadTask.h",
]


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _ninja_literal(value):
    """Decode literal Ninja escapes; never guess unresolved variable expansion."""
    value = re.sub(r"\$([ $:])", lambda match: match[1], value)
    if "$" in value:
        raise ValueError("Internal test configuration requires literal native Ninja bindings")
    return value


def _compile_binding(text, relative):
    object_name = "CMakeFiles/tgfx.dir/" + relative + ".o"
    prefix = "build " + object_name + ": "
    lines = text.replace("$\n", "").splitlines()
    matches = [index for index, line in enumerate(lines) if line.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one native compile record for {relative}")
    index = matches[0]
    command = lines[index][len(prefix):].split(" ", 1)
    if len(command) != 2:
        raise ValueError(f"Missing native source input for {relative}")
    source = _ninja_literal(command[1].split(" |", 1)[0])
    bindings = {}
    for line in lines[index + 1:]:
        if not line.startswith("  "):
            break
        if " = " in line:
            name, value = line.strip().split(" = ", 1)
            bindings[name] = _ninja_literal(value)
    if "DEFINES" not in bindings or "FLAGS" not in bindings:
        raise ValueError(f"Missing native DEFINES/FLAGS for {relative}")
    return object_name, source, bindings


def _definitions(value, definitions_only=False):
    words = shlex.split(value)
    definitions = []
    index = 0
    while index < len(words):
        word = words[index]
        if word == "-D":
            index += 1
            if index == len(words):
                raise ValueError("Native -D is missing its definition")
            definition = words[index]
        elif word.startswith("-D"):
            definition = word[2:]
        elif word.startswith("-U"):
            raise ValueError("Native undefinitions cannot be exported through test_support defines")
        else:
            if definitions_only:
                raise ValueError(f"Unexpected native DEFINES token: {word}")
            index += 1
            continue
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(?:=[^\s'\"\\]+)?", definition):
            raise ValueError(f"Unsupported native test definition: {definition}")
        definitions.append(definition)
        index += 1
    result = {}
    for definition in definitions:
        name = definition.split("=", 1)[0]
        if name in result and result[name] != definition:
            raise ValueError(f"Conflicting native definition for {name}")
        result[name] = definition
    return list(result.values())


def collect_native_test_configuration(profile, build, source, cache, archive_info, archive_objects):
    """Read actual compile records and require their real object bytes in tgfx.a."""
    build, source = build.resolve(strict=True), source.resolve(strict=True)
    if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != source:
        raise ValueError("Native internal-test source root differs from CMake source root")
    ninja = build / "build.ninja"
    before = digest_file(ninja)
    text = ninja.read_text()
    anchors = {}
    common = None
    for relative in COMPILE_ANCHORS:
        object_name, native_source, bindings = _compile_binding(text, relative)
        if Path(native_source).resolve() != (source / relative).resolve():
            raise ValueError(f"Native compile record uses a different source for {relative}")
        explicit = _definitions(bindings["DEFINES"], definitions_only=True)
        from_flags = _definitions(bindings["FLAGS"])
        combined = _definitions(" ".join("-D" + item for item in explicit + from_flags))
        if common is None:
            common = (explicit, from_flags, combined)
        elif common != (explicit, from_flags, combined):
            raise ValueError(f"Native internal-test owners have inconsistent definitions: {relative}")
        object_path = build / object_name
        digest = digest_file(object_path)
        if digest not in archive_objects:
            raise ValueError(f"Native owner object is not in the supplied TGFX archive: {relative}")
        anchors[relative] = {
            "object": str(object_path), "object_sha256": digest,
            "source_sha256": digest_file(source / relative),
            "native_defines": bindings["DEFINES"], "native_flags": bindings["FLAGS"],
        }
    explicit, from_flags, combined = common
    macros = {item.split("=", 1)[0]: item.split("=", 1)[1] if "=" in item else "1"
              for item in combined}
    for option in [
        "THREADS", "FREETYPE", "METAL", "PNG_DECODE", "PNG_ENCODE",
        "JPEG_DECODE", "JPEG_ENCODE", "WEBP_DECODE", "WEBP_ENCODE",
    ]:
        macro = "TGFX_USE_" + option
        if (cache.get(macro) == "ON") != (macro in macros):
            raise ValueError(f"Native test definitions disagree with CMake {macro}")
        if macro in macros and macros[macro] != "1":
            raise ValueError(f"Unexpected native feature value: {macro}={macros[macro]}")
    if macros.get("TGFX_USE_THREADS") != "1":
        raise ValueError("The full internal GPU fixture requires actual native TGFX_USE_THREADS")
    if profile.startswith(("macos_", "ios_")):
        if macros.get("TGFX_USE_METAL") != "1" or macros.get("TGFX_USE_CORE_GRAPHICS") != "1":
            raise ValueError("Apple internal tests require native Metal/CoreGraphics definitions")
        if "TGFX_USE_FREETYPE" in macros:
            raise ValueError("Apple internal tests do not match the native CoreGraphics font layout")
    else:
        if macros.get("TGFX_USE_FREETYPE") != "1" or macros.get("TGFX_USE_GLYPH_TO_UNICODE") != "1":
            raise ValueError("Android internal tests require native FreeType/glyph-to-Unicode definitions")
        if cache.get("TGFX_USE_OPENGL") != "ON" or "TGFX_USE_METAL" in macros:
            raise ValueError("Android internal test configuration is not the native OpenGLES build")
    if before != digest_file(ninja):
        raise ValueError("Native Ninja compile metadata changed during internal-test inspection")
    return {
        "schema": 1, "profile": profile, "source_root": str(source),
        "defines": combined, "explicit_defines": explicit, "flag_defines": from_flags,
        "archive_sha256": archive_info["sha256"],
        "build_ninja": str(ninja), "build_ninja_sha256": before,
        "cmake_cache": str(build / "CMakeCache.txt"),
        "cmake_cache_sha256": digest_file(build / "CMakeCache.txt"),
        "compile_anchors": anchors,
        "abi_headers_sha256": {name: digest_file(source / name) for name in ABI_HEADERS},
    }


def stage_test_support(destination, tgfx, sources, native):
    """Copy header bytes only from the already verified TGFX/dependency snapshots."""
    destination.mkdir(parents=True, exist_ok=True)
    for relative, anchor in native["compile_anchors"].items():
        if digest_file(tgfx / relative) != anchor["source_sha256"]:
            raise ValueError(f"Staged internal-test source differs from the native source: {relative}")
        if digest_file(Path(anchor["object"])) != anchor["object_sha256"]:
            raise ValueError(f"Native owner object changed before header staging: {relative}")
    for relative, digest in native["abi_headers_sha256"].items():
        if digest_file(tgfx / relative) != digest:
            raise ValueError(f"Staged internal ABI header differs from native snapshot: {relative}")
    for key in ["build_ninja", "cmake_cache"]:
        if digest_file(Path(native[key])) != native[key + "_sha256"]:
            raise ValueError(f"Native metadata changed before internal-header staging: {key}")

    internal = tgfx / "src"
    queue = tgfx / "third_party/concurrentqueue"
    skcms = tgfx / "third_party/skcms"
    internal_files = sorted(path for path in internal.rglob("*") if path.suffix in HEADER_SUFFIXES)
    # Production concurrentqueue headers only; benchmark/relacy suites are not
    # native TGFX inputs and must not accidentally become another include tree.
    queue_files = sorted(path for path in queue.iterdir() if path.suffix in HEADER_SUFFIXES)
    if (queue / "internal").exists():
        queue_files += sorted(path for path in (queue / "internal").rglob("*")
                              if path.suffix in HEADER_SUFFIXES)
    if not (queue / "concurrentqueue.h").is_file() or not (queue / "lightweightsemaphore.h").is_file():
        raise ValueError("Verified concurrentqueue snapshot lacks required native headers")
    for root, files, prefix in [
        (internal, internal_files, "tgfx_src"),
        (queue, queue_files, "concurrentqueue"),
        # ColorSpaceXformHelper.h uses <skcms.h>, whose public API includes
        # src/skcms_public.h relative to that dependency's own include root.
        (skcms, [skcms / "skcms.h", skcms / "src/skcms_public.h"], "skcms"),
    ]:
        for original in files:
            actual = contained_source_path(tgfx, original)
            if not actual.is_file():
                raise ValueError(f"Verified internal header is missing/not a file: {original}")
            target = destination / prefix / original.relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(actual, target)
    guard = ["#pragma once", "// Generated from the native Task.cpp.o compile record."]
    for definition in native["defines"]:
        name, _, value = definition.partition("=")
        guard.extend([f"#ifndef {name}", f'#error "Missing native TGFX definition: {name}"', "#endif"])
        if not value or re.fullmatch(r"[0-9]+", value):
            guard.extend([f"#if {name} != {value or '1'}",
                          f'#error "Conflicting native TGFX definition: {name}"', "#endif"])
    (destination / "tgfx_test_native_profile.h").write_text("\n".join(guard) + "\n")
    source_digests = {}
    for key in ["tgfx", "tgfx/third_party/concurrentqueue", "tgfx/third_party/skcms"]:
        item = sources[key]
        source_digests[key] = item.get("tracked_files_sha256", item.get("verified_files_sha256"))
        if not source_digests[key]:
            raise ValueError(f"Missing verified source-snapshot digest for {key}")
    return {
        "schema": 1, "profile": native["profile"],
        "defines": native["defines"], "native_configuration": native,
        "archive_sha256": native["archive_sha256"],
        "source_snapshots_sha256": source_digests,
        "provenance": "Test-only copied internal headers from the same verified source snapshots; native owner objects verified in the matching archive.",
    }
