# CoreText run4 follow-up: source evidence and bounded native probe

The parent executed the real root9 backend on the current host:
17/20 text cases passed. The clone/readback case and both collection cases
failed. The parent subsequently compiled and ran the frozen standalone probe
with exit 0; that diagnostic evidence is distinguished below from execution
of the complete updated text suite.

## Evidence preserved before changes

`/tmp/valdi-tgfx-full-20261104/text-run4-preserved/` contains the parent's compiling source (as `.cpp.txt` and
`.hpp.txt`), TTC, collection manifest, generator, Lottie JSON, and provenance
as used before this follow-up. `preservation-receipt.json` records every
preserved payload's SHA-256 and hashes of the unchanged source fonts/licenses.
`run4-preservation-receipt.json` in the active fixture directory records the
archive location and copies those preservation hashes; the large historical
TTC and source snapshots are not part of the active app fixture payload.
The source includes the parent's qualified `snap::drawing::Size/Rect/Duration`
and default-metrics syntax corrections. Those corrections are preserved.

The TTC has not been regenerated or replaced. Its SHA-256 remains
`30af1141f412f2490e3f724856223b5a6e0040ca9e6d28f8f667dc216e51c725`.
Source-data inspection found version 1, two in-bounds directories at 20/336,
19/18 sorted table records, valid aligned offsets and all table checksums.
The parent probe below now additionally establishes actual CoreText acceptance.

## Parent's actual native diagnostic

Evidence:
[receipt.json](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/receipt.json) and
[output.log](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/output.log).
The compiled probe source SHA-256 is
`3009bd4824dda0def37ce65b568a19ff072b7e1646c55bc94eb75e654eb59061`.
The implementation agent did not compile or execute it.

- Both plural-data and URL factories return 21 descriptors for the unchanged
  TTC: 20 named Roboto Flex instances with glyph IDs 191/659, followed by
  Noto Sans at descriptor 20 with glyph IDs 194/373.
- Standalone Roboto Flex yields 20 descriptors through either factory;
  standalone Noto Sans yields one. Exactly two backing collection faces
  therefore does not imply exactly two native descriptors.
- The old clone source is valid at weight 350/width 80, including after the
  production-equivalent tracking/optical preparation copy. Its sibling
  has glyph IDs 0/0, no native axes and no variation dictionary
  ([output.log:150](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/output.log:150)).
  This was an invalid clone, not merely an omitted default coordinate.
- The complete-dictionary/intermediate-optical clone retains all 13 axes,
  glyph IDs 191/659, weight 900/width 80, including after preparation.
  The source remains weight 350/width 80
  ([output.log:209](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/output.log:209),
  [output.log:223](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/output.log:223),
  [output.log:237](/tmp/valdi-tgfx-full-20261104/coretext-diagnostic1/output.log:237)).

## Confirmed API-contract differences

The official local Xcode SDK's
[CTFont.h:1220](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFont.h:1220)
states that since macOS 10.12/iOS 10 the dictionary
contains only non-default axis values. `CTFontCopyVariationAxes` provides
supported identifiers and their default values. The fixture now reconstructs
actual design coordinates from both native API results. It still requires
the axis to exist and compares against the same exact requested values.
A lost weight 900 must fail as default 400, rather than pass because the
dictionary was null. HarfBuzz's omitted design coordinates likewise mean
defaults, not unsupported axes.
The declaration is explicitly nullable at
[CTFont.h:1229](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFont.h:1229);
native axis metadata must still establish that variations and the requested
axis exist. No unreadable web body is used as evidence.

The actual baseline Skia source
`src/ports/SkTypeface_mac_ct.cpp`, functions
`ctvariation_from_SkFontArguments` and `SkTypeface_Mac::onMakeClone`,
builds a complete axis dictionary. It documents a CoreText defect when
copying an already-variable font while leaving optical size unchanged,
including fonts created from data. Skia first makes a copy with a different
valid optical size, then applies the complete desired variation. The
fixture's native clone helper now follows that sequence and preserves the
actual inherited coordinates. Clone independence still requires weight
350/width 80 on the source before and after the sibling, weight 900/width
80 on the sibling, identical source cold/warm shaped glyphs, and the
existing independent raster-cache assertions.

The official local
[CTFontManager.h:96](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFontManager.h:96)
explicitly limits the singular data factory to the first TTC/OTC font.
The plural data factory at
[CTFontManager.h:108](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFontManager.h:108)
returns the font descriptor array and permits an empty array for invalid or
unsupported input. The URL-factory documentation at
[CTFontManager.h:78](/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/CoreText.framework/Headers/CTFontManager.h:78)
documents
`CTFontManagerCreateFontDescriptorsFromURL` as enumerating fonts in the
given font file. It does not establish descriptor-array ordering as a TTC
face-index API. The former fixture assumed exactly two descriptors and
indexed the data-factory array directly. This font also has 20 named
variation instances; the old failure message did not expose actual array
counts. The parent probe now confirms named-instance enumeration, not
invalid TTC bytes or data-factory rejection, as the collection failure cause.

The helper retains the plural data factory and requires both distinct
backing-face identities, established by their complete independent cmap
tables plus distinguishing glyph IDs. It accepts named-instance descriptors
and selects the requested backing identity, so descriptor index 1 cannot be
mistaken for TTC face 1. URL enumeration remains diagnostic only. All selected
native-table, checksum, glyph, reopen and ThorVG assertions remain intact.
The TTC itself is unchanged.

## Preserved probe recipe

Use the same preserved TTC path, plus unchanged RobotoFlex.ttf and
NotoSans-Regular.ttf as controls. No font registration is needed.
`coretext_run4_probe.cpp.txt` is prepared standalone C++ diagnostic source for
the parent's native slot. It depends only on CoreText/CoreFoundation/
CoreGraphics and the standard library; its argument is the absolute active
text-data directory. It is not a `.cpp` runner source, does not modify data or
register fonts, and its zero exit code is not a backend-test PASS.

1. For each of these three paths, call both
   `CTFontManagerCreateFontDescriptorsFromData` on its exact bytes and
   `CTFontManagerCreateFontDescriptorsFromURL` on its file URL. Print null
   versus array count. For each returned descriptor, instantiate
   `CTFontCreateWithFontDescriptor`; print PostScript name and actual glyph
   IDs for U+0100/U+03A9. This distinguishes no accepted face from multiple
   descriptors/named instances and verifies the URL path.
2. Using unchanged Roboto Flex, capture `CTFontCopyVariationAxes`,
   `CTFontCopyVariation` and `CFEqual` results for the old helper sequence:
   raw font -> `{wght=350, wdth=80}` -> sibling changing only `wght=900`.
   Also capture source/raw and production-prepared drawing handles before
   and after copying. Then compare with the complete-dictionary,
   intermediate-optical-size sequence in corrected `vary()`.
3. If the instrumented fixture helper is compiled for this probe, the existing
   `runTextChecks` report now includes both data/URL descriptor counts and
   each data/URL descriptor's two glyph IDs. The clone case labels its native/HB
   checks as source-before, sibling-after/inherited, and source-after.
   Counts remain diagnostic; selected identity and numeric assertions decide
   PASS/FAIL.

These diagnostic results explain the original three failures. They are not
a passing result for the complete updated text suite, which also includes
the separately prepared deferred TextLayer shadow cases.
