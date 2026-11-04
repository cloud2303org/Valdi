# Full-backend effects pixel checks

`effects_checks.cpp` is source preparation for the parent-owned full backend
target. It has not been compiled or executed as part of this task. The existing
16-check `../../vector_checks.cpp` fixture is unchanged.

The callable interface is:

```cpp
#include "effects_checks.hpp"

auto report = runEffectsChecks();
// Print report.details; the process must fail unless report.success is true.
// report.checks contains each name, pass/fail status and failure diagnostic.
```

The parent supplies the executable/target and links the real complete
SnapDrawing backend, its Valdi dependencies and the pinned TGFX library.
Both `SNAP_DRAWING_TGFX_EXPERIMENT` and `SNAP_DRAWING_TGFX_COMPLETE` must be
defined. The fixture supplies no `main`, build configuration, substitute
backend, shader evaluator, path-effect implementation, or platform launcher.

## Execution and reporting contract

`runEffectsChecks()` creates a real `GrGraphicsContext::makeOffscreen()`, obtains
its `TGFXContextLock`, and creates RGBA8 sRGB GPU surfaces while that lock is
alive. It records through the actual `DrawingContext`, destroys the recorder
and scene-local resources, replays the resulting `LayerContent` pictures, and
reads back premultiplied RGBA8 pixels synchronously. Input images are immutable
TGFX pixel images wrapped by the actual Valdi-ref-counted `Image`.

Group, opacity, viewport, clip and mask cases go through the actual
`DisplayList::draw` and `DrawableSurfaceCanvas`; the fixture does not clone the
display-list visitor. `CompositionState` is also exercised against fixed
absolute opacity, transform and clip expectations. Masks are the actual
`PaintMask` and `CompositeMask`, retained through real display-list operations.

The current source registers 82 effects/composition cases plus one backend
setup/readback control (83 results). Each registered case runs independently
after setup. An assertion, exception,
surface failure or readback failure emits `FAIL` with its case name and
diagnostic. Pixel errors include coordinates, actual/expected premultiplied
RGBA, and tolerance. A failed device/lock/setup readback emits a failed setup
check **and a failed result for every dependent case**, explicitly identifying
the prerequisite; unexecuted cases are never counted as passes. There is no
skip, expected-failure, software fallback, or success-on-unsupported path.

The return report exposes `passed`, `failed`, `success`, printable `details`
and all individual `checks`. The caller must use `success` for its exit status,
not merely test whether a report or GPU device exists. All GPU surfaces are
released on the calling thread before its context lock and device.

## Coverage and oracles

- Linear gradients: uneven explicit stops, mismatched-location even spacing,
  duplicate hard stops, partial endpoint stops, all eight orientations,
  unpremultiplied color interpolation with premultiplied readback, canvas
  transforms, rounded drawing, and the degenerate clamp's final color.
- Radial gradients: center and intermediate explicit stops, elliptical local
  transforms, bounds/color/location updates and recorded-shader lifetime.
- Shaders: independent paint color and shader alpha, empty-vs-transparent
  behavior, SrcATop color filters and filter reset, immutable image lifetime,
  clamp behavior, composed local matrices plus a canvas transform, bilinear
  sampling, and the public high-quality Mitchell sampling contract.
- All 31 public `BlendMode` values, including the two marker aliases: the exact
  native enum mapping and **both paint blending and blended shaders** are
  checked. Porter-Duff inputs have different fractional alphas (85 and 170).
  Advanced modes use nonneutral opaque RGB values, including distinct hue,
  saturation, color and luminosity expectations.
- Effects: paint copy construction and assignment, replacement of a previous
  effect, dashed rectangles and fill-vs-stroke behavior, contour phase reset,
  conic dashes, filled circle stamps independent of stroke width, conic
  arc-length placement, cubic and quadratic interiors, multiple contours,
  and an empty dot stamp that must not become an ordinary stroke.
- All four blur styles: soft interior edges, exterior halo vs zero exterior,
  solid/empty interiors, finite support, premultiplied halo color, geometry
  coverage without shader-RGB smearing, copied filters and independent reset,
  the default `respectCTM=true`, explicit `false`, and scaled picture replay.
  Box shadows check offset, doubled blur sigma and bleed beyond recorder
  bounds.
- Composition: overlapping primitives and pictures under one opacity group,
  nested group/picture alpha, clipping and restore, zero opacity, native-scale
  partial viewports, decoration beyond layer bounds, rect/path/composite mask
  bounds, reverse composite unwinding, transformed masks, gradient alpha masks,
  retained mask ownership, and preservation of pixels predating a mask.

Expected pixels are fixed constants, not results obtained from TGFX blend or
sampling routines. For the blend table, translucent destination/source are
unpremultiplied blue/alpha 170 and red/alpha 85. Advanced destination/source are
`(64,128,192)` and `(192,32,64)`. The nonseparable expectations use the public
blend contract's luminosity coefficients `.30/.59/.11`; there is no runtime
implementation of those algorithms in this fixture.

Gradient midpoint samples use analytically chosen pixel-center coordinates.
The conic dot fixture uses the independent quarter-circle half-length
`pi*56/4` and its known 45-degree location; no test-side `PathMeasure` or
polyline approximation supplies expected geometry. Dense cubic/quadratic
stamps must hit their known curve interiors and leave the endpoint chord empty.

Exact solid pixels use zero tolerance; rounding-sensitive colors generally
allow 1–3 byte levels. Blur checks assert coverage ranges and relationships
that distinguish missing blur, wrong style, RGB blur and wrong CTM defaults,
rather than certifying a particular vendor kernel. The default-vs-explicit CTM
comparison examines the full readback. These checks are not a full Skia/TGFX
image-diff suite, a universal antialiasing oracle, font/image-codec/Lottie
coverage, or an application/device result.

## Contract gaps remain failures

These cases deliberately assert the public behavior even where the currently
prepared implementation documents a limitation. They are ordinary checks,
not expected failures, and must remain `FAIL` until the behavior is implemented:

- `shader-image-high-quality-mitchell-center`: at the green texel center of
  red/green/blue/white input, Mitchell's `1/18,8/9,1/18` weights require
  approximately `(14,227,14,255)`. A bilinear fallback gives green and fails.
- `blur-respect-ctm-false-picture-replay`: the same device geometry with
  `respectCTM=false` must keep the same device-space sigma whether scaling is
  applied before recording or during display-list replay. Scaling a
  previously resolved mask fails this invariant.
- `mask-bounded-save-behind-preserves-outside-destination-blends`: DstOver
  outside a mask's saved subset must retain the existing destination. Isolating
  the entire canvas with saveLayer changes that pixel from red to green and
  fails.

No successful result count is claimed here. The parent owns target wiring,
compilation, actual GPU execution, interpretation of failures and any backend
fixes outside these new fixture files.
