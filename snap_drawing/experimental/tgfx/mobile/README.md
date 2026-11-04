# Mobile execution of the same vector checks

The mobile targets link the actual nine adapted SnapDrawing translation units,
the shared `vector_checks.cpp` fixture and a separately cross-built real TGFX
static library. No application runtime, Hermes, image renderer or Lottie
replacement is supplied by these targets.

`vector_checks.cpp` contains the same geometry/GPU assertion function bodies as
the committed 16-case macOS fixture at `8a42d138`. Only the report collector and
entry points were factored out. Both mobile shells call `runVectorChecks()`.
They display success only after the real checks report all 16 cases passed.
A missing OpenGL context, failed readback, pixel mismatch or exception produces
a failure report.

## Observed results

- iOS 26.5, booted iPhone 17 arm64 simulator: 16/16 native checks passed.
  The final five-step restart/report flow also passed without warnings.
- Android API 36, local arm64 emulator `ValdiRoutes_NoHwKeyboard`: 16/16 native
  checks passed. The saved five-step restart/report flow passed without warnings.
- The extracted shared fixture also passed 16/16 on macOS in optimized and
  ASan/UBSan builds. Mobile binaries and the Release TGFX dependencies were
  not sanitizer-instrumented.

Both mobile reports were read from the running app accessibility tree, including
all sixteen PASS lines and the final RESULT line. Screenshot and build/artifact
hashes are recorded in `../evidence/mobile-results.json`.

## Build inputs

Use the same pinned TGFX checkout and source dependencies described in the
parent README, including its recorded OpenGL-only CMake patch.

iOS uses the official `third_party/vendor_tools/ios.toolchain.cmake` with
`PLATFORM=SIMULATORARM64`, deployment target 16.0, and OpenGL ES. This is an
iOS simulator build, separate from the earlier macOS archive.

Android uses NDK `27.1.12297006`, arm64, API 24 and `c++_shared`. Its path
rasterizer needs `TGFX_USE_FREETYPE=ON`, even though these checks do not render
text. Obtain these additional exact source revisions from the pinned `DEPS`:

- FreeType: `0a0221a1347e2f1e07c395263540026e9a0aa7c7`
- libpng: `3061454d980de7d53608f594194cfac722721d2a`
- zlib: `da607da739fa6047df13e66a2af6b8bec7c2a498`

TGFX's vendor tooling builds/merges those actual dependencies. Disabling
FreeType produced a static archive but left a real `PathRasterizer::MakeFrom`
undefined when linking the Android consumer. The original failed link is
preserved. Android also links both GLESv2 and GLESv3, matching TGFX's native
platform dependencies.

## Native configuration

Use absolute paths for `TGFX`, `BUILD`, `FMT`, `NDK` and `LIBRARY`. `FMT` is the
real fmt include directory; `LIBRARY` is the matching mobile TGFX archive.

From this directory, choose one platform argument array below and use the
shared feature array. For Android, FreeType enables its required PNG support.

```sh
features=(
  -DTGFX_BUILD_TESTS=OFF -DTGFX_BUILD_HELLO2D=OFF
  -DTGFX_BUILD_SVG=OFF -DTGFX_BUILD_PDF=OFF -DTGFX_BUILD_LAYERS=OFF
  -DTGFX_USE_METAL=OFF -DTGFX_USE_OPENGL=ON
  -DTGFX_USE_PNG_DECODE=OFF -DTGFX_USE_PNG_ENCODE=OFF
  -DTGFX_USE_JPEG_DECODE=OFF -DTGFX_USE_JPEG_ENCODE=OFF
  -DTGFX_USE_WEBP_DECODE=OFF -DTGFX_USE_WEBP_ENCODE=OFF
)

# Choose iOS:
platform=(
  "-DCMAKE_TOOLCHAIN_FILE=$TGFX/third_party/vendor_tools/ios.toolchain.cmake"
  -DPLATFORM=SIMULATORARM64 -DDEPLOYMENT_TARGET=16.0
)
freetype=-DTGFX_USE_FREETYPE=OFF

# Or choose Android instead of the preceding iOS assignment:
platform=(
  "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
  -DANDROID_STL=c++_shared
)
freetype=-DTGFX_USE_FREETYPE=ON

cmake -S "$TGFX" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "${features[@]}" "${platform[@]}" "$freetype"
cmake --build "$BUILD" --target tgfx --parallel 4
```

Configure this directory with the chosen platform array and a separate
`PROBE_BUILD` output directory:

```sh
cmake -S . -B "$PROBE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTGFX_SOURCE="$TGFX" -DTGFX_LIBRARY="$LIBRARY" -DFMT_INCLUDE="$FMT" \
  "${platform[@]}"
cmake --build "$PROBE_BUILD" --parallel 4
```

The iOS output is `TGFXVectorProbe.app`; ad-hoc sign that simulator bundle
before installation. Start the iOS checks after `didBecomeActive`, allowing
TGFX's actual foreground-notification gate to open first.

For Android, stage `libsnap_tgfx_probe.so` and the matching
`libc++_shared.so`, then use the offline Gradle packaging recipe in
`android/README.md`. Its native link uses `--no-undefined`. Packaging does not
configure or build another renderer.

## Runtime checks and scope

The unique test application identifier is `io.valdiroutes.tgfxprobe` on both
platforms. These are standalone test shells, not replacements for Valdi's
application hosts.

The saved Argent flows in `../.argent/e2e/` restart the installed probe and
assert the actual native report. The UI report is structural/runtime evidence;
the assertions inside the shared fixture validate the GPU API readback pixels.
This is not a screenshot-based Skia/TGFX image comparison or evidence that the
simulator uses physical-device graphics hardware.

iOS's UITextView exposes its individual lines to AX rather than its container
identifier. The final flow therefore locates the actual `RESULT 16 ...` line
by its text. An initial failed container-selector wait remains in the raw
recording under `/tmp`; it is not a native-rendering failure.

The initial iOS replay's 15-second `idle` step could not confirm stillness
because runner-tree reads were slow. The final flow raises that gate to 30
seconds; it completed in 20.6 seconds without warnings, followed by passing
status/report checks. The initial warning and raw failed-selector recording
remain preserved in the evidence.

Images, fonts, full SnapDrawing layer composition, full Valdi/JS integration,
Lottie and physical-device execution remain outside this verification.
No application-size comparison has been made.
