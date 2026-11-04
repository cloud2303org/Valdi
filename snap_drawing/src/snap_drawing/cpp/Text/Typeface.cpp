//
//  Typeface.cpp
//  snap_drawing-macos
//
//  Created by Simon Corsin on 1/28/22.
//

#include "snap_drawing/cpp/Text/Typeface.hpp"
#include "snap_drawing/cpp/Text/FontStyle.hpp"

#include "valdi_core/cpp/Text/CharacterSet.hpp"
#include "valdi_core/cpp/Utils/ByteBuffer.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Text/NativeFontManager.hpp"
#if defined(__APPLE__)
#include "tgfx/platform/apple/CTTypeface.h"
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <vector>
#include "hb-ot.h"
#include "hb.h"
#else
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkStream.h"
#endif

namespace snap::drawing {

constexpr Scalar kDefaultLineHeightToUnderlineThicknessRatio = 0.06;
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
constexpr Scalar kDefaultDescentUnderlineToPositionRatio = 0.2;
#endif

constexpr Scalar kDefaultLineHeightToStrikethroughThicknessRatio = 0.06;
constexpr Scalar kDefaultAscentToStrikethroughPositionRatio = 0.3;

#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && defined(__APPLE__)
static NativeTypeface makeCoreTextRenderingTypeface(const NativeTypeface& typeface) {
    auto font = tgfx::CTTypeface::GetCTFont(typeface.get());
    if (!font) return typeface;

    // Match SkCTFontCreateExactCopy: native 'trak' tracking must not be added
    // to glyph advances that HarfBuzz will position, and resizing must retain
    // the typeface's optical instance (including its glyph IDs).
    auto opticalSizeKey = CFSTR("NSCTFontOpticalSizeAttribute");
    auto opticalSizeAttribute = CTFontCopyAttribute(font, opticalSizeKey);
    double opticalSize = 0;
    if (!opticalSizeAttribute || CFGetTypeID(opticalSizeAttribute) != CFNumberGetTypeID() ||
        !CFNumberGetValue(static_cast<CFNumberRef>(opticalSizeAttribute), kCFNumberDoubleType, &opticalSize) ||
        opticalSize <= 0) {
        opticalSize = CTFontGetSize(font);
    }
    if (opticalSizeAttribute) CFRelease(opticalSizeAttribute);

    int tracking = 0;
    auto opticalSizeNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &opticalSize);
    auto trackingNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &tracking);
    const void* keys[] = {opticalSizeKey, CFSTR("NSCTFontUnscaledTrackingAttribute")};
    const void* values[] = {opticalSizeNumber, trackingNumber};
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    auto renderingFont = CTFontCreateCopyWithAttributes(font, 0, nullptr, descriptor);
    auto renderingTypeface = tgfx::CTTypeface::MakeFromCTFont(renderingFont);
    if (renderingFont) CFRelease(renderingFont);
    CFRelease(descriptor);
    CFRelease(attributes);
    CFRelease(trackingNumber);
    CFRelease(opticalSizeNumber);
    return renderingTypeface ? renderingTypeface : typeface;
}

#endif

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
// The table API is face-specific even when openStream() returns an entire TTC.
// Both collection export and streamless CoreText export use this standalone
// SFNT writer, recomputing directory and whole-font checksums after relocation.
static Valdi::BytesView copyFontDataForTables(const NativeTypeface& typeface, const std::vector<uint32_t>& tags) {
    std::map<uint32_t, std::shared_ptr<tgfx::Data>> tables;
    for (auto tag : tags) {
        auto table = typeface->copyTableData(tag);
        if (table) tables.emplace(tag, std::move(table));
    }
    if (tables.empty() || tables.size() > 4095) return {};
    auto head = tables.find(0x68656164);
    if (head == tables.end() || head->second->size() < 12) return {};
    size_t total = 12 + tables.size() * 16;
    for (const auto& table : tables) {
        if (table.second->size() > std::numeric_limits<uint32_t>::max() - 3) return {};
        auto padded = (table.second->size() + 3) & ~size_t(3);
        if (total > std::numeric_limits<uint32_t>::max() - padded) return {};
        total += padded;
    }
    auto buffer = Valdi::makeShared<Valdi::ByteBuffer>();
    buffer->resize(total);
    auto* bytes = buffer->data();
    std::memset(bytes, 0, total);
    auto put16 = [](uint8_t* p, uint16_t value) {
        p[0] = static_cast<uint8_t>(value >> 8); p[1] = static_cast<uint8_t>(value);
    };
    auto put32 = [&put16](uint8_t* p, uint32_t value) {
        put16(p, static_cast<uint16_t>(value >> 16)); put16(p + 2, static_cast<uint16_t>(value));
    };
    auto checksum = [](const uint8_t* p, size_t size) {
        uint32_t sum = 0;
        for (size_t i = 0; i < size; i += 4) {
            sum += (static_cast<uint32_t>(p[i]) << 24) | (static_cast<uint32_t>(p[i + 1]) << 16) |
                   (static_cast<uint32_t>(p[i + 2]) << 8) | p[i + 3];
        }
        return sum;
    };
    put32(bytes, tables.count(0x43464620) || tables.count(0x43464632) ? 0x4f54544f : 0x00010000);
    auto count = static_cast<uint16_t>(tables.size());
    uint16_t power = 1, selector = 0;
    while (power * 2 <= count) { power *= 2; ++selector; }
    put16(bytes + 4, count);
    put16(bytes + 6, power * 16);
    put16(bytes + 8, selector);
    put16(bytes + 10, count * 16 - power * 16);
    size_t offset = 12 + tables.size() * 16;
    size_t headOffset = 0, record = 12;
    for (const auto& table : tables) {
        auto length = table.second->size();
        auto padded = (length + 3) & ~size_t(3);
        if (length) std::memcpy(bytes + offset, table.second->data(), length);
        if (table.first == 0x68656164 && length >= 12) {
            headOffset = offset;
            put32(bytes + offset + 8, 0);
        }
        put32(bytes + record, table.first);
        put32(bytes + record + 4, checksum(bytes + offset, padded));
        put32(bytes + record + 8, static_cast<uint32_t>(offset));
        put32(bytes + record + 12, static_cast<uint32_t>(length));
        record += 16;
        offset += padded;
    }
    if (headOffset) put32(bytes + headOffset + 8, 0xb1b0afba - checksum(bytes, total));
    return buffer->toBytesView();
}

static Valdi::BytesView copyCollectionFaceData(const NativeTypeface& typeface, const Valdi::BytesView& stream) {
    auto read16 = [](const uint8_t* p) {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    };
    auto read32 = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | p[3];
    };
    if (stream.size() < 12) return {};
    auto count = read32(stream.data() + 8);
    if (!count || count > (stream.size() - 12) / 4) return {};
    std::vector<uint32_t> tags;
    // The public TGFX API has no table-tag enumeration or TTC-index getter.
    // Inventory all collection directories, then fetch each candidate through
    // the selected face's actual table API. Tables unique to other faces are
    // absent there. Never export a different face's directory/table payload.
    for (uint32_t i = 0; i < count; ++i) {
        auto offset = static_cast<size_t>(read32(stream.data() + 12 + static_cast<size_t>(i) * 4));
        if (offset > stream.size() || stream.size() - offset < 12) return {};
        auto tableCount = read16(stream.data() + offset + 4);
        if (tableCount > (stream.size() - offset - 12) / 16) return {};
        for (size_t table = 0; table < tableCount; ++table) {
            const auto* record = stream.data() + offset + 12 + table * 16;
            auto tableOffset = static_cast<size_t>(read32(record + 8));
            auto tableSize = static_cast<size_t>(read32(record + 12));
            if (tableOffset > stream.size() || tableSize > stream.size() - tableOffset) return {};
            tags.push_back(read32(record));
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return copyFontDataForTables(typeface, tags);
}

#if defined(__APPLE__)
// System CoreText faces have table access but often no backing stream in TGFX.
static Valdi::BytesView copyCoreTextFontData(const NativeTypeface& typeface) {
    auto ctFont = tgfx::CTTypeface::GetCTFont(typeface.get());
    if (!ctFont) return {};
    auto available = CTFontCopyAvailableTables(ctFont, kCTFontTableOptionNoOptions);
    if (!available) return {};
    std::vector<uint32_t> tags;
    tags.reserve(static_cast<size_t>(CFArrayGetCount(available)));
    for (CFIndex i = 0; i < CFArrayGetCount(available); ++i) {
        // CoreText stores integer tags directly, not CFNumber objects.
        tags.push_back(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(CFArrayGetValueAtIndex(available, i))));
    }
    CFRelease(available);
    return copyFontDataForTables(typeface, tags);
}
#endif
#endif

Typeface::Typeface(NativeTypeface&& typeface, const String& familyName, bool isCustom)
    : _typeface(std::move(typeface)), _familyName(familyName), _fontStyle(
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
          getTgfxTypefaceStyle(_typeface)
#else
          _typeface->fontStyle()
#endif
      ), _isCustom(isCustom) {
#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && defined(__APPLE__)
    _fontDataTypeface = _typeface;
    _typeface = makeCoreTextRenderingTypeface(_typeface);
#endif
    _hbFace = Harfbuzz::createFace(_typeface);
    if (_hbFace.get() != nullptr) {
        _hbFont = Harfbuzz::createMainFont(_typeface, _hbFace);
    }

    _characterSet = _hbFace.getCharacters();
    // This seems arbitrary, but I'm not sure if there is a better way
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _isEmoji = _typeface->hasColor() && (supportsCharacter(0x270C) || supportsCharacter(0x1F600));
    hb_codepoint_t space = 0;
    if (hb_font_get_nominal_glyph(_hbFont.get(), ' ', &space)) {
        auto* glyphs = hb_set_create();
        const hb_tag_t layoutTables[] = {HB_TAG('G', 'S', 'U', 'B'), HB_TAG('G', 'P', 'O', 'S')};
        for (auto table : layoutTables) {
            auto count = hb_ot_layout_table_get_lookup_count(_hbFace.get(), table);
            for (unsigned i = 0; i < count; ++i) {
                hb_set_clear(glyphs);
                hb_ot_layout_lookup_collect_glyphs(_hbFace.get(), table, i, glyphs, glyphs, glyphs, nullptr);
                if (hb_set_has(glyphs, space)) {
                    _hasSpaceInLigaturesOrKerning = true;
                    break;
                }
            }
            if (_hasSpaceInLigaturesOrKerning) break;
        }
        auto kern = _typeface->copyTableData(HB_TAG('k', 'e', 'r', 'n'));
        // Legacy kern tables have no lookup-collection API. Avoid splitting
        // words for those fonts rather than lose cross-space kerning.
        _hasSpaceInLigaturesOrKerning |= kern && !kern->empty();
        hb_set_destroy(glyphs);
    }
#else
    _isEmoji = supportsCharacter(0x270C);
#endif
}

Typeface::~Typeface() = default;

uint32_t Typeface::getId() const {
    return _typeface->uniqueID();
}

const NativeTypeface& Typeface::getSkValue() const {
    return _typeface;
}

const String& Typeface::familyName() const {
    return _familyName;
}

const FontStyle& Typeface::fontStyle() const {
    return _fontStyle;
}

bool Typeface::isCustom() const {
    return _isCustom;
}

bool Typeface::supportsCharacter(Character character) {
    return _characterSet->contains(character);
}

const HBFace& Typeface::getHBFace() const {
    return _hbFace;
}

const HBFont& Typeface::getHBFont() const {
    return _hbFont;
}

bool Typeface::isEmoji() const {
    return _isEmoji;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool Typeface::hasSpaceInLigaturesOrKerning() const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return _hasSpaceInLigaturesOrKerning;
#else
    // TODO(simon): Implement
    return false;
#endif
}

Valdi::BytesView Typeface::makeFontData() {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#if defined(__APPLE__)
    auto skStreamAsset = _fontDataTypeface->openStream();
#else
    auto skStreamAsset = _typeface->openStream();
#endif
#else
    auto skStreamAsset = _typeface->openStream(nullptr);
#endif
    if (!skStreamAsset) {
#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && defined(__APPLE__)
        return copyCoreTextFontData(_typeface);
#else
        return Valdi::BytesView();
#endif
    }

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto streamSize = skStreamAsset->size();
#else
    auto streamSize = skStreamAsset->getLength();
#endif
    auto buffer = Valdi::makeShared<Valdi::ByteBuffer>();
    buffer->resize(streamSize);

    auto realSize = skStreamAsset->read(buffer->data(), streamSize);
    buffer->resize(realSize);
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto view = buffer->toBytesView();
    if (view.size() >= 4 && std::memcmp(view.data(), "ttcf", 4) == 0) {
        return copyCollectionFaceData(_typeface, view);
    }
#endif
    return buffer->toBytesView();
}

FontMetrics Typeface::getFontMetrics(double fontSize) {
    std::lock_guard<Valdi::Mutex> lock(_mutex);

    const auto& it = _fontMetricsBySize.find(fontSize);
    if (it != _fontMetricsBySize.end()) {
        return it->second;
    }

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    NativeFont font(_typeface, static_cast<float>(fontSize));
    auto metrics = font.getMetrics();
    FontMetrics out;
    out.ascent = metrics.ascent;
    out.descent = metrics.descent;
    out.lineSpacing = metrics.descent - metrics.ascent + metrics.leading;
    auto lineHeight = metrics.descent - metrics.ascent;
    out.underlineThickness = metrics.underlineThickness > 0 ? metrics.underlineThickness :
        kDefaultLineHeightToUnderlineThicknessRatio * lineHeight;
    out.underlinePosition = metrics.underlinePosition;
    out.strikeoutThickness = kDefaultLineHeightToStrikethroughThicknessRatio * lineHeight;
    out.strikeoutPosition = kDefaultAscentToStrikethroughPositionRatio * metrics.ascent - out.strikeoutThickness / 2;
    // TGFX metrics have no strikeout fields. Read the actual OpenType OS/2
    // values in font units; retain the established fallback for older fonts.
    auto os2 = _typeface->copyTableData(0x4f532f32);
    auto upem = _typeface->unitsPerEm();
    if (os2 && os2->size() >= 30 && upem > 0) {
        auto signedShort = [](const uint8_t* p) {
            return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
        };
        auto thickness = signedShort(os2->bytes() + 26);
        if (thickness > 0) out.strikeoutThickness = thickness * fontSize / upem;
        out.strikeoutPosition = -signedShort(os2->bytes() + 28) * fontSize / upem;
    }
#else
    SkFont font(_typeface, fontSize);
    font.setEdging(SkFont::Edging::kAntiAlias);

    SkFontMetrics metrics;

    FontMetrics out;
    out.lineSpacing = font.getMetrics(&metrics);
    out.descent = metrics.fDescent;
    out.ascent = metrics.fAscent;

    auto lineHeight = metrics.fDescent - metrics.fAscent;

    if (!metrics.hasUnderlineThickness(&out.underlineThickness)) {
        out.underlineThickness = kDefaultLineHeightToUnderlineThicknessRatio * lineHeight;
    }

    if (!metrics.hasUnderlinePosition(&out.underlinePosition)) {
        out.underlinePosition = kDefaultDescentUnderlineToPositionRatio * metrics.fDescent;
    }

    if (!metrics.hasStrikeoutThickness(&out.strikeoutThickness)) {
        out.strikeoutThickness = kDefaultLineHeightToStrikethroughThicknessRatio * lineHeight;
    }

    if (!metrics.hasStrikeoutPosition(&out.strikeoutPosition)) {
        out.strikeoutPosition =
            (kDefaultAscentToStrikethroughPositionRatio * metrics.fAscent) - out.strikeoutThickness / 2.0f;
    }

#endif

    _fontMetricsBySize[fontSize] = out;

    return out;
}

} // namespace snap::drawing
