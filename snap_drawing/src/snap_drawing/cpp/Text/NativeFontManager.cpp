#include "snap_drawing/cpp/Text/NativeFontManager.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <tuple>

#if defined(__APPLE__)
#include "tgfx/platform/apple/CTTypeface.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#elif defined(__ANDROID__)
// Android's fonts.xml supplies aliases, TTC indices, languages, and fallback
// ordering which TGFX's MakeFromName() does not implement.
#include <android/font.h>
#include <android/font_matcher.h>
#include <android/system_fonts.h>
#include <dlfcn.h>
#elif defined(__linux__) && __has_include(<fontconfig/fontconfig.h>)
#include <fontconfig/fontconfig.h>
#else
#error "TGFX system fonts need a platform font manager bridge on this platform"
#endif

namespace snap::drawing {
namespace {

uint16_t readU16(const uint8_t* bytes) {
    return (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
}

#if !defined(__APPLE__)
uint32_t readU32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(readU16(bytes)) << 16) | readU16(bytes + 2);
}
#endif

FontWeight weightFromCSS(int weight) {
    return static_cast<FontWeight>(std::clamp((weight + 50) / 100, 0, 10));
}

// CSS Fonts matching order: width first, then slant, then weight. In the
// 400..500 interval search towards 500 before searching below the request.
auto styleDistance(FontStyle desired, FontStyle candidate) {
    int dw = desired.getWidth();
    int cw = candidate.getWidth();
    int width = dw <= FontWidthNormal ? (cw <= dw ? dw - cw : cw - dw + 9) :
                                       (cw >= dw ? cw - dw : dw - cw + 9);
    int slant = desired.getSlant() == candidate.getSlant() ? 0 :
                (desired.getSlant() != FontSlantUpright && candidate.getSlant() != FontSlantUpright ? 1 : 2);
    int d = desired.getWeight();
    int c = candidate.getWeight();
    int weight;
    if (d >= FontWeightNormal && d <= FontWeightMedium) {
        weight = c >= d && c <= FontWeightMedium ? c - d :
                 (c < d ? d - c + 11 : c - d + 22);
    } else if (d < FontWeightNormal) {
        weight = c <= d ? d - c : c - d + 11;
    } else {
        weight = c >= d ? c - d : d - c + 11;
    }
    return std::make_tuple(width, slant, weight);
}

#if defined(__APPLE__)
template<typename T>
class CFHandle {
public:
    explicit CFHandle(T value = nullptr) : _value(value) {}
    ~CFHandle() { if (_value) CFRelease(_value); }
    CFHandle(const CFHandle&) = delete;
    CFHandle& operator=(const CFHandle&) = delete;
    T get() const { return _value; }
private:
    T _value;
};

std::string stringFromCF(CFStringRef string) {
    if (!string) return {};
    auto length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(length), '\0');
    if (!CFStringGetCString(string, out.data(), length, kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

CFStringRef stringToCF(const std::string& string) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(string.data()),
                                   static_cast<CFIndex>(string.size()), kCFStringEncodingUTF8, false);
}

float numericTrait(CFDictionaryRef traits, CFStringRef key) {
    float value = 0;
    if (traits) {
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(traits, key));
        if (number) CFNumberGetValue(number, kCFNumberFloatType, &value);
    }
    return value;
}

const std::array<CGFloat, 11>& coreTextNativeWeights() {
    static const auto weights = [] {
        std::array<CGFloat, 11> defaults{-1, -.8, -.6, -.4, 0, .23, .3, .4, .56, .62, 1};
#if TARGET_OS_OSX
#define SNAP_DRAWING_FONT_WEIGHT_PREFIX "NSFontWeight"
#else
#define SNAP_DRAWING_FONT_WEIGHT_PREFIX "UIFontWeight"
#endif
        const char* names[] = {
            SNAP_DRAWING_FONT_WEIGHT_PREFIX "UltraLight", SNAP_DRAWING_FONT_WEIGHT_PREFIX "Thin",
            SNAP_DRAWING_FONT_WEIGHT_PREFIX "Light", SNAP_DRAWING_FONT_WEIGHT_PREFIX "Regular",
            SNAP_DRAWING_FONT_WEIGHT_PREFIX "Medium", SNAP_DRAWING_FONT_WEIGHT_PREFIX "Semibold",
            SNAP_DRAWING_FONT_WEIGHT_PREFIX "Bold", SNAP_DRAWING_FONT_WEIGHT_PREFIX "Heavy",
            SNAP_DRAWING_FONT_WEIGHT_PREFIX "Black"};
#undef SNAP_DRAWING_FONT_WEIGHT_PREFIX
        auto resolved = defaults;
        for (size_t i = 0; i < std::size(names); ++i) {
            auto* value = static_cast<const CGFloat*>(dlsym(RTLD_DEFAULT, names[i]));
            if (!value) return defaults;
            resolved[i + 1] = *value;
        }
        return resolved;
    }();
    return weights;
}

CTFontRef createCoreTextFont(const char* family, NativeFontStyle style) {
    auto weight = coreTextNativeWeights()[static_cast<size_t>(style.weight())];
    // CoreText's native CSS width mapping is [-.5, .5] over CSS [0, 10].
    // TGFX's width enum is zero based, corresponding to CSS widths [1, 9].
    CGFloat width = (static_cast<int>(style.width()) + 1) * .1 - .5;
    CGFloat slant = style.slant() == tgfx::FontSlant::Upright ? 0 : .07;
    CFHandle<CFNumberRef> weightNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &weight));
    CFHandle<CFNumberRef> widthNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &width));
    CFHandle<CFNumberRef> slantNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &slant));
    const void* traitKeys[] = {kCTFontWeightTrait, kCTFontWidthTrait, kCTFontSlantTrait};
    const void* traitValues[] = {weightNumber.get(), widthNumber.get(), slantNumber.get()};
    CFHandle<CFDictionaryRef> traits(CFDictionaryCreate(kCFAllocatorDefault, traitKeys, traitValues, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFHandle<CFStringRef> familyName(family && *family ? stringToCF(family) : nullptr);
    const void* keys[] = {kCTFontTraitsAttribute, kCTFontFamilyNameAttribute};
    const void* values[] = {traits.get(), familyName.get()};
    CFHandle<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values,
        familyName.get() ? 2 : 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFHandle<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
    return CTFontCreateWithFontDescriptor(descriptor.get(), 0, nullptr);
}
#endif

#if defined(__ANDROID__)
// A small XML reader for Android's system font configuration. It handles quoted
// attributes, entities, comments, declarations, and nested axis elements; font
// filenames are text nodes, never inferred from the family name.
struct XmlNode {
    std::string name;
    std::string text;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;

    std::string attribute(const char* key) const {
        auto it = attributes.find(key);
        return it == attributes.end() ? std::string() : it->second;
    }
};

std::string trim(std::string text) {
    const char* whitespace = " \t\r\n";
    auto first = text.find_first_not_of(whitespace);
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::string decodeEntities(std::string text) {
    static const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (const auto& entity : entities) {
        size_t at = 0;
        while ((at = text.find(entity.first, at)) != std::string::npos) {
            text.replace(at, std::char_traits<char>::length(entity.first), entity.second);
            at += std::char_traits<char>::length(entity.second);
        }
    }
    return text;
}

class FontConfigReader {
public:
    explicit FontConfigReader(std::string source) : _source(std::move(source)) {}
    bool read(XmlNode& root) {
        while (skipMisc()) {}
        return readNode(root);
    }
private:
    std::string _source;
    size_t _at = 0;

    void whitespace() {
        _at = _source.find_first_not_of(" \t\r\n", _at);
        if (_at == std::string::npos) _at = _source.size();
    }
    bool skipMisc() {
        whitespace();
        const char* ending = nullptr;
        if (_source.compare(_at, 4, "<!--") == 0) ending = "-->";
        else if (_source.compare(_at, 2, "<?") == 0) ending = "?>";
        if (!ending) return false;
        auto end = _source.find(ending, _at);
        _at = end == std::string::npos ? _source.size() : end + std::char_traits<char>::length(ending);
        return true;
    }
    std::string name() {
        auto start = _at;
        while (_at < _source.size() && _source.find_first_of(" \t\r\n/=>", _at) != _at) ++_at;
        return _source.substr(start, _at - start);
    }
    bool readNode(XmlNode& node) {
        whitespace();
        if (_at >= _source.size() || _source[_at++] != '<') return false;
        node.name = name();
        if (node.name.empty()) return false;
        while (_at < _source.size()) {
            whitespace();
            if (_source.compare(_at, 2, "/>") == 0) { _at += 2; return true; }
            if (_at < _source.size() && _source[_at] == '>') { ++_at; break; }
            auto key = name();
            whitespace();
            if (key.empty() || _at >= _source.size() || _source[_at++] != '=') return false;
            whitespace();
            if (_at >= _source.size()) return false;
            auto quote = _source[_at++];
            if (quote != '\'' && quote != '"') return false;
            auto end = _source.find(quote, _at);
            if (end == std::string::npos) return false;
            node.attributes[key] = decodeEntities(_source.substr(_at, end - _at));
            _at = end + 1;
        }
        while (_at < _source.size()) {
            if (skipMisc()) continue;
            if (_at >= _source.size()) return false;
            if (_source.compare(_at, 2, "</") == 0) {
                _at += 2;
                if (name() != node.name) return false;
                whitespace();
                return _at < _source.size() && _source[_at++] == '>';
            }
            if (_source[_at] == '<') {
                XmlNode child;
                if (!readNode(child)) return false;
                node.children.push_back(std::move(child));
            } else {
                auto end = _source.find('<', _at);
                if (end == std::string::npos) return false;
                node.text += decodeEntities(_source.substr(_at, end - _at));
                _at = end;
            }
        }
        return false;
    }
};

// Resolve API 29 system-font services at runtime so the same binary still loads
// on older Android releases. These are the NDK's declared function signatures.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#endif
struct AndroidFonts {
    void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    decltype(&AFontMatcher_create) create = nullptr;
    decltype(&AFontMatcher_destroy) destroy = nullptr;
    decltype(&AFontMatcher_setStyle) setStyle = nullptr;
    decltype(&AFontMatcher_setLocales) setLocales = nullptr;
    decltype(&AFontMatcher_match) match = nullptr;
    decltype(&AFont_close) close = nullptr;
    decltype(&AFont_getFontFilePath) path = nullptr;
    decltype(&AFont_getCollectionIndex) index = nullptr;
    decltype(&AFont_getWeight) weight = nullptr;
    decltype(&AFont_isItalic) italic = nullptr;
    decltype(&AFont_getLocale) locale = nullptr;
    decltype(&AFont_getAxisCount) axisCount = nullptr;
    decltype(&AFont_getAxisTag) axisTag = nullptr;
    decltype(&AFont_getAxisValue) axisValue = nullptr;
    decltype(&ASystemFontIterator_open) iteratorOpen = nullptr;
    decltype(&ASystemFontIterator_next) iteratorNext = nullptr;
    decltype(&ASystemFontIterator_close) iteratorClose = nullptr;

    AndroidFonts() {
        if (!library) return;
#define LOAD_ANDROID_FONT(field, name) field = reinterpret_cast<decltype(field)>(dlsym(library, #name))
        LOAD_ANDROID_FONT(create, AFontMatcher_create);
        LOAD_ANDROID_FONT(destroy, AFontMatcher_destroy);
        LOAD_ANDROID_FONT(setStyle, AFontMatcher_setStyle);
        LOAD_ANDROID_FONT(setLocales, AFontMatcher_setLocales);
        LOAD_ANDROID_FONT(match, AFontMatcher_match);
        LOAD_ANDROID_FONT(close, AFont_close);
        LOAD_ANDROID_FONT(path, AFont_getFontFilePath);
        LOAD_ANDROID_FONT(index, AFont_getCollectionIndex);
        LOAD_ANDROID_FONT(weight, AFont_getWeight);
        LOAD_ANDROID_FONT(italic, AFont_isItalic);
        LOAD_ANDROID_FONT(locale, AFont_getLocale);
        LOAD_ANDROID_FONT(axisCount, AFont_getAxisCount);
        LOAD_ANDROID_FONT(axisTag, AFont_getAxisTag);
        LOAD_ANDROID_FONT(axisValue, AFont_getAxisValue);
        LOAD_ANDROID_FONT(iteratorOpen, ASystemFontIterator_open);
        LOAD_ANDROID_FONT(iteratorNext, ASystemFontIterator_next);
        LOAD_ANDROID_FONT(iteratorClose, ASystemFontIterator_close);
#undef LOAD_ANDROID_FONT
    }
    ~AndroidFonts() { if (library) dlclose(library); }
    bool available() const {
        return create && destroy && setStyle && setLocales && match && close && path && index && weight &&
               italic && locale && axisCount && axisTag && axisValue;
    }
    TgfxFontStyleSet::Entry entry(const AFont* font) const {
        TgfxFontStyleSet::Entry out;
        out.path = path(font);
        out.collectionIndex = static_cast<int>(index(font));
        out.style = FontStyle(FontWidthNormal, weightFromCSS(weight(font)),
                              italic(font) ? FontSlantItalic : FontSlantUpright);
        for (size_t i = 0; i < axisCount(font); ++i)
            out.variations.emplace_back(axisTag(font, static_cast<uint32_t>(i)),
                                        axisValue(font, static_cast<uint32_t>(i)));
        return out;
    }
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
} // namespace

FontStyle getTgfxTypefaceStyle(const NativeTypeface& typeface) {
    auto weight = FontWeightNormal;
    auto width = FontWidthNormal;
    auto slant = FontSlantUpright;
    auto os2 = typeface->copyTableData(0x4f532f32); // OS/2
    if (os2 && os2->size() >= 8) {
        weight = weightFromCSS(readU16(os2->bytes() + 4));
        width = static_cast<FontWidth>(std::clamp<int>(readU16(os2->bytes() + 6), 1, 9) - 1);
        if (os2->size() >= 64) {
            auto selection = readU16(os2->bytes() + 62);
            if (selection & (1 << 9)) slant = FontSlantOblique;
            else if (selection & 1) slant = FontSlantItalic;
        }
    } else {
        auto head = typeface->copyTableData(0x68656164);
        if (head && head->size() >= 46) {
            auto macStyle = readU16(head->bytes() + 44);
            if (macStyle & 1) weight = FontWeightBold;
            if (macStyle & 2) slant = FontSlantItalic;
        }
    }
#if defined(__APPLE__)
    auto ctFont = tgfx::CTTypeface::GetCTFont(typeface.get());
    if (ctFont) {
        CFHandle<CFDictionaryRef> traits(CTFontCopyTraits(ctFont));
        const auto& weights = coreTextNativeWeights();
        auto value = numericTrait(traits.get(), kCTFontWeightTrait);
        int closest = 0;
        for (int i = 1; i < 11; ++i) {
            if (std::abs(weights[i] - value) < std::abs(weights[closest] - value)) closest = i;
        }
        weight = static_cast<FontWeight>(closest);
        width = static_cast<FontWidth>(std::clamp(
            static_cast<int>(std::lround(numericTrait(traits.get(), kCTFontWidthTrait) * 10)) + 4, 0, 8));
        if (CTFontGetSymbolicTraits(ctFont) & kCTFontItalicTrait) slant = FontSlantItalic;
    }
#else
    bool hasSlantAxis = false;
    bool italicInstance = false;
    bool obliqueInstance = false;
    for (const auto& coordinate : typeface->getVariationDesignPosition()) {
        switch (coordinate.axis) {
            case 0x77676874: // wght
                weight = weightFromCSS(static_cast<int>(std::lround(coordinate.value)));
                break;
            case 0x77647468: { // wdth
                constexpr float widths[] = {50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
                int closest = 0;
                for (int i = 1; i < 9; ++i)
                    if (std::abs(widths[i] - coordinate.value) < std::abs(widths[closest] - coordinate.value))
                        closest = i;
                width = static_cast<FontWidth>(closest);
                break;
            }
            case 0x6974616c: // ital
                hasSlantAxis = true;
                italicInstance = coordinate.value >= 0.5f;
                break;
            case 0x736c6e74: // slnt
                hasSlantAxis = true;
                obliqueInstance = coordinate.value != 0;
                break;
            default:
                break;
        }
    }
    if (hasSlantAxis)
        slant = italicInstance ? FontSlantItalic : (obliqueInstance ? FontSlantOblique : FontSlantUpright);
#endif
    return FontStyle(width, weight, slant);
}

#if !defined(__APPLE__)
NativeTypeface makeTgfxTypefaceWithStyle(const NativeTypeface& typeface, FontStyle style) {
    auto current = getTgfxTypefaceStyle(typeface);
    auto coordinates = typeface->getVariationDesignPosition();
    if (coordinates.empty()) return nullptr;
    auto hasAxis = [&](uint32_t axis) {
        return std::any_of(coordinates.begin(), coordinates.end(),
                           [axis](const auto& coordinate) { return coordinate.axis == axis; });
    };
    if (style.getWeight() != current.getWeight() && !hasAxis(0x77676874)) return nullptr;
    if (style.getWidth() != current.getWidth() && !hasAxis(0x77647468)) return nullptr;
    if (style.getSlant() != current.getSlant() && !hasAxis(0x6974616c) && !hasAxis(0x736c6e74)) return nullptr;
    for (auto& coordinate : coordinates) {
        switch (coordinate.axis) {
            case 0x77676874: // wght
                coordinate.value = static_cast<float>(style.getWeight()) * 100;
                break;
            case 0x77647468: { // wdth
                constexpr float widths[] = {50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
                coordinate.value = widths[static_cast<size_t>(style.getWidth())];
                break;
            }
            case 0x6974616c: // ital
                coordinate.value = style.getSlant() == FontSlantItalic ||
                                   (style.getSlant() == FontSlantOblique && !hasAxis(0x736c6e74)) ? 1.0f : 0.0f;
                break;
            case 0x736c6e74: { // slnt
                auto wantsSlant = style.getSlant() == FontSlantOblique ||
                                  (style.getSlant() == FontSlantItalic && !hasAxis(0x6974616c));
                if (!wantsSlant) {
                    coordinate.value = 0;
                } else if (coordinate.value == 0) {
                    auto fvar = typeface->copyTableData(0x66766172);
                    if (!fvar || fvar->size() < 16) return nullptr;
                    auto offset = readU16(fvar->bytes() + 4);
                    auto count = readU16(fvar->bytes() + 8);
                    auto stride = readU16(fvar->bytes() + 10);
                    if (stride < 20 || offset > fvar->size() || count > (fvar->size() - offset) / stride)
                        return nullptr;
                    for (size_t i = 0; i < count; ++i) {
                        const auto* axis = fvar->bytes() + offset + i * stride;
                        if (readU32(axis) != coordinate.axis) continue;
                        auto minimum = static_cast<int32_t>(readU32(axis + 4)) / 65536.0f;
                        auto maximum = static_cast<int32_t>(readU32(axis + 12)) / 65536.0f;
                        coordinate.value = minimum < 0 ? minimum : maximum;
                        break;
                    }
                    if (coordinate.value == 0) return nullptr;
                }
                break;
            }
            default:
                break;
        }
    }
    return typeface->makeWithVariations(coordinates);
}
#endif

TgfxFontStyleSet::TgfxFontStyleSet(std::vector<Entry> entries) : _entries(std::move(entries)) {}
size_t TgfxFontStyleSet::count() const { return _entries.size(); }

std::vector<size_t> TgfxFontStyleSet::rank(FontStyle style) const {
    std::vector<size_t> indices(_entries.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        auto candidateStyle = [&](const Entry& entry) {
            auto candidate = entry.style;
            if (entry.supportedAxes.find("wght") != std::string::npos) candidate.setWeight(style.getWeight());
            if (entry.supportedAxes.find("ital") != std::string::npos)
                candidate.setSlant(style.getSlant() == FontSlantUpright ? FontSlantUpright : FontSlantItalic);
            return candidate;
        };
        return styleDistance(style, candidateStyle(_entries[a])) < styleDistance(style, candidateStyle(_entries[b]));
    });
    return indices;
}

NativeTypeface TgfxFontStyleSet::load(size_t index) {
    auto& entry = _entries[index];
    if (!entry.typeface && !entry.path.empty()) {
        if (entry.attemptedLoad) return nullptr;
        entry.attemptedLoad = true;
        auto face = tgfx::Typeface::MakeFromPath(entry.path, entry.collectionIndex);
        if (!face) return nullptr;
#if !defined(__APPLE__)
        if (!entry.variations.empty()) {
            std::vector<tgfx::FontVariation> coordinates;
            coordinates.reserve(entry.variations.size());
            for (const auto& variation : entry.variations)
                coordinates.push_back({variation.first, variation.second});
            face = face->makeWithVariations(coordinates);
            if (!face) return nullptr;
        }
#endif
        entry.typeface = std::move(face);
    }
    return entry.typeface;
}

NativeTypeface TgfxFontStyleSet::resolveStyle(size_t index, NativeFontStyle nativeStyle) {
    auto face = load(index);
    if (!face) return nullptr;
#if !defined(__APPLE__)
    auto& entry = _entries[index];
    if (!entry.supportedAxes.empty()) {
        auto style = FontStyle(nativeStyle);
        auto found = entry.instances.find(style);
        if (found != entry.instances.end()) return found->second;
        std::vector<tgfx::FontVariation> coordinates;
        if (entry.supportedAxes.find("wght") != std::string::npos)
            coordinates.push_back({0x77676874, static_cast<float>(style.getWeight()) * 100});
        if (entry.supportedAxes.find("ital") != std::string::npos)
            coordinates.push_back({0x6974616c, style.getSlant() != FontSlantUpright ? 1.0f : 0.0f});
        face = face->makeWithVariations(coordinates);
        if (!face) return nullptr;
        entry.instances[style] = face;
    }
#else
    (void)nativeStyle;
#endif
    return face;
}

NativeTypeface TgfxFontStyleSet::matchStyle(NativeFontStyle nativeStyle) {
    std::lock_guard<std::mutex> guard(_mutex);
    for (auto index : rank(FontStyle(nativeStyle))) {
        auto face = resolveStyle(index, nativeStyle);
        if (face) return face;
        if (!_entries[index].variations.empty() || !_entries[index].supportedAxes.empty()) return nullptr;
    }
    return nullptr;
}

NativeTypeface TgfxFontStyleSet::matchCharacter(NativeFontStyle nativeStyle, Character character) {
    std::lock_guard<std::mutex> guard(_mutex);
    for (auto index : rank(FontStyle(nativeStyle))) {
        auto face = resolveStyle(index, nativeStyle);
        if (!face && (!_entries[index].variations.empty() || !_entries[index].supportedAxes.empty())) return nullptr;
        if (face && face->getGlyphID(static_cast<tgfx::Unichar>(character)) != 0) return face;
    }
    return nullptr;
}

bool TgfxFontStyleSet::containsTypeface(uint32_t uniqueID) {
    std::lock_guard<std::mutex> guard(_mutex);
    return std::any_of(_entries.begin(), _entries.end(), [uniqueID](const Entry& entry) {
        if (entry.typeface && entry.typeface->uniqueID() == uniqueID) return true;
        return std::any_of(entry.instances.begin(), entry.instances.end(), [uniqueID](const auto& instance) {
            return instance.second && instance.second->uniqueID() == uniqueID;
        });
    });
}

struct TgfxFontManager::Impl {
    struct Family {
        std::string name;
        std::string languages;
        std::string fallbackFor;
        std::shared_ptr<TgfxFontStyleSet> styles;
    };
    std::mutex mutex;
    std::vector<Family> families;
    std::map<std::string, size_t> byName;
    std::map<std::pair<std::string, size_t>, NativeTypeface> instances;
    std::vector<std::string> unsupported;
    std::string defaultFamily;
#if defined(__ANDROID__)
    AndroidFonts androidFonts;
    std::map<std::string, NativeTypeface> androidInstances;
#endif

    void add(Family family) {
        if (!family.name.empty()) byName[family.name] = families.size();
        families.push_back(std::move(family));
    }

#if defined(__ANDROID__)
    bool readConfig(const char* path, const char* fontDirectory) {
        std::ifstream input(path);
        if (!input) return false;
        std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        XmlNode root;
        if (!FontConfigReader(std::move(source)).read(root)) {
            unsupported.emplace_back(std::string("Malformed Android font configuration: ") + path);
            return false;
        }
        if (root.name != "familyset" && root.name != "fonts-modification") {
            unsupported.emplace_back(std::string("Unsupported Android font schema: ") + path);
            return false;
        }
        for (const auto& familyNode : root.children) {
            if (familyNode.name != "family") continue;
            Family family;
            family.name = familyNode.attribute("name");
            family.languages = familyNode.attribute("lang");
            std::vector<TgfxFontStyleSet::Entry> entries;
            for (const auto& font : familyNode.children) {
                if (font.name != "font") continue;
                auto filename = trim(font.text);
                if (filename.empty()) continue;
                TgfxFontStyleSet::Entry entry;
                entry.path = filename.front() == '/' ? filename : std::string(fontDirectory) + filename;
                auto weight = font.attribute("weight");
                entry.style = FontStyle(FontWidthNormal,
                                        weightFromCSS(weight.empty() ? 400 : std::atoi(weight.c_str())),
                                        font.attribute("style") == "italic" ? FontSlantItalic : FontSlantUpright);
                auto index = font.attribute("index");
                entry.collectionIndex = index.empty() ? 0 : std::atoi(index.c_str());
                entry.supportedAxes = font.attribute("supportedAxes");
                for (const auto& axis : font.children) {
                    if (axis.name != "axis") continue;
                    auto tag = axis.attribute("tag");
                    auto value = axis.attribute("stylevalue");
                    if (tag.size() != 4 || value.empty()) continue;
                    uint32_t axisTag = (static_cast<uint32_t>(static_cast<unsigned char>(tag[0])) << 24) |
                                       (static_cast<uint32_t>(static_cast<unsigned char>(tag[1])) << 16) |
                                       (static_cast<uint32_t>(static_cast<unsigned char>(tag[2])) << 8) |
                                       static_cast<unsigned char>(tag[3]);
                    entry.variations.emplace_back(axisTag, std::strtof(value.c_str(), nullptr));
                }
                auto fallbackFor = font.attribute("fallbackFor");
                if (!fallbackFor.empty()) {
                    Family specific;
                    specific.languages = family.languages;
                    specific.fallbackFor = std::move(fallbackFor);
                    specific.styles = std::make_shared<TgfxFontStyleSet>(
                        std::vector<TgfxFontStyleSet::Entry>{std::move(entry)});
                    add(std::move(specific));
                } else {
                    entries.push_back(std::move(entry));
                }
            }
            if (!entries.empty()) {
                if (defaultFamily.empty() && !family.name.empty()) defaultFamily = family.name;
                family.styles = std::make_shared<TgfxFontStyleSet>(std::move(entries));
                add(std::move(family));
            }
        }
        // Aliases may refer to aliases later in the document. Resolve to a fixed
        // point, bounded by the number of declarations.
        for (size_t pass = 0; pass < root.children.size(); ++pass) {
            bool changed = false;
            for (const auto& alias : root.children) {
                if (alias.name != "alias") continue;
                auto name = alias.attribute("name");
                auto target = byName.find(alias.attribute("to"));
                if (name.empty() || byName.count(name) || target == byName.end()) continue;
                auto weight = alias.attribute("weight");
                if (weight.empty()) {
                    byName[name] = target->second;
                } else {
                    auto styles = families[target->second].styles;
                    auto face = styles->matchStyle(FontStyle(FontWidthNormal,
                        weightFromCSS(std::atoi(weight.c_str())), FontSlantUpright).getSkValue());
                    if (!face) continue;
                    TgfxFontStyleSet::Entry entry;
                    entry.style = getTgfxTypefaceStyle(face);
                    entry.typeface = std::move(face);
                    add({name, {}, {}, std::make_shared<TgfxFontStyleSet>(
                        std::vector<TgfxFontStyleSet::Entry>{std::move(entry)})});
                }
                changed = true;
            }
            if (!changed) break;
        }
        return true;
    }
#endif
};

TgfxFontManager::TgfxFontManager() : _impl(std::make_unique<Impl>()) {
#if defined(__APPLE__)
    CFHandle<CFArrayRef> names(CTFontManagerCopyAvailableFontFamilyNames());
    if (names.get()) {
        for (CFIndex i = 0; i < CFArrayGetCount(names.get()); ++i) {
            auto name = stringFromCF(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names.get(), i)));
            if (!name.empty()) _impl->add({name, {}, {}, nullptr});
        }
    }
    CFHandle<CTFontRef> systemFont(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0, nullptr));
    if (systemFont.get()) {
        CFHandle<CFStringRef> name(CTFontCopyFamilyName(systemFont.get()));
        _impl->defaultFamily = stringFromCF(name.get());
        if (!_impl->byName.count(_impl->defaultFamily)) _impl->add({_impl->defaultFamily, {}, {}, nullptr});
    }
#elif defined(__ANDROID__)
    if (!_impl->readConfig("/system/etc/fonts.xml", "/system/fonts/")) {
        _impl->unsupported.emplace_back("Android system fonts.xml unavailable; legacy font XML is not supported");
    }
    _impl->readConfig("/product/etc/fonts_customization.xml", "/product/fonts/");
    auto& api = _impl->androidFonts;
    if (api.available() && api.iteratorOpen && api.iteratorNext && api.iteratorClose) {
        auto* iterator = api.iteratorOpen();
        if (iterator) {
            std::map<std::pair<std::string, std::string>, std::vector<TgfxFontStyleSet::Entry>> nativeFamilies;
            while (auto* font = api.iteratorNext(iterator)) {
                auto entry = api.entry(font);
                auto styles = TgfxFontStyleSet(std::vector<TgfxFontStyleSet::Entry>{entry});
                auto face = styles.matchStyle(entry.style.getSkValue());
                if (face) {
                    auto language = api.locale(font);
                    entry.typeface = face;
                    nativeFamilies[{face->fontFamily(), language ? language : ""}].push_back(std::move(entry));
                } else if (!entry.variations.empty()) {
                    _impl->unsupported.emplace_back("Could not create the Android variable font instance: " + entry.path);
                }
                api.close(font);
            }
            api.iteratorClose(iterator);
            for (auto& family : nativeFamilies) {
                // Keep XML alias resolution, while exposing actual native
                // system/updated-font enumeration and styles under SFNT names.
                auto key = family.first.first;
                if (!_impl->byName.count(key)) {
                    _impl->add({key, family.first.second, {},
                        std::make_shared<TgfxFontStyleSet>(std::move(family.second))});
                }
            }
        }
    }
    // Give unnamed system fallback families a stable internal lookup key and
    // expose their real SFNT names as aliases. Keep separate language/style
    // sets separate even when several files share the same SFNT family name.
    for (size_t i = 0; i < _impl->families.size(); ++i) {
        auto& family = _impl->families[i];
        if (!family.name.empty() || !family.styles) continue;
        auto face = family.styles->matchStyle(tgfx::FontStyle::Normal());
        if (!face || face->fontFamily().empty()) continue;
        auto actualName = face->fontFamily();
        family.name = actualName + "-" + std::to_string(i) + "##fallback";
        _impl->byName[family.name] = i;
        _impl->byName.emplace(std::move(actualName), i);
    }
#elif defined(__linux__)
    auto* config = FcInitLoadConfigAndFonts();
    if (config) {
        auto* pattern = FcPatternCreate();
        auto* objects = FcObjectSetBuild(FC_FAMILY, FC_FILE, FC_INDEX, FC_WEIGHT, FC_WIDTH, FC_SLANT, nullptr);
        auto* fonts = FcFontList(config, pattern, objects);
        std::map<std::string, std::vector<TgfxFontStyleSet::Entry>> families;
        if (fonts) {
            for (int i = 0; i < fonts->nfont; ++i) {
                FcChar8* name = nullptr;
                FcChar8* path = nullptr;
                auto* font = fonts->fonts[i];
                if (FcPatternGetString(font, FC_FAMILY, 0, &name) != FcResultMatch ||
                    FcPatternGetString(font, FC_FILE, 0, &path) != FcResultMatch) continue;
                int weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN, width = FC_WIDTH_NORMAL, index = 0;
                FcPatternGetInteger(font, FC_WEIGHT, 0, &weight);
                FcPatternGetInteger(font, FC_WIDTH, 0, &width);
                FcPatternGetInteger(font, FC_SLANT, 0, &slant);
                FcPatternGetInteger(font, FC_INDEX, 0, &index);
                constexpr int widths[] = {50, 63, 75, 87, 100, 113, 125, 150, 200};
                int closest = 0;
                for (int w = 1; w < 9; ++w) {
                    if (std::abs(widths[w] - width) < std::abs(widths[closest] - width)) closest = w;
                }
                TgfxFontStyleSet::Entry entry;
                entry.path = reinterpret_cast<const char*>(path);
                entry.collectionIndex = index;
                entry.style = FontStyle(static_cast<FontWidth>(closest), weightFromCSS(FcWeightToOpenType(weight)),
                    slant == FC_SLANT_ROMAN ? FontSlantUpright :
                    (slant == FC_SLANT_ITALIC ? FontSlantItalic : FontSlantOblique));
                families[reinterpret_cast<const char*>(name)].push_back(std::move(entry));
            }
            FcFontSetDestroy(fonts);
        }
        FcObjectSetDestroy(objects);
        FcPatternDestroy(pattern);
        for (auto& family : families) _impl->add({family.first, {}, {},
            std::make_shared<TgfxFontStyleSet>(std::move(family.second))});
        auto* request = FcPatternCreate();
        FcPatternAddString(request, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
        FcConfigSubstitute(config, request, FcMatchPattern);
        FcDefaultSubstitute(request);
        FcResult result;
        auto* match = FcFontMatch(config, request, &result);
        FcChar8* name = nullptr;
        if (match && FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch)
            _impl->defaultFamily = reinterpret_cast<const char*>(name);
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(request);
        FcConfigDestroy(config);
    }
#endif
}

TgfxFontManager::~TgfxFontManager() = default;

size_t TgfxFontManager::countFamilies() const { return _impl->byName.size(); }

std::string TgfxFontManager::familyName(size_t index) const {
    if (index >= _impl->byName.size()) return {};
    auto it = _impl->byName.begin();
    std::advance(it, index);
    return it->first;
}

const std::string& TgfxFontManager::defaultFamilyName() const {
    return _impl->defaultFamily;
}

const std::vector<std::string>& TgfxFontManager::unsupportedConfigurations() const {
    return _impl->unsupported;
}

std::shared_ptr<TgfxFontStyleSet> TgfxFontManager::matchFamily(const char* name) {
    std::lock_guard<std::mutex> guard(_impl->mutex);
    auto it = _impl->byName.find(name && *name ? name : _impl->defaultFamily);
    if (it == _impl->byName.end()) return nullptr;
    auto& family = _impl->families[it->second];
#if defined(__APPLE__)
    if (!family.styles) {
        CFHandle<CFStringRef> familyName(stringToCF(family.name));
        const void* keys[] = {kCTFontFamilyNameAttribute};
        const void* values[] = {familyName.get()};
        CFHandle<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFHandle<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
        CFHandle<CFSetRef> mandatory(CFSetCreate(kCFAllocatorDefault, keys, 1, &kCFTypeSetCallBacks));
        CFHandle<CFArrayRef> descriptors(CTFontDescriptorCreateMatchingFontDescriptors(descriptor.get(), mandatory.get()));
        std::vector<TgfxFontStyleSet::Entry> entries;
        if (descriptors.get()) {
            for (CFIndex i = 0; i < CFArrayGetCount(descriptors.get()); ++i) {
                auto candidate = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors.get(), i));
                CFHandle<CTFontRef> font(CTFontCreateWithFontDescriptor(candidate, 0, nullptr));
                auto face = tgfx::CTTypeface::MakeFromCTFont(font.get());
                if (face) {
                    TgfxFontStyleSet::Entry entry;
                    entry.style = getTgfxTypefaceStyle(face);
                    entry.typeface = std::move(face);
                    entries.push_back(std::move(entry));
                }
            }
        }
        if (entries.empty() && family.name == _impl->defaultFamily) {
            CFHandle<CTFontRef> font(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0, nullptr));
            auto face = tgfx::CTTypeface::MakeFromCTFont(font.get());
            if (face) {
                TgfxFontStyleSet::Entry entry;
                entry.style = getTgfxTypefaceStyle(face);
                entry.typeface = std::move(face);
                entries.push_back(std::move(entry));
            }
        }
        family.styles = std::make_shared<TgfxFontStyleSet>(std::move(entries));
    }
#endif
    return family.styles;
}

NativeTypeface TgfxFontManager::matchFamilyStyle(const char* name, NativeFontStyle style) {
#if defined(__APPLE__)
    // CoreText resolves named instances on variable system families. The exact
    // CTFont survives into TGFX and the HarfBuzz variation coordinates.
    auto family = name && *name ? std::string(name) : _impl->defaultFamily;
    if (!_impl->byName.count(family)) return nullptr;
    {
        std::lock_guard<std::mutex> guard(_impl->mutex);
        auto key = std::make_pair(family, FontStyle(style).hash());
        auto cached = _impl->instances.find(key);
        if (cached != _impl->instances.end()) return cached->second;
        CFHandle<CTFontRef> font(createCoreTextFont(family.c_str(), style));
        auto face = tgfx::CTTypeface::MakeFromCTFont(font.get());
        if (face) {
            _impl->instances.emplace(std::move(key), face);
            return face;
        }
    }
#endif
    auto set = matchFamily(name);
    return set ? set->matchStyle(style) : nullptr;
}

NativeTypeface TgfxFontManager::matchTypefaceStyle(const NativeTypeface& typeface, NativeFontStyle style) {
#if defined(__APPLE__)
    return matchFamilyStyle(typeface->fontFamily().c_str(), style);
#else
    auto variable = makeTgfxTypefaceWithStyle(typeface, FontStyle(style));
    if (variable) return variable;
    for (const auto& family : _impl->families) {
        auto set = family.styles;
        if (set && set->containsTypeface(typeface->uniqueID())) {
            return family.name.empty() ? set->matchStyle(style) : matchFamilyStyle(family.name.c_str(), style);
        }
    }
    return matchFamilyStyle(typeface->fontFamily().c_str(), style);
#endif
}

NativeTypeface TgfxFontManager::matchEmoji(NativeFontStyle style, Character character) {
#if defined(__APPLE__)
    auto face = matchFamilyStyleCharacter(nullptr, style, nullptr, 0, 0x1f600);
    if (face && face->hasColor() && face->getGlyphID(static_cast<tgfx::Unichar>(character))) return face;
#endif
    for (const auto& family : _impl->families) {
        auto set = family.name.empty() ? family.styles : matchFamily(family.name.c_str());
        auto face = set ? set->matchCharacter(style, character) : nullptr;
        if (face && face->hasColor()) return face;
    }
    return nullptr;
}

NativeTypeface TgfxFontManager::matchFamilyStyleCharacter(
    const char* name, NativeFontStyle style, const char** languages, size_t languageCount, Character character) {
    if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) return nullptr;
#if defined(__APPLE__)
    // Character fallback is a CoreText descriptor request, not a lookup in
    // our enumerated families. A registered family such as "Test Sans" need
    // not exist in that catalog. Skipping CoreText in that case makes the
    // first enumerated face with coverage override the native cascade.
    const char* family = name && _impl->byName.count(name) ? name : nullptr;
    CFHandle<CTFontRef> base(createCoreTextFont(family, style));
    if (base.get()) {
        UniChar chars[2];
        CFIndex count = 1;
        if (character <= 0xffff) chars[0] = static_cast<UniChar>(character);
        else {
            auto value = static_cast<uint32_t>(character) - 0x10000;
            chars[0] = static_cast<UniChar>(0xd800 + (value >> 10));
            chars[1] = static_cast<UniChar>(0xdc00 + (value & 0x3ff));
            count = 2;
        }
        CFHandle<CFStringRef> text(CFStringCreateWithCharacters(kCFAllocatorDefault, chars, count));
        CFHandle<CFStringRef> language(languageCount && languages && languages[0] ? stringToCF(languages[0]) : nullptr);
        CFHandle<CTFontRef> fallback(CTFontCreateForStringWithLanguage(base.get(),
            text.get(), CFRangeMake(0, count), language.get()));
        auto face = tgfx::CTTypeface::MakeFromCTFont(fallback.get());
        if (face && face->getGlyphID(static_cast<tgfx::Unichar>(character))) return face;
    }
#elif defined(__ANDROID__)
    auto& api = _impl->androidFonts;
    if (api.available() && character <= 0x10ffff) {
        auto* matcher = api.create();
        if (matcher) {
            api.setStyle(matcher, static_cast<uint16_t>(static_cast<int>(style.weight()) * 100),
                         style.slant() != tgfx::FontSlant::Upright);
            std::string locales;
            for (size_t i = 0; languages && i < languageCount; ++i) {
                if (!languages[i] || !*languages[i]) continue;
                if (!locales.empty()) locales += ',';
                locales += languages[i];
            }
            api.setLocales(matcher, locales.c_str());
            uint16_t text[2];
            uint32_t length = 1;
            if (character <= 0xffff) text[0] = static_cast<uint16_t>(character);
            else {
                auto value = character - 0x10000;
                text[0] = static_cast<uint16_t>(0xd800 + (value >> 10));
                text[1] = static_cast<uint16_t>(0xdc00 + (value & 0x3ff));
                length = 2;
            }
            auto* font = api.match(matcher, name && *name ? name : _impl->defaultFamily.c_str(),
                                    text, length, nullptr);
            NativeTypeface face;
            bool failedInstance = false;
            if (font) {
                auto entry = api.entry(font);
                bool hasCoordinates = !entry.variations.empty();
                auto key = entry.path + ":" + std::to_string(entry.collectionIndex);
                for (const auto& axis : entry.variations)
                    key += ":" + std::to_string(axis.first) + "=" + std::to_string(axis.second);
                {
                    std::lock_guard<std::mutex> guard(_impl->mutex);
                    auto cached = _impl->androidInstances.find(key);
                    if (cached != _impl->androidInstances.end()) face = cached->second;
                    else {
                        TgfxFontStyleSet set(std::vector<TgfxFontStyleSet::Entry>{std::move(entry)});
                        face = set.matchStyle(style);
                        if (face) _impl->androidInstances.emplace(std::move(key), face);
                    }
                    failedInstance = hasCoordinates && !face;
                }
                api.close(font);
            }
            api.destroy(matcher);
            if (failedInstance) return nullptr;
            // The native matcher returns tofu for uncovered input. Preserve the
            // registry's no-coverage error contract by checking the real face.
            if (face && face->getGlyphID(static_cast<tgfx::Unichar>(character))) return face;
        }
    }
    if (name && *name) {
        auto set = matchFamily(name);
        auto face = set ? set->matchCharacter(style, character) : nullptr;
        if (face) return face;
    }
#elif defined(__linux__)
    auto* config = FcInitLoadConfigAndFonts();
    if (config) {
        auto* request = FcPatternCreate();
        FcPatternAddString(request, FC_FAMILY, reinterpret_cast<const FcChar8*>(
            name && *name ? name : _impl->defaultFamily.c_str()));
        FcPatternAddInteger(request, FC_WEIGHT, FcWeightFromOpenType(static_cast<int>(style.weight()) * 100));
        constexpr int widths[] = {50, 63, 75, 87, 100, 113, 125, 150, 200};
        FcPatternAddInteger(request, FC_WIDTH, widths[static_cast<size_t>(style.width())]);
        FcPatternAddInteger(request, FC_SLANT, style.slant() == tgfx::FontSlant::Upright ? FC_SLANT_ROMAN :
            (style.slant() == tgfx::FontSlant::Italic ? FC_SLANT_ITALIC : FC_SLANT_OBLIQUE));
        auto* charset = FcCharSetCreate();
        FcCharSetAddChar(charset, character);
        FcPatternAddCharSet(request, FC_CHARSET, charset);
        FcCharSetDestroy(charset);
        auto* locales = FcLangSetCreate();
        for (size_t i = 0; languages && i < languageCount; ++i) {
            if (languages[i]) FcLangSetAdd(locales, reinterpret_cast<const FcChar8*>(languages[i]));
        }
        FcPatternAddLangSet(request, FC_LANG, locales);
        FcLangSetDestroy(locales);
        FcConfigSubstitute(config, request, FcMatchPattern);
        FcDefaultSubstitute(request);
        FcResult result;
        auto* fonts = FcFontSort(config, request, FcFalse, nullptr, &result);
        NativeTypeface selected;
        bool failedInstance = false;
        for (int i = 0; fonts && i < fonts->nfont && !selected; ++i) {
            FcChar8* path = nullptr;
            int index = 0;
            if (FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &path) != FcResultMatch) continue;
            FcPatternGetInteger(fonts->fonts[i], FC_INDEX, 0, &index);
            TgfxFontStyleSet::Entry entry;
            entry.path = reinterpret_cast<const char*>(path);
            entry.collectionIndex = index;
            FcChar8* variations = nullptr;
            if (FcPatternGetString(fonts->fonts[i], FC_FONT_VARIATIONS, 0, &variations) == FcResultMatch) {
                std::string axes(reinterpret_cast<const char*>(variations));
                size_t at = 0;
                while (at < axes.size()) {
                    auto end = axes.find(',', at);
                    auto token = axes.substr(at, end == std::string::npos ? end : end - at);
                    if (token.size() > 5 && token[4] == '=') {
                        auto tag = readU32(reinterpret_cast<const uint8_t*>(token.data()));
                        entry.variations.emplace_back(tag, std::strtof(token.c_str() + 5, nullptr));
                    }
                    if (end == std::string::npos) break;
                    at = end + 1;
                }
            }
            bool hasCoordinates = !entry.variations.empty();
            TgfxFontStyleSet set(std::vector<TgfxFontStyleSet::Entry>{std::move(entry)});
            auto instance = set.matchStyle(style);
            if (!instance && hasCoordinates) {
                failedInstance = true;
                break;
            }
            if (instance && instance->getGlyphID(static_cast<tgfx::Unichar>(character)))
                selected = std::move(instance);
        }
        if (fonts) FcFontSetDestroy(fonts);
        FcPatternDestroy(request);
        FcConfigDestroy(config);
        if (failedInstance) return nullptr;
        if (selected) return selected;
    }
#else
    if (name && *name) {
        auto set = matchFamily(name);
        auto face = set ? set->matchCharacter(style, character) : nullptr;
        if (face) return face;
    }
#endif
    // Language-specific candidates first, retaining system XML order within a
    // language. Empty-language fallback families retain their configured order.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < _impl->families.size(); ++i) {
            auto& family = _impl->families[i];
            if (!family.fallbackFor.empty() && (!name || family.fallbackFor != name)) continue;
            bool languageMatch = false;
            for (size_t language = 0; languages && language < languageCount; ++language) {
                if (languages[language] && !family.languages.empty() &&
                    family.languages.find(languages[language]) != std::string::npos) languageMatch = true;
            }
            if ((pass == 0) != languageMatch) continue;
            auto set = family.name.empty() ? family.styles : matchFamily(family.name.c_str());
            auto face = set ? set->matchCharacter(style, character) : nullptr;
            if (face) return face;
        }
    }
    return nullptr;
}

} // namespace snap::drawing
#endif
