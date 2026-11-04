# Run5 variable-layout glyph coverage: backing-data lifetime

Source-only correction, 2026-11-04. Parent runtime evidence is
`/tmp/valdi-tgfx-full-20261104/backend-checks-run5.log`, lines 413-444
(the suite is also printed again later in the log). It reports 29/32:
only the three `text.variable-layout-gpu-coverage` axis cases fail with
`Actual text raster has no glyph coverage`. Coordinate readback, shaped
advances, clone independence, raster-cache independence, collection exports
and all twelve shadow cases pass. This note does not claim a passing rerun
after the correction.

## Concrete source trace

The three failing cases call `instancePicture` in `../text_checks.cpp`.
That helper builds an actual variable native face, verifies its drawing and
HarfBuzz coordinates, builds/records the real TextLayout, and returns only
the picture. Its Font, Typeface, layout, builder and recorder die before
GPU replay. This is an intentional ownership regression contract.

The passing `text.variable-clone-raster-cache-independence` also draws a
variable font through layout and picture recording, but keeps its source
Font alive while rendering. This runtime difference matches the concrete
ownership gap below; it is not evidence to alter font coordinates or bounds.

The following vendor files were inspected read-only under
`/Users/cloud/project/ValdiNativeRoutes/.tools/vendor/tgfx/`:

1. `src/core/vectors/coregraphics/CGTypeface.cpp`,
   `Typeface::MakeFromData`: creates CFData with
   `CFDataCreateWithBytesNoCopy(..., kCFAllocatorNull)`. The original real
   CGTypeface separately owns the `tgfx::Data` allocation.
2. `src/platform/apple/CTTypeface.mm`, `MakeFromCTFont`: calls
   `CGTypeface::Make(ctFont)` without that data owner.
3. SnapDrawing `Text/Typeface.cpp`, `makeCoreTextRenderingTypeface` and
   `Typeface::Typeface`: prepare a new native CTFont/rendering face for
   optical-size/tracking policy, while retaining the input face separately
   in `_fontDataTypeface`. For the fixture input, that face also carries the
   explicit original-byte-owner chain from the actual variation helper.
4. `src/core/Font.cpp`, `Font::Font`: retains a ScalerContext rather than the
   caller's shared typeface pointer.
5. `CGTypeface.cpp`, `onCreateScalerContext`: creates CGScalerContext using
   the native face's own `weakThis.lock()`. `src/core/ScalerContext.h`
   retains that rendering face. Merely aliasing the input native typeface
   shared pointer with extra source owners cannot preserve those owners
   through this weak-pointer lookup.
6. `src/core/TextBlobBuilder.cpp`, `allocRunInternal`: copies the native Font
   into each RunRecord. Those native Fonts retain the prepared rendering
   face but do not retain SnapDrawing's original `_fontDataTypeface` owner.
7. SnapDrawing `Text/TextLayoutBuilder.cpp`: previously emitted only the
   native blob. When the returned layout and source Fonts died, their
   original byte-owner chain could end while the picture still referenced
   the separately prepared rendering face.
8. `src/core/PictureContext.cpp`, `drawTextBlob`, and
   `src/core/PictureRecords.h`, `DrawTextBlob`: store and replay the actual
   shared TextBlob pointer, preserving its shared ownership control block.

This establishes a production lifetime defect at the layout-to-picture
boundary. The empty coverage is consistent with native access after the
original custom-byte owner is released. No runtime memory probe was run by
this sub-agent, so this is a source-supported diagnosis rather than a claim
to have observed a particular freed-memory read.

## Minimal owned-source correction

Only production file `Text/TextLayoutBuilder.cpp` changes.
Under `SNAP_DRAWING_TGFX_EXPERIMENT && __APPLE__`, collect each distinct
actual run `Valdi::Ref<Font>` while emitting its glyphs. Once the real native
blob is built, return a standard aliasing `std::shared_ptr<tgfx::TextBlob>`
whose owner contains those Fonts and the actual blob. The pointer still
targets the real TGFX object. Its original blob owner is destroyed before
the retained Fonts, preserving the backing data through blob destruction.
The picture recorder already copies this exact shared pointer, so the
Font/Typeface/custom-byte chain survives layout release and picture replay.

No typeface/scaler native ABI is replaced, and no private vendor field is
modified. There are no public getter or signature changes. Glyph IDs,
positions, native variation/scaler instances, HarfBuzz policy, metrics,
rounding, layout bounds and every test assertion remain unchanged. Default
Skia and TGFX FreeType behavior are unchanged. Native FreeType already owns
its clone data and is outside this Apple-specific ownership correction.
The retained Fonts may also retain their normal resolver while their blob
lives; no global font registry, cache or unbounded lifetime is added.

## Preserved evidence, count and next native check

Before the edit, every payload of `composition-shadow-source-freeze.json`
was verified and copied to
`/tmp/valdi-tgfx-full-20261104/text-run5-preserved/`, together with that
manifest and the prior TextLayoutBuilder/Typeface sources. The archive's
`receipt.json` records paths, sizes and SHA-256 hashes. Sources are saved
with `.cpp.txt` / `.hpp.txt` suffixes.

The prior 32-case `text_checks.cpp` remains exactly
SHA-256 `c0fbd8f87c8559e86028692bfcdc6da3a0e81b0920efbcc30a538ea420689a80`.
No fixture, case registration, coverage assertion, numeric assertion, font
or collection bytes were changed.

The parent should rebuild its existing runner from the new source and run
the same 32 cases. The decisive checks remain the existing three axis
coverage cases: actual nonempty/differing alpha after source owners die,
plus greater integrated coverage for the heavier weight. The existing
clone/cache, CoreText coordinates, collection and shadow cases remain
regression controls. No new helper, launcher, runner or build-rule change
is required. Mobile acceptance remains the parent's responsibility.

This correction concerns the recorded blob's backing-data ownership.
The parent/GPU owner separately owns asynchronous GPU work lifetime and
error propagation. This source-only work does not certify those paths or
claim to resolve the separately reported GPU/media failures.
