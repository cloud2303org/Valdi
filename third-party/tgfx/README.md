# Full SnapDrawing TGFX dependency seam

This is a source implementation of the optional **root** backend. It does not
replace the existing standalone vector experiment or change the default backend.
No native build, configure, test, cquery, compiler, linker, or device validation
was performed when this seam was implemented.

Select it with:

```sh
bazel build //snap_drawing //valdi \
  --define=snap_drawing_backend=tgfx \
  --platforms=@snap_platforms//os:macos_arm64
```

The root switch exports both `SNAP_DRAWING_TGFX_EXPERIMENT` and
`SNAP_DRAWING_TGFX_COMPLETE` using `cc_library.defines`. Those definitions reach
dependent Valdi C++/Objective-C++ translation units, including
`valdi_snap_drawing`; a per-target `-D` would not preserve public class layouts.
The standalone vector experiment still defines only `EXPERIMENT`.
`valdi_runtime` itself has no SnapDrawing dependency; no global compile flags or
ValdiCore ABI/library substitution are introduced.

The ordinary branch still selects all the original Skia dependencies through
`//snap_drawing:skia_backend`. The TGFX branch selects
`//third-party/tgfx:backend`, containing TGFX, ThorVG, giflib, WebP headers, and
zlib headers. None of these targets has a library dependency on `@skia`.
The existing runtime sources still compile in Bazel; no Valdi/SnapDrawing
archive is substituted for them. This graph description is based on source
inspection, not a configured-dependency query.

## Profile mappings

Every profile imports `.tgfx-deps/PROFILE/lib/tgfx.a`,
`.tgfx-deps/PROFILE/lib/libthorvg-1.a`, and, after `add-giflib`,
`.tgfx-deps/PROFILE/lib/libgif.a`. Selectors have no default archive.

| Profile | Bazel platform | TGFX vendor tuple | GPU |
| --- | --- | --- | --- |
| `macos_arm64` | `@snap_platforms//os:macos_arm64` | `mac/arm64` | Metal |
| `ios_sim_arm64` | `@snap_platforms//:ios_sim_arm64` | `ios/arm64-simulator` | Metal |
| `ios_device_arm64` | `@snap_platforms//:ios_arm64` | `ios/arm64` | Metal |
| `android_arm64` | `@snap_platforms//os:android_arm64` | `android/arm64` | OpenGLES/EGL |
| `android_x86_64` | `@snap_platforms//os:android_x86_64` | `android/x64` | OpenGLES/EGL |

The iOS selectors require `apple_support`'s `simulator` or `device` constraint.
The similarly named `@snap_platforms//os:ios_arm64` lacks that constraint and
intentionally does not match. Other architectures, Linux, macOS x86_64, and
arm64e intentionally fail selection. An absent prepared profile also fails;
it never borrows a different profile's archive.

Apple deployment floors in this recipe are macOS 12 and iOS 15, following the
inspected TGFX build. They must be compatible with the consuming application's
deployment settings. Android uses API 21, PIC, and the NDK's `c++_shared`; the
parent must use a compatible NDK/stdlib for Bazel and package that NDK's
`libc++_shared.so` in an eventual APK. No shared runtime is bundled by this
source-only seam.

## Parent-owned native build

`prepare.py build` runs real CMake and Meson commands. Run it only in the
parent's native slot, one profile invocation at a time. It requires installed
Python 3.9+, Git, Node, CMake, Ninja, Meson, Xcode for Apple profiles, and the
chosen NDK for Android. TGFX's vendor builder can create its own worker pool;
`--jobs` controls the outer CMake and Meson builds, not every upstream worker.

On Apple Silicon, the parent-tested Android x86_64 full-codec build also
requires the project-local real host-Yasm/CMake-wrapper setup documented below.
`prepare.py build` does **not** construct that setup or coordinate the wrapper
with its fresh staged libjpeg source directory. Use the manual native build
followed by `import-existing` for that tested path.

Generated Meson machine files use single-quoted strings, including every
element of argument arrays. Paths/arguments containing single quotes,
backslashes, or control characters are rejected instead of emitting unsupported
JSON double-quoted literals.

The source pins are:

* TGFX `4cf8991d5a3a010c9314538dd2fea937593b206b`
* ThorVG `f0fd698d56b3b8ae797a242cf06aab6b70aed840`

Supply source paths as arguments; no developer machine path is committed in
`MODULE.bazel`. The full experiment uses patched TGFX and ThorVG working trees.
Set `TGFX_PATCH_SHA256` and `THORVG_PATCH_SHA256` to the reviewed current
working-tree diff hashes described below. For example, from the worktree root:

```sh
TGFX_SOURCE=/path/to/tgfx
THORVG_SOURCE=/path/to/thorvg

python3 third-party/tgfx/prepare.py build \
  --profile macos_arm64 \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256"

python3 third-party/tgfx/prepare.py build \
  --profile ios_sim_arm64 \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256"

python3 third-party/tgfx/prepare.py build \
  --profile ios_device_arm64 \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256"

ANDROID_NDK=/path/to/the/compatible/ndk

python3 third-party/tgfx/prepare.py build \
  --profile android_arm64 \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256" \
  --ndk "$ANDROID_NDK"

# Android x86_64 on Apple Silicon: use the manual host-Yasm/wrapper
# prerequisite below for the full native build, then import-existing.
# The fresh-build helper does not create this host-tool recipe.
```

Every build stages verified source files into a **fresh** directory beneath
`.tgfx-deps/build/`. No source checkout is modified and no existing source-tree
vendor output or Valdi build archive is imported. Enabled TGFX dependency Git
commit IDs are read from its pinned `DEPS` and checked before copying: vendor_tools,
pathkit, skcms, zlib, libpng, libwebp, libjpeg-turbo, concurrentqueue,
highway, plus FreeType on Android and shaderc, glslang, SPIRV-Headers,
SPIRV-Tools, SPIRV-Cross for Metal.
TGFX and ThorVG roots use their actual Git working trees. Dependency sources
can be actual Git checkouts or extracted codeload tarballs with explicit
source-archive receipts, as described below. The parent supplies those real
sources before building. Disabled
ANGLE/SwiftShader binary packages are excluded.

The prepared staging directory uses a `depsync` guard only after those source
checks. It suppresses TGFX's configure-time source synchronization; it does not
replace any compiler, build target, decoder, renderer, or archive. Shaderc's
enabled dependency trees must already be present, so the automatic missing-
glslang sync path is not used. Missing sources cause a pre-build failure.

Tracked source changes are rejected unless their exact patch hash is approved.
The full experiment includes TGFX build/API changes, including the variable-font
API, and a ThorVG source patch isolating its bundled premultiplied WebP fork's
symbols from TGFX's full libwebp. The latter is preserved at
`snap_drawing/experimental/tgfx/full/patches/thorvg-webp-symbol-prefix.patch`
and modifies ThorVG's tracked `src/loaders/webp/webp/types.h`. Both real decoder
implementations retain their own semantics; this is source-level symbol
isolation, without linker duplicate suppression or ABI substitution.

Supply both reviewed patch hashes for the corresponding rebuilt native
archives and matching headers:

```sh
python3 third-party/tgfx/prepare.py build \
  --profile macos_arm64 \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256"
```

Each value is the SHA256 of the **entire current tracked working-tree diff**
against that source root's pinned `HEAD`, emitted by
`git diff --no-ext-diff --no-textconv --binary HEAD -- .`. It is not the
standalone patch-file hash. Updating either source patch requires its new
reviewed diff hash. The helper reports the required hash on mismatch.

Dependency patch keys use paths such as `tgfx/third_party/vendor_tools`.
An approved diff is copied alongside the staged source and its hash is recorded
as `approved_patch_sha256` on the corresponding source pin. Neither patched
TGFX nor patched ThorVG is represented as a pristine upstream build.

TGFX's official `CMakeLists.txt` creates `tgfx` with `PREFIX ""` and uses
`merge_libraries_into(tgfx ...)`, so the required output is **`tgfx.a`**, not
`libtgfx.a`. Its merged static vendors supply the full PNG/JPEG/WebP
decode/encode implementations, path operations, and Metal shader
translation where enabled. TGFX SVG/PDF/layers are disabled: ThorVG supplies
the actual CPU JSON Lottie and SVG renderer. ThorVG's `src/meson.build` names
the static target `thorvg-1`, yielding **`src/libthorvg-1.a`**. The recipe enables
CPU, SVG/Lottie, TTF/OTF, and embedded PNG/JPEG/WebP asset loaders, disables
OpenMP/extra engines/expressions, and uses scalar CPU code.

The fresh Apple recipe sets `TGFX_USE_FREETYPE=OFF`, selecting the actual
CoreGraphics vector implementation and `TGFX_USE_CORE_GRAPHICS` definition.
This follows the pinned TGFX `CMakeLists.txt` branch and SnapDrawing's Apple
font bridge: `NativeFontManager.cpp` calls the real `tgfx::CTTypeface`
`MakeFromCTFont`/`GetCTFont` APIs. FreeType is not staged or linked as a TGFX
vendor for Apple profiles. The fresh Android recipe sets
`TGFX_USE_FREETYPE=ON` and stages the matching pinned FreeType source. Fresh
build manifests record `vector_backend: apple_coregraphics` or `freetype`
accordingly.

## Android x86_64 on Apple Silicon: manual host-assembler prerequisite

This is the parent's tested **local native build procedure**, separate from
the automated fresh-build recipe. The NDK used in that run supplies an
Intel-host Yasm executable which cannot execute natively on the ARM64 Mac.
The parent built actual Yasm 1.3.0 from its official release tarball and used
it to assemble the actual libjpeg-turbo x86_64 SIMD sources. The preserved
successful build log reports `WITH_SIMD = 1`. No Rosetta installation, system
SDK/NDK edits, SIMD disabling, fabricated assembler, or prebuilt Valdi archive
was used in this procedure.

A manual configure override in the libjpeg leaf directory is insufficient:
TGFX's pinned `third_party/vendor_tools/lib/CMake.js` deletes `buildPath` when
`options.incremental` is false, before building and again after copying
outputs. The outer vendor build therefore removes the manually adjusted leaf
cache. The working procedure puts a transparent project-local `cmake` wrapper
on `PATH` for the **entire outer build**. Every actual libjpeg-turbo configure
with `-DANDROID_ABI=x86_64` receives:

```text
-DCMAKE_ASM_NASM_COMPILE_OBJECT=REAL_ARM64_YASM -DELF <DEFINES> <INCLUDES> <FLAGS> -f elf64 -o <OBJECT> <SOURCE>
```

That rule invokes the real native-host assembler to emit ELF64 x86_64 objects.
The wrapper delegates all other invocations and original arguments to the
chosen absolute real CMake executable, including `cmake --build`. It does not
rewrite vendor source or install anything into the SDK.

### Preserved evidence and observed hashes

The inspected evidence directory is
`/tmp/valdi-tgfx-full-20261104/host-yasm/`:

* `receipt.json` records official Yasm 1.3.0 release source, its archive hash,
  and the real configure/`make -j4 yasm` commands.
* `cmake-wrapper/cmake` and `cmake-wrapper-receipt.json` record the actual
  wrapper bytes, absolute real CMake path `/opt/homebrew/bin/cmake`, and
  expected local Yasm hash.
* `cmake-wrapper-calls.jsonl` records the qualifying configure with the
  actual libjpeg source, Android x86_64/NDK arguments, and appended real
  assembler compile rule.
* `jpeg-real-assembler-receipt.json` records successful leaf configure/build.

The source URL recorded by the parent is
`https://www.tortall.net/projects/yasm/releases/yasm-1.3.0.tar.gz`.
The exact official release **archive** SHA256 in that receipt is:

```text
3dce6601b495f5b3d45b59f7d2492a340ee7e84b5beca17e48f862502bd5603f
```

The following are observations of that parent's **local tool outputs**:

```text
ARM64 Yasm executable:
bbfa66afc6cc6f48c0e68010c7680cd4118292a7398c2e495d875ee47c813303

Parent's path-specific CMake wrapper:
5b0d56eac16e30cea92736f20ec4c4f86fb83bdb211a3041219281dc588fabc7
```

These executable/wrapper hashes are not guaranteed results of rebuilding the
same source archive on another host. The wrapper contains local paths; a
parameterized adaptation has different bytes and must receive its own hash.
The existing wrapper receipt identifies real CMake by path, not a real-CMake
binary hash; record the actual real-CMake hash as additional tool evidence
for a new run.

The successful outer build evidence is
`/tmp/valdi-tgfx-full-20261104/tgfx-full-android-x86_64-round2-fixed-build2.log`
and sibling receipt `android-x86_64-round2-build2.json`. The receipt records
exit 0, unchanged source-diff SHA256
`6acf662757eccfa321dd83a841d50ae3e146173b3f2f0d1ea1f5cb6f8e1d4fdc`,
and resulting TGFX archive SHA256:

```text
b375a611c1166b7cf6ca335133f41b0cb40385bedc322abd7c46afc1a19a6e34
```

This records the native TGFX dependency build's result. It does not by itself
establish Android app/device/runtime acceptance or guarantee a future output
hash.

### Parameterized manual procedure

The following is a parameterized adaptation of the inspected procedure,
provided for the parent native slot; it was not executed during this
documentation update. Supply a local official source tarball, verify its
release hash, and keep the host-tool directory outside the SDK/NDK:

```sh
set -eu
YASM_TARBALL=/path/to/local/yasm-1.3.0.tar.gz
HOST_TOOL_ROOT=/path/to/project-local/host-yasm
TGFX_SOURCE=/absolute/path/to/the/pinned/tgfx

printf '%s  %s\n' \
  3dce6601b495f5b3d45b59f7d2492a340ee7e84b5beca17e48f862502bd5603f \
  "$YASM_TARBALL" | shasum -a 256 -c -

mkdir -p "$HOST_TOOL_ROOT/source" "$HOST_TOOL_ROOT/build"
tar -xf "$YASM_TARBALL" --strip-components=1 -C "$HOST_TOOL_ROOT/source"
(
  cd "$HOST_TOOL_ROOT/build"
  "$HOST_TOOL_ROOT/source/configure" \
    --build=aarch64-apple-darwin \
    --disable-nls \
    --prefix="$HOST_TOOL_ROOT/install"
  make -j4 yasm
)

export TGFX_REAL_CMAKE=/absolute/path/to/real/cmake
export TGFX_YASM="$HOST_TOOL_ROOT/build/yasm"
export TGFX_YASM_SHA256
TGFX_YASM_SHA256="$(shasum -a 256 "$TGFX_YASM" | awk '{print $1}')"
export TGFX_JPEG_SOURCE="$TGFX_SOURCE/third_party/libjpeg-turbo"
export TGFX_CMAKE_WRAPPER_LOG="$HOST_TOOL_ROOT/cmake-wrapper-calls.jsonl"
```

Choose the real CMake path **before** prepending the wrapper directory to
`PATH`. Save the following adaptation as
`$HOST_TOOL_ROOT/cmake-wrapper/cmake`, create that directory, and make the
file executable. For the assembler rule use absolute POSIX tool paths without
whitespace or quotes, matching the tested command's path semantics:

```python
#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import sys

real = os.environ["TGFX_REAL_CMAKE"]
yasm = os.environ["TGFX_YASM"]
expected = os.environ["TGFX_YASM_SHA256"]
source = Path(os.environ["TGFX_JPEG_SOURCE"]).resolve()
args = sys.argv[1:]

if not Path(real).is_absolute() or Path(real).resolve() == Path(__file__).resolve():
    raise SystemExit("Choose the absolute real CMake executable, not this wrapper")

is_jpeg = any(
    Path(arg).resolve() == source
    for arg in args if not arg.startswith("-")
)
if is_jpeg and "-DANDROID_ABI=x86_64" in args and "--build" not in args:
    if not Path(yasm).is_absolute() or any(c.isspace() or c in "'\"" for c in yasm):
        raise SystemExit("Use an absolute Yasm path without whitespace or quotes")
    if hashlib.sha256(Path(yasm).read_bytes()).hexdigest() != expected:
        raise SystemExit("Pinned local Yasm hash changed")
    rule = yasm + " -DELF <DEFINES> <INCLUDES> <FLAGS> -f elf64 -o <OBJECT> <SOURCE>"
    args.append("-DCMAKE_ASM_NASM_COMPILE_OBJECT=" + rule)
    with open(os.environ["TGFX_CMAKE_WRAPPER_LOG"], "a") as log:
        log.write(json.dumps({
            "actual_cmake": real, "argv": args, "yasm_sha256": expected
        }) + "\n")
os.execv(real, [real, *args])
```

With that wrapper in place, run the parent's existing full TGFX native
configure/build under the scoped `PATH`, preserving all actual Android ABI,
NDK, codec, GPU and source-patch options:

```sh
TGFX_ANDROID_X86_64_BUILD=/path/to/the/configured/full-android-x86_64-build

shasum -a 256 \
  "$TGFX_REAL_CMAKE" "$TGFX_YASM" "$HOST_TOOL_ROOT/cmake-wrapper/cmake"

PATH="$HOST_TOOL_ROOT/cmake-wrapper:$PATH" \
  "$TGFX_REAL_CMAKE" --build "$TGFX_ANDROID_X86_64_BUILD" --parallel 4
```

`TGFX_ANDROID_X86_64_BUILD` must already be the parent's correctly configured
full Android x86_64 TGFX build directory. The scoped `PATH` must also cover any
outer configure that can run vendor configuration. Use
`TGFX_JPEG_SOURCE` for the actual source directory passed to that vendor
configure; an original-checkout path will not match a freshly staged helper
copy. A manually successful leaf build alone does not satisfy the outer-build
prerequisite.

The automated `prepare.py build` currently neither builds Yasm nor generates
this wrapper, records its tool receipts, or adjusts its exact-source-path
match for a fresh staging directory. Installing the wrapper for the manual
native recipe is a parent-owned prerequisite, not an advertised implemented
fresh-builder feature. No source or helper change is required to use the
completed matching native outputs through `import-existing`.

### Import and tool-provenance boundary

After a successful matching native build, use `import-existing` with
`--profile android_x86_64`, the actual original build directories, current
source-diff approvals and dependency source-archive receipts, expected
TGFX/ThorVG/SIMD archive hashes, and the actual full build log through
`--tgfx-build-log`. Android configuration still needs the declared ABI,
FreeType/full codecs/OpenGLES/EGL and compatible NDK/stdlib settings required
by the importer. Import does not re-run or replace the tool recipe.

The importer checks current source/configuration/log hashes, archive hashes
and ELF CPU evidence for the matching outputs. It does not automatically
discover or ingest the separate Yasm archive/tool receipt, wrapper hash,
wrapper calls JSONL, or real-CMake hash, nor attest them merely from their
presence on disk. Preserve and hash those sidecars with the parent's full
build evidence as supplemental tool provenance. Their inspection and the
parent's successful build support this local recipe; the automated import
manifest remains import-time evidence, without a hermetic or retrospective
toolchain claim.

## GIF closure

The actual media implementation includes `<gif_lib.h>`, `<webp/demux.h>`, and
`<zlib.h>`. TGFX's inspected `vendor.json` builds and merges `webp`,
`webpdemux`, `sharpyuv`, and `libwebpmux`; the prepared headers come from its
matching pinned source. The zlib headers include the platform-generated
`zconf.h` from that same build.

The parent supplies a real upstream giflib Git commit and builds its static
library for each target profile. The supplied official SourceForge source
commit is `a8e3114a81f0987a61d06a41c99fd7cc2d58232c`; it is not silently
substituted for any future caller's explicit `--commit`.
No host Homebrew giflib archive or header is used as a cross-platform fallback.
After that real native build, bind its output with:

```sh
python3 third-party/tgfx/prepare.py add-giflib \
  --profile macos_arm64 \
  --source /path/to/the/upstream/giflib/git/checkout \
  --commit FULL_UPSTREAM_GIT_COMMIT \
  --archive /path/to/the/macos-arm64/libgif.a \
  --archive-sha256 EXPECTED_ARCHIVE_SHA256 \
  --build-log /path/to/the/actual/giflib-build.log
```

Repeat with the corresponding archive for each profile. The importer verifies
the supplied source commit and archive hash, snapshots the unmodified tracked
source, and exports that source's actual `gif_lib.h`. `--header` accepts a
different path within the source snapshot if the chosen upstream uses one.
The manifest explicitly identifies this as a parent-supplied local archive;
the importer neither runs nor independently attests that build. Its archive
hash, header hash, source commit/snapshot hash, and build-log hash are recorded.
Until this step, standalone TGFX/ThorVG imports can be used, but the full
backend fails on the missing giflib input.

## Local imports and receipts

The prepared layout is:

```text
.tgfx-deps/
  receipt.json
  build/PROFILE-UNIQUE/
    source/{tgfx,thorvg}/...
    tgfx-build/tgfx.a
    thorvg-build/src/libthorvg-1.a
    commands.json
    build.log
    thorvg.cross.ini
  PROFILE/
    manifest.json
    lib/{tgfx.a,libthorvg-1.a,libgif.a}
    include/tgfx/...            # real TGFX public headers
    include/thorvg/{thorvg.h,thorvg_lottie.h}
    include/webp/...            # real matching WebP public headers
    include/{zlib.h,zconf.h,gif_lib.h}
```

For generated zlib headers, the verified upstream layout is
`third_party/out/zlib/PLATFORM/include/{zlib.h,zconf.h}`: for example,
`third_party/out/zlib/mac/include/zconf.h`. There is **no architecture component**
between `mac` and `include`. Existing-output import copies those generated
headers into its own snapshot so a later native build cannot silently change
an imported profile's `zconf.h`.

The profile files are symlinks to the built outputs and matching staged source.
The local Bazel repository checks manifest and archive SHA256 values while
copying those inputs with `file://` URLs, and exposes the matching header tree.
It does not run a build or fetch upstream sources. The repository rule is
lazy; default Skia does not require `.tgfx-deps` preparation.

Receipts record source commits, tracked-source snapshot hashes, approved patch
hashes, build commands, cache/options hashes, local SDK/NDK identity, build-log
hashes, and prepared archive/header hashes. They are local provenance receipts,
not a claim of hermetic or reproducible toolchain/transitive inputs. Preserve
the build directories while their symlinks are in use. Existing prepared
profiles are never silently replaced.

The parent can check prepared input hashes without compiling:

```sh
python3 third-party/tgfx/prepare.py verify
```

The link settings follow TGFX's inspected `CMakeLists.txt`. Apple profiles link
Metal and Foundation/QuartzCore/CoreGraphics/CoreText/CoreVideo/CoreMedia/
ImageIO, with Cocoa/ApplicationServices on macOS or UIKit on iOS, plus iconv
and libc++. Android links android, jnigraphics, log, EGL, GLESv2/GLESv3, dl,
math, and `c++_shared`. ThorVG's Apple CPU scheduler also links pthread.
These are committed in `prepared.BUILD.bazel`; no host OpenGL framework is used
for the Metal profiles.

## Extracted source-archive dependencies

Both `build` and `import-existing` accept repeated `--dependency-receipts FILE`.
They support the parent's existing list format:

```json
[
  {
    "name": "shaderc",
    "commit": "3362e24c42ab5bf7ad32c0fec64b0a0ddeb2fda1",
    "archiveSHA256": "2ce7ff9388f47261f12f70f653d94c85e7acc0a17b5c0fd078aec83671fd61f1",
    "archive": "/path/to/shaderc.tar.gz"
  }
]
```

`archive` can be relative to the receipt. If omitted, it is exactly
`NAME.tar.gz` beside the receipt, matching the parent's full-dependency
receipt layout. `archive_sha256` is also accepted as a spelling of
`archiveSHA256`. A file may instead wrap the list as `{"dependencies": [...]}`.
Receipts for initial/prototype dependencies can be supplied in a second file;
each extracted dependency needs its own explicit archive/hash/commit entry.
Missing entries do not fall back to trusting an extracted directory or
inventing a Git checkout.

For each enabled dependency the helper:

1. Matches the receipt commit to the TGFX `DEPS` entry and checks the complete
   source tarball SHA256.
2. Requires one codeload root directory ending in the full expected commit.
3. Compares every regular tar member's content with the actual extracted
   source file, and checks safe relative symlinks against their tar entries.
4. Stages only the verified archive members; existing build outputs and
   unreceipted extra files are excluded. Nested shader dependencies are
   independently verified and staged from their own receipts.
5. Records the archive path/hash, receipt path/hash, declared source commit,
   archive root, verified file-set hash, and `input_kind: source_archive`.

No Git directory or commit is created. A mismatch with an extracted source
file fails; `--allow-patch` applies to actual Git working trees only.

## Import existing parent native outputs

`import-existing` performs file reads, hashing, metadata parsing, source
snapshotting, and local import publication. It invokes no CMake, Meson,
compiler, archiver, linker, or download. It can consume a fresh official
CMake/Meson build already completed in the parent's native slot:

```sh
python3 third-party/tgfx/prepare.py import-existing \
  --profile ios_sim_arm64 \
  --allow-coregraphics \
  --tgfx-source "$TGFX_SOURCE" \
  --thorvg-source "$THORVG_SOURCE" \
  --allow-patch "tgfx=$TGFX_PATCH_SHA256" \
  --allow-patch "thorvg=$THORVG_PATCH_SHA256" \
  --tgfx-build /path/to/finished/tgfx-ios-simulator-build \
  --thorvg-build /path/to/finished/thorvg-ios-simulator-build \
  --tgfx-sha256 "$EXPECTED_TGFX_ARCHIVE_SHA256" \
  --thorvg-sha256 "$EXPECTED_THORVG_ARCHIVE_SHA256" \
  --thorvg-simd-sha256 "$EXPECTED_SIMD_ARCHIVE_SHA256" \
  --tgfx-build-log /path/to/actual/tgfx-build.log \
  --thorvg-build-log /path/to/actual/thorvg-build.log \
  --dependency-receipts /path/to/full/dependencies/receipts.json \
  --dependency-receipts /path/to/prototype-dependency-receipts.json
```

The repeated `--allow-patch` entries cover both actual patched Git roots, as
with `build`; the importer stages and records each approved diff, including
ThorVG's WebP symbol isolation. Apple imports of this full experiment must add
`--allow-coregraphics`; the importer requires the actual CoreGraphics
compile definition and records that vector backend, matching the fresh Apple
recipe. Android import requires the configured ABI and
`c++_shared` explicitly.

The importer checks the CMake source root, full codec and GPU options; Meson
source root, static/PIC/CPU/full-loader/extra options and target machine; real
object CPU/platform headers; and the exact expected native archive hashes.
Apple Mach-O `LC_BUILD_VERSION` distinguishes simulator from device even when
both are arm64. Android requires ELF CPU evidence plus Android build metadata.
Uninspectable bitcode/thin archives are rejected. Actual maximum Apple object
deployment requirements are recorded. An artifact above the profile floor
requires an explicit `--allow-higher-deployment` and a compatible consuming
application deployment floor.

When effective ThorVG SIMD is enabled, both the real
`src/renderer/cpu_engine/libthorvg-sw-simd.a` and the main `src/libthorvg-1.a`
are inspected. Every SIMD object's content hash must already occur in the
main archive, proving the inspected Meson `link_whole` closure. The side
archive/hash is preserved as evidence; the main archive provides the Bazel
link input. An incomplete closure fails rather than silently omitting SIMD.
For scalar builds, `--thorvg-simd-sha256` is unnecessary; the explicit
`simd=false` build recipe remains supported.

Import receipts bind the current source snapshots, original build metadata/
logs and generated-header hashes, archive/object target evidence, effective
options, SIMD closure, and exact artifact hashes. These are import-time
verification records, not retrospective proof that source/configuration
remained unchanged throughout an earlier build.

Import staging lives under `.tgfx-deps/imports/PROFILE-UNIQUE/`; native outputs
remain at the supplied parent-owned paths. An existing prepared profile is
rejected **before staging or publishing**. In particular, the parent's already
prepared `macos_arm64` profile must not be reimported or replaced.

See [IOS_IMPORT.md](IOS_IMPORT.md) for the concrete parent iOS simulator paths,
observed source/artifact hashes, exact CLI invocation, and remaining receipt
requirements. That handoff does not execute or publish an import.

## Internal native GPU fixture support

`//third-party/tgfx:test_support` aliases the prepared repository's
`cc_library(testonly = True)`. Only fixture targets declared `testonly = True`
may depend on it. The production `tgfx` target is unchanged, and
`//valdi`/the production backend do not depend on this internal test target.

Normal `prepare.py build` and `prepare.py import-existing` calls now prepare:

```text
PROFILE/test_include/tgfx_src/...             # .h/.hpp/.inl/.inc from verified TGFX src
PROFILE/test_include/concurrentqueue/...      # verified production queue headers
PROFILE/test_include/skcms/skcms.h            # verified pinned skcms public entry
PROFILE/test_include/skcms/src/skcms_public.h # its included public API
PROFILE/test_include/tgfx_test_native_profile.h
```

These are copies from the same verified staged TGFX, pinned concurrentqueue, and skcms
snapshots used by that profile's import. They do not reference live vendor
trees. All copied headers are hashed in `manifest.files` and
`manifest.test_support.headers_sha256`. The repository verifies their hashes
before exposing declared Bazel `hdrs`. The include roots support actual
`gpu/DrawingManager.h`, `gpu/ProxyProvider.h`, `gpu/VertexProvider.h`,
`gpu/tasks/GPUBufferUploadTask.h`, the concurrentqueue header closure, and
`<skcms.h>` with its relative `src/skcms_public.h` include. The skcms snapshot
digest is bound in the test-support receipt alongside TGFX and concurrentqueue.
An older profile without this metadata cannot supply the required test guard
header; production imports remain usable without test metadata.

The new `native_test_support.py` APIs are
`collect_native_test_configuration(...)` and `stage_test_support(...)`.
The first reads actual `Task.cpp.o`, `DrawingManager.cpp.o`,
`ProxyProvider.cpp.o`, `VertexProvider.cpp.o`, and `GPUBufferUploadTask.cpp.o`
records in the native Ninja graph. Their definitions must agree, match the
CMake feature configuration, and their actual object hashes must occur in
the supplied `tgfx.a`. ABI header/source hashes are checked again against the
verified snapshot before copying. The receipt binds the native archive,
profile, source snapshots, metadata, owner objects, header hashes, and
definitions. No classes or ABI declarations are fabricated.

The repository generates `test_support_config.bzl` from that receipt, exporting
the observed definitions through the test-only library's `defines`. It does
not guess platform macros. All five inspected profiles include
`TGFX_USE_THREADS`; Apple additionally has native CoreGraphics/Metal and
Android has native FreeType/glyph-to-Unicode. Android's observed definition
list contains no `TGFX_USE_OPENGL` despite OpenGL being enabled in CMake.
The full codec definitions and observed `-D` values in native `FLAGS` are
preserved as well. This keeps internal `TaskGroup` layout consistent for
mobile consumers. The generated guard can optionally be included as
`"tgfx_test_native_profile.h"` to diagnose missing/conflicting definitions.

There is no new CLI option. The parent runs its existing `import-existing`
command for each completed frozen profile, then binds giflib as before:

```sh
# Existing complete source/config/archive/receipt arguments are unchanged.
python3 third-party/tgfx/prepare.py import-existing --help
python3 third-party/tgfx/prepare.py verify --profile macos_arm64

# Parent fixture BUILD files own testonly=True and this dependency.
bazel build //snap_drawing/experimental/tgfx/full:backend_checks \
  --define=snap_drawing_backend=tgfx \
  --platforms=@snap_platforms//os:macos_arm64
```

No helper, native build, or fixture consumer build was executed while adding
this support. Existing prepared profiles, historical imports, and evidence
are never updated in place; the parent owns fresh imports and validation.

## Parent validation commands (not executed during implementation)

First confirm default Skia still works with no prepared TGFX repository. Then,
after preparing the relevant profile and binding giflib, inspect the configured
graph and build the actual runtime:

```sh
bazel cquery \
  'filter("skia", deps(//snap_drawing) union deps(//valdi))' \
  --define=snap_drawing_backend=tgfx \
  --platforms=@snap_platforms//os:macos_arm64

bazel build //snap_drawing //valdi \
  --define=snap_drawing_backend=tgfx \
  --platforms=@snap_platforms//os:macos_arm64
```

The configured query should return no Skia library targets. A separate
`bazel aquery` of the dependent Valdi C++/Objective-C++ actions should confirm
both public definitions. Repeat with the four other explicit platforms above,
using the consuming build's normal platform/toolchain flags and deployment
floors. Analysis/build/runtime acceptance, JavaVM initialization and Java
support required by TGFX's Android platform code, optional platform font
Objective-C++ sources, and the actual C++ rendering ports remain the parent's
and the corresponding source owners' validation responsibilities.

## Android RTTI for real codec subclasses

The complete backend implements real `tgfx::ImageCodec` subclasses. An Android
archive built with TGFX's original forced `-fno-rtti` lacks the base type
information required by those C++ consumers. The strict shared-library link
reported that missing symbol before the following build correction.

The pinned source patch adds `TGFX_ANDROID_ENABLE_RTTI`, default `OFF`, retaining
the original vendor flags unless explicitly enabled. This integration's
`prepare.py build` sets it to `ON` for Android only, and `import-existing`
requires the matching cache value. The manual existing-build path must also
configure `-DTGFX_ANDROID_ENABLE_RTTI=ON` and rebuild the complete archive.
Exceptions remain disabled in TGFX; consumers retain their own existing RTTI
and exception policies. No type-information stubs are supplied.

The matching parent ARM64 build and import records are
`/tmp/valdi-tgfx-full-20261104/tgfx-android-arm64-rtti-build.json` and
`android-rtti-import-commands.json`. Its archive hash is
`337cf59c294fffce861f39e9e93c94cc9d04255f585bb1aaf30197cc0e60bf05`.
The subsequently linked native checks executed 260/260 cases on the local
Android API 36 emulator; see `snap_drawing/experimental/tgfx/full/mobile/README.md`
for artifact/report identities and the separate Valdi/Hermes application gate.
