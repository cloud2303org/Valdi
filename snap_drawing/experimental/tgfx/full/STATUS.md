# Complete backend integration

This is active work on `experiment/tgfx-prototype-20261104`. The application
and framework main checkouts still use their existing revisions. The earlier
16-case vector results do not validate this expanded source tree.

The root selection is `--define=snap_drawing_backend=tgfx`. It builds actual
SnapDrawing and Valdi source and exports both backend definitions to consumers.
The selected backend must not link the Skia renderer, Ganesh, Skottie, or SkSVG.
TGFX itself still uses its upstream PathKit implementation.

Current validation:

- Actual pinned TGFX native Metal with PNG/JPEG/WebP codecs: macOS archive built.
- Actual pinned ThorVG CPU SVG/Lottie/font/image loaders: macOS archives built.
- Official giflib source: macOS archive built.
- Complete root SnapDrawing and Valdi SnapDrawing source compilation: passed.
- Full test-executable linking: correcting bundled WebP symbol collisions.
- Expanded media/effects/text/composition regression execution: pending.
- Expanded iOS/Android runtime execution: pending.
- Independent full-backend review: pending.

Parent build evidence is preserved at `/tmp/valdi-tgfx-full-20261104`.
`.tgfx-deps/macos_arm64` records the actual local imports and hashes.
These are local source-built inputs, not hermetic toolchain attestations.

Known differences requiring explicit validation include 8-bit TGFX GPU targets,
eager animation frame retention, ThorVG versus Skottie/SVG feature coverage,
native shader sampling, mask isolation, blur under later picture transforms,
spatial-index performance, and public persistent shader-cache support.
Variable-font instance support is being completed in the real TGFX/FreeType
implementation rather than substituted with a different shaping instance.
