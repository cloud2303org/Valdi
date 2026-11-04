# Native GPU render performance fixture

Source preparation only. No builds, compiler checks, configuration, native
execution, device interaction, energy measurement or size measurement were
performed while authoring this directory. The existing full backend suites and
their 260/260 result are independent and unchanged.

The shared entry point is:

```cpp
#include "snap_drawing/experimental/tgfx/full/performance/native/render_performance.hpp"
auto json = snap::drawing::performance::runRenderPerformance(mediaDirectory);
```

`mediaDirectory` must be the unchanged `full/tests/media-data` directory or its
packaged copy. Run once on a dedicated worker thread, with no concurrent runs,
compilers or other benchmark processes. The C++ core defines no `main`, JNI
entry point, JS module or app lifecycle hooks. Android's JNI
bridge and the iOS UIKit entry point are separate consumers.

## What is measured

Five required scenes use the same 360x640, RGBA8888 premultiplied sRGB GPU target,
sample count 1, 30 warmup frames and 300 measured frames. Both backends draw
through production `DrawingContext`, `LayerContent`, `DisplayList` and
`DrawableSurfaceCanvas`. Both complete every frame with owned synchronous RGBA
GPU readback. No raster target is substituted if GPU initialization fails.

- Vector: 72 rounded paths and 72 antialiased cubic strokes.
- Text: 20 lines, the pinned Noto Sans font, production font manager and
  `TextLayoutBuilder`, with real shaped text blobs. Font loading, shaping and
  static recording precede timing.
- Bitmap: `Image::make(pattern.png)`, drawn 32 times using `drawImage`.
- SVG: `AnimatedImage::make(pattern.svg)`, recorded at six fixed positions.
- Lottie: `AnimatedImage::make(moving-solid.json)`, explicitly sought at
  `i * (1.19 / 299)` seconds, from 0.00s through 1.19s without endpoint clamping.
  Seek, per-frame picture/DisplayList
  construction, replay and completion are timed for this scene.

Static scenes replay an existing picture/DisplayList. Backend work reached by
replay is intentionally timed: in fullTGFX, ThorVG SVG/Lottie materialization
uses its production CPU raster path and the resulting image is drawn to the
actual GPU target. This is **not** a claim that all SVG/Lottie work is GPU-native.

TGFX creates its real device with `GrGraphicsContext::makeOffscreen()` and uses
the checked `DrawableSurfaceCanvas::readPixels()` completion seam, including
sticky/deferred error validation. Skia's common `GrGraphicsContext` has no
`makeOffscreen()` factory. The local Android adapter follows production
`ANativeWindowGraphicsContext`'s EGL/Ganesh bootstrap using a pbuffer; the Apple
adapter follows `MetalGraphicsContext::makeContext`'s retained device/queue and
`GrDirectContexts::MakeMetal`. `SkSurfaces::RenderTarget` creates the real Skia
GPU surface, and checked `SkSurface::readPixels` completes it. Metal is selected
on Apple, OpenGL ES on Android. Backend-internal allocation, transfer and
validation costs remain included.

The metric is **completed GPU render + readback throughput**, not presented
display FPS. Pixel readback, synchronization and owned output allocation are
part of every sample. Static scene recording, fixture I/O, decoding, PNG
encoding and first/last canary scans are outside timing. Each owned readback's
destruction occurs after the per-frame timer and within measured-loop wall/CPU
accounting. Do not compare these results to the managed bitmap benchmark:
managed Skia is CPU raster whereas the native Skia target here is GPU.

The JSON contains all 300 `frame_ms` samples, mean/min/max, nearest-rank p50/p95,
`sampled_throughput_frames_per_second`, and loop throughput including sample
bookkeeping/teardown. `RUSAGE_SELF` user/system CPU milliseconds cover all
process threads during the measured loop; they are **not energy**.
`process_peak_rss_bytes` is a lifetime high-water mark, not per-scene allocation.
`on_screen_fps`, `energy_joules`, and `package_size_bytes` remain null.

Native first/last canaries run outside timing and report FNV-1a hashes, visible
pixel counts and changed pixel counts. Vector/bitmap/SVG have known color
probes, text requires visible ink, and Lottie requires red/transparent probes
in the expected moving-square positions plus at least 128 changed pixels.
Static first/last frames must match exactly within each run. Cross-backend
hash equality is not required because antialiasing may differ. These are bounded
canaries, not a replacement for the independent correctness suites.

The core fails on setup/decode/record/readback/canary errors. It never publishes
successful aggregate timing statistics for a failed scene, and a missing or
disabled Lottie capability prevents overall success. All five scene outcomes
remain present after per-scene preparation failures.

Pinned input sizes and FNV-1a fingerprints are checked at runtime:

| Input | Bytes | FNV-1a 64 |
| --- | ---: | --- |
| NotoSans-Regular.ttf | 569208 | ae99b7ce843cff8d |
| pattern.png | 75 | 3b980986c0e91411 |
| pattern.svg | 180 | 8f47efa4f4a52003 |
| moving-solid.json | 390 | cb40538e2981af8f |

These fingerprints are identity checks, not cryptographic attestations.
The original manifest supplies SHA-256/provenance; the font license is bundled.

## Parent native wiring

Targets in `//snap_drawing/experimental/tgfx/full/performance/native`:

- `:render_performance`: reusable static C++ core.
- `:librender_performance.so`: Android JNI shared-library consumer.
- `:render_performance_ios`: Apple static-library consumer.

The core sets `linkstatic = True` so it does not generate a competing
`librender_performance.so`; the JNI `cc_binary` owns that shared-library output.
This BUILD-only correction supersedes the prior BUILD freeze hash. Native
source APIs, target names, workloads, 30/300 frame counts, and shell settle
behavior are unchanged. The parent's first Android TGFX attempt stopped at
analysis with the conflicting output actions; no compile errors were observed
in that attempt. The parent owns retrying the matching backend/platform targets.

The subsequent source review corrected only the SVG canary x coordinates:
red `(30,40)` became `(120,40)` and blue `(300,40)` became `(240,40)`.
Production `Rect::makeFittingSize` uses the minimum scale for
`FittingSizeModeCenterScaleFit`. The pinned 16x8 SVG inside `(16,16,328,82)`
therefore has scale `min(328/16,82/8)=10.25`, size 164x82, and fitted bounds
`(98,16,164,82)`. Its red half covers x98..180 and blue half x180..262.
Both old probes were outside the image; the new probes are inside their
respective solid halves. Skia's `SVGAnimatedImage::doDraw` and fullTGFX's
`ThorvgMedia::draw`/`drawAtPlayback` use this same production fitting method.
The render geometry, SVG input, colors, y coordinates and tolerance are
unchanged. This is source/formula verification, not runtime pixel evidence.

Freeze after the static-link and SVG-canary corrections:

```text
render_performance.cpp before canary correction SHA-256:
e7370e3f13698656ac3cc6a53c55ba48f04b565cfa2e7cb444bbb54af1b18fdf
render_performance.cpp after canary correction SHA-256:
025f9b89dfaf4a7e8684b3b1819e8f81e2397c5ba508dd7633969f12de4c92ba
BUILD.bazel after linkstatic=True correction SHA-256:
7549a0e1f66feaeed0eb10b4aa445c81c40ed70ac17c4d7230a1866d48f56874
```

Reversing exactly the two canary-coordinate substitutions reproduces the
preserved pre-correction source hash. No source API, measurement, target name,
or other renderer implementation changed. These inputs are refrozen for the
parent's next native compile attempt; no build was run by this child.

The parent's second Android TGFX attempt passed analysis and reported five
invalid unary `!` checks on `Valdi::Ref` in `render_performance.cpp`. The actual
`Shared.hpp` API provides `== nullptr`/`!= nullptr`, without boolean conversion.
The five checks on the font result's value, text layout, image, animation and
display list now use `== nullptr`. Auditing the remaining owned sources found
one additional check on the `Ref<GrGraphicsContext>` member in `gpu_target.cpp`;
it uses `== nullptr` as well. Valid `Valdi::Result`, `std::shared_ptr`, `sk_sp`,
`TGFXContextLock`, stream, Objective-C and raw-pointer checks remain unchanged.

Latest freeze after the six null-check substitutions:

```text
render_performance.cpp before Ref correction SHA-256:
025f9b89dfaf4a7e8684b3b1819e8f81e2397c5ba508dd7633969f12de4c92ba
render_performance.cpp after Ref correction SHA-256:
2b941a613099fc93d6a717b4b100cba0754afffa2180207757c81cf71b21c4da
gpu_target.cpp before Ref correction SHA-256:
a99f46e5a6cb05ae8cc0c823f341a20ae8e8ae05d768549ce61fac3a6d07347f
gpu_target.cpp after Ref correction SHA-256:
15cfcb5d183844c8250f7b132b22dec369f180410e73994dc59ce577f088459a
BUILD.bazel unchanged SHA-256:
7549a0e1f66feaeed0eb10b4aa445c81c40ed70ac17c4d7230a1866d48f56874
```

Reversing only the five and one null-check substitutions reproduces each
preserved before hash. Thresholds, timing boundaries, workloads, canaries,
30/300 counts, target names and shell behavior are unchanged. This is source
verification; compile/link and runtime validation remain with the parent.

The parent's first Android Skia native runtime probe completed 4/5 scenes;
text failed preparation before warmup because the fixture appended a newline
after its twentieth line. Production `TextLayoutBuilder` checks the following
empty line's height even when the newline is the last character. The pinned
font's source/table metrics give approximately 29.4192px per line at size 18
and multiplier 1.2: twenty lines use 588.384px, while the unintended twenty-first
uses 617.8032px and exceeds the unchanged 608px height. These calculations are
source-derived, not newly observed runtime metrics.

The fixture now inserts nineteen separators between twenty unchanged lines.
The null-layout/fit/width checks remain intact. Failure diagnostics include
actual native font metrics, line height, newline count, fit result, bounds,
container size and entry/segment counts. Font, geometry, line-height multiple,
rendering, canaries, timing and all other scenes are unchanged.

The complete failed report and its exact source snapshot are preserved in
[`evidence/android-skia-text-layout-failure/`](evidence/android-skia-text-layout-failure/README.md),
with provenance, SHA-256 values and the production-source explanation.
Latest `render_performance.cpp` freeze SHA-256:
`4783124669e0abeded5e20205879ecfd7ddc3a4db6a913a4397052cd56abe414`.
`BUILD.bazel` and `gpu_target.cpp` retain the hashes above. The child performed
source inspection only; parent rebuild and rerun are required to validate
the correction.

The core directly depends on `//snap_drawing:snap_drawing_cc` and
`//valdi_core:valdi_core_cc`. Apple platform implementation sources enter once
through `:apple_offscreen_context` and `//snap_drawing`. Android does not need
the platform facade's alwayslinked Valdi Java adapters; EGL/GLES/android/log
system linkage is explicit. The bridge directly references the real utility
`getEnv()` so its `JvmUtils.cpp` and sole `JNI_OnLoad` cannot be discarded.
No new `JNI_OnLoad` is defined. Use normal JNI-enabled Android configuration.

Compile the **same consumer target** twice, in serial, changing only the normal
backend selection. Do not set backend macros on just this fixture: production
headers and libraries must share the exported layout defines.

Example parent commands from the repository root, using its installed Bazel
toolchain and prepared backend dependencies (not executed here):

```sh
PERF_LABEL=//snap_drawing/experimental/tgfx/full/performance/native
bazel build -c opt \
  --platforms=@snap_platforms//os:android_arm64 \
  --repo_env=VALDI_PLATFORM_DEPENDENCIES=android \
  --define=snap_drawing_backend=skia \
  "$PERF_LABEL:librender_performance.so"
# Copy the resulting .so and matching libc++_shared.so to the Skia stage below
# before building TGFX, since bazel-bin otherwise names the latest selection.
bazel build -c opt \
  --platforms=@snap_platforms//os:android_arm64 \
  --repo_env=VALDI_PLATFORM_DEPENDENCIES=android \
  --define=snap_drawing_backend=tgfx \
  "$PERF_LABEL:librender_performance.so"
```

For iOS, use `:render_performance_ios`, `-c opt`,
`--repo_env=VALDI_PLATFORM_DEPENDENCIES=ios`, and the parent's SDK platform
`--platforms=@snap_platforms//:ios_sim_arm64` or the actual device platform.
Build each backend separately and preserve its archive before the next build.
The parent must inspect actual output locations and architecture/platform.

The iOS shell needs a single merged archive containing the fixture, drawing
backend and dependency closure, or the fixture archive with an explicit
remaining static closure. An arm64 simulator archive is not a device archive.
Do not also link a heavy ValdiRuntime app/framework or component archives
already merged into the supplied archive. No `--whole-archive`/force-load
duplication is required: the shell directly references the fixture entry point.

## Android Release shell

Both variants use bundle ID `io.valdiroutes.renderperformance`, with the same
Java host, fixture assets, native filename and NDK C++ runtime. Stage each:

```text
android/.local/skia/jniLibs/arm64-v8a/librender_performance.so
android/.local/skia/jniLibs/arm64-v8a/libc++_shared.so
android/.local/tgfx/jniLibs/arm64-v8a/librender_performance.so
android/.local/tgfx/jniLibs/arm64-v8a/libc++_shared.so
```

Package additional non-system shared dependencies if the parent's ELF closure
requires them. Gradle packages the parent's libraries; it does not invoke an
NDK build. Run the parent's installed compatible Gradle distribution, e.g.:

```sh
gradle -p snap_drawing/experimental/tgfx/full/performance/native/android \
  -PnativeBackend=skia assembleRelease
gradle -p snap_drawing/experimental/tgfx/full/performance/native/android \
  -PnativeBackend=tgfx assembleRelease
```

Outputs are separated under `android/.local/build/{skia,tgfx}/outputs/apk/release`.
Supply the same parent signing configuration for both Release APKs; this
source shell does not invent signing credentials. For local installation only,
the parent can explicitly opt into `-PlocalDebugSigning=true` to sign both
non-debuggable Release APKs with its existing Android debug key.
`debuggable=false` is set by the Release build type; the source manifest omits
that attribute so Gradle assigns it without a `HardcodedDebugMode` lint error.
The host also rejects a debuggable application at runtime. No lint suppression
or baseline is added. This manifest-only correction preserves the eight-second
settle and all frozen native sources/targets. The host rejects a non-optimized native
library or an unexpected backend. `ndebug` is reported separately: parent
must verify actual `-c opt`/Release compiler/linker flags for the whole closure.

The actual Djinni Java source root is included, with desktop Dropbox sources
excluded, alongside real ExifInterface and javax.inject dependencies. The
current pinned TGFX revision needs no separate helper Java tree; a revision
with real helpers can supply `-PtgfxAndroidJavaDir=/absolute/source/root`.
Do not add another copy if already packaged.

The final report is mirrored, byte-for-byte, to internal files and:

```text
/sdcard/Android/data/io.valdiroutes.renderperformance/files/render-performance.json
```

The parent can pull it with `adb pull` without `run-as` or storage permission.
Persistence failure displays FAIL and preserves a nested native report in a
failure envelope where possible. Stable resource IDs are
`io.valdiroutes.renderperformance:id/render_performance_status` and
`...:id/render_performance_results`; descriptions are
`render-performance-status` and `render-performance-results`. Status text ends
as exactly PASS or FAIL. A fresh process runs once; force-stop between trials.

On foreground entry the status is `SETTLING` for a fixed eight seconds before
the benchmark worker starts. Pausing cancels the pending callback; returning to
foreground starts a fresh full eight-second settle. The callback checks that
the Activity is still resumed/alive, and the worker checks foreground again
immediately before calling the unchanged native entry point. This startup
delay precedes all native warmup and measured frames. Collect launch UI
screenshots/trees during this interval, then avoid UI polling during timings.

## iOS Release shell

The CMake project compiles only `ios/main.mm`, imports the explicit parent
archive, and packages the same seven fixture/provenance/license files.
Example parent configuration:

```sh
cmake -S snap_drawing/experimental/tgfx/full/performance/native/ios \
  -B /absolute/parent-output/skia-shell -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DRENDER_PERFORMANCE_BACKEND=skia \
  -DRENDER_PERFORMANCE_ARCHIVE=/absolute/skia-native-closure.a
cmake --build /absolute/parent-output/skia-shell --config Release
```

Repeat serially with a separate output directory, `BACKEND=tgfx`, and its
matching archive. If dependencies are not merged, pass their explicit paths
in `-DRENDER_PERFORMANCE_LINK_LIBRARIES='...;...'`. Device builds use the
matching SDK/archive and the parent's actual signing configuration, including
`-DRENDER_PERFORMANCE_SIGNING_ALLOWED=YES`. Both variants use bundle ID
`io.valdiroutes.renderperformance` and executable `NativeRenderPerformance`.

The app waits for eight uninterrupted foreground seconds with status `SETTLING`
before starting its worker. Resigning active invalidates pending callbacks;
returning active starts a new full eight-second settle. The main-queue callback
checks `UIApplicationStateActive`, and an atomic foreground flag is checked at
worker entry before the unchanged native call. The delay is excluded from
native warmup and measured timings. Results are written
to `Documents/render-performance.json`; file sharing is enabled. Accessibility
identifiers are `render-performance-status` and `render-performance-results`,
with final PASS/FAIL status. Shell startup/parse/persistence failures cannot
become PASS.

This settle change touches only the Android Java shell, `ios/main.mm`, and
documentation. Core `.cpp`/`.hpp`, JNI `android/bridge.cpp`, `BUILD.bazel`, target
labels, backend/optimization validation, and 30/300 frame counts remain frozen.
Repackage/rebuild the shells around the parent's existing native artifacts.
The independently owned neutral managed Android/iOS hosts must apply the same
eight-second foreground settle before starting their managed benchmark; they
are outside this directory's edit ownership.

Parent measurements still required: source/build identity, actual Release
flags, ELF/Mach-O closure, install/run and report retrieval, device/OS/thermal
conditions, repeated serial trials, actual energy instrumentation and artifact
sizes. This source fixture supplies no fabricated FPS, joules or size numbers.
