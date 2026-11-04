//
//  Harfbuzz.cpp
//  snap_drawing-macos
//
//  Created by Simon Corsin on 10/18/22.
//

#include "snap_drawing/cpp/Text/Harfbuzz.hpp"
#include "valdi_core/cpp/Text/CharacterSet.hpp"

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkTypeface.h"
#include "include/private/base/SkTemplates.h"
#else
#include <cmath>
#include <limits>
#if defined(__APPLE__)
#include "tgfx/platform/apple/CTTypeface.h"
#endif
#endif

#include "hb-ot.h"
#include "hb.h"

namespace snap::drawing {

HBBlob::HBBlob(HBBlob&& other) noexcept : HBResource<hb_blob_t>(other._ptr) {
    other._ptr = nullptr;
}

HBBlob::~HBBlob() {
    if (_ptr) {
        hb_blob_destroy(_ptr);
    }
}

HBBlob& HBBlob::operator=(HBBlob&& other) noexcept {
    if (_ptr) {
        hb_blob_destroy(_ptr);
        _ptr = nullptr;
    }

    std::swap(_ptr, other._ptr);

    return *this;
}

HBFace::HBFace(HBFace&& other) noexcept : HBResource<hb_face_t>(other._ptr) {
    other._ptr = nullptr;
}

HBFace::~HBFace() {
    if (_ptr) {
        hb_face_destroy(_ptr);
    }
}

HBFace& HBFace::operator=(HBFace&& other) noexcept {
    if (_ptr) {
        hb_face_destroy(_ptr);
        _ptr = nullptr;
    }

    std::swap(_ptr, other._ptr);

    return *this;
}

static size_t countRanges(hb_set_t* set) {
    hb_codepoint_t first = 0;
    hb_codepoint_t last = HB_SET_VALUE_INVALID;
    size_t rangesCount = 0;

    while (hb_set_next_range(set, &first, &last) != 0) {
        rangesCount++;
    }

    return rangesCount;
}

Ref<Valdi::CharacterSet> HBFace::getCharacters() {
    auto* set = hb_set_create();
    hb_face_collect_unicodes(_ptr, set);

    std::vector<Valdi::CharacterRange> ranges;
    ranges.reserve(countRanges(set));

    hb_codepoint_t first = 0;
    hb_codepoint_t last = HB_SET_VALUE_INVALID;

    while (hb_set_next_range(set, &first, &last) != 0) {
        ranges.emplace_back(static_cast<uint32_t>(first), static_cast<uint32_t>(last));
    }

    hb_set_destroy(set);

    return Valdi::CharacterSet::make(ranges.data(), ranges.size());
}

HBFont::HBFont(HBFont&& other) noexcept : HBResource<hb_font_t>(other._ptr) {
    other._ptr = nullptr;
}

HBFont::~HBFont() {
    if (_ptr) {
        hb_font_destroy(_ptr);
    }
}

HBFont& HBFont::operator=(HBFont&& other) noexcept {
    if (_ptr) {
        hb_font_destroy(_ptr);
        _ptr = nullptr;
    }

    std::swap(_ptr, other._ptr);

    return *this;
}

HBBuffer::HBBuffer(HBBuffer&& other) noexcept : HBResource<hb_buffer_t>(other._ptr) {
    other._ptr = nullptr;
}

HBBuffer::~HBBuffer() {
    if (_ptr) {
        hb_buffer_destroy(_ptr);
    }
}

HBBuffer& HBBuffer::operator=(HBBuffer&& other) noexcept {
    if (_ptr) {
        hb_buffer_destroy(_ptr);
        _ptr = nullptr;
    }

    std::swap(_ptr, other._ptr);

    return *this;
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
static hb_position_t sskhbPosition(SkScalar value) {
    // Treat HarfBuzz hb_position_t as 16.16 fixed-point.
    constexpr int kHbPosition1 = 1 << 16;
    return SkScalarRoundToInt(value * kHbPosition1);
}

static hb_bool_t skhbGlyph(hb_font_t* /*hbFont*/,
                           void* fontData,
                           hb_codepoint_t unicode,
                           hb_codepoint_t /*variantionSelector*/,
                           hb_codepoint_t* glyph,
                           void* /*userData*/) {
    SkFont& font = *reinterpret_cast<SkFont*>(fontData);

    *glyph = font.unicharToGlyph(unicode);
    return static_cast<hb_bool_t>(*glyph != 0);
}

static hb_bool_t skhbNominalGlyph(
    hb_font_t* hbFont, void* fontData, hb_codepoint_t unicode, hb_codepoint_t* glyph, void* userData) {
    return skhbGlyph(hbFont, fontData, unicode, 0, glyph, userData);
}

static unsigned skhbNominalGlyphs(hb_font_t* /*hbFont*/,
                                  void* fontData,
                                  unsigned int count,
                                  const hb_codepoint_t* unicodes,
                                  unsigned int unicodeStride,
                                  hb_codepoint_t* glyphs,
                                  unsigned int glyphStride,
                                  void* /*userData*/) {
    SkFont& font = *reinterpret_cast<SkFont*>(fontData);

    // Batch call textToGlyphs since entry cost is not cheap.
    // Copy requred because textToGlyphs is dense and hb is strided.
    skia_private::AutoSTMalloc<256, SkUnichar> unicode(count);
    for (unsigned i = 0; i < count; i++) {
        unicode[i] = *unicodes;
        unicodes = SkTAddOffset<const hb_codepoint_t>(unicodes, unicodeStride);
    }
    skia_private::AutoSTMalloc<256, SkGlyphID> glyph(count);
    font.textToGlyphs(unicode.get(), count * sizeof(SkUnichar), SkTextEncoding::kUTF32, glyph.get(), count);

    // Copy the results back to the sparse array.
    unsigned int done;
    for (done = 0; done < count && glyph[done] != 0; done++) {
        *glyphs = glyph[done];
        glyphs = SkTAddOffset<hb_codepoint_t>(glyphs, glyphStride);
    }
    // return 'done' to allow HarfBuzz to synthesize with NFC and spaces, return 'count' to avoid
    return done;
}

static hb_position_t skhbGlyphHAdvance(hb_font_t* /*hbFont*/,
                                       void* fontData,
                                       hb_codepoint_t hbGlyph,
                                       void* /*userData*/) {
    SkFont& font = *reinterpret_cast<SkFont*>(fontData);

    SkScalar advance;
    auto skGlyph = SkTo<SkGlyphID>(hbGlyph);

    font.getWidths(&skGlyph, 1, &advance);
    if (!font.isSubpixel()) {
        advance = SkScalarRoundToInt(advance);
    }
    return sskhbPosition(advance);
}

static void skhbGlyphHAdvances(hb_font_t* /*hbFont*/,
                               void* fontData,
                               unsigned count,
                               const hb_codepoint_t* glyphs,
                               unsigned int glyphStride,
                               hb_position_t* advances,
                               unsigned int advanceStride,
                               void* /*userData*/) {
    SkFont& font = *reinterpret_cast<SkFont*>(fontData);

    // Batch call getWidths since entry cost is not cheap.
    // Copy requred because getWidths is dense and hb is strided.
    skia_private::AutoSTMalloc<256, SkGlyphID> glyph(count);
    for (unsigned i = 0; i < count; i++) {
        glyph[i] = *glyphs;
        glyphs = SkTAddOffset<const hb_codepoint_t>(glyphs, glyphStride);
    }
    skia_private::AutoSTMalloc<256, SkScalar> advance(count);
    font.getWidths(glyph.get(), count, advance.get());

    if (!font.isSubpixel()) {
        for (unsigned i = 0; i < count; i++) {
            advance[i] = SkScalarRoundToInt(advance[i]);
        }
    }

    // Copy the results back to the sparse array.
    for (unsigned i = 0; i < count; i++) {
        *advances = sskhbPosition(advance[i]);
        advances = SkTAddOffset<hb_position_t>(advances, advanceStride);
    }
}

// HarfBuzz callback to retrieve glyph extents, mainly used by HarfBuzz for
// fallback mark positioning, i.e. the situation when the font does not have
// mark anchors or other mark positioning rules, but instead HarfBuzz is
// supposed to heuristically place combining marks around base glyphs. HarfBuzz
// does this by measuring "ink boxes" of glyphs, and placing them according to
// Unicode mark classes. Above, below, centered or left or right, etc.
static hb_bool_t skhbGlyphExtents(
    hb_font_t* /*hbFont*/, void* fontData, hb_codepoint_t hbGlyph, hb_glyph_extents_t* extents, void* /*userData*/) {
    SkFont& font = *reinterpret_cast<SkFont*>(fontData);

    SkRect skBounds;
    auto skGlyph = SkTo<SkGlyphID>(hbGlyph);

    font.getWidths(&skGlyph, 1, nullptr, &skBounds);
    if (!font.isSubpixel()) {
        skBounds.set(skBounds.roundOut());
    }

    // Skia is y-down but HarfBuzz is y-up.
    extents->x_bearing = sskhbPosition(skBounds.fLeft);
    extents->y_bearing = sskhbPosition(-skBounds.fTop);
    extents->width = sskhbPosition(skBounds.width());
    extents->height = sskhbPosition(-skBounds.height());
    return static_cast<hb_bool_t>(true);
}

static hb_font_funcs_t* skhbGetFontFuncs() {
    static hb_font_funcs_t* const funcs = [] {
        // HarfBuzz will use the default (parent) implementation if they aren't set.
        hb_font_funcs_t* const funcs = hb_font_funcs_create();
        hb_font_funcs_set_variation_glyph_func(funcs, skhbGlyph, nullptr, nullptr);
        hb_font_funcs_set_nominal_glyph_func(funcs, skhbNominalGlyph, nullptr, nullptr);
        hb_font_funcs_set_nominal_glyphs_func(funcs, skhbNominalGlyphs, nullptr, nullptr);
        hb_font_funcs_set_glyph_h_advance_func(funcs, skhbGlyphHAdvance, nullptr, nullptr);
        hb_font_funcs_set_glyph_h_advances_func(funcs, skhbGlyphHAdvances, nullptr, nullptr);
        hb_font_funcs_set_glyph_extents_func(funcs, skhbGlyphExtents, nullptr, nullptr);
        hb_font_funcs_make_immutable(funcs);
        return funcs;
    }();
    return funcs;
}

static hb_blob_t* skhbGetTable(hb_face_t* /*face*/, hb_tag_t tag, void* userData) {
    SkTypeface& typeface = *reinterpret_cast<SkTypeface*>(userData);

    auto data = typeface.copyTableData(tag);
    if (!data) {
        return nullptr;
    }
    SkData* rawData = data.release();
    return hb_blob_create(reinterpret_cast<char*>(rawData->writable_data()),
                          static_cast<unsigned int>(rawData->size()),
                          HB_MEMORY_MODE_READONLY,
                          rawData,
                          [](void* ctx) { SkSafeUnref(reinterpret_cast<SkData*>(ctx)); });
}

HBFace Harfbuzz::createFace(const NativeTypeface& typeface) {
    HBFace face(hb_face_create_for_tables(
        skhbGetTable, SkRef(typeface.get()), [](void* userData) { SkSafeUnref(reinterpret_cast<SkTypeface*>(userData)); }));

    if (face.get() == nullptr) {
        return HBFace(nullptr);
    }

    hb_face_set_upem(face.get(), typeface->getUnitsPerEm());

    return face;
}

HBFont Harfbuzz::createMainFont(const NativeTypeface& typeface, const HBFace& hbFace) {
    HBFont hbFont(hb_font_create(hbFace.get()));
    if (hbFont.get() == nullptr) {
        return HBFont(nullptr);
    }
    hb_ot_font_set_funcs(hbFont.get());
    int axisCount = typeface->getVariationDesignPosition(nullptr, 0);
    if (axisCount > 0) {
        skia_private::AutoSTMalloc<4, SkFontArguments::VariationPosition::Coordinate> axisValues(axisCount);
        if (typeface->getVariationDesignPosition(axisValues, axisCount) == axisCount) {
            hb_font_set_variations(hbFont.get(), reinterpret_cast<hb_variation_t*>(axisValues.get()), axisCount);
        }
    }

    return hbFont;
}

HBFont Harfbuzz::createSubFont(const HBFont& mainFont, NativeFont* font) {
    // Creating a sub font means that non-available functions
    // are found from the parent.
    HBFont hbFont(hb_font_create_sub_font(mainFont.get()));
    if (hbFont.get() == nullptr) {
        return HBFont(nullptr);
    }

    hb_font_set_funcs(hbFont.get(), skhbGetFontFuncs(), font, nullptr);
    int scale = sskhbPosition(font->getSize());
    hb_font_set_scale(hbFont.get(), scale, scale);

    return hbFont;
}

#else

namespace {
hb_position_t hbPosition(float value) {
    // Keep the same 16.16 contract used by TextShaperHarfbuzz.
    return static_cast<hb_position_t>(std::lround(static_cast<double>(value) * 65536.0));
}

hb_bool_t nominalGlyph(hb_font_t*, void* data, hb_codepoint_t unicode, hb_codepoint_t* glyph, void*) {
    const auto& font = *static_cast<NativeFont*>(data);
    *glyph = font.getGlyphID(static_cast<tgfx::Unichar>(unicode));
    return *glyph != 0;
}

unsigned nominalGlyphs(hb_font_t* font, void* data, unsigned count, const hb_codepoint_t* unicodes,
                      unsigned unicodeStride, hb_codepoint_t* glyphs, unsigned glyphStride, void*) {
    unsigned done = 0;
    for (; done < count; ++done) {
        if (!nominalGlyph(font, data, *unicodes, glyphs, nullptr)) break;
        unicodes = reinterpret_cast<const hb_codepoint_t*>(reinterpret_cast<const uint8_t*>(unicodes) + unicodeStride);
        glyphs = reinterpret_cast<hb_codepoint_t*>(reinterpret_cast<uint8_t*>(glyphs) + glyphStride);
    }
    // Returning the first missing glyph permits HarfBuzz NFC/space synthesis.
    return done;
}

hb_position_t glyphAdvance(hb_font_t*, void* data, hb_codepoint_t glyph, void*) {
    auto advance = static_cast<NativeFont*>(data)->getAdvance(static_cast<tgfx::GlyphID>(glyph));
    // Font leaves SkFont's subpixel positioning disabled in the default
    // backend. Round each nominal advance before HarfBuzz applies GPOS, just
    // as skhbGlyphHAdvance/skhbGlyphHAdvances do above. Rounding only the
    // final shaped advance in TextLayoutBuilder changes kerning results.
    return hbPosition(static_cast<float>(std::floor(static_cast<double>(advance) + 0.5)));
}

void glyphAdvances(hb_font_t* font, void* data, unsigned count, const hb_codepoint_t* glyphs,
                   unsigned glyphStride, hb_position_t* advances, unsigned advanceStride, void*) {
    for (unsigned i = 0; i < count; ++i) {
        *advances = glyphAdvance(font, data, *glyphs, nullptr);
        glyphs = reinterpret_cast<const hb_codepoint_t*>(reinterpret_cast<const uint8_t*>(glyphs) + glyphStride);
        advances = reinterpret_cast<hb_position_t*>(reinterpret_cast<uint8_t*>(advances) + advanceStride);
    }
}

hb_bool_t glyphExtents(hb_font_t*, void* data, hb_codepoint_t glyph, hb_glyph_extents_t* extents, void*) {
    auto bounds = static_cast<NativeFont*>(data)->getBounds(static_cast<tgfx::GlyphID>(glyph));
    extents->x_bearing = hbPosition(bounds.left);
    extents->y_bearing = hbPosition(-bounds.top);
    extents->width = hbPosition(bounds.width());
    extents->height = hbPosition(-bounds.height());
    return true;
}

hb_font_funcs_t* fontFunctions() {
    static auto* functions = [] {
        auto* value = hb_font_funcs_create();
        hb_font_funcs_set_nominal_glyph_func(value, nominalGlyph, nullptr, nullptr);
        hb_font_funcs_set_nominal_glyphs_func(value, nominalGlyphs, nullptr, nullptr);
        hb_font_funcs_set_glyph_h_advance_func(value, glyphAdvance, nullptr, nullptr);
        hb_font_funcs_set_glyph_h_advances_func(value, glyphAdvances, nullptr, nullptr);
        hb_font_funcs_set_glyph_extents_func(value, glyphExtents, nullptr, nullptr);
        // Variation selectors deliberately use the parent's real OpenType cmap
        // implementation. Ignoring a selector breaks emoji/text presentation.
        hb_font_funcs_make_immutable(value);
        return value;
    }();
    return functions;
}

hb_blob_t* fontTable(hb_face_t*, hb_tag_t tag, void* userData) {
    const auto& face = *static_cast<NativeTypeface*>(userData);
    auto table = face->copyTableData(tag);
    if (!table || table->size() > std::numeric_limits<unsigned>::max()) return hb_blob_get_empty();
    auto* owner = new std::shared_ptr<tgfx::Data>(std::move(table));
    return hb_blob_create(static_cast<const char*>((*owner)->data()), static_cast<unsigned>((*owner)->size()),
                          HB_MEMORY_MODE_READONLY, owner,
                          [](void* data) { delete static_cast<std::shared_ptr<tgfx::Data>*>(data); });
}
} // namespace

HBFace Harfbuzz::createFace(const NativeTypeface& typeface) {
    auto* owner = new NativeTypeface(typeface);
    HBFace face(hb_face_create_for_tables(fontTable, owner, [](void* data) {
        delete static_cast<NativeTypeface*>(data);
    }));
    hb_face_set_upem(face.get(), static_cast<unsigned>(typeface->unitsPerEm()));
    hb_face_set_glyph_count(face.get(), static_cast<unsigned>(typeface->glyphsCount()));
    return face;
}

#if defined(__APPLE__)
static void applyCoreTextVariations(hb_font_t* font, CTFontRef ctFont) {
    auto variations = ctFont ? CTFontCopyVariation(ctFont) : nullptr;
    if (variations) {
        auto count = CFDictionaryGetCount(variations);
        std::vector<const void*> keys(static_cast<size_t>(count)), values(static_cast<size_t>(count));
        CFDictionaryGetKeysAndValues(variations, keys.data(), values.data());
        std::vector<hb_variation_t> coordinates;
        coordinates.reserve(static_cast<size_t>(count));
        for (CFIndex i = 0; i < count; ++i) {
            int64_t tag = 0;
            float value = 0;
            if (CFNumberGetValue(static_cast<CFNumberRef>(keys[i]), kCFNumberSInt64Type, &tag) &&
                CFNumberGetValue(static_cast<CFNumberRef>(values[i]), kCFNumberFloatType, &value)) {
                coordinates.push_back({static_cast<hb_tag_t>(tag), value});
            }
        }
        hb_font_set_variations(font, coordinates.data(), static_cast<unsigned>(coordinates.size()));
        CFRelease(variations);
    }
}
#endif

HBFont Harfbuzz::createMainFont(const NativeTypeface& typeface, const HBFace& face) {
    HBFont font(hb_font_create(face.get()));
    hb_ot_font_set_funcs(font.get());
#if defined(__APPLE__)
    applyCoreTextVariations(font.get(), tgfx::CTTypeface::GetCTFont(typeface.get()));
#else
    std::vector<hb_variation_t> coordinates;
    for (const auto& coordinate : typeface->getVariationDesignPosition())
        coordinates.push_back({coordinate.axis, coordinate.value});
    hb_font_set_variations(font.get(), coordinates.data(), static_cast<unsigned>(coordinates.size()));
#endif
    return font;
}

HBFont Harfbuzz::createSubFont(const HBFont& mainFont, NativeFont* nativeFont) {
    HBFont font(hb_font_create_sub_font(mainFont.get()));
    hb_font_set_funcs(font.get(), fontFunctions(), nativeFont, nullptr);
    auto scale = hbPosition(nativeFont->getSize());
    hb_font_set_scale(font.get(), scale, scale);
#if defined(__APPLE__)
    // TGFX's CGScalerContext makes this same sized CTFont copy. Capture its
    // optical-size coordinates so GPOS/GSUB see the instance actually drawn.
    auto ctFont = tgfx::CTTypeface::GetCTFont(nativeFont->getTypeface().get());
    auto sizedFont = ctFont ? CTFontCreateCopyWithAttributes(ctFont, nativeFont->getSize(), nullptr, nullptr) : nullptr;
    if (sizedFont) {
        applyCoreTextVariations(font.get(), sizedFont);
        CFRelease(sizedFont);
    }
#endif
    return font;
}
#endif

}; // namespace snap::drawing
