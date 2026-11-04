# ROUND3 TextLayer opacity and offscreen shadow fixtures

Source-only preparation on 2026-11-04. The suite now registers 32 cases:
the prior 26 are unchanged and six composition regressions are appended.
No compiler, test runner, native probe, device, download or network was used
for this follow-up. There is no claim that the remaining opacity route is
fixed. The parent owns compilation and execution after the GPU owner's
native safety and final-transform work is ready.

## Required native contract, via the parent to the GPU owner

The public production route is visible in these actual sources:

- `snap_drawing/src/snap_drawing/cpp/Layers/Layer.cpp`: a leaf's
  `hasOverlappingRendering()` is false; opacity `.5` is attached to its
  cached picture. A parent with children instead uses context opacity.
- `snap_drawing/src/snap_drawing/cpp/Drawing/DisplayList/DrawDisplayListVisitor.cpp`:
  picture opacity is passed as paint to `Canvas::drawPicture`; context
  opacity uses `Canvas::saveLayer`.
- `snap_drawing/src/snap_drawing/cpp/Layers/TextLayer.cpp`:
  `drawTextShadowCoverage` records immutable actual blob/paint/filter data
  through `Canvas::drawDeferred`. Its callback reads the playback canvas
  matrix and total clip, expands contributing source alpha beyond the clip,
  and resolves actual glyph coverage and blur in device coordinates.
- Native source inspected read-only at
  `/Users/cloud/project/ValdiNativeRoutes/.tools/vendor/tgfx/src/core/images/PictureImage.cpp`:
  `asFragmentProcessor` uses scalar `args.drawScale` to construct the
  offscreen `extraMatrix`, while `drawPicture` creates a new RenderContext
  and plays back the picture. That scalar alone does not carry the complete
  final nonuniform transform.

The native implementation must preserve the complete final transform and
consistent device-coordinate matrix/clip semantics through picture-paint
opacity and nested group offscreens. Scalar raster sampling must not become
the metric for `respectCTM=false` device-radius blur. Matrix resets used by
the deferred coverage callback and final clip handling must remain correct
inside nested offscreens, including subsequent reuse of the same pictures
under a different final transform or clip. Clip rejection must retain source
glyph alpha outside the destination clip that contributes to the interior
blur halo. This describes observable behavior; it does not prescribe a
vendor storage layout or add a new production API.

No vendor edit or TextLayer scalar workaround is included. The GPU owner
owns that implementation. These cases use the existing public contracts
and require no new production getter.

## Six added cases

1. `text.shadow-direct-nonuniform-device-radius`
2. `text.shadow-picture-opacity-half-nonuniform-device-radius`
3. `text.shadow-nested-opacity-offscreens-nonuniform-device-radius`
4. `text.shadow-nested-opacity-final-clip-preserves-halo`
5. `text.shadow-color-emoji-opacity-half-nonuniform-device-radius`
6. `text.shadow-color-emoji-nested-offscreen-nonuniform-device-radius`

All scenes come from actual `TextLayer::draw` into a real DisplayList, then
GPU replay/readback through DrawableSurfaceCanvas. Foreground text is visible;
its blue shadow is offset far enough to measure the actual shadow separately.
The real pinned Roboto Flex glyph is `H`. The emoji cases use native color
U+1F60A; they require visibly colored foreground rather than accepting tofu
or a monochrome replacement. Coverage is not synthesized from outlines.
The prior emoji silhouette assertion remains unchanged in the original 26.
Scene Layers and Resources die before picture replay.

Each real parent opacity group contains both the content and a visible
nonoverlapping peer Layer to exercise composition. The leaf has picture
opacity `.5`; each enclosing group has opacity `.75`. The fixture compares
sharp-shadow integrated alpha to a fully opaque control and checks the
known opacity product. This makes missing or duplicate opacity a failure.
The direct case uses visible foreground and shadow with opacity 1 and no
group, providing a separate control for the composition routes.

The existing alpha-moment oracle is reused unchanged: sigma 3 must add
variance in [5,14] device-pixel squared independently on both axes; routes
and replay scales may differ by at most 3 per axis. These bounds reject the
reported scalar sample 2 error under final scale `(2,1)`, whose vertical
sigma becomes 1.5 and nominal added variance 2.25. Each comparison subtracts
matching real sharp glyph variance. It does not use a device-font golden.
The same opacity pictures are replayed `(2,1) -> (1,2) -> (2,1)`.

The clip case uses one actual group plus leaf picture opacity and final
scale `(2,1)`. A final device clip cuts through the sharp shadow's measured
center. Outside alpha must be zero, inside alpha must match the complete
render within two levels, and an unclipped replay after the clipped replay
must restore exactly the original shadow pixels. The full-frame foreground
positive control runs before the clip deliberately removes that foreground.

## Freeze and remaining boundaries

`shadow-source-freeze.json` remains the unchanged 26-case manifest. Its exact
payload snapshot is at
`/tmp/valdi-tgfx-full-20261104/text-shadow26-preserved/`; source snapshots use
`.cpp.txt` and `.hpp.txt` suffixes so they are not accidental build inputs.
The real TTC bytes and all original licensed fonts are unchanged.
`composition-shadow-source-freeze.json` records the new source hashes,
32-case count, and preservation checks.

The current native result for these six additions is unexecuted. The earlier
CoreText corrections also still require a complete-suite rerun; the actual
standalone probe is documented separately in `CORETEXT_RUN4_DIAGNOSIS.md`.
No missing emoji/font/GPU support is counted as a skip or pass.

This bounded addition covers axis-aligned nonuniform scaling, picture/group
opacity, reuse and a final rectangular clip. It does not certify rotation,
shear, perspective, every blend mode, every clip shape, all emoji formats,
or every possible offscreen nesting pattern. Parent integration and the
public report signature are unchanged. The header comment still mentions
the previous 26 cases because this round owns only implementation/data and
conditional TextLayer edits; it is not used to determine success.
