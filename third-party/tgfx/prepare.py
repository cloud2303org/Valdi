#!/usr/bin/env python3
"""Build official sources, then prepare local Bazel imports; never import Valdi archives.

The build subcommand is intentionally opt-in and is for the parent's native slot.
The verify subcommand only checks already prepared source/archive hash receipts.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

from source_archive import contained_source_path, load_receipts, snapshot_archive, verified_symlink_text
from native_test_support import collect_native_test_configuration, stage_test_support


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PINS = json.loads((HERE / "pins.json").read_text())
DEPS = ROOT / ".tgfx-deps"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def output(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd)


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def snapshot(source, destination, expected, key, approved_patches):
    """Copy tracked working-tree files, excluding all vendor binary packages/caches."""
    source = source.resolve(strict=True)
    top = Path(output("git", "rev-parse", "--show-toplevel", cwd=source).decode().strip())
    if top.resolve() != source:
        raise ValueError(
            f"{key}: source is not its own Git checkout: {source}; "
            "for extracted dependencies supply explicit --dependency-receipts"
        )
    actual = output("git", "rev-parse", "HEAD", cwd=source).decode().strip()
    if actual != expected:
        raise ValueError(f"{key}: expected {expected}, found {actual}")
    patch = output(
        "git", "diff", "--no-ext-diff", "--no-textconv", "--binary", "HEAD", "--", ".", cwd=source
    )
    patch_hash = hashlib.sha256(patch).hexdigest() if patch else None
    if patch and approved_patches.get(key) != patch_hash:
        raise ValueError(
            f"{key}: modified tracked sources; review the diff, then explicitly pass "
            f"--allow-patch {key}={patch_hash}"
        )
    if not patch and key in approved_patches:
        raise ValueError(f"{key}: approved patch is no longer present")
    files = output("git", "ls-files", "-z", cwd=source).split(b"\0")
    destination.mkdir(parents=True, exist_ok=True)
    copied = {}
    for name in files:
        if not name:
            continue
        relative = Path(os.fsdecode(name))
        # Disabled ANGLE/SwiftShader prebuilt packages are not experiment inputs.
        if relative.parts[0] == "vendor":
            continue
        original = source / relative
        # Check ancestors before accessing the tracked entry: a replaced
        # directory must not redirect even metadata/content reads outside root.
        contained_source_path(source, original.parent)
        if original.suffix in {".a", ".so", ".dylib", ".dll", ".lib", ".o", ".obj"}:
            raise ValueError(f"{key}: refusing tracked binary input {relative}")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if original.is_symlink():
            # Preserve file and directory symlinks as links. Hash the same
            # explicit representation used by source-archive snapshots.
            link = verified_symlink_text(source, original)
            target.symlink_to(link)
            copied[relative.as_posix()] = "symlink:" + link
            continue
        if not original.is_file():
            raise ValueError(f"{key}: tracked source missing or not a file: {relative}")
        shutil.copy2(original, target)
        copied[relative.as_posix()] = sha256(target)
    if patch:
        (destination.parent / (destination.name + ".source.patch")).write_bytes(patch)
    return {
        "source": str(source),
        "input_kind": "git_working_tree",
        "commit": actual,
        "approved_patch_sha256": patch_hash,
        "tracked_files_sha256": hashlib.sha256(
            json.dumps(copied, sort_keys=True).encode()
        ).hexdigest(),
    }


def stage_sources(args, directory, approved, use_freetype=None):
    if use_freetype is None:
        use_freetype = PINS["profiles"][args.profile]["system"] != "darwin"
    tgfx = directory / "source" / "tgfx"
    thorvg = directory / "source" / "thorvg"
    sources = {
        "tgfx": snapshot(args.tgfx_source, tgfx, PINS["tgfx"], "tgfx", approved),
        "thorvg": snapshot(args.thorvg_source, thorvg, PINS["thorvg"], "thorvg", approved),
    }
    names = {
        "vendor_tools", "pathkit", "skcms", "zlib", "libpng", "libwebp",
        "libjpeg-turbo", "concurrentqueue", "highway",
    }
    if use_freetype:
        names.add("freetype")
    if PINS["profiles"][args.profile]["system"] == "darwin":
        names.update({
            "shaderc", "glslang", "spirv-headers", "spirv-tools", "SPIRV-Cross",
        })
    definitions = json.loads((tgfx / "DEPS").read_text())["repos"]["common"]
    receipts = load_receipts(args.dependency_receipts)
    for dependency in definitions:
        relative = Path(dependency["dir"])
        if relative.name not in names:
            continue
        key = "tgfx/" + relative.as_posix()
        if relative.name in receipts:
            sources[key] = snapshot_archive(
                args.tgfx_source / relative, tgfx / relative,
                dependency["commit"], key, receipts[relative.name],
            )
        else:
            sources[key] = snapshot(
                args.tgfx_source / relative, tgfx / relative,
                dependency["commit"], key, approved,
            )
        names.remove(relative.name)
    if names:
        raise ValueError(f"TGFX DEPS lacks required source pins: {sorted(names)}")
    # vendor.cmake invokes depsync at configure time. All enabled dependencies
    # were copied and verified above; prevent a sync from replacing that snapshot.
    tools = directory / "tools"
    tools.mkdir()
    depsync = tools / "depsync"
    depsync.write_text(
        "#!/bin/sh\n"
        "# No download: this fresh snapshot was verified against TGFX DEPS.\n"
        'test -f "$PWD/.tgfx-sources-verified"\n'
    )
    depsync.chmod(0o755)
    write_json(tgfx / ".tgfx-sources-verified", sources)
    return tgfx, thorvg, tools, sources


def meson_machine_literal(value):
    """Render only the string/list values used by these Meson machine files."""
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(meson_machine_literal(item) for item in value) + "]"
    if not isinstance(value, str):
        raise ValueError("Meson machine-file values must be strings or lists of strings")
    # These profiles use POSIX paths. Reject characters requiring escape rules
    # rather than letting Python/JSON choose unsupported double-quoted syntax.
    if any(character in "'\\" or ord(character) < 32 or ord(character) == 127 for character in value):
        raise ValueError("Meson machine-file strings cannot contain single quotes, backslashes, or control characters")
    return "'" + value + "'"


def toolchain(args, tgfx, directory):
    profile = PINS["profiles"][args.profile]
    if profile["system"] == "darwin":
        if platform.system() != "Darwin":
            raise ValueError("Apple profiles require a macOS/Xcode host")
        sdk = output("xcrun", "--sdk", profile["sdk"], "--show-sdk-path").decode().strip()
        clang = output("xcrun", "--sdk", profile["sdk"], "--find", "clang").decode().strip()
        clangxx = output("xcrun", "--sdk", profile["sdk"], "--find", "clang++").decode().strip()
        ar = output("xcrun", "--find", "ar").decode().strip()
        strip = output("xcrun", "--find", "strip").decode().strip()
        if args.profile == "macos_arm64":
            triple = "arm64-apple-macos" + profile["minimum"]
            cmake = [
                "-DCMAKE_OSX_ARCHITECTURES=arm64",
                "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + profile["minimum"],
                "-DCMAKE_OSX_SYSROOT=" + sdk,
                "-DCMAKE_C_COMPILER=" + clang,
                "-DCMAKE_CXX_COMPILER=" + clangxx,
            ]
        else:
            triple = "arm64-apple-ios" + profile["minimum"]
            if args.profile == "ios_sim_arm64":
                triple += "-simulator"
            cmake = [
                "-DCMAKE_TOOLCHAIN_FILE=" + str(tgfx / "third_party/vendor_tools/ios.toolchain.cmake"),
                "-DPLATFORM=" + ("SIMULATORARM64" if args.profile == "ios_sim_arm64" else "OS64"),
                "-DDEPLOYMENT_TARGET=" + profile["minimum"],
                "-DCMAKE_OSX_SYSROOT=" + sdk,
                "-DENABLE_BITCODE=OFF",
            ]
        flags = ["-target", triple, "-isysroot", sdk, "-fPIC"]
        identity = {"sdk": sdk, "triple": triple, "clang": clangxx}
    else:
        if not args.ndk:
            raise ValueError("Android profiles require --ndk pointing to the Bazel-compatible NDK")
        ndk = args.ndk.resolve(strict=True)
        host = args.ndk_host_tag or (
            "darwin-x86_64" if platform.system() == "Darwin" else "linux-x86_64"
        )
        bin_dir = ndk / "toolchains/llvm/prebuilt" / host / "bin"
        triple = profile["triple"] + str(profile["api"])
        clang = str(bin_dir / (triple + "-clang"))
        clangxx = str(bin_dir / (triple + "-clang++"))
        ar, strip = str(bin_dir / "llvm-ar"), str(bin_dir / "llvm-strip")
        cmake = [
            "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
            "-DANDROID_NDK=" + str(ndk),
            "-DANDROID_ABI=" + profile["abi"],
            "-DANDROID_PLATFORM=android-" + str(profile["api"]),
            "-DANDROID_STL=c++_shared",
        ]
        flags = ["-fPIC"]
        identity = {
            "ndk": str(ndk),
            "ndk_source_properties_sha256": sha256(ndk / "source.properties"),
            "host_tag": host, "triple": triple, "stdlib": "c++_shared",
        }
    cross = directory / "thorvg.cross.ini"
    # Meson machine files require single-quoted string literals, also in arrays.
    q = meson_machine_literal
    cross.write_text(
        "[binaries]\n"
        f"c = {q(clang)}\ncpp = {q(clangxx)}\nar = {q(ar)}\nstrip = {q(strip)}\n"
        "[host_machine]\n"
        f"system = {q(profile['system'])}\n"
        f"cpu_family = {q(profile['cpu'])}\ncpu = {q(profile['cpu'])}\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n"
        "[built-in options]\n"
        f"c_args = {q(flags)}\ncpp_args = {q(flags)}\n"
        f"c_link_args = {q(flags)}\ncpp_link_args = {q(flags)}\n"
    )
    return cmake, cross, identity


def run(command, cwd, env, directory, commands):
    commands.append({"argv": command, "cwd": str(cwd)})
    write_json(directory / "commands.json", commands)
    print("+ " + shlex.join(command), flush=True)
    with (directory / "build.log").open("ab") as log:
        log.write(("\n+ " + shlex.join(command) + "\n").encode())
        log.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {directory / 'build.log'}")


def prepare(profile, directory, tgfx, thorvg, manifest, archive_inputs=None, generated_zlib=None,
            test_native=None):
    """Publish matching source headers and newly built archives via symlinks."""
    # Exact upstream output names; never glob for a convenient existing archive.
    inputs = {
        "lib/tgfx.a": directory / "tgfx-build/tgfx.a",
        "lib/libthorvg-1.a": directory / "thorvg-build/src/libthorvg-1.a",
        "include/tgfx": tgfx / "include/tgfx",
        "include/webp": tgfx / "third_party/libwebp/src/webp",
    }
    if archive_inputs:
        inputs.update(archive_inputs)
    generated = generated_zlib or (
        # vendor_tools exports PLATFORM/include shared by that platform's
        # architectures, not PLATFORM/ARCH/include.
        tgfx / "third_party/out/zlib" / PINS["profiles"][profile]["vendor_platform"] / "include"
    )
    inputs["include/zlib.h"] = generated / "zlib.h"
    inputs["include/zconf.h"] = generated / "zconf.h"
    destination = DEPS / profile
    if destination.exists():
        raise ValueError(f"{destination} already exists; use a new checkout or remove that profile explicitly")
    temporary = Path(tempfile.mkdtemp(prefix=profile + ".prepare-", dir=DEPS))
    try:
        for name, source in inputs.items():
            source = source.resolve(strict=True)
            if name.startswith("lib/"):
                with source.open("rb") as stream:
                    if stream.read(8) != b"!<arch>\n":
                        raise ValueError(f"Expected a real static archive: {source}")
            target = temporary / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(source, target_is_directory=source.is_dir())
        headers = temporary / "include/thorvg"
        headers.mkdir(parents=True)
        for source in [
            thorvg / "inc/thorvg.h",
            thorvg / "src/loaders/lottie/thorvg_lottie.h",
        ]:
            (headers / source.name).symlink_to(source.resolve(strict=True))
        if test_native is not None:
            if test_native["profile"] != profile:
                raise ValueError("Native internal-test configuration has the wrong profile")
            manifest["test_support"] = stage_test_support(
                temporary / "test_include", tgfx, manifest["sources"], test_native
            )
        files = {
            path.relative_to(temporary).as_posix(): sha256(path)
            for path in sorted(temporary.rglob("*")) if path.is_file()
        }
        # Path.rglob does not descend through directory symlinks on Python 3.11.
        for name in ["include/tgfx", "include/webp"]:
            for path in sorted((temporary / name).rglob("*.h")):
                files[path.relative_to(temporary).as_posix()] = sha256(path)
        if test_native is not None:
            if files["lib/tgfx.a"] != test_native["archive_sha256"]:
                raise ValueError("Native internal-test configuration belongs to a different TGFX archive")
            manifest["test_support"]["headers_sha256"] = {
                name: digest for name, digest in files.items() if name.startswith("test_include/")
            }
        manifest["files"] = files
        write_json(temporary / "manifest.json", manifest)
        temporary.rename(destination)
    except BaseException:
        shutil.rmtree(temporary)
        raise
    receipt_path = DEPS / "receipt.json"
    receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {
        "schema": 1, "pins": PINS, "profiles": {},
    }
    if receipt["pins"] != PINS:
        raise ValueError("Existing receipt uses different pins")
    receipt["profiles"][profile] = {"manifest_sha256": sha256(destination / "manifest.json")}
    write_json(receipt_path, receipt)
    print(f"Prepared {destination}; receipt: {receipt_path}", flush=True)


def build(args):
    if args.jobs < 1:
        raise ValueError("--jobs must be positive")
    if (DEPS / args.profile).exists():
        raise ValueError("Profile is already prepared; refusing to replace existing inputs")
    receipt_path = DEPS / "receipt.json"
    if receipt_path.exists():
        receipt = json.loads(receipt_path.read_text())
        if receipt.get("schema") != 1 or receipt.get("pins") != PINS:
            raise ValueError("Existing receipt uses different pins/schema")
    approved = {}
    for item in args.allow_patch:
        key, value = item.rsplit("=", 1)
        if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError("--allow-patch requires SOURCE_KEY=SHA256")
        approved[key] = value
    DEPS.mkdir(exist_ok=True)
    builds = DEPS / "build"
    builds.mkdir(exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=args.profile + "-", dir=builds))
    print(f"Fresh build directory: {directory}", flush=True)
    apple = PINS["profiles"][args.profile]["system"] == "darwin"
    use_freetype = not apple
    tgfx, thorvg, tools, sources = stage_sources(
        args, directory, approved, use_freetype=use_freetype
    )
    cmake_options, cross, identity = toolchain(args, tgfx, directory)
    env = dict(os.environ)
    env["PATH"] = str(tools) + os.pathsep + env["PATH"]
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
    commands = []
    cmake = [
        "cmake", "-S", str(tgfx), "-B", str(directory / "tgfx-build"), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DTGFX_BUILD_TESTS=OFF", "-DTGFX_BUILD_HELLO2D=OFF",
        "-DTGFX_BUILD_FRAMEWORK=OFF", "-DTGFX_BUILD_SVG=OFF",
        "-DTGFX_BUILD_PDF=OFF", "-DTGFX_BUILD_LAYERS=OFF",
        "-DTGFX_USE_FREETYPE=" + ("ON" if use_freetype else "OFF"),
        "-DTGFX_USE_THREADS=ON",
        "-DTGFX_USE_PNG_DECODE=ON", "-DTGFX_USE_PNG_ENCODE=ON",
        "-DTGFX_USE_JPEG_DECODE=ON", "-DTGFX_USE_JPEG_ENCODE=ON",
        "-DTGFX_USE_WEBP_DECODE=ON", "-DTGFX_USE_WEBP_ENCODE=ON",
        "-DTGFX_USE_METAL=" + ("ON" if apple else "OFF"),
        "-DTGFX_USE_OPENGL=" + ("OFF" if apple else "ON"),
        "-DTGFX_USE_VULKAN=OFF", "-DTGFX_USE_D3D12=OFF", "-DTGFX_USE_WEBGPU=OFF",
        "-DTGFX_USE_QT=OFF", "-DTGFX_USE_ANGLE=OFF", "-DTGFX_USE_SWIFTSHADER=OFF",
    ] + (["-DTGFX_ANDROID_ENABLE_RTTI=ON"] if not apple else []) + cmake_options
    run(cmake, tgfx, env, directory, commands)
    run(["cmake", "--build", str(directory / "tgfx-build"),
         "--target", "tgfx", "--parallel", str(args.jobs)], tgfx, env, directory, commands)
    from import_existing import cmake_cache, inspect_artifact
    native_build = directory / "tgfx-build"
    native_info, native_objects = inspect_artifact(native_build / "tgfx.a", args.profile)
    test_native = collect_native_test_configuration(
        args.profile, native_build, tgfx, cmake_cache(native_build / "CMakeCache.txt"),
        native_info, native_objects,
    )
    # Embedded PNG/JPEG/WebP loaders cover SVG/Lottie raster assets without
    # discovering host codecs or a second external codec ABI.
    run([
        "meson", "setup", str(directory / "thorvg-build"), str(thorvg),
        "--cross-file", str(cross), "--wrap-mode=nodownload",
        "--buildtype=release", "--default-library=static",
        "-Dstatic=true", "-Db_staticpic=true", "-Dengines=cpu",
        "-Dloaders=svg,lottie,ttf,otf,png,jpg,webp", "-Dsavers=",
        "-Dbindings=", "-Dextra=", "-Dtests=false",
        "-Dthreads=true", "-Dsimd=false",
    ], thorvg, env, directory, commands)
    run(["meson", "compile", "-C", str(directory / "thorvg-build"),
         "-j", str(args.jobs)], thorvg, env, directory, commands)
    manifest = {
        "schema": 1, "profile": args.profile, "pins": PINS,
        "sources": sources, "commands": commands, "toolchain": identity,
        "recipe_sha256": {name: sha256(HERE / name) for name in [
            "prepare.py", "source_archive.py", "import_existing.py", "native_test_support.py",
        ]},
        "profile_options": PINS["profiles"][args.profile],
        "build_log_sha256": sha256(directory / "build.log"),
        "cmake_cache_sha256": sha256(directory / "tgfx-build/CMakeCache.txt"),
        "meson_buildoptions_sha256": sha256(
            directory / "thorvg-build/meson-info/intro-buildoptions.json"
        ),
        "provenance": "Local official-source build with recorded patches; not a hermetic toolchain attestation.",
        "thorvg_simd": {"requested": False, "closure": "explicit_scalar_recipe"},
        "vector_backend": "freetype" if use_freetype else "apple_coregraphics",
    }
    prepare(args.profile, directory, tgfx, thorvg, manifest, test_native=test_native)


def verify(args):
    receipt = json.loads((DEPS / "receipt.json").read_text())
    if receipt.get("schema") != 1 or receipt.get("pins") != PINS:
        raise ValueError("Receipt pins/schema do not match")
    profiles = [args.profile] if args.profile else receipt["profiles"]
    for profile in profiles:
        manifest_path = DEPS / profile / "manifest.json"
        if sha256(manifest_path) != receipt["profiles"][profile]["manifest_sha256"]:
            raise ValueError(f"{profile}: manifest hash changed")
        manifest = json.loads(manifest_path.read_text())
        if manifest["profile"] != profile or manifest["pins"] != PINS:
            raise ValueError(f"{profile}: manifest profile/pins mismatch")
        for relative, digest in manifest["files"].items():
            if sha256(DEPS / profile / relative) != digest:
                raise ValueError(f"{profile}: prepared input hash changed: {relative}")
        print(f"{profile}: prepared archive/header hashes match the local receipt")


def add_giflib(args):
    """Bind a parent's real cross-build output; do not claim this importer built it."""
    # The given commit and expected archive hash are supplied by the native-slot
    # owner after inspecting/building its chosen upstream giflib source.
    if len(args.commit) != 40 or any(c not in "0123456789abcdef" for c in args.commit):
        raise ValueError("--commit must be the full upstream giflib Git commit")
    destination = DEPS / args.profile
    manifest_path = destination / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    receipt = json.loads((DEPS / "receipt.json").read_text())
    if manifest["pins"] != PINS or receipt["pins"] != PINS:
        raise ValueError("Prepared TGFX/ThorVG pins mismatch")
    if sha256(manifest_path) != receipt["profiles"][args.profile]["manifest_sha256"]:
        raise ValueError("Prepared profile manifest hash mismatch")
    if "lib/libgif.a" in manifest["files"]:
        raise ValueError("giflib is already bound for this profile")
    archive = args.archive.resolve(strict=True)
    if sha256(archive) != args.archive_sha256:
        raise ValueError("giflib archive does not match the parent's expected SHA256")
    with archive.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise ValueError("giflib input is not a static archive")
    build_log_hash = sha256(args.build_log)
    staged = destination / "giflib-source"
    source = snapshot(args.source, staged, args.commit, "giflib", {})
    header = (staged / args.header).resolve(strict=True)
    if not header.is_relative_to(staged.resolve()):
        raise ValueError("--header must stay inside the giflib source snapshot")
    source["archive_sha256"] = args.archive_sha256
    source["build_log_sha256"] = build_log_hash
    source["build_log"] = str(args.build_log.resolve())
    source["profile"] = args.profile
    source["provenance"] = (
        "Parent-supplied local giflib cross-build archive, pinned source/header "
        "snapshot and hash receipt. Importer did not run or attest the native build."
    )
    for relative, original in {
        "lib/libgif.a": archive, "include/gif_lib.h": header,
    }.items():
        target = destination / relative
        if target.exists() or target.is_symlink():
            raise ValueError(f"Refusing to replace {target}")
        target.symlink_to(original)
        manifest["files"][relative] = sha256(original)
    manifest["sources"]["giflib"] = source
    write_json(manifest_path, manifest)
    receipt["profiles"][args.profile]["manifest_sha256"] = sha256(manifest_path)
    write_json(DEPS / "receipt.json", receipt)
    print(f"{args.profile}: bound the parent's giflib archive and matching source header")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    builder = subcommands.add_parser("build", help="Parent native slot: build one fresh profile and prepare it")
    builder.add_argument("--profile", choices=PINS["profiles"], required=True)
    builder.add_argument("--tgfx-source", type=Path, required=True)
    builder.add_argument("--thorvg-source", type=Path, required=True)
    builder.add_argument("--ndk", type=Path)
    builder.add_argument("--ndk-host-tag")
    builder.add_argument("--jobs", type=int, default=4)
    builder.add_argument("--allow-patch", action="append", default=[], metavar="SOURCE_KEY=SHA256")
    builder.add_argument("--dependency-receipts", action="append", type=Path, default=[])
    builder.set_defaults(action=build)
    verifier = subcommands.add_parser("verify", help="Read/hash prepared inputs; no native tools")
    verifier.add_argument("--profile", choices=PINS["profiles"])
    verifier.set_defaults(action=verify)
    giflib = subcommands.add_parser(
        "add-giflib", help="Bind a parent's source-pinned, already cross-built giflib archive"
    )
    giflib.add_argument("--profile", choices=PINS["profiles"], required=True)
    giflib.add_argument("--source", type=Path, required=True)
    giflib.add_argument("--commit", required=True)
    giflib.add_argument("--archive", type=Path, required=True)
    giflib.add_argument("--archive-sha256", required=True)
    giflib.add_argument("--build-log", type=Path, required=True)
    giflib.add_argument("--header", default="gif_lib.h")
    giflib.set_defaults(action=add_giflib)
    importer = subcommands.add_parser(
        "import-existing", help="Hash/verify parent native outputs and prepare imports without rebuilding"
    )
    importer.add_argument("--profile", choices=PINS["profiles"], required=True)
    importer.add_argument("--tgfx-source", type=Path, required=True)
    importer.add_argument("--thorvg-source", type=Path, required=True)
    importer.add_argument("--tgfx-build", type=Path, required=True)
    importer.add_argument("--thorvg-build", type=Path, required=True)
    importer.add_argument("--tgfx-sha256", required=True)
    importer.add_argument("--thorvg-sha256", required=True)
    importer.add_argument("--thorvg-simd-sha256")
    importer.add_argument("--tgfx-build-log", type=Path, required=True)
    importer.add_argument("--thorvg-build-log", type=Path, required=True)
    importer.add_argument("--allow-coregraphics", action="store_true")
    importer.add_argument("--allow-higher-deployment", action="store_true")
    importer.add_argument("--dependency-receipts", action="append", type=Path, default=[])
    importer.add_argument("--allow-patch", action="append", default=[], metavar="SOURCE_KEY=SHA256")
    def import_action(arguments):
        from import_existing import import_existing
        import_existing(arguments)
    importer.set_defaults(action=import_action)
    args = parser.parse_args()
    try:
        args.action(args)
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError, KeyError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
