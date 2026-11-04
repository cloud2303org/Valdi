# Media fixture provenance and expected results

These fixtures support `../media_checks.cpp`. The C++ checks use the production
SnapDrawing APIs and do not import the fixture generator or implement decoders.
No renderer checks were run when this source-only fixture suite was authored.
The parent's first native media run later exited inside the malformed JPEG
decoder before returning a report. The parent's second native run returned
46 PASS / 2 FAIL, with all JPEG recovery regressions passing and the two
GIF/WebP boundary failures documented below. The subsequent timing fix is
source-only and has not been executed here.

All patterns, SVG, Lottie JSON, and the PNG/APNG, GIF and WebP container assembly
were authored specifically for this suite and are covered by the repository
MIT license. `generate_fixtures.py` records the authoring recipe. It uses Python
stdlib zlib/struct for PNG/APNG, a minimal literal-only GIF encoder, and Pillow
solely as an independent JPEG/VP8L encoder. The WebP animation container is
assembled explicitly so its rectangle, blend and disposal controls are known.
Pillow is not required to compile or run the C++ suite.

`NotoSans-Regular.ttf` is an unchanged copy of
`snap_drawing/testdata/NotoSans-Regular.ttf`. Its original copyright and SIL
Open Font License 1.1 are preserved in the unchanged `NotoSans-LICENSE.txt`.
The font is not subsetted, renamed or redistributed under the MIT license.
The alias `ValdiMediaFixture` is registered at runtime through the real
`FontManager`; it does not rename the font file.

`Montserrat-Regular.ttf` is an unchanged copy of the repository's
`snap_drawing/testdata/montserrat_regular.ttf` (201052 bytes). Its embedded
name table declares copyright 2011 The Montserrat Project Authors
and SIL Open Font License 1.1. `Montserrat-LICENSE.txt` preserves that embedded
copyright and the complete SIL OFL 1.1 terms, reusing the identical generic
license body already distributed with Noto Sans. No font tables, glyphs or
internal names are changed. This second distinct bundled face makes provider
isolation independent of installed platform fonts.

`manifest.json` records fixture sizes, SHA-256 hashes and the independent
Pillow encoder version. Regenerating the format fixtures does not alter the
copied font or license.

## Still images

- `pattern.png`: 2x2 RGBA8, rows red/green and blue/opaque white.
- `pattern.webp`: lossless 2x2, rows red/green and blue/half-transparent white.
- `solid.jpg`: 16x16 RGB (200, 40, 80), independently encoded at quality 100
  without chroma subsampling. Pixel assertions allow JPEG quantization.
- `interlaced.gif`: 4x4 with red, green, blue and white rows. Its encoded
  interlace row order is 0, 2, 1, 3.

## Four-frame animations

`disposal.gif`, `disposal.apng` and `disposal.webp` have a 4x4 logical canvas,
four frames and delays 100/200/300/400 ms. Total duration is 1000 ms. Seeking
tests sample both sides of each boundary, the endpoint, and clamped times.
All pixel expectations below are premultiplied RGBA at matching output size.

1. `[0,100)` ms: full opaque red canvas.
2. `[100,300)` ms: green top-left 2x2, remaining canvas red. Dispose this
   rectangle to transparent background before the next frame.
3. `[300,600)` ms: blue bottom-right 2x2, transparent top-left, red elsewhere.
   GIF/APNG dispose this frame to its previous canvas; WebP leaves it in place.
4. `[600,1000]` ms:
   - GIF: opaque white top-right, red bottom-right, transparent top-left.
   - APNG: half-white OVER red at top-right gives approximately
     (255,128,128,255); bottom-right is red and top-left remains transparent.
   - WebP: half-white OVER transparent at top-left gives approximately
     (128,128,128,128); blue bottom-right remains and top-right stays red.

`poster.apng` has an ordinary yellow IDAT poster excluded from its animation.
The animation has only two frames: a blue top-left 2x2 for 100 ms, then an
additional red top-right 2x2 for 200 ms. The rest remains transparent.
The still `Image` API should display the yellow poster; the animated API must
not synthesize a poster frame or include its pixels.

## Vector media

- `moving-solid.json`: authored Lottie, 32x16, 10 fps, 20 frames, two seconds.
  An opaque red 8x8 solid uses hold keyframes: x=4 at 0.25 seconds and x=20 at
  1.25 seconds, y=4 throughout. There are no external assets or fonts.
- `pattern.svg`: authored 16x8 SVG with a red left half and blue right half.
  Preferred width 32 implies height 16. Center-fit into 32x32 leaves clear
  letterboxing above and below.
- `custom-font.json`: authored Lottie text `HgQ`, 32 point, baseline position
  (8,44), using the registered `ValdiMediaFixture` font alias. The check first
  requires an actual custom typeface and exportable font bytes. It compares
  ThorVG output against direct TGFX text using that same real typeface:
  substantial red coverage, bounds within three pixels, and at least 80%
  agreement on the reference glyphs' stable opaque interiors. This permits
  rasterizer edge differences; it is not a pixel-identical text parity claim.

## Deliberately invalid inputs

`not-image.bin` is explicitly plain text. `truncated.*` files are cut before
complete container data. `bad-crc.apng` has a deliberately corrupted IEND CRC.
`malformed.json` and `malformed.svg` are unfinished authored literals. These
are expected rejections, not optional fixtures. Deferred still-image creation
is permitted only if actual pixel decoding then rejects the truncated input.

Raw stride, owner-release, disposal, overflow and F32-copy checks author tiny
pixel spans in C++ rather than storing duplicate files. Their expected bytes
are literals; their actual operations all call production implementations.

## JPEG recovery regressions

These fixtures derive from independently Pillow-encoded JPEGs using only
marker-envelope inspection and byte slicing in `generate_fixtures.py`. No
production codec is used to generate or classify them.

- `truncated.jpg`: the first 20 bytes of `solid.jpg`, ending within the APP0
  segment. This is the original malformed-header process-exit reproduction.
- `jpeg-truncated-after-sos.jpg`: `solid.jpg` through its complete SOS header,
  with all entropy data and EOI removed.
- `jpeg-missing-eoi.jpg`: `solid.jpg` with its final EOI marker removed.
- `jpeg-missing-quant-tables.jpg`: all pre-SOS DQT segments removed from
  `solid.jpg`, retaining its complete SOF/SOS headers and entropy data. This
  reaches libjpeg's actual missing-quantization-table error during pixel decode.
- `jpeg-truncated-during-scanlines.jpg`: a 64x64 RGB pattern with channels
  `(37*x+11*y, 7*x+43*y, 53*x+3*y) mod 256`, encoded at quality 95 without
  subsampling, then cut three-fifths of the way through its entropy stream.

Every complete-header regression requires actual TGFX codec creation to
succeed, actual F16 pixel decoding to reject the body, and a subsequent valid
JPEG decode to work in the same process. F16 exercises the production JPEG
conversion path's Bitmap/Pixmap lifetimes during recovery.

The encode regression constructs a real Gray8 65536x1 Pixmap in C++. Its width
exceeds libjpeg's JPEG dimension limit and must return failure without exiting.
A subsequent 128x128 xorshift Gray8 image must encode to more than 4096 bytes
and decode through production APIs, exercising destination-buffer growth.
There are no injected error handlers, mocked allocators or codec substitutes.

The SVG regressions use explicit authored C++ literals: an unfinished root,
mismatched elements, a valid document with a trailing comment, and a viewBox-only
document. They exercise real Expat structure validation followed by ThorVG
loading and rendering, including the required preferred-size contract.

## GIF/WebP boundary timing correction

The native baseline is
`/tmp/valdi-tgfx-full-20261104/backend-checks-run2.log`, lines 194-196
(the same failures are duplicated in its report details). The GIF and WebP
metadata/timing/disposal cases fail; APNG passes. Both failing cases select
the preceding frame at exactly 0.300 and 0.600 seconds. For example, at 0.300
the top-left pixel is still opaque green and bottom-right still red, instead
of transparent and blue. At 0.600 GIF still displays the blue bottom-right
frame, and WebP has not yet drawn the half-white top-left frame.

The fixtures encode unchanged delays of 100/200/300/400 ms. Their timebases
are supported by the actual dependency source:

- giflib `gif_lib.h`, `GraphicsControlBlock::DelayTime`: hundredths of a second.
  `dgif_lib.c`, `DGifExtensionToGCB`, reads the unsigned little-endian 16-bit
  field directly. The authored GCB values are 10/20/30/40.
- TGFX's bundled libwebp `src/webp/demux.h`, `WebPIterator::duration`: milliseconds.
  `src/demux/anim_decode.c`, `WebPAnimDecoderGetNext`, computes
  `timestamp = prev_frame_timestamp_ + iter.duration` and returns it.
  The authored ANMF values are 100/200/300/400.
- Production `Valdi::Duration` stores double seconds and
  `fromMilliseconds(value)` divides by 1000. Production
  `SkCodecAnimatedImage::doDraw` selects the first exclusive endpoint greater
  than the requested time, so seeking exactly to a boundary must advance.

Before: `appendFrame` added each separately converted duration to the previous
double endpoint. GIF supplied each delay; WebP subtracted consecutive decoder
timestamps to obtain a delay. Summing 0.1 + 0.2 produces
0.30000000000000004, and then adding 0.3 produces 0.6000000000000001.
Both endpoints exceed their intended boundary, explaining the logged old-frame
pixels without changing disposal or blending.

After: `appendFrame` accepts an absolute `Duration` endpoint. GIF accumulates
its delays in `int64_t` milliseconds; WebP uses its returned timestamp directly.
Each converts 100/300/600/1000 ms once. APNG continues converting its cumulative
milliseconds once, using the same helper without overwriting the endpoint.
No epsilon, timestamp rounding, pixel tolerance change or fixture correction
is used. The existing exact-boundary assertions remain the regression controls.

Unchanged baseline fixture SHA-256 values:

- `disposal.gif` (173 bytes):
  `ed165cae525bbfc5db5cabe20e8a0bdbb6136c9b39cbfe3306676384feaff755`
- `disposal.webp` (238 bytes):
  `b3fe8234aca96b03bc88648c01712d0433b28d1f342a320c65791e0c3dd7138a`

This is source-backed diagnosis and a source-only production fix. A subsequent
native run is still required to establish its runtime result.

## Capacity and validation-raster regressions

These additions are source-only. They preserve all 48 existing assertions and
add fourteen named cases, bringing the suite to 62. There is no skip outcome,
and a generic malformed-data error cannot satisfy a capacity rejection: both
the public animated factory and direct production codec must return the
explicit `Animated media capacity exceeded` error.

`large-intrinsic.svg` is an authored 153-byte literal with intrinsic/viewBox
size 20000x20000 and a full red rectangle. The check queries those unchanged
dimensions, requests a real 32x32 static raster, and draws the actual SVG
animated API to 32x32. Load validation must still render through ThorVG, but
its target is independently bounded to a longest edge of 32 pixels. It must
not first allocate a 1.6 GB intrinsic RGBA canvas.

The generator writes three families for each of GIF/WebP/APNG:

- `capacity-canvas.*`: a 20000x20000 logical canvas. GIF and WebP are complete
  tiny files with a genuine encoded 1x1 red frame. APNG deliberately contains
  only CRC-valid IHDR/acTL headers: those fields already exceed capacity, so
  the explicit capacity error must precede attempts to read/decode image data.
- `capacity-delta-timeline.*`: a 1000x1000 logical canvas and 100 genuine 1x1
  red delta frames at (0,0), with 100 ms delays. Cached full RGBA frames alone
  would occupy 400,000,000 bytes despite the tiny compressed rectangles.
- `capacity-frame-count.*`: a 1x1 canvas and 4097 real frames, exceeding the
  independent metadata/count limit without relying on large pixel dimensions.
- `capacity-small-control.*`: the same encoding recipe on a 4x4 canvas with
  three frames. These must decode, report 3 frames/300 ms and render red at
  (0,0) with transparency at (3,3), proving the admission rule does not turn
  valid delta animations into blanket format rejections.

GIF uses an authored two-entry palette and actual LZW codes CLEAR=4,
literal=1, STOP=5, packed as bytes `4c 01`. Its GCB sets transparent index 0
and disposal 1. The production implementation uses giflib for headers,
extension parsing, compressed-block iteration and actual LZW scanline decode.
The row interlace order remains the four passes used in giflib's `DGifSlurp`;
the preexisting interlace and disposal assertions remain unchanged.

WebP uses real independent Pillow VP8L 1x1 payloads inside authored VP8X,
ANIM and ANMF envelopes. The production admission check uses real
`WebPDemuxGetI`; the pinned `anim_decode.c` allocates two full canvases inside
`WebPAnimDecoderNewInternal`, so that call must follow admission.

APNG uses a genuine transparent default poster excluded from the timeline.
For the largest fully encoded fixture, the generator's raw poster span is
4 MB; no expanded animation is constructed. `capacity-extra-frames.apng`
declares one frame but supplies three, testing rejection at the first extra
`fcTL` before it is added to parsed frame storage.

The implementation's per-decode admission formula is documented in the
README media section. All byte products and residual capacity divisions are
checked before canvas allocation or retained-frame copying. Native codec
entropy structures and allocator overhead are not an exact process RSS
measurement; the rule includes explicit workspace/staging reserves and bounds
input size, canvas area, parsed count and retained pixel storage.

## SVG font ownership and final-transform raster regressions

The original 62 cases/assertions and capacity fixtures are preserved. Sixteen
new cases bring the source suite to 78. These additions have not been compiled
or run here, and there is no passing claim or skip outcome.

Four font cases exercise actual font/loader APIs:

- `svg-arial-text.svg`: authored 128x64 SVG, red Arial `Hello`, font-size 32,
  x=8/y=44. The first media case initializes real ThorVG and requires
  `Text::font(nullptr)` to fail before any media font is loaded. Its actual
  `Image::make` call supplies no font manager and must match the selected native
  Arial face. Run media before other suites retaining ThorVG fonts; both parent
  runners currently place media before text. The clean-cache condition is
  checked rather than assumed, reset globally or silently skipped.
- The same document is loaded while the original `custom-font.json` Lottie
  asset remains alive. A real cached-font probe must now succeed, establishing
  the unrelated-cache control. SVG must still match native Arial, independently
  of that asset's Noto Sans.
- `svg-owned-text.svg`: authored 128x64 SVG with red `HgQ` using
  `ValdiMediaFixture`. Two real FontManagers register different native faces
  (the unchanged bundled Noto Sans and Montserrat fixtures) under this same alias.
  Each SVG must match its own provider and loading the second must leave the
  first unchanged. Matching requires real ink, bounds within 3 pixels and at
  least 80% agreement on stable opaque native glyph interiors.
- The actual ThorVG `Picture` API installs a strict host resolver that rejects
  `name:Arial` while an unrelated font remains cached. `Picture::load` itself
  must reject after invoking that resolver, rather than returning a successful
  empty or cache-fallback picture. This is an actual host asset rejection, not
  a mocked font codec.

All native font references use the production `Typeface::makeFontData()`
standalone selected-face export. This covers the default CoreText route too;
a null `openStream()` or raw TTC collection is not treated as export success.
The SVG/renderer patch, exact hash and C++ ABI rebuild requirements are recorded
in the README media section and `full/patches/thorvg-svg-font-resolver.patch`.

`sharp-detail.svg` and `sharp-detail.json` are authored 100x100 documents with
50 one-unit-wide vertical stripes, alternating red and blue with transparent
gaps. Each stripe starts at x=`2*index + 0.25`. Lottie uses actual solid layers,
10 fps and 20 frames; hold keyframes shift each stripe another 0.5 units at
one second. There are no external assets or fonts.

For each format, direct 2x and 3x drawing is compared against an independent
actual ThorVG render at 200x200 or 300x300. A single TGFX picture is also
recorded at logical size with center-fit into 100x140 and a recording clip.
The live animation is advanced to 1.25 seconds and released before that same
picture is replayed at both scales with an additional final-device clip. The
reference retains the original 0.25 seconds, logical fit and matching physical
raster dimensions. This exercises immutable ownership/time/mode, scale chosen
at replay, and both clips.

Additional per-format cases use affine matrix
`[2,.75,0; 0,3,0; 0,0,1]` with a 200x310 native raster, and finite perspective
`[2,0,0; 0,2,0; 1/1024,0,1]` with a 201x220 native raster. These dimensions
follow the affine column lengths and conservative projective-Jacobian bounds,
respectively. All sharp-detail comparisons use real GPU readback and matching
linear/no-mipmap image composition, permitting at most one byte of difference
in each channel and requiring substantial nonempty reference coverage.

Two final cases require native render failure and rejected production snapshot
for NaN, huge physical raster and horizon-crossing transforms, and require
success with no raster allocation when the final device clip is empty.

## ROUND3 memory-font and nested SVG regressions

The prior seven-file resolver patch, 78-case test source and 47-entry fixture
manifest were copied before any ROUND3 edits to
`full/patches/round2-before-round3/`. Its `freeze.json` records the exact hashes.
All 47 prior fixture entries and byte hashes are unchanged. Four additions
bring the manifest to 51 entries and fourteen new cases bring the source suite
to 92. No native compilation or execution was performed for this follow-up.
Every registered check returns PASS or FAIL; cache preconditions are assertions.

The actual pinned `thorvg.h` documents memory-font removal through
`Text::load(name, nullptr, 0)` and file-font removal through
`Text::unload(filename)`. Before ROUND3, the first route called
`retrieve(font(name))`, whose lookup increment was canceled by `close()`;
the second called `retrieve(_findFromCache(filename))` with the same effect.
Neither detached its registration. The media destructor also incorrectly
passed private memory names to the filename API. The ROUND3 patch removes
the matching registry reference without a temporary lookup reference and
preserves retained Text references; no global cache reset is involved.

Seven actual API/lifecycle cases cover:

- Thirty-two repeated copied memory registrations, caller-byte release, exact
  documented unregister results, a failed second unregister and unavailable
  name/any-font lookup afterward.
- Real borrowed Noto registration replaced by owned Montserrat under the same
  name while two Text paints retain the old face. Caller bytes for the copied
  replacement are released. Both old and new paints render fresh `HgQ` after
  unregister; old pixels must equal the retained original-face control and
  differ from the replacement face.
- Thirty-two repeated filename registrations, removal using the actual full
  fixture path, rejection of a second removal, and live retained paints
  rendering after unregister.
- A memory name equal to a real filename, proving filename removal preserves
  that independent memory registration and memory removal cannot remove a
  file registration.
- Sixteen real SVG size queries and static loads/releases, checking actual
  `Text::font(nullptr)` immediately after each query and release.
- Sixteen real animated SVG custom-provider loads/renders/releases, proving a
  font is available while each asset lives and absent after release.
- Eight real embedded `@font-face` loads/renders/releases. An additional Text
  retains the embedded face beyond parser destruction, must decode new `AW`
  glyphs after the parser's decoded bytes are freed, and remains unavailable
  to new name/any-font lookup. The loader must retain its copied bytes.

The remaining seven cases exercise nested SVG through actual Picture loaders:

- `svg-nested-arial.svg` wraps the unchanged `svg-arial-text.svg` in an SVG
  `<image>` using `data:image/svg+xml;base64,...`; both documents are 128x64,
  with the child at x=0/y=0 and width=128/height=64. The envelope is authored
  XML; Python's standard base64 encoder embeds the exact child bytes.
- `svg-nested-twice-arial.svg` applies that same envelope again.
- `svg-nested-owned.svg` embeds the unchanged `svg-owned-text.svg` in the same
  envelope for Noto/Montserrat provider isolation under `ValdiMediaFixture`.
- Default nested rendering from a verified empty cache and while unrelated
  Noto Lottie text remains live must match native Arial. Two provider-owned
  nested documents must match their own selected native faces and remain
  independent. The existing comparison tolerances are unchanged.
- Both nesting depths install a real strict resolver refusing `name:Arial`.
  It must be invoked at the child Text and fail the outer Picture load even
  while an unrelated face is cached.
- A two-level control invokes the actual SFNT loader on unchanged
  `not-image.bin` bytes from the host resolver. Font decode and outer Picture
  load must fail, with no failed registration left behind.

Before ROUND3, `_imageBuildHelper` constructed each child Picture without the
outer resolver or strict-font flag. The patch copies the actual policy before
child load and propagates strict SVG load failure through every ancestor.
There is no XML rewriting, global font preseed or cache fallback substitute.

`svg-embedded-font.svg` is authored XML/CSS containing the complete unchanged
`NotoSans-Regular.ttf` bytes in its base64 `@font-face` URL, with red `HgQ` at
x=8/y=44 and font-size 32. Its XML envelope is repository MIT; the embedded
font retains the SIL Open Font License in `NotoSans-LICENSE.txt`. Font bytes
are neither regenerated nor subsetted. The generator now records this
fixture's separate font license in the manifest.

## ROUND4 preallocation admission and verified fixture corrections

The preceding 92-case source, 51-entry fixture manifest, media allocation
sources and patch evidence are preserved under
`full/patches/round3-before-round4/`. Four additional cases bring the source
suite to 96; one addition brings the fixture manifest to 52. The prior 92 case
bodies/assertions and all glyph/pixel tolerances remain unchanged.

`capacity-static-source.png` is an authored 70-byte bounded error fixture:
PNG signature, complete CRC-valid RGBA8 IHDR declaring 20000x20000, a complete
IDAT containing only a filtered red 1x1 pixel, and IEND. It intentionally lacks
the declared huge pixel payload. Real TGFX PNG header parsing must accept its
declared dimensions; production `Image::make` must preserve those dimensions
without substitution. Pixel conversion, 32x32 thumbnail resize and encode
must reject source staging before entropy decoding or huge allocation.
The pixel-read control uses a correctly sized 32x32 output with canaries.
The fixture does not claim to be a complete valid 20000x20000 image. Its small
encoded span makes this an admission regression independent of decoding a
multi-gigabyte image; rejection requires the actual production admission helper
and the actual public Image paths.

The other new cases require default `Image::make(large-intrinsic.svg)`,
default `Image::makeFromSVG`, and default `rasterizeSVG` to reject the resolved
20000x20000 output before factory allocation. The unchanged previous control
still requires intrinsic 20000x20000 metadata, a real preferred 32x32 static
raster, and a real 32x32 animated draw.
An ordinary real PNG must reject 20000x20000 destination bitmaps and 32x32
destinations with a 5 MiB row stride, while preserving 32x32 rowBytes=0
normalization. A 2048x2048 F32 destination tests aggregate admission: its
64 MiB output and two 32 MiB F16 destination stages individually fit but jointly
consume the whole 128 MiB limit before source, decoder rows and workspace.

The source checks account for actual TGFX `PngCodec::onReadPixels`, which
allocates an RGBA8 source buffer and row pointers when converting to F16.
The production admission formula is recorded in the README. It rejects before
all conversion vectors and applies before public Image output allocations.
No downscaled substitute, dropped data, allocation-exception recovery or fake
decoder is used. The original matching raw/F32 copies remain unchanged.

Parent native run5 at
`/tmp/valdi-tgfx-full-20261104/backend-checks-run5.log` reported 81/92 media
cases for the preceding source. It exposed two verified test prerequisites:

- `thorvg.memory-font-retained-paints-survive-borrow-replacement-and-unregister`
  failed `Canvas::add`. The pinned `Canvas::Impl::add` rejects a paint whose
  renderer differs from the new canvas renderer, even after canvas destruction.
  The CPU helper now duplicates the real paint before adding it. Actual
  `TextImpl::duplicate` retains the font loader, so fresh-glyph decoding and
  post-unregister lifetime remain exercised without moving renderer ownership.
- The embedded font fixture used `src:url(data:font/ttf;base64,...)`. The pinned
  `_srcFromUrl` explicitly searches for opening and closing single quotes and
  returns zero without them. The corrected CSS is
  `src:url('data:font/ttf;base64,...')`; the embedded Noto bytes, glyph text,
  dimensions, coordinates and license are unchanged. The preceding fixture and
  hash remain in the preservation snapshot, and manifest hashes record the
  corrected bytes.

Only that fixture changes among the previous 51 entries; all other 50 retain
their byte hashes. Both Noto and Montserrat have real format-4 cmap mappings
for H/g/Q (respectively 43/74/52 and 79/466/167), so the separate two-provider
coverage failure is not attributed to nonexistent fixture glyphs. Its existing
assertion now reports actual/native/interior counts without weakening any
condition. It remains subject to the parent's next native run, as do the
unchanged final-matrix density and empty-clip checks owned jointly with the GPU
integration. No passing result is claimed for the 96-case source suite.

The fixture generator was edited but not executed for ROUND4. The new literal
PNG bytes and embedded CSS quoting were prepared directly; no native helper,
codec, compiler, build, test, vendor program or device was executed.
