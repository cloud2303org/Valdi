"""Import parent-built official native outputs using only file/metadata inspection."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile

from prepare import DEPS, HERE, PINS, prepare, sha256, stage_sources
from native_test_support import collect_native_test_configuration


def ar_members(path):
    """Read ordinary BSD/GNU archives directly; no ar, compiler, or linker invocation."""
    with path.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise ValueError(f"Not a regular static archive (thin archives unsupported): {path}")
        strings = b""
        while True:
            header = stream.read(60)
            if not header:
                return
            if len(header) != 60 or header[58:] != b"`\n":
                raise ValueError(f"Malformed ar member in {path}")
            size = int(header[48:58].strip())
            name = header[:16].decode().strip()
            data = stream.read(size)
            if len(data) != size:
                raise ValueError(f"Truncated ar member in {path}")
            if size % 2:
                stream.read(1)
            if name == "//":
                strings = data
                continue
            if name in {"/", "/SYM64/"}:
                continue
            if name.startswith("#1/"):
                length = int(name[3:])
                name, data = data[:length].rstrip(b"\0").decode(), data[length:]
            elif name.startswith("/") and name[1:].isdigit():
                start = int(name[1:])
                name = strings[start:].split(b"/\n", 1)[0].decode()
            else:
                name = name.rstrip("/")
            if name.startswith("__.SYMDEF"):
                continue
            yield name, data


def inspect_artifact(path, profile, expected=None):
    """Verify CPU/platform from real object headers and bind exact archive bytes."""
    before = path.stat()
    digest = sha256(path)
    if expected is not None and digest != expected:
        raise ValueError(f"Parent-supplied expected archive SHA256 mismatch: {path}")
    objects = set()
    deployments = set()
    platforms = {
        "macos_arm64": 1, "ios_device_arm64": 2, "ios_sim_arm64": 7,
    }
    for name, data in ar_members(path):
        if data[:4] == b"\xcf\xfa\xed\xfe":
            if profile not in platforms:
                raise ValueError(f"Mach-O object in Android profile: {path}:{name}")
            cpu, = struct.unpack_from("<I", data, 4)
            if cpu != 0x100000C:
                raise ValueError(f"Expected arm64 Mach-O object: {path}:{name}")
            commands, = struct.unpack_from("<I", data, 16)
            position, found = 32, False
            for _ in range(commands):
                command, size = struct.unpack_from("<II", data, position)
                if size < 8 or position + size > len(data):
                    raise ValueError(f"Malformed Mach-O load command: {path}:{name}")
                if command == 0x32:  # LC_BUILD_VERSION, including simulator vs device.
                    target, minimum = struct.unpack_from("<II", data, position + 8)
                    if target != platforms[profile]:
                        raise ValueError(f"Wrong Apple platform {target} for {profile}: {path}:{name}")
                    deployments.add((minimum >> 16, (minimum >> 8) & 255, minimum & 255))
                    found = True
                position += size
            if not found:
                raise ValueError(f"No explicit LC_BUILD_VERSION platform evidence: {path}:{name}")
        elif data[:4] == b"\x7fELF":
            if profile not in {"android_arm64", "android_x86_64"}:
                raise ValueError(f"ELF object in Apple profile: {path}:{name}")
            if data[4:6] != b"\x02\x01":
                raise ValueError(f"Expected 64-bit little-endian ELF: {path}:{name}")
            machine, = struct.unpack_from("<H", data, 18)
            if machine != (183 if profile == "android_arm64" else 62):
                raise ValueError(f"Wrong ELF CPU for {profile}: {path}:{name}")
        else:
            # An uninspectable bitcode object cannot attest target identity.
            raise ValueError(f"Unsupported/non-object archive member: {path}:{name}")
        objects.add(hashlib.sha256(data).hexdigest())
    after = path.stat()
    if not objects or (before.st_ino, before.st_size, before.st_mtime_ns) != (
        after.st_ino, after.st_size, after.st_mtime_ns
    ):
        raise ValueError(f"Empty or concurrently modified archive: {path}")
    return {
        "path": str(path), "sha256": digest, "profile": profile,
        "unique_object_count": len(objects),
        "maximum_object_deployment": list(max(deployments)) if deployments else None,
    }, objects


def cmake_cache(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith(("//", "#")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.split(":", 1)[0]] = value
    return result


def require_options(actual, expected, description):
    for key, value in expected.items():
        if actual.get(key) != value:
            raise ValueError(f"{description}: expected {key}={value!r}, found {actual.get(key)!r}")


def import_existing(args):
    if (DEPS / args.profile).exists():
        raise ValueError(f"Refusing to overwrite existing prepared profile: {args.profile}")
    if (DEPS / "receipt.json").exists():
        receipt = json.loads((DEPS / "receipt.json").read_text())
        if receipt.get("pins") != PINS or receipt.get("schema") != 1:
            raise ValueError("Existing prepared receipt uses different source pins/schema")
    tgfx_build = args.tgfx_build.resolve(strict=True)
    thorvg_build = args.thorvg_build.resolve(strict=True)
    metadata = [
        tgfx_build / "CMakeCache.txt", tgfx_build / "build.ninja",
        thorvg_build / "build.ninja", thorvg_build / "config.h",
    ] + [
        thorvg_build / "meson-info" / name for name in [
            "meson-info.json", "intro-buildoptions.json", "intro-machines.json",
            "intro-buildsystem_files.json", "intro-targets.json", "intro-compilers.json",
        ]
    ] + [args.tgfx_build_log.resolve(strict=True), args.thorvg_build_log.resolve(strict=True)]
    system_files = list((tgfx_build / "CMakeFiles").glob("*/CMakeSystem.cmake"))
    if len(system_files) != 1:
        raise ValueError("Expected one generated target CMakeSystem.cmake")
    metadata += system_files
    before = {str(path): sha256(path) for path in metadata}
    cache = cmake_cache(tgfx_build / "CMakeCache.txt")
    require_options(cache, {
        "CMAKE_HOME_DIRECTORY": str(args.tgfx_source.resolve()),
        **{f"TGFX_USE_{codec}_{operation}": "ON"
           for codec in ["PNG", "JPEG", "WEBP"] for operation in ["DECODE", "ENCODE"]},
        **{f"TGFX_BUILD_{feature}": "OFF" for feature in ["TESTS", "HELLO2D", "FRAMEWORK", "SVG", "PDF", "LAYERS"]},
        **{f"TGFX_USE_{backend}": "OFF" for backend in ["VULKAN", "D3D12", "WEBGPU", "QT", "ANGLE", "SWIFTSHADER"]},
    }, "TGFX CMake cache")
    apple = PINS["profiles"][args.profile]["system"] == "darwin"
    require_options(cache, {
        "TGFX_USE_METAL": "ON" if apple else "OFF",
        "TGFX_USE_OPENGL": "OFF" if apple else "ON",
    }, "TGFX GPU backend")
    freetype = cache.get("TGFX_USE_FREETYPE") == "ON"
    if not freetype and not (apple and args.allow_coregraphics):
        raise ValueError("FreeType is disabled; only explicit Apple --allow-coregraphics permits this")
    if apple and not freetype and "-DTGFX_USE_CORE_GRAPHICS" not in (tgfx_build / "build.ninja").read_text():
        raise ValueError("Apple FreeType-off import lacks the actual CoreGraphics compile definition")
    if not apple:
        require_options(cache, {
            "ANDROID_ABI": PINS["profiles"][args.profile]["abi"],
            "ANDROID_STL": "c++_shared",
            "TGFX_ANDROID_ENABLE_RTTI": "ON",
        }, "TGFX Android target/stdlib/RTTI")
    info = json.loads((thorvg_build / "meson-info/meson-info.json").read_text())
    if Path(info["directories"]["source"]).resolve() != args.thorvg_source.resolve():
        raise ValueError("ThorVG Meson source directory does not match the pinned source")
    options = {item["name"]: item["value"] for item in json.loads(
        (thorvg_build / "meson-info/intro-buildoptions.json").read_text()
    )}
    require_options(options, {
        "default_library": "static", "static": True, "b_staticpic": True,
        "engines": ["cpu"], "tests": False, "extra": [],
    }, "ThorVG Meson options")
    required_loaders = {"svg", "lottie", "ttf", "otf", "png", "jpg", "webp"}
    if not required_loaders.issubset(options.get("loaders", [])):
        raise ValueError("ThorVG lacks required actual CPU SVG/Lottie/raster asset loaders")
    machine = json.loads((thorvg_build / "meson-info/intro-machines.json").read_text())["host"]
    require_options(machine, {
        "system": PINS["profiles"][args.profile]["system"],
        "cpu_family": PINS["profiles"][args.profile]["cpu"],
    }, "ThorVG host machine")
    artifacts = {
        "lib/tgfx.a": tgfx_build / "tgfx.a",
        "lib/libthorvg-1.a": thorvg_build / "src/libthorvg-1.a",
    }
    tgfx_info, tgfx_objects = inspect_artifact(artifacts["lib/tgfx.a"], args.profile, args.tgfx_sha256)
    test_native = collect_native_test_configuration(
        args.profile, tgfx_build, args.tgfx_source, cache, tgfx_info, tgfx_objects,
    )
    thorvg_info, main_objects = inspect_artifact(
        artifacts["lib/libthorvg-1.a"], args.profile, args.thorvg_sha256
    )
    simd = {"requested": options["simd"], "closure": "explicit_scalar_recipe"}
    if options["simd"]:
        config = (thorvg_build / "config.h").read_text()
        enabled = bool(re.search(r"#define THORVG_(NEON|AVX)_SUPPORT 1", config))
        if enabled:
            if not args.thorvg_simd_sha256:
                raise ValueError("Effective ThorVG SIMD requires --thorvg-simd-sha256")
            side = thorvg_build / "src/renderer/cpu_engine/libthorvg-sw-simd.a"
            side_info, side_objects = inspect_artifact(side, args.profile, args.thorvg_simd_sha256)
            if not side_objects.issubset(main_objects):
                raise ValueError("ThorVG main archive does not embed every SIMD object; import refuses incomplete closure")
            simd.update({
                "closure": "verified_simd_objects_embedded_by_meson_link_whole",
                "side_archive": side_info,
                "embedded_object_sha256": sorted(side_objects),
            })
            artifacts["lib/libthorvg-sw-simd.a"] = side
        else:
            simd["closure"] = "requested_but_unsupported_scalar_config"
    for artifact in [tgfx_info, thorvg_info]:
        minimum = artifact["maximum_object_deployment"]
        if minimum:
            floor = tuple(int(part) for part in PINS["profiles"][args.profile]["minimum"].split("."))
            floor = (floor + (0, 0, 0))[:3]
            if tuple(minimum) > floor and not args.allow_higher_deployment:
                raise ValueError(
                    f"{artifact['path']} requires deployment {minimum}, above profile floor {floor}; "
                    "use a compatible consumer floor and explicit --allow-higher-deployment"
                )
    approved = dict(item.rsplit("=", 1) for item in args.allow_patch)
    DEPS.mkdir(exist_ok=True)
    imports = DEPS / "imports"
    imports.mkdir(exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=args.profile + "-", dir=imports))
    tgfx, thorvg, _, sources = stage_sources(args, directory, approved, use_freetype=freetype)
    # Vendor generated headers have PLATFORM/include, never PLATFORM/ARCH/include.
    generated = args.tgfx_source.resolve() / "third_party/out/zlib" / (
        PINS["profiles"][args.profile]["vendor_platform"]
    ) / "include"
    generated_snapshot = directory / "generated-zlib"
    generated_snapshot.mkdir()
    for header in ["zlib.h", "zconf.h"]:
        original = generated / header
        before[str(original)] = sha256(original)
        shutil.copy2(original, generated_snapshot / header)
    for path, digest in before.items():
        if sha256(Path(path)) != digest:
            raise ValueError(f"Native metadata/header changed during import: {path}")
    for name, path in artifacts.items():
        expected = {
            "lib/tgfx.a": args.tgfx_sha256, "lib/libthorvg-1.a": args.thorvg_sha256,
            "lib/libthorvg-sw-simd.a": args.thorvg_simd_sha256,
        }[name]
        if sha256(path) != expected:
            raise ValueError(f"Native archive changed during import: {path}")
    manifest = {
        "schema": 1, "profile": args.profile, "pins": PINS,
        "sources": sources, "native_metadata_sha256": before,
        "tgfx_configuration": cache, "thorvg_configuration": options,
        "thorvg_simd": simd, "artifacts": [tgfx_info, thorvg_info],
        "vector_backend": "freetype" if freetype else "apple_coregraphics",
        "profile_options": PINS["profiles"][args.profile],
        "recipe_sha256": {name: sha256(HERE / name) for name in [
            "prepare.py", "source_archive.py", "import_existing.py", "native_test_support.py",
        ]},
        "provenance": (
            "Existing parent-owned native outputs imported without rebuilding. "
            "Current source trees, source archives, build configuration/logs, object target identity "
            "and exact artifact hashes verified at import; no retrospective/hermetic build attestation."
        ),
    }
    prepare(
        args.profile, directory, tgfx, thorvg, manifest,
        archive_inputs=artifacts, generated_zlib=generated_snapshot, test_native=test_native,
    )
