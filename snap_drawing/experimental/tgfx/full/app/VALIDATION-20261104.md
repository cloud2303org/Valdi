# Valdi / Hermes / TGFX application validation

Validated on November 4, 2026, in the isolated experimental worktree. This is
application execution evidence, in addition to the earlier native-suite shells.
The application and framework main checkouts and their pins remain unchanged.

## Results

| Target | Actual application result | Replay |
| --- | --- | --- |
| iOS arm64 simulator, iOS 26.5, UIKit root | 9/9 scenes, 86 assertions | 4/4; no warnings |
| Android arm64 emulator, API 36, native root | 9/9 scenes, 86 assertions | 4/4; one idle warning |
| Android arm64 emulator, API 36, SnapDrawing root | 9/9 scenes, 86 assertions, SurfaceView PixelCopy control | 4/4; one idle warning |

All three runs verified the executing Hermes runtime, OSS release
`260318099.0.4`, bytecode version 99. Both application and drawing modules were
compiled again with the corresponding exported-runtime receipt; unbound compiler
diagnostic archives were not reused.

The scenes exercise composition, clipping, a geometric path, two distinct label
strings, byte-backed and catalog-backed bitmaps, SVG, and Lottie at two fixed
times. Each scene saves a real 160x120 PNG and checks content plus native PNG
decode/round-trip results. Every image case requires asset tracking, a successful
load callback, and no asset errors.

The Android root presentation control additionally uses the platform PixelCopy
API on the real SurfaceView. The recorded 1080x1164 image contains 1,230,606
opaque near-white pixels and 16,303 opaque blue pixels. The full image and counts
were persisted before PASS. This catches the observed black-surface failure;
it is a content control, not exact onscreen parity for all nine scenes. The
capture can precede the final UI status update.

The Android replay warnings report a small changing screen region during idle.
The terminal PASS checks, completed application reports and PixelCopy content
gate passed independently. Screenshot inspection found the rendered preview
and no loading spinner; no claim of strict screen stillness is made.

## Integration failures repaired

- Canonicalized the staged Node tool path. The original tools symlink caused
  tsx's resolved entry point to differ from the runtime compiler's direct-entry
  check, so the CLI exited successfully without producing Jasmine outputs.
  A negative entry-point probe and the real export confirmed the correction.
- Deduplicated canonical public-header Labels when explicitly exporting drawing.
  The existing drawing API header target remains included.
- Supplied explicit intrinsic dimensions on the acceptance SVG. Its original
  viewBox-only geometry cannot enter the static loader without output dimensions.
  Only root width/height were added; geometry, pixel assertions and tolerances
  were preserved. Production viewBox-only raster behavior was retained.
- Registered generated bindings through AGP's built-in Kotlin source directories.
- Deferred Android EGL connection until `prepareCanvas()`. Surface creation and
  change callbacks can report the same native window; the previous eager
  connection ran before the old drawable was released. This caused
  `already connected` / `EGL_BAD_ALLOC` and a black root despite passing managed
  snapshots. The final root process has no such connection error in the bounded
  log and passes the actual SurfaceView content check.

Independent bounded source reviews found no remaining concrete P1/P2 issue in
these corrections, the explicit Android Activity, or the PixelCopy lifetime and
completion gate. Reviews did not execute the applications.

## Evidence

Machine-readable final index:

`/tmp/valdi-tgfx-full-20261104/app-final-validation.json`

Final application reports and PNGs:

- `/tmp/valdi-tgfx-full-20261104/app-ios-run3/`
- `/tmp/valdi-tgfx-full-20261104/app-android-native-run2/`
- `/tmp/valdi-tgfx-full-20261104/app-android-presented-run2/`

Runtime exports:

- `.local/ios-runtime-export5/ValdiRuntime.xcframework`
- `.local/android-runtime-export2/valdi_runtime.aar`

Bound receipts:

- `.local/ios-runtime-receipt2.json`
- `.local/android-runtime-receipt2.json`

The receipts bind actual platform artifacts, successful export logs, configured
dependency graphs, selected-profile source envelopes and bounded async-safety
review evidence. Both configured graphs reject the Skia renderer dependency;
TGFX's PathKit geometry dependency still contains Skia-derived code.

Replay files are under `.argent/e2e/`. The Android presented test is a fragment
whose prerequisite is installation of the reviewed APK. It restarts the explicit
`SnapDrawingAcceptanceActivity`; the default launcher uses the native root.
No simulator servers were stopped.

Earlier native suites remain separate evidence: both platforms passed 260/260
(97 effects, 99 media, 32 text, 32 GPU) and clean 4/4 shell replays. Those shells
do not start Hermes. The later Android-only window-lifecycle correction was
verified by the actual application rather than inferred from those suites.

## Bounds

The host applications are Debug builds; their Valdi modules use Release Hermes
bytecode. Physical devices, performance/energy benchmarks, release-size
comparisons, iOS whole-screen TGFX presentation, and production-app rollout
remain outside this validation. The Release vendor archives are unsanitized;
native passing tests and source reviews do not establish universal safety or
complete pixel parity with Skia. Lottie remains enabled through the real ThorVG
integration.
