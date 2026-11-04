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
The subsequent mobile verification cross-built real TGFX libraries, linked
the nine adapted files into standalone iOS/Android probes, and ran the same
16 checks successfully on both simulators. Build details, replay results and
limits are in `mobile/README.md`.

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
- Mobile verification covers standalone probes on simulators. No physical
  device, full Valdi/JS runtime or production application switch was performed.
- The approximately 5 MiB TGFX archive has optional functionality disabled.
  It is not comparable with the complete production Skia archive, and is not
  evidence of an application-size reduction.

Save counts are translated at DrawingContext, and restore counts are clamped
before reaching TGFX. Colors and geometry convert by value. Pictures use
TGFX's `shared_ptr` ownership; borrowed canvases stay with their recorder or
surface. No Skia/TGFX layout or ABI casts are used.

## Evidence

`evidence/results.json` preserves the original `8a42d138` checkpoint's inputs,
hashes and results. Its source manifest describes that historical commit,
before the shared-fixture extraction. `evidence/mobile-results.json` records
the later mobile source/build/runtime evidence. The original successful test
outputs remain in `evidence/opt-test.txt` and `evidence/sanitizer-test.txt`.

The first three Bazel attempts failed during target/toolchain setup; the fourth
compiled and ran the initial 15 checks. The review regression made the final
fixture 16 checks (15 passing controls plus one real GPU pixel failure before
the fix). An initial TGFX configuration failure and an
interrupted dependency download are preserved separately. These failures are
not behavioral RED tests and are not presented as such.

## Full-backend media checks

`full/tests/media_checks.cpp` is a source-only suite for the complete production
TGFX media migration. It is separate from the historical nine-source vector
probe described above. The parent's first native run exited inside libjpeg
while decoding the original malformed JPEG fixture, before a media report
returned. After the matching TGFX JPEG/effects rebuild, the parent's second
native run returned 48 media cases: 46 PASS and 2 FAIL, confined to GIF/WebP
boundary timing. All JPEG recovery cases passed. The timing fix now accumulates
GIF delays in integer milliseconds and preserves libwebp's cumulative
millisecond endpoints before converting each endpoint to `Duration` once.
The original assertions, tolerances and fixture bytes are unchanged.
Before/after timing evidence is recorded in the fixture provenance. The current
capacity/font/deferred-raster changes are source-only and await the parent's
matching archive rebuild/run; no new native result is claimed here.

The parent runner can include `full/tests/media_checks.hpp`, link the actual
full SnapDrawing implementation, and call:

```cpp
MediaTestReport report = runMediaChecks("/absolute/path/to/full/tests/media-data");
std::cout << report.details;
return report.success ? 0 : 1;
```

The interface has no `main`. It returns `passed`, `failed`, `success`, text
details, and individual named `MediaCheckResult` records. The source requires
both `SNAP_DRAWING_TGFX_EXPERIMENT` and `SNAP_DRAWING_TGFX_COMPLETE`. It calls
the actual `Image`, `Bitmap`, `BitmapFactory`, `AnimatedImage`,
`SkCodecAnimatedImage`, `LottieAnimatedImage`, `SVGAnimatedImage`, `SVGUtils`,
`FontManager`, `DrawingContext` and `DrawableSurfaceCanvas` APIs. The target
must include the real `TgfxMedia.cpp`, `TgfxMediaAnimation.cpp`,
`ThorvgMedia.cpp`, text and GPU integration sources and their actual native
dependency closure. The suite provides no substitute codec or symbol provider.

The 78 named cases cover (the preceding 62 cases/assertions are preserved):

- PNG/JPEG/lossless WebP pixels and metadata; production PNG/JPEG/WebP encoding
  and decoding; CPU resize followed by production image recording and GPU draw.
- GIF/APNG/WebP frame delays, boundary seeks, background/previous disposal,
  blending, duration/frame metadata, current-time clamping and center-fit.
  Additional checks cover GIF interlace and APNG's excluded default poster.
- Lottie rendering at two actual times, metadata and fit modes; SVG intrinsic
  and preferred sizes, static and animated factory routing, pixels and time.
- Real custom-font registration from the bundled Noto Sans bytes, byte export
  and ThorVG Lottie glyph mapping compared with the same actual TGFX typeface.
- Raw no-copy owner retention through last native-image release on a worker,
  source-independent copies, actual bitmap disposal while an image pins its
  bytes, padded strides/canaries and bit-exact F32 raw copies.
- Production surface snapshots after surface/context destruction, texture
  readback using an already locked context, and encoding through a retained
  owning device after the original surface is released.
- Malformed and truncated containers, bad APNG CRCs, empty/non-image input,
  short raw spans, invalid stride, allocation overflow and non-finite quality.
- Original public decode-error labels and byte/magic metadata, fatal JPEG
  header recovery, four complete-header JPEG pixel failures, real encoder
  dimension-limit recovery and successful destination growth after failure.
- Incomplete/mismatched SVG structure rejection with a valid trailing-comment
  control, and viewBox-only rasterization requiring both preferred dimensions.
- Nine explicit capacity rejections across GIF/WebP/APNG (large canvas, many
  delta frames, excessive count), three valid small delta controls, incremental
  APNG count validation, and a 20000x20000 SVG size query/32x32 preferred raster.
- Actual SVG text with a cold ThorVG font cache, an unrelated live asset's font
  cache, two asset-owned providers using the same alias, and strict native
  resolver rejection propagated as a real SVG load error.
- SVG/Lottie thin-detail rasters at 2x/3x and one recorded picture replayed at
  both scales after changing the live animation time and dropping its public
  owner; captured center-fit bounds, recording/final clips, anisotropic skew,
  finite perspective, invalid/capacity failure status and empty final clipping.

The font group runs first and verifies the cold-cache precondition explicitly.
Run the media suite before any other suite that retains ThorVG fonts; the
current desktop and mobile parent runners already run media before text.
A prepopulated cache is a named failure, never a skip or inferred cold result.

`full/patches/thorvg-svg-font-resolver.patch` is the frozen actual ThorVG change
at pin `f0fd698d56b3b8ae797a242cf06aab6b70aed840`, SHA-256
`706a80e20f54fe3a9e5d5d97b4431a39c32b429e5ae81fefbb4d95bca0484a0d`.
Its seven files wire the SVG builder to the actual picture resolver and add
`Picture::resolver(func, data, fallback, strictSvgFonts=false)`. Enabling the
last argument makes the owner resolver exclusive for SVG text; rejection
returns a load failure without named-cache or arbitrary-font fallback. The
default preserves the original policy. The C++ resolver symbol and internal
context/resolver layouts change: rebuild ThorVG and all consumers against the
matching header. The parent's separate WebP prefix change is excluded.

Default SVG font resolution now uses an asset-owned actual `FontManager`, its
native platform fallback policy, and `Typeface::makeFontData()` selected-face
SFNT exporter, including CoreText fonts without a TGFX backing stream.
The experimental animated SVG factory also accepts and
preserves its caller's `IFontManager`. Both routes register private per-asset
ThorVG names. An empty export or loader failure remains an error; no raw TTC
stream fallback is used.

ThorVG drawing uses actual `Canvas::drawDeferred` records that own media,
time, bounds and fitting mode. Each playback resolves the final device matrix
and clip. Affine column lengths retain scale, rotation, skew and anisotropy;
finite perspective uses a conservative bound on the projective Jacobian
across the fitted rectangle. The raster is drawn back into the same logical
bounds. Non-finite/horizon-crossing transforms and physical rasters exceeding
32 MiB of RGBA pixels fail through `Canvas::markRenderFailure`, using the GPU
owner's public replayable error API. An empty final clip allocates no raster.
The independent 32-pixel load-validation cap remains unchanged.

The eager animation cache has explicit local admission limits:
128 MiB decoded/staging storage, 16 MiB encoded input and 4096 frames per
decode. For RGBA canvas bytes `C`, encoded bytes `E`, and frame count `N`,
admission requires `1 MiB + 6*E + W*C + N*(C + 4096) <= 128 MiB`.
`W` is 3 for streamed GIF (composite, restore-previous and row reserve) and 8
for WebP/APNG. WebP includes its two native canvases plus pixel/transform/row
workspace reserve; APNG includes composite, restore, decoded rectangle, native
conversion and row workspace. Source staging and metadata are charged even
when a particular format uses less. This is an admission budget for one decode,
not a process-wide allocator/RSS or TGFX GPU-cache limit.

GIF first scans real giflib headers and compressed subblocks with no decoded
canvas, then reads scanlines through `DGifGetLine`; it no longer calls
`DGifSlurp` or retains `SavedImages`/full indexed rasters. WebP checks real
`WebPDemuxGetI` canvas/count metadata before `WebPAnimDecoderNew` allocates its
canvases. APNG checks `acTL` and each `fcTL` before growing parsed frame storage;
every retained frame is checked again. Over-capacity inputs return explicit
errors; frames are never silently dropped or downscaled.

The complete media target requires the real `@expat//:expat` dependency
(`expat.h` and its library) for bounded XML parsing before ThorVG loading.
SVG source is capped at 64 MiB, parsed in 64 KiB chunks with a nesting limit of
1024, with parameter-entity parsing disabled and no external entity resolver.
Root dimensions and viewBox geometry remain distinct; viewBox alone supplies
no intrinsic raster size. The default Skia branch is preserved.
ThorVG load/asset validation still renders the real first frame, with a longest
edge of at most 32 pixels independently of intrinsic size. Size queries retain
the original dimensions, and subsequent rendering uses the caller's requested
target. A large document with a small preferred raster therefore avoids an
intrinsic-size validation allocation.

`full/patches/tgfx-jpeg-recoverable-errors.patch` records the actual change to
the pinned TGFX `src/core/codecs/jpeg/JpegCodec.cpp`. Each libjpeg operation
uses a separate C-only recovery boundary, restores its original error manager,
and returns explicit failure before normal C++ cleanup. Encoding uses the
public libjpeg destination-manager API so every buffer growth updates the
caller-owned allocation immediately, including on an error path. Only premature
input warnings are promoted to decode failures; ordinary diagnostics remain.

The fixtures and preserved font license are documented in
`full/tests/media-data/PROVENANCE.md`; `manifest.json` records their hashes.
The C++ runner needs that directory at runtime. Python/Pillow are only needed
to regenerate authored encoding fixtures, never to run the checks.

There is no skip outcome. Missing fixture files, unsupported parsers, missing
font export, inability to create/lock the actual platform offscreen context,
or failed GPU readback produce named failures and `success=false`. On Apple,
GPU observations use the production `GrGraphicsContext::makeOffscreen()` Metal
seam. The suite does not create a separate OpenGL fallback.

Coverage boundaries remain explicit: these are small deterministic fixtures,
not a format conformance corpus or application/device QA. The glyph comparison
permits rasterizer edge differences. Static SVG factories use the default
native provider; custom SVG bytes are covered through the experimental animated
factory's actual font manager. F32 checks certify raw byte-copy preservation, not HDR GPU
parity; the pinned TGFX GPU supports native 8-bit buffers. No universal
Skottie/ThorVG, ICC-profile or antialiasing equivalence is claimed.
