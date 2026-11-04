# TGFX variable fonts: source preparation

Date: 2026-11-04. This is an implementation handoff, not build or runtime evidence.

## Source and patch

The actual TGFX checkout is `/Users/cloud/project/ValdiNativeRoutes/.tools/vendor/tgfx`,
at upstream commit `4cf8991d5a3a010c9314538dd2fea937593b206b`.
`patches/tgfx-variable-fonts.patch` contains only the changes to:

- `include/tgfx/core/Typeface.h`
- `src/core/Typeface.cpp`
- `src/core/vectors/freetype/FTTypeface.h`
- `src/core/vectors/freetype/FTTypeface.cpp`

No third-party FreeType library, scaler-context, JNI renderer, CoreText, build configuration, or pin
files were edited. The checkout already had a separate `CMakeLists.txt` change;
it is excluded from this patch.

## API and ownership

`FontVariation` contains an OpenType axis tag and a float design coordinate.
`Typeface::makeWithVariations(coordinates)` returns an immutable clone when the
request is nonempty. Unspecified axes preserve the original instance's values.
An empty request returns the existing shared typeface. Finite values are
clamped to the font's axis range. Unknown axes, non-finite values, or backend
failures return null; no default instance is substituted.

`Typeface::getVariationDesignPosition()` returns the complete applied design
position in font-axis order. The FreeType implementation obtains this position
with `FT_Get_Var_Design_Coordinates`, including named TTC instances. It applies
requested coordinates with `FT_Set_Var_Design_Coordinates` before publishing the
new typeface or creating any scaler contexts.

Every clone owns a separately opened `FT_Face`, a fresh unique ID, and its own
existing TGFX scaler/bounds caches. The original face is never reconfigured.
Font data ownership remains in `FTFontData`; memory clones retain the original
shared TGFX data. Face creation/destruction uses the existing FreeType library
mutex. The variation descriptor and unclaimed faces have scoped cleanup.

The new virtual entries are appended after existing declarations, without
adding base-class fields or moving existing virtual declarations. This does
not establish binary compatibility for calling the new API against an old
archive. Android must be rebuilt from the patched source before using it.
Any other platform that uses these new methods must also use a matching build.

## Text integration

The guarded SnapDrawing Text bridge now:

- Applies configured Android XML/NDK and Linux Fontconfig coordinates using the
  actual clone API instead of accepting only default `fvar` values.
- Resolves Android `supportedAxes` weight/italic instances and caches them by
  requested style.
- Reads the actual instance position into the HarfBuzz main font, so its
  variation-dependent GSUB/GPOS and TGFX's glyph advances/outlines refer to the
  same native instance.
- Derives numeric style information from actual `wght`, `wdth`, `ital`, and
  `slnt` coordinates rather than the base font's OS/2 defaults.
- Supports variable custom-font style changes for available style axes.
- Stops selection when a specifically requested variable instance cannot be
  constructed, instead of silently substituting its base/default face.

The existing Apple CoreText selection and variation-coordinate path is
unchanged. Calls to the new TGFX methods in Text are excluded on Apple. This
follow-up does not require edits to Apple's CoreText implementation.

## Android color-vector rendering

TGFX's existing Android color-vector renderer draws through a Java `Typeface`.
The FTTypeface implementation now builds that handle with the same applied
axis array and TTC collection index as the native face:

- File-backed variable color fonts use public `Typeface.Builder` APIs, available
  from API 26.
- Memory-backed variable color fonts use the public API 29 `Font.Builder`,
  `FontFamily.Builder`, and `Typeface.CustomFallbackBuilder` path. A Java-owned
  direct buffer holds a copy of the custom bytes; no temporary native allocation
  is borrowed.
- Named-instance bits are represented by the complete coordinate array; Java
  receives only the TTC collection index.

If a matching Java handle cannot be constructed, the variable color font fails
explicitly with a TGFX error. It is never rendered through a null/default Java
typeface. Ordinary FreeType outline variation support does not depend on these
Java builders. The older-platform boundary for memory-backed variable color
fonts is explicit; no private Android API or temporary font file is used.

## Strict compile fixes from the parent log

The Text errors reported in
`/tmp/valdi-tgfx-full-20261104/root-build-attempt2.log` were corrected in source:

- `Valdi::Ref` tests use explicit `== nullptr` / `!= nullptr` checks.
- The Skia-only underline fallback constant has a backend guard.
- The non-Apple 32-bit font-table reader has a platform guard.
- Invalid system-font configuration diagnostics use `ILogger` directly and
  consume the message even when ordinary logging macros are compiled out.

No warning flags were relaxed.

## Parent validation and artifact work

No build, configure, compiler, linker, test suite, download, or device commands
were run for this follow-up. Source diff/whitespace and patch consistency checks
are the only local verification.

The parent owns rebuilding the Android TGFX archive, staging its matching public
headers, and refreshing every affected profile manifest/archive hash. The
existing archive predates this extension and is not evidence for it.

Required validation includes:

1. Real variable-font fixtures for `wght`, `wdth`, `ital`/`slnt`, and another
   arbitrary axis; simultaneous sizes and two different live instances.
2. Partial-coordinate cloning, named TTC instances, default coordinates,
   clamping, duplicate tags, unknown tags, and NaN/infinity rejection.
3. Actual coordinates versus HarfBuzz variation state, shaped advances,
   rendered outlines, and metrics; unchanged base instance after cloning.
4. Android XML axes, `supportedAxes`, NDK fallback/locale selection, custom-byte
   lifetime after registration, and style/cache identity.
5. File- and memory-backed variable color fonts through the Java renderer,
   including TTC index and lifetime after native input release.
6. Default Skia regressions and the parent's strict Apple whole-module retry.
