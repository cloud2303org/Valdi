# Full TGFX text fixture and source-only checks

`RobotoFlex.ttf` is an unmodified copy downloaded by the parent from
`googlefonts/roboto-flex` at commit
`739e06dc46ebb14cddd88b9768a6c1504d4677f6`. Its upstream filename is:

```text
fonts/RobotoFlex[GRAD,XOPQ,XTRA,YOPQ,YTAS,YTDE,YTFI,YTLC,YTUC,opsz,slnt,wdth,wght].ttf
```

The font is copyright 2011 The Roboto Flex Project Authors and is distributed
under the SIL Open Font License 1.1. `OFL.txt` is the unmodified upstream
license. The local font filename was shortened; the font itself was not
modified or subsetted.

| Local file | Bytes | SHA-256 |
| --- | ---: | --- |
| `RobotoFlex.ttf` | 1775480 | `94a7ea95ccee28c54885a507e3cc0a534ce41ec61d413935df0e07261a7ffe63` |
| `OFL.txt` | 4395 | `454ec26838f2b8f2e2588e759dd3742577e0d305d3359a7b81c86641fcab7d21` |

`receipt.json` and `listing.json` are copied unchanged from the parent's
download receipt and repository listing. They contain exact upstream URLs,
commit, font Git blob ID and checksums. Both copied payload hashes were
verified against that receipt during source preparation; no network or
native command was used by the text-check implementation agent.

The real font has 13 `fvar` axes, `gvar`, `HVAR`, `GSUB` and `GPOS` tables.
The cases select in-range coordinates from three independent axes:

| Axis | Fixture min/default/max | Selected low/high |
| --- | --- | --- |
| `wght` | 100 / 400 / 1000 | 100 / 900 |
| `wdth` | 25 / 100 / 151 | 50 / 150 |
| `opsz` | 8 / 14 / 144 | 9 / 100 |

## Real collection fixture

`NotoSans-Regular.ttf` and `NotoSans-LICENSE.txt` are copied unchanged from
the existing media fixtures, whose font is the repository's
`snap_drawing/testdata/NotoSans-Regular.ttf`. It is copyright 2018 The Noto
Project Authors, under SIL OFL 1.1. Its SHA-256 is
`b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5`
(569208 bytes).

`generate_collection.py` uses only Python's standard library to pack complete
Roboto Flex (face 0) and Noto Sans (face 1) into the real version-1 collection
`selected-face.ttc`. It preserves all font-program tables, relocates table
directory offsets, zeroes `head.checkSumAdjustment` per collection convention,
and recalculates table checksums. It does not invent glyphs or subset fonts.
Both original font licenses accompany the collection. The collection is
2344708 bytes, SHA-256
`30af1141f412f2490e3f724856223b5a6e0040ca9e6d28f8f667dc216e51c725`.

`collection-manifest.json` records source/output hashes, directory offsets
and independently parsed source-cmap glyph IDs. U+0100 and U+03A9 are 191
and 659 in face 0, versus 194 and 373 in selected face 1.
`collection-font.json` is a suite-authored MIT-licensed Lottie fixture
rendering `HgQ` followed by those two characters through the actual font
provider. No font-rendering tool was used to generate this data.

## Parent integration

Compile `../text_checks.cpp` into the parent's actual complete backend runner,
include `../text_checks.hpp`, and call:

```cpp
auto report = runTextChecks(textDataDirectory);
// Print report.details and make the process fail unless report.success.
```

Pass this `text-data` directory itself, not the test directory or repository
root. Copy the licensed fixture unchanged to an app-readable directory on
mobile. No file in this contribution changes the runner or build rules.
Compilation and runtime execution remain the parent's responsibility.

Both `SNAP_DRAWING_TGFX_EXPERIMENT` and `SNAP_DRAWING_TGFX_COMPLETE` must be
defined. Link real SnapDrawing Text/font/layout, HarfBuzz + ICU, DrawingContext
and TGFX GPU implementations. On Android/other FreeType targets, the linked
TGFX library must contain the variable-font extension recorded in
`../../patches/tgfx-variable-fonts.patch`; an old archive is insufficient.
Apple uses `CTFontCreateCopyWithAttributes`, actual variation dictionaries,
and `CTTypeface::MakeFromCTFont`, rather than invoking the unsupported base
TGFX variation virtual on its CoreText backend.
The collection case uses the plural CoreText data descriptor-array API
(macOS 10.13+/iOS 11+), groups named-instance descriptors by complete native
cmap tables and distinguishing glyph IDs, and requires both backing faces
before selecting the requested identity. It reports both data-factory and
URL-factory counts and glyph identities for diagnosis; URL enumeration is not
a success fallback for rejected collection data. The singular data factory
returns only the first TTC/OTC font, per the official local Xcode SDK's
[CTFontManager.h:96](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFontManager.h:96).
The plural contract is documented at
[CTFontManager.h:108](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFontManager.h:108).
It does not claim that the current
TGFX Apple `MakeFromBytes` implements `ttcIndex` (it ignores that argument).
The ThorVG case additionally links the production Lottie/ThorvgMedia backend;
it does not initialize a separate ThorVG runtime or duplicate its resolver.

The Apple helper explicitly retains the original real TGFX byte-backed face
alongside the cloned real CTTypeface, using normal shared ownership. The
public CTFont factory retains the native font but does not copy the original
TGFX stream owner into the clone. FreeType instances exercise their native
clone-data ownership without this Apple bridge. No stand-in font or ABI cast
is used.

## Exactly 32 named cases

1. `text.fixture-real-variable-tables`: pinned size, real glyphs, all 13 axes,
   actual variable/layout tables and fixture axis ranges.
2. `text.system-enumeration-default-metrics`: nonempty native enumeration,
   finite positive default metrics and real default-font Latin shaping.
3. `text.variable-coordinate-readback.wght`
4. `text.variable-coordinate-readback.wdth`
5. `text.variable-coordinate-readback.opsz`

   Each readback case checks both selected values on the actual drawing
   NativeFont after production preparation, checks the sized HarfBuzz font's
   design coordinates, and requires distinct font/cache IDs.

6. `text.variable-harfbuzz-native-advance-and-coordinates`: a combined
   weight/width/optical instance reaches drawing and HarfBuzz. The actual HB
   advance callback and production single-glyph shaping must equal the
   native instance's nominal advance rounded before shaping.
7. `text.variable-width-shaped-advance`: the wide instance must produce
   greater real shaped advance than the narrow instance.
8. `text.variable-layout-gpu-coverage.wght`
9. `text.variable-layout-gpu-coverage.wdth`
10. `text.variable-layout-gpu-coverage.opsz`

    Production `TextLayoutBuilder` emits the actual positioned native blobs.
    Production DrawingContext records them. After source font, layout and
    recorder owners die, real GPU picture replay must produce nonempty,
    differing alpha coverage for each axis. Heavy weight must increase
    integrated coverage. There are no absolute device-font pixel goldens.

11. `text.variable-clone-independence-and-shaper-cache`: a sibling weight
    instance preserves its inherited width. The source's drawing/HB
    coordinates and cold/warm shaped glyphs stay unchanged.
12. `text.variable-clone-raster-cache-independence`: render a source, render
    a heavy sibling on the same real GPU context, then rebuild/render the
    source. Sibling coverage differs; source RGBA remains exactly identical.
13. `text.custom-borrowed-bytes-lazy-load-and-export-lifetime`: register an
    ownerless borrowed buffer through FontManager, overwrite and destroy it
    before the first lazy load, shape successfully, then export exact owned
    original bytes that survive the font. The caller's manager reference
    leaves scope; a Font may retain its resolver normally.
14. `text.custom-registered-fallback-precedes-system`: a covering registered
    custom fallback wins the public compatible-font lookup ahead of native
    candidates.
15. `text.icu-bidi-segments-and-control-exclusion`: mixed Latin/Arabic with
    explicit embedding controls produces both directions; every non-control
    character belongs to exactly one valid shapeable range and controls
    belong to none.
16. `text.arabic-system-fallback-and-real-joining`: the custom Latin primary
    lacks Arabic, native `ar` fallback covers the sample, and actual RTL
    shaping performs contextual joining instead of reversed nominal glyphs.
17. `text.word-cache-cold-warm-and-clear-contract`: actual variable-font
    glyph IDs, advances, offsets and character flags agree between cached
    and uncached default-spacing shaping before/after cache clear.
18. `text.emoji-native-coverage-shaping-and-color-gpu`: real native emoji
    coverage and shaping for U+1F60A, then actual layout/blob picture replay
    must yield nonempty colored pixels rather than monochrome/tofu.
19. `text.collection-selected-face-standalone-sfnt-and-checksums`: load both
    real collection faces, require the fixed distinguishing glyph IDs for
    face 1, export a standalone SFNT, compare every selected-native table
    payload (except the relocated head checksum field), validate every
    directory checksum and whole-font `0xB1B0AFBA` sum, and reopen through
    actual TGFX. FreeType's underlying raw stream must still be a TTC.
20. `text.collection-selected-face-thorvg-font-load-and-glyphs`: register
    the exported selected SFNT through real FontManager, require its selected
    glyph IDs, resolve/load through actual Lottie/ThorvgMedia, and compare
    rendered glyph bounds and stable alpha interiors against real native
    selected-face rendering. Antialiasing edges can differ; bounds allow
    three pixels and stable interiors must match at least 80% in both
    directions. No platform-font golden is involved.
21. `text.shadow-device-radius-1x`: real TextLayer records a single pinned-font
    glyph with sigma 3 and with sigma 0. The added alpha-distribution variance
    must reflect a device-space blur instead of no blur.
22. `text.shadow-device-radius-picture-replay-2x`: replay the same real
    TextLayer pictures at 1x, 2x and again 1x; device blur variance stays fixed.
23. `text.shadow-device-radius-display-scale-2x`: actual Resources display
    scale 2, native font scaling and TextLayer's inverse recording transform
    retain the same device blur at 1x/2x playback.
24. `text.shadow-playback-clip-preserves-interior-halo`: a playback clip
    through the glyph removes outside pixels while retaining the complete
    blurred image inside. Source alpha beyond the clip must still contribute.
25. `text.shadow-color-emoji-alpha-silhouette`: an actual color emoji's
    alpha matches the zero-radius white shadow at the same device position;
    emoji RGB cannot leak into that shadow. A rectangle/outline substitute
    cannot satisfy the real-alpha comparison.
26. `text.shadow-color-emoji-device-radius-replay-2x`: real color/bitmap emoji
    alpha retains device sigma during 1x/2x replay of the same captured picture.
27. `text.shadow-direct-nonuniform-device-radius`: visible red foreground
    plus spatially separated blue shadow, with final display-list scale
    `(2,1)` and no opacity group, establishes the direct-route device radius.
28. `text.shadow-picture-opacity-half-nonuniform-device-radius`: the real
    leaf TextLayer has opacity `.5` and both visible draws. Replay the same
    pictures at `(2,1)`, `(1,2)`, then `(2,1)`; require the original radius
    bounds on both axes, agreement with the direct control, and the expected
    sharp-alpha opacity relative to a fully opaque scene.
29. `text.shadow-nested-opacity-offscreens-nonuniform-device-radius`:
    two actual parent Layers with opacity `.75` each and visible peer Layers
    force nested group composition around the leaf picture opacity `.5`.
    Final scale `(2,1)` must retain the direct device radius and the combined
    opacity `.5 * .75 * .75`.
30. `text.shadow-nested-opacity-final-clip-preserves-halo`: one parent group
    plus leaf picture opacity, with a final device clip through the shadow.
    Require zero outside the clip, agreement with the full render inside it
    within two alpha levels, and exactly unchanged shadow pixels when the
    same picture is replayed without that clip afterward.
31. `text.shadow-color-emoji-opacity-half-nonuniform-device-radius`: actual
    visible colored emoji and its separated blue alpha shadow must preserve
    the direct-route device radius under picture opacity `.5`, scale `(2,1)`.
32. `text.shadow-color-emoji-nested-offscreen-nonuniform-device-radius`:
    the actual emoji coverage must retain the same radius through a parent
    opacity group and leaf picture opacity at final scale `(2,1)`.

The twelve shadow cases use public `TextLayer::draw`, real Resources,
DisplayList/DrawableSurfaceCanvas, real GPU surfaces and readback. No
test-side glyph outline/path or fake shadow renderer is used. The TextLayer
and Resources die before replay, so the deferred record must retain its real
blob/font/paint/filter.

The six composition additions preserve every function and assertion of the
previous 26 cases. The leaf's visible foreground and shadow retain multiple
draws in its picture; parent groups contain visible nonoverlapping green peer
Layers. The composition oracle isolates the actual blue shadow in a
geometry-separated region, requires an empty separation band, verifies that
shadow blue equals alpha with no foreground-color leakage, and checks the
visible foreground before any intentionally foreground-removing clip.
Sharp-alpha mass must equal the known combined opacity within
`max(.02, .10 * expectedOpacity)`. The opacity control guards against
optimizations that would ignore or double-apply the intended composition.

The radius oracle uses a convolution invariant: blur adds kernel variance
to the actual glyph alpha variance. Sigma 3 nominally adds 9 device-pixel
squared per axis. The accepted [5,14] range accommodates finite tails and
8-bit alpha quantization while rejecting no blur and the approximately 36
added by stretching a preblurred mask 2x. Added variance across playback
scales may differ by at most 3 per axis. Each comparison uses the matching
actual zero-radius glyph alpha at that geometry; it is not a font pixel
golden or a test-side implementation of the blur kernel.
These unchanged variance bounds also reject the ROUND3 scalar-sampling
failure: at final scale `(2,1)`, a sigma-3 shadow resolved at scalar sample 2
and then compressed vertically has vertical sigma 1.5, nominal added variance
2.25, below the required minimum 5. The composition cases exercise actual
production rendering; they add no scalar compensation or TextLayer workaround.
`COMPOSITION_SHADOW_ROUND3.md` records the required native contract and
source-only boundary.

## Failure and evidence boundaries

Every registered case emits one PASS or FAIL with details. Missing fixture,
variation readback, native fallback/color emoji support, GPU device/lock,
surface or readback fails the dependent case; none becomes a skip/pass.
Cases are independent, so CPU checks still run if GPU setup is unavailable.
The report succeeds only with all 32 passes. Surfaces die before context
unlock, on the calling thread.

The runtime verifies fixture size and font-table contracts; the parent
deployment should preserve the receipt-verified bytes. These bounded checks
do not certify every variation axis, arbitrary TTCs/named instances, invalid-coordinate
policy, every script/emoji sequence, concurrency, or cross-platform pixel
identity. They exercise actual TextLayout blobs and picture replay plus the
twelve real TextLayer shadow cases; they do not certify text gesture/selection
or decoration presentation paths.
No compilation, test execution, device evidence or passing result is claimed
by this source-only contribution.

The parent's root9/run4 execution passed 17/20 text cases. The original
source/data and hashes are preserved at
`/tmp/valdi-tgfx-full-20261104/text-run4-preserved/`; the active directory's
`run4-preservation-receipt.json` records the archive and hashes.
`CORETEXT_RUN4_DIAGNOSIS.md` records the subsequent source correction, native
API/source evidence, unchanged TTC hash, and the parent's completed standalone
probe with observed counts and clone coordinates. No complete-suite rerun
result is claimed for those corrections or the six added shadow cases.
The ROUND3 composition additions likewise have no build or runtime result.
The parent's five-profile build predates these six additions and is not
execution evidence. Native execution of the corrected CoreText cases and
all shadow cases remains pending the parent's native safety work.

The prior 26-case freeze remains unchanged in `shadow-source-freeze.json`
(SHA-256 `3329d7db201d55a40d6d9e15d9c85e38ada3d5dd421f70f0366087de8c772a75`).
Its exact seven payloads and the manifest were verified before editing and
copied to `/tmp/valdi-tgfx-full-20261104/text-shadow26-preserved/`.
`composition-shadow-source-freeze.json` records the separate 32-case source
freeze. No TextLayer or vendor source was changed for ROUND3. The public
header is retained byte-for-byte under this round's narrow ownership; its
comment still describes the previous 26-case registration, while the actual
runner success condition and this manifest require 32.

Cached/uncached comparisons assert the public output contract, not internal
hit counters. Fonts whose space-related layout rules require a cache bypass
may take that production path. Collection export preserves selected face
tables; it does not bake arbitrary variable design coordinates into static
outlines for ThorVG.

## Run5 variable-layout lifetime correction

The parent rebuilt root11 and ran the current 32-case suite. Its actual log,
`/tmp/valdi-tgfx-full-20261104/backend-checks-run5.log`, reports 29/32:
only `text.variable-layout-gpu-coverage.wght`, `.wdth`, and `.opsz` failed
with no glyph coverage. The corrected CoreText/clone/collection cases and
all twelve shadow cases passed in that run.

`RUN5_VARIABLE_LAYOUT_LIFETIME.md` records the source trace and narrow
production correction. Apple TGFX layout blobs now retain the distinct
actual run Fonts alongside the real native blob. This carries the original
custom-font backing bytes through picture replay after the source Font and
layout are released. Native glyph IDs, design coordinates, HarfBuzz,
metrics, and rendering APIs are unchanged. The public `NativeTextBlob`
remains a shared pointer to the actual TGFX TextBlob, with normal
`Valdi::Ref<Font>` ownership; no substitute native type is introduced.

The current 32-case implementation, all assertions, licensed fonts and TTC
are unchanged. Before editing, all 20 payloads in the prior composition
freeze were verified and preserved together with the prior production
sources under `/tmp/valdi-tgfx-full-20261104/text-run5-preserved/`.
`run5-text-lifetime-source-freeze.json` records the separate new source
freeze. No compilation or native execution of this correction has occurred
in the text sub-agent; the parent's rerun is required to confirm the result.
