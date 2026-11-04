# Preserved Android Skia text-layout failure

These are byte-for-byte copies made before changing the text fixture.
The report was produced by the parent's runtime probe; this child did not
compile, launch or execute the benchmark.

Original report:
`/tmp/valdi-tgfx-performance-20261104/trials/native/android/skia-probe/report.json`.

```text
render_performance.cpp.before
bytes: 24365
SHA-256: 2b941a613099fc93d6a717b4b100cba0754afffa2180207757c81cf71b21c4da

report.json
bytes: 15380
SHA-256: 4ad08176bb5b47a63d80e3b02c8e66e07a8a01d129e3e44f8b7e8d75f7891abb
```

The report identifies Skia/OpenGL ES, optimized native code with `ndebug=true`,
and overall `success=false`, with 4/5 scenes completed successfully. The text
scene failed preparation with `Production text layout is empty or does not fit`,
zero warmup/measured frames, no completed canaries, and null timing aggregates.
Other scene samples remain preserved in the complete JSON. This failed run is
diagnostic evidence, not a complete five-scene comparison baseline.

## Source-confirmed cause

The failed fixture appended `SnapDrawing GPU 0123456789\n` twenty times,
requesting an unintended empty twenty-first line.

In production `TextLayoutBuilder.cpp`:

- `lineBreak()` reports a consumed newline as `reachedEndLine=true` and
  `brokenByNewLineCharacter=true`.
- `processUnidirectionalRun()` advances the vertical position and increments
  the line count, then calls `computeLineMetrics()` for the next line even if
  that newline was the final character.
- `computeLineMetrics()` requires position plus line height to be at most the
  maximum height. Its failure calls `processEllipsis()`, which sets
  `_reachedMaxLines` even for `TextOverflowClip`.
- `build()` returns `fitsInMaxSize = !_reachedMaxLines`; trailing newline
  whitespace also participates in the recorded layout bounds.

`TextLayoutLineHeight::multiple(1.2f)` scales native ascent/descent, rather than
simply multiplying font size. The unchanged Noto Sans SFNT tables contain
1000 units/em and ascent/descent 1069/-293 in both `hhea` and OS/2 typo metrics.
The pinned Skia and TGFX scalable FreeType metric paths use unrounded design
ascent/descent scaled to font size. At size 18, the source/table calculation is:

```text
line height = (1069 - (-293)) / 1000 * 18 * 1.2 = 29.4192 px
20 lines = 588.384 px <= 608 px
21 lines = 617.8032 px > 608 px
```

These are source/table-derived values, not metrics captured from the failed
runtime report, which did not yet include native layout details.

## Narrow correction and freeze

The corrected fixture adds newlines only between its twenty unchanged text
lines: nineteen separators, 539 ASCII bytes, no trailing newline. Font size 18,
scale 1, geometry 328x608, maximum line count 24, line-height multiplier 1.2,
fit predicate, text blobs, rendering, canaries and 30/300 timing are unchanged.

The failure branch now reports actual native font size/scale/ascent/descent/
line spacing, derived layout line height, input newline count, layout fit,
bounds, maximum size, entry count and segment count. It still fails on the
same null-layout, failed-fit or empty-width predicate.

Corrected `render_performance.cpp` SHA-256:
`4783124669e0abeded5e20205879ecfd7ddc3a4db6a913a4397052cd56abe414`.

Source comparison against the preserved snapshot confirms that changes are
confined to text construction and its failure diagnostic. No compiler,
configure, device, native execution or child delegation was used. The source
is refrozen for parent rebuild and runtime validation.
