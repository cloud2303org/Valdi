# SnapDrawing vector drawing with TGFX

This isolated experiment compiles nine actual SnapDrawing translation units
against real TGFX. It does not enable TGFX in the Valdi runtime or application.
The normal Skia branches remain selected unless
`SNAP_DRAWING_TGFX_EXPERIMENT` is defined.

## Tested result

On macOS arm64, the standalone Bazel target compiled and linked the adapted
sources and passed 16 checks, including nine actual offscreen OpenGL rendering
checks. The same checks passed with ASan/UBSan on the adapter and test code.
The TGFX dependency was a Release static library without sanitizer
instrumentation; leak detection was disabled. These results are not full
dependency sanitizer coverage.

The nine sources are DrawingContext, Paint, LayerContent, Geometry, Matrix,
Path, Color, BorderRadius and LazyPath. `framework.BUILD.bazel` lists them
explicitly. Preparation exposes file symlinks to these actual sources, avoiding
the parent repository's nested Bazel packages. It neither copies their
algorithms into the test nor builds the entire renderer.

Checks cover geometry, paint defaults, ARGB conversion, affine ordering,
perspective rejection, conic iteration/conversion, empty recording, opaque
rectangles, alpha blending, save/clip/transform/restore, path clips, rounded
borders, conics, path intersection, and oversized restore counts. Picture
replay occurs after destroying its recording DrawingContext.
An empty-path append regression initially produced a solid center where an
EvenOdd hole was expected. Saving/restoring the destination fill rule around
TGFX append corrected it; the same 16-case fixture then passed.

Default Skia compilation, without the experimental define, also succeeded
for the same nine source files using real cached Skia/runtime headers. This
was object compilation, not a full Skia runtime link or application test.
The nine adapted files also compiled to Android arm64 and iOS simulator arm64
objects (18 objects). Those are header/API portability checks; the mobile
TGFX library was not built or linked, and no mobile renderer was executed.

## Dependencies

- TGFX: official `Tencent/tgfx`, commit
  `4cf8991d5a3a010c9314538dd2fea937593b206b` (main snapshot, not a release).
- Source dependencies from that checkout's `DEPS`:
  - vendor_tools: `fdcd3c28c02c9d39aeef5aefede96645b8bc593b`
  - pathkit: `6dc44ae85b4614d4e5accac2d7059d7420db9ed2`
  - skcms: `0a4e501a0e61d62dc38a0157e4355a58f5b01b81`
  - concurrentqueue: `6dd38b8a1dbaa7863aa907045f32308a56a6ff5d`
  - highway: `a523516d35e22a4ba8e2e70a319062cb87352de6`
- Real fmt 7.1.3 source, already available in the framework's dependency cache.
- Bazel 8.2.1, CMake/Ninja, and an Apple C++/Objective-C++ toolchain.

The checkout's LFS demo assets and binary vendor packages were not downloaded.
This configuration builds the required source dependencies and does not use
those pointer files. TGFX PathKit contains Skia-derived path implementation;
this experiment does not remove every piece of Skia-derived code.

Apply `patches/tgfx-opengl-shader-sync.patch` to the pinned TGFX checkout before
building. It limits shaderc synchronization to backends that use shaderc.
The unpatched OpenGL-only configuration failed while attempting that unused
dependency. No TGFX rendering/parser source was changed.

## Reproduction

Clone TGFX at the exact commit above and obtain its listed source dependencies
at the revisions declared in `DEPS`, placing them under its `third_party/`
directory. This experiment's preparation script does not download dependencies.
Apply the recorded patch, then run from this directory:

```sh
bash build_tgfx.sh /absolute/path/to/tgfx /absolute/path/to/tgfx-build
python3 prepare.py \
  --tgfx-source /absolute/path/to/tgfx \
  --tgfx-library /absolute/path/to/tgfx-build/tgfx.a \
  --fmt-source /absolute/path/to/fmt
bazel --ignore_all_rc_files test //:vector_test -c opt --test_output=all
bazel --ignore_all_rc_files test //:vector_test -c dbg \
  --copt=-fsanitize=address,undefined \
  --copt=-fno-omit-frame-pointer \
  --linkopt=-fsanitize=address,undefined \
  --test_env=ASAN_OPTIONS=detect_leaks=0 \
  --test_env=UBSAN_OPTIONS=halt_on_error=1 --test_output=all
```

`prepare.py` checks the TGFX HEAD, the tracked diff against the recorded patch,
the archive format, and a real fmt header. It records paths/hashes in ignored
`.local/inputs.json`. It does not attest that an arbitrary supplied archive was
built from those sources. The saved run records describe the actual local
build used here.

The test requires a working macOS OpenGL context. Failure to create a context
or read back pixels is a test failure, not a skip or a software-rendered
substitute.

## Deliberate scope limits

- Images, bitmaps, external surfaces, fonts, full layer composition and Lottie
  are unavailable in this experimental interface.
- Skottie's `SkCanvas` contract is not implemented by a TGFX canvas. Supporting
  Lottie requires a separate renderer or an explicit real-Skia raster fallback.
- Dash/dot effects, shaders/mask filters and path diagnostics are excluded.
- Recording bounds are retained as metadata, without inserting a hard clip.
  TGFX provides no Skia-style recorder cull-bounds parameter. Out-of-bounds
  behavior and antialiased edges are not certified as equivalent.
- Pixel assertions target stable interior/exterior samples; there is no
  image-wide Skia/TGFX comparison or universal geometry-parity claim.
- No Android/iOS TGFX dependency build or executable link, simulator/device
  run, full Valdi/JS runtime or production application switch was performed.
- The approximately 5 MiB TGFX archive has optional functionality disabled.
  It is not comparable with the complete production Skia archive, and is not
  evidence of an application-size reduction.

Save counts are translated at DrawingContext, and restore counts are clamped
before reaching TGFX. Colors and geometry convert by value. Pictures use
TGFX's `shared_ptr` ownership; borrowed canvases stay with their recorder or
surface. No Skia/TGFX layout or ABI casts are used.

## Evidence

`evidence/results.json` records local inputs, source/artifact hashes, test
outcomes, and paths/hashes for preserved logs. `evidence/opt-test.txt` and
`evidence/sanitizer-test.txt` contain the successful test output.

The first three Bazel attempts failed during target/toolchain setup; the fourth
compiled and ran the initial 15 checks. The review regression made the final
fixture 16 checks (15 passing controls plus one real GPU pixel failure before
the fix). An initial TGFX configuration failure and an
interrupted dependency download are preserved separately. These failures are
not behavioral RED tests and are not presented as such.
