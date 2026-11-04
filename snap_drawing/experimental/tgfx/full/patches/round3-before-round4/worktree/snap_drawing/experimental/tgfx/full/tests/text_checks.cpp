#include "text_checks.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "text_checks requires the actual complete experimental TGFX backend"
#endif

#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/DisplayList/DisplayList.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "snap_drawing/cpp/Layers/TextLayer.hpp"
#include "snap_drawing/cpp/Resources.hpp"
#include "snap_drawing/cpp/Text/FontManager.hpp"
#include "snap_drawing/cpp/Text/TextLayoutBuilder.hpp"
#include "snap_drawing/cpp/Text/TextShaper.hpp"
#include "snap_drawing/cpp/Text/Typeface.hpp"
#include "snap_drawing/cpp/Utils/LottieAnimatedImage.hpp"
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"
#include "tgfx/core/ColorSpace.h"
#include "tgfx/core/ImageInfo.h"
#include "tgfx/core/Paint.h"
#include "tgfx/core/Surface.h"
#include "hb-ot.h"
#include "hb.h"

#if defined(__APPLE__)
#include "tgfx/platform/apple/CTTypeface.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace snap::drawing;

constexpr uint32_t kWeight = HB_TAG('w', 'g', 'h', 't');
constexpr uint32_t kWidth = HB_TAG('w', 'd', 't', 'h');
constexpr uint32_t kOpticalSize = HB_TAG('o', 'p', 's', 'z');
constexpr int kFrameWidth = 768;
constexpr int kFrameHeight = 128;
constexpr float kFontSize = 48;
constexpr const char* kSampleUTF8 = "HAMBURGE";
const std::u32string kSample = U"HAMBURGE";
using Coordinates = std::vector<std::pair<uint32_t, float>>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void near(float actual, float expected, const std::string& operation, float tolerance = 0.001f) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            operation + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected));
}

std::string axisName(uint32_t axis) {
    return {static_cast<char>(axis >> 24), static_cast<char>(axis >> 16),
            static_cast<char>(axis >> 8), static_cast<char>(axis)};
}

template<typename Body>
void run(TextCheckReport& report, const std::string& name, Body&& body) {
    TextCheckResult result{name, false, {}};
    try {
        result.details = body();
        result.passed = true;
    } catch (const std::exception& error) {
        result.details = error.what();
    } catch (...) {
        result.details = "Unknown exception from actual text/backend execution";
    }
    if (result.passed) ++report.passed;
    else ++report.failed;
    report.details += std::string(result.passed ? "PASS " : "FAIL ") + name;
    if (!result.details.empty()) report.details += ": " + result.details;
    report.details += "\n";
    report.checks.push_back(std::move(result));
}

std::vector<uint8_t> fixtureBytes(const std::string& directory) {
    const auto path = directory + "/RobotoFlex.ttf";
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.is_open(), "Cannot open required pinned fixture: " + path);
    const auto length = stream.tellg();
    require(length == 1775480, "RobotoFlex fixture length differs from pinned receipt: " + path);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "Cannot read complete fixture: " + path);
    return bytes;
}

std::vector<uint8_t> collectionBytes(const std::string& directory, const char* name, size_t expectedSize = 0) {
    auto path = directory + "/" + name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.is_open(), "Cannot open required collection fixture: " + path);
    auto length = stream.tellg();
    require(length > 0 && length <= 4 * 1024 * 1024, "Invalid collection fixture length: " + path);
    auto size = static_cast<size_t>(length);
    require(!expectedSize || size == expectedSize, "Collection fixture differs from generation manifest: " + path);
    std::vector<uint8_t> bytes(size);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    require(static_cast<bool>(stream), "Cannot read full collection fixture: " + path);
    return bytes;
}

NativeTypeface loadFace(const std::string& directory) {
    auto bytes = fixtureBytes(directory);
    auto face = tgfx::Typeface::MakeFromBytes(bytes.data(), bytes.size());
    require(face != nullptr, "Actual TGFX MakeFromBytes failed for pinned Roboto Flex");
    // No caller buffer survives. All instance/HarfBuzz/raster cases therefore
    // also exercise the native face's ownership of the original font data.
    return face;
}

Ref<FontManager> manager() {
    auto value = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger());
    value->load();
    return value;
}

Ref<Font> wrap(NativeTypeface face, float size = kFontSize) {
    auto family = Valdi::StringCache::makeStringFromCLiteral("TGFX Variable Fixture");
    auto typeface = Valdi::makeShared<Typeface>(std::move(face), family, true);
    return Valdi::makeShared<Font>(nullptr, std::move(typeface), size, 1.0, false);
}

hb_ot_var_axis_info_t axisInfo(const NativeTypeface& face, uint32_t axis) {
    auto hbFace = Harfbuzz::createFace(face);
    hb_ot_var_axis_info_t info{};
    require(hb_ot_var_find_axis_info(hbFace.get(), axis, &info) != 0,
            "Pinned font has no real fvar axis " + axisName(axis));
    return info;
}

#if defined(__APPLE__)
template<typename T>
class CFHandle {
public:
    explicit CFHandle(T value) : _value(value) {}
    ~CFHandle() { if (_value) CFRelease(_value); }
    CFHandle(const CFHandle&) = delete;
    CFHandle& operator=(const CFHandle&) = delete;
    T get() const { return _value; }
private:
    T _value;
};

Coordinates coreTextCoordinates(CTFontRef font) {
    require(font != nullptr, "Cannot read a null native CoreText font");
    CFHandle<CFArrayRef> axes(CTFontCopyVariationAxes(font));
    require(axes.get() && CFArrayGetCount(axes.get()) > 0, "Native CoreText font exposes no variation axes");
    // CTFont.h: since macOS 10.12/iOS 10, CopyVariation contains only
    // non-default values. Axis metadata establishes both supported axes and
    // the defaults for omitted entries; a null/sparse dictionary is not an
    // assertion failure by itself. Numeric comparisons remain unconditional.
    CFHandle<CFDictionaryRef> current(CTFontCopyVariation(font));
    Coordinates result;
    for (CFIndex i = 0; i < CFArrayGetCount(axes.get()); ++i) {
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(axes.get(), i));
        auto key = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCTFontVariationAxisIdentifierKey));
        require(key && CFGetTypeID(key) == CFNumberGetTypeID(), "Malformed native CoreText axis identifier");
        auto value = static_cast<CFNumberRef>(current.get() ? CFDictionaryGetValue(current.get(), key) : nullptr);
        if (!value) value = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCTFontVariationAxisDefaultValueKey));
        int64_t tag = 0;
        float coordinate = 0;
        require(key && value && CFNumberGetValue(key, kCFNumberSInt64Type, &tag) &&
                    tag >= 0 && static_cast<uint64_t>(tag) <= UINT32_MAX &&
                    CFNumberGetValue(value, kCFNumberFloatType, &coordinate) && std::isfinite(coordinate),
                "Malformed actual CoreText variation axis/default");
        result.emplace_back(static_cast<uint32_t>(tag), coordinate);
    }
    return result;
}
#endif

NativeTypeface collectionFace(const std::string& directory, int index, std::string* diagnostics = nullptr) {
    auto bytes = collectionBytes(directory, "selected-face.ttc", 2344708);
#if defined(__APPLE__)
    // TGFX's Apple MakeFromBytes currently ignores ttcIndex. Select the actual
    // CoreText descriptor instead, retaining its real byte source explicitly.
    auto owner = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    CFHandle<CFDataRef> data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, owner->data(),
        static_cast<CFIndex>(owner->size()), kCFAllocatorNull));
    require(data.get() != nullptr, "Cannot create actual CoreText collection data");
    CFHandle<CFArrayRef> dataDescriptors(CTFontManagerCreateFontDescriptorsFromData(data.get()));
    auto path = directory + "/selected-face.ttc";
    CFHandle<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false));
    // Keep the plural data factory as the collection loader. URL enumeration
    // is diagnostic evidence, never a substitute that hides data rejection.
    CFHandle<CFArrayRef> descriptors(url.get() ? CTFontManagerCreateFontDescriptorsFromURL(url.get()) : nullptr);
    auto dataCount = dataDescriptors.get() ? CFArrayGetCount(dataDescriptors.get()) : -1;
    auto urlCount = descriptors.get() ? CFArrayGetCount(descriptors.get()) : -1;
    auto summary = "CoreText data descriptors=" + std::to_string(dataCount) +
        ", URL descriptors=" + std::to_string(urlCount);
    const tgfx::GlyphID expected[2][2] = {{191, 659}, {194, 373}};
    auto roboto = loadFace(directory);
    auto notoBytes = collectionBytes(directory, "NotoSans-Regular.ttf", 569208);
    auto noto = tgfx::Typeface::MakeFromBytes(notoBytes.data(), notoBytes.size());
    require(noto != nullptr, "Cannot load independent Noto source face for collection identity");
    const std::shared_ptr<tgfx::Data> cmaps[] = {
        roboto->copyTableData(HB_TAG('c', 'm', 'a', 'p')), noto->copyTableData(HB_TAG('c', 'm', 'a', 'p'))};
    require(cmaps[0] && cmaps[1], "Independent collection identity sources lack actual cmap tables");
    std::vector<NativeTypeface> dataFaces;
    auto inspect = [&](CFArrayRef array, const char* label, bool retain) {
        auto count = array ? CFArrayGetCount(array) : 0;
        for (CFIndex i = 0; i < count && i < 128; ++i) {
            auto descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(array, i));
            CFHandle<CTFontRef> candidate(CTFontCreateWithFontDescriptor(descriptor, 0, nullptr));
            auto native = tgfx::CTTypeface::MakeFromCTFont(candidate.get());
            summary += "; " + std::string(label) + std::to_string(i);
            if (native != nullptr) {
                summary += " glyphs=" + std::to_string(native->getGlyphID(0x0100)) +
                    "/" + std::to_string(native->getGlyphID(0x03a9));
            } else {
                summary += " null-font";
            }
            if (retain) dataFaces.push_back(std::move(native));
        }
    };
    inspect(dataDescriptors.get(), "data", true);
    inspect(descriptors.get(), "URL", false);
    require(index >= 0 && index < 2 && dataCount >= 2 && dataCount <= 128 &&
                dataFaces.size() == static_cast<size_t>(dataCount),
            summary + "; plural data factory must enumerate both real backing faces");
    // Observed CoreText result: 20 named Roboto Flex instances plus Noto Sans,
    // not one descriptor per TTC entry. Group actual descriptors by the full
    // independent cmap table and distinguishing glyph IDs; never reinterpret
    // descriptor index 1 as backing collection face index 1.
    bool found[2] = {false, false};
    NativeTypeface face;
    for (const auto& native : dataFaces) {
        require(native != nullptr, summary + "; enumerated collection font is null");
        for (int entry = 0; entry < 2; ++entry) {
            if (native->getGlyphID(0x0100) != expected[entry][0] ||
                native->getGlyphID(0x03a9) != expected[entry][1]) continue;
            auto cmap = native->copyTableData(HB_TAG('c', 'm', 'a', 'p'));
            require(cmap && cmap->size() == cmaps[entry]->size() &&
                        std::equal(cmap->bytes(), cmap->bytes() + cmap->size(), cmaps[entry]->bytes()),
                    summary + "; descriptor does not preserve its independent backing face cmap");
            found[entry] = true;
            if (entry == index && face == nullptr) face = native;
        }
    }
    require(found[0] && found[1] && face != nullptr,
            summary + "; both exact backing-face identities and the selected face are required");
    if (diagnostics) *diagnostics = summary;
    auto owners = std::make_shared<std::pair<decltype(owner), NativeTypeface>>(owner, face);
    return NativeTypeface(std::move(owners), face.get());
#else
    if (diagnostics) *diagnostics = "Actual FreeType TTC index=" + std::to_string(index);
    auto face = tgfx::Typeface::MakeFromBytes(bytes.data(), bytes.size(), index);
    require(face != nullptr, "Actual TGFX FreeType collection-index load failed");
    return face;
#endif
}

uint32_t be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

uint16_t be16(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

uint32_t sfntChecksum(const uint8_t* bytes, size_t size) {
    uint32_t sum = 0;
    // Independent byte-wise OpenType checksum validation, including implicit
    // zero padding of a final partial word.
    for (size_t i = 0; i < size; ++i) sum += static_cast<uint32_t>(bytes[i]) << (24 - 8 * (i % 4));
    return sum;
}

void checkSelectedGlyphs(const NativeTypeface& face) {
    // Fixed expectations independently parsed from the licensed input cmaps
    // by generate_collection.py; face 0 has IDs 191 and 659 instead.
    require(face->getGlyphID(0x0100) == 194 && face->getGlyphID(0x03a9) == 373,
            "Selected face is not Noto Sans TTC index 1 (U+0100=194, U+03A9=373)");
}

void validateSelectedSFNT(const NativeTypeface& face, const Valdi::BytesView& bytes, uint16_t expectedTables) {
    require(bytes.getSource() != nullptr && bytes.size() >= 12 && bytes.size() % 4 == 0,
            "Selected-face export has no complete owned SFNT");
    require(be32(bytes.data()) == 0x00010000, "Selected TrueType face was exported as a collection/unsupported container");
    auto count = be16(bytes.data() + 4);
    require(count == expectedTables && count <= (bytes.size() - 12) / 16, "Selected-face table directory changed/leaked other face tables");
    require(sfntChecksum(bytes.data(), bytes.size()) == 0xb1b0afba, "Selected-face whole-font checkSumAdjustment is invalid");
    for (size_t i = 0; i < count; ++i) {
        auto* record = bytes.data() + 12 + 16 * i;
        auto tag = be32(record);
        auto offset = static_cast<size_t>(be32(record + 8));
        auto size = static_cast<size_t>(be32(record + 12));
        require(offset % 4 == 0 && offset <= bytes.size() && size <= bytes.size() - offset,
                "Exported table is unaligned or out of bounds: " + axisName(tag));
        auto checksum = sfntChecksum(bytes.data() + offset, size);
        if (tag == HB_TAG('h', 'e', 'a', 'd')) {
            require(size >= 12, "Exported head table is too short");
            checksum -= be32(bytes.data() + offset + 8);
        }
        require(checksum == be32(record + 4), "Exported table checksum failed: " + axisName(tag));
        auto native = face->copyTableData(tag);
        require(native != nullptr && native->size() == size, "Exported table is not from selected native face");
        for (size_t j = 0; j < size; ++j) {
            if (tag == HB_TAG('h', 'e', 'a', 'd') && j >= 8 && j < 12) continue;
            require(bytes.data()[offset + j] == native->bytes()[j],
                    "Exported selected native table payload changed: " + axisName(tag));
        }
    }
}

NativeTypeface vary(const NativeTypeface& source, const Coordinates& coordinates) {
    require(source != nullptr, "Cannot vary a null native face");
    for (const auto& coordinate : coordinates) {
        auto info = axisInfo(source, coordinate.first);
        require(std::isfinite(coordinate.second) && coordinate.second >= info.min_value &&
                    coordinate.second <= info.max_value, "Fixture requests an out-of-range coordinate");
    }
#if defined(__APPLE__)
    auto base = tgfx::CTTypeface::GetCTFont(source.get());
    require(base != nullptr, "No native CoreText instance behind Apple TGFX face");
    auto complete = coreTextCoordinates(base);
    float previousOpticalSize = 0;
    float desiredOpticalSize = 0;
    for (auto& coordinate : complete) {
        if (coordinate.first == kOpticalSize) previousOpticalSize = coordinate.second;
        for (const auto& requested : coordinates) {
            if (coordinate.first == requested.first) coordinate.second = requested.second;
        }
        if (coordinate.first == kOpticalSize) desiredOpticalSize = coordinate.second;
    }
    CFHandle<CFMutableDictionaryRef> variations(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    require(variations.get() != nullptr, "Cannot allocate CoreText variation dictionary");
    CFHandle<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    require(attributes.get() != nullptr, "Cannot allocate CoreText font attributes");
    for (const auto& coordinate : complete) {
        int64_t tag = coordinate.first;
        CFHandle<CFNumberRef> key(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &tag));
        CFHandle<CFNumberRef> value(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &coordinate.second));
        require(key.get() && value.get(), "Cannot allocate CoreText coordinate");
        CFDictionarySetValue(variations.get(), key.get(), value.get());
        if (coordinate.first == kOpticalSize) {
            CFDictionarySetValue(attributes.get(), CFSTR("NSCTFontOpticalSizeAttribute"), value.get());
        }
    }
    CTFontRef seed = base;
    CTFontRef changedOpticalSize = nullptr;
    if (previousOpticalSize == desiredOpticalSize) {
        // Same workaround as SkTypeface_Mac::onMakeClone: CoreText can lose
        // variation changes when a variable instance is copied with unchanged
        // opsz. Move opsz to a different valid value before applying the full
        // desired coordinates. This also applies to fonts created from data.
        auto info = axisInfo(source, kOpticalSize);
        float different = info.min_value + (info.max_value - info.min_value) / 2;
        if (different == desiredOpticalSize) different = info.min_value + (info.max_value - info.min_value) / 4;
        require(different != desiredOpticalSize, "No distinct valid intermediate optical-size value");
        CFHandle<CFNumberRef> differentNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &different));
        int64_t tag = kOpticalSize;
        CFHandle<CFNumberRef> opticalKey(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &tag));
        CFHandle<CFMutableDictionaryRef> temporary(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, variations.get()));
        CFDictionarySetValue(temporary.get(), opticalKey.get(), differentNumber.get());
        CFDictionarySetValue(attributes.get(), kCTFontVariationAttribute, temporary.get());
        CFDictionarySetValue(attributes.get(), CFSTR("NSCTFontOpticalSizeAttribute"), differentNumber.get());
        CFHandle<CTFontDescriptorRef> temporaryDescriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
        changedOpticalSize = CTFontCreateCopyWithAttributes(base, 0, nullptr, temporaryDescriptor.get());
        require(changedOpticalSize != nullptr, "CoreText intermediate optical-size clone failed");
        seed = changedOpticalSize;
        CFHandle<CFNumberRef> desiredNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &desiredOpticalSize));
        CFDictionarySetValue(attributes.get(), CFSTR("NSCTFontOpticalSizeAttribute"), desiredNumber.get());
    }
    CFHandle<CTFontRef> seedOwner(changedOpticalSize);
    CFDictionarySetValue(attributes.get(), kCTFontVariationAttribute, variations.get());
    CFHandle<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
    require(descriptor.get() != nullptr, "Cannot create native variable-font descriptor");
    CFHandle<CTFontRef> instance(CTFontCreateCopyWithAttributes(seed, 0, nullptr, descriptor.get()));
    auto result = tgfx::CTTypeface::MakeFromCTFont(instance.get());
#else
    std::vector<tgfx::FontVariation> requested;
    for (const auto& coordinate : coordinates) requested.push_back({coordinate.first, coordinate.second});
    auto result = source->makeWithVariations(requested);
#endif
    require(result != nullptr, "Native variation instance creation failed; no default-instance substitute is allowed");
    require(result->uniqueID() != source->uniqueID(), "Variation clone reused the source glyph/scaler cache identity");
#if defined(__APPLE__)
    // CTTypeface::MakeFromCTFont retains CTFont, but does not carry TGFX's
    // original no-copy CFData backing owner into the clone. Retain both real
    // native faces; the alias still points to the actual cloned TGFX object.
    // FreeType's own clone-data ownership is tested without this bridge.
    auto owners = std::make_shared<std::pair<NativeTypeface, NativeTypeface>>(source, result);
    return NativeTypeface(std::move(owners), result.get());
#else
    return result;
#endif
}

float readAxis(const NativeTypeface& face, uint32_t axis) {
#if defined(__APPLE__)
    auto font = tgfx::CTTypeface::GetCTFont(face.get());
    for (const auto& coordinate : coreTextCoordinates(font)) {
        if (coordinate.first == axis) return coordinate.second;
    }
    throw std::runtime_error("Actual CoreText font does not support requested axis " + axisName(axis));
#else
    auto coordinates = face->getVariationDesignPosition();
    for (const auto& coordinate : coordinates) {
        if (coordinate.axis == axis) return coordinate.value;
    }
    throw std::runtime_error("Native TGFX variation readback omitted " + axisName(axis));
#endif
}

void checkInstance(Font& font, uint32_t axis, float expected, const std::string& phase = {}) {
    // Check the face on the actual NativeFont used to draw, after SnapDrawing's
    // platform font preparation, rather than only inspecting our input clone.
    const auto& drawnFace = font.getSkValue().getTypeface();
    near(readAxis(drawnFace, axis), expected, phase + " drawn native " + axisName(axis));
    hb_ot_var_axis_info_t info{};
    require(hb_ot_var_find_axis_info(font.typeface()->getHBFace().get(), axis, &info) != 0,
            "SnapDrawing HarfBuzz face lost the real variable axis");
    unsigned count = 0;
    auto* values = hb_font_get_var_coords_design(font.getHBFont().get(), &count);
    require(values || count == 0, "Malformed HarfBuzz design-coordinate result");
    // Unspecified HarfBuzz coordinates are axis defaults, too. A lost
    // non-default request still fails this exact numeric comparison.
    float actual = values && info.axis_index < count ? values[info.axis_index] : info.default_value;
    near(actual, expected, phase + " sized HarfBuzz " + axisName(axis));
}

std::vector<ShapedGlyph> shape(TextShaper& shaper, Font& font, const std::u32string& text,
                              bool rtl = false, float letterSpacing = 0) {
    require(!text.empty(), "Fixture requested empty shaping input");
    std::vector<Character> characters(text.begin(), text.end());
    std::vector<ShapedGlyph> glyphs;
    auto count = shaper.shape(characters.data(), characters.size(), font, rtl, letterSpacing,
                              TextScript::fromCharacter(characters.front()), glyphs);
    require(count == glyphs.size() && count > 0, "Actual text shaper returned no complete glyph sequence");
    for (const auto& glyph : glyphs) {
        require(glyph.glyphID != 0, "Actual text shaper emitted .notdef for covered fixture input");
        require(std::isfinite(glyph.advanceX) && std::isfinite(glyph.offsetX) && std::isfinite(glyph.offsetY),
                "Non-finite glyph position from actual text shaper");
    }
    return glyphs;
}

float advance(const std::vector<ShapedGlyph>& glyphs) {
    float result = 0;
    for (const auto& glyph : glyphs) result += glyph.advanceX;
    require(std::isfinite(result) && result > 0, "No positive shaped advance");
    return result;
}

Ref<TextLayout> layout(const Ref<FontManager>& fonts, const Ref<Font>& font, const std::string& text) {
    TextLayoutBuilder builder(TextAlignLeft, TextOverflowClip,
                              snap::drawing::Size::make(kFrameWidth - 16, kFrameHeight - 16), 1, fonts, false, 1);
    builder.setIncludeSegments(true);
    builder.setIncludeTextBlob(true);
    builder.append(text, font, TextLayoutLineHeight::multiple(1), 0, TextDecorationNone);
    auto result = builder.build();
    require(result != nullptr && result->fitsInMaxSize(), "Actual text layout did not fit bounded fixture surface");
    require(!result->getEntries().empty() && result->getBounds().width() > 0 &&
                result->getBounds().height() > 0, "Actual text layout has no finite visible bounds");
    return result;
}

LayerContent record(const Ref<TextLayout>& text) {
    DrawingContext drawing(kFrameWidth, kFrameHeight);
    tgfx::Paint paint;
    paint.setColor(tgfx::Color::White());
    size_t blobs = 0;
    for (const auto& entry : text->getEntries()) {
        if (entry.textBlob != nullptr) {
            drawing.canvas()->drawTextBlob(entry.textBlob, 8, 8, paint);
            ++blobs;
        }
    }
    require(blobs > 0, "Production TextLayoutBuilder emitted no drawable native text blob");
    auto result = drawing.finish();
    require(result.picture != nullptr, "Production DrawingContext did not retain recorded text");
    return result;
}

struct GPU {
    Ref<GrGraphicsContext> graphics;
    std::unique_ptr<TGFXContextLock> lock;
    GPU() : graphics(GrGraphicsContext::makeOffscreen()) {
        require(graphics != nullptr, "Cannot create real platform offscreen text device");
        lock = graphics->lockContext();
        require(lock != nullptr && static_cast<bool>(*lock), "Cannot lock real TGFX text device");
    }
};

template<typename Draw>
std::vector<uint8_t> pixelsForDraw(GPU& gpu, Draw&& draw) {
    auto surface = tgfx::Surface::Make(gpu.lock->get(), kFrameWidth, kFrameHeight,
        tgfx::ColorType::RGBA_8888, 1, false, 0, tgfx::ColorSpace::SRGB());
    require(surface != nullptr, "Cannot create actual RGBA8 text surface");
    surface->getCanvas()->clear(tgfx::Color::Transparent());
    draw(surface->getCanvas());
    require(!surface->getCanvas()->hasError(), "Native text picture replay failed");
    auto info = tgfx::ImageInfo::Make(kFrameWidth, kFrameHeight,
        tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    std::vector<uint8_t> pixels(info.byteSize(), 0xcd);
    require(surface->readPixels(info, pixels.data()), "Actual text GPU readback failed");
    require(!surface->getCanvas()->hasError(), "Native text GPU readback reported a render error");
    return pixels;
}

std::vector<uint8_t> render(GPU& gpu, const LayerContent& content) {
    return pixelsForDraw(gpu, [&](NativeCanvas* canvas) { canvas->drawPicture(content.picture); });
}

uint64_t ink(const std::vector<uint8_t>& pixels) {
    uint64_t result = 0;
    for (size_t i = 3; i < pixels.size(); i += 4) result += pixels[i];
    require(result > 0, "Actual text raster has no glyph coverage");
    return result;
}

size_t changedAlpha(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second) {
    require(first.size() == second.size() && !first.empty(), "Text readback sizes differ");
    ink(first);
    ink(second);
    size_t changed = 0;
    for (size_t i = 3; i < first.size(); i += 4) {
        if (first[i] != second[i]) ++changed;
    }
    require(changed > 0, "Distinct variable instances rasterized identical glyph coverage");
    return changed;
}

void matchingGlyphInteriors(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& reference) {
    require(actual.size() == reference.size(), "ThorVG/native text readback dimensions differ");
    ink(actual);
    ink(reference);
    auto bounds = [](const std::vector<uint8_t>& pixels) {
        std::array<int, 4> box{kFrameWidth, kFrameHeight, -1, -1};
        for (int y = 0; y < kFrameHeight; ++y) {
            for (int x = 0; x < kFrameWidth; ++x) {
                if (pixels[(static_cast<size_t>(y) * kFrameWidth + x) * 4 + 3]) {
                    box[0] = std::min(box[0], x); box[1] = std::min(box[1], y);
                    box[2] = std::max(box[2], x); box[3] = std::max(box[3], y);
                }
            }
        }
        return box;
    };
    auto actualBounds = bounds(actual);
    auto referenceBounds = bounds(reference);
    for (size_t i = 0; i < actualBounds.size(); ++i) {
        require(std::abs(actualBounds[i] - referenceBounds[i]) <= 3,
                "ThorVG selected-face glyph bounds differ from the real native selected face");
    }
    for (const auto& pair : {std::make_pair(&actual, &reference), std::make_pair(&reference, &actual)}) {
        size_t interiors = 0;
        size_t matching = 0;
        for (int y = 1; y < kFrameHeight - 1; ++y) {
            for (int x = 1; x < kFrameWidth - 1; ++x) {
                auto offset = (static_cast<size_t>(y) * kFrameWidth + x) * 4 + 3;
                const auto& a = *pair.first;
                if (a[offset] >= 240 && a[offset - 4] >= 240 && a[offset + 4] >= 240 &&
                    a[offset - kFrameWidth * 4] >= 240 && a[offset + kFrameWidth * 4] >= 240) {
                    ++interiors;
                    if ((*pair.second)[offset] >= 200) ++matching;
                }
            }
        }
        require(interiors > 10 && matching * 5 >= interiors * 4,
                "ThorVG/native selected-face stable glyph interiors differ (80% required in both directions)");
    }
}

LayerContent instancePicture(const std::string& directory, uint32_t axis, float value) {
    auto fonts = manager();
    auto font = wrap(vary(loadFace(directory), {{axis, value}}));
    checkInstance(*font, axis, value);
    // Font, typeface, layout, builder and recorder all die before GPU replay.
    // The actual picture/text blob must retain the correct drawing instance.
    return record(layout(fonts, font, kSampleUTF8));
}

Ref<Font> registeredFont(const Ref<FontManager>& fonts, const std::string& directory,
                         const char* name, bool fallback, bool invalidateCaller = false) {
    auto bytes = fixtureBytes(directory);
    auto family = Valdi::StringCache::makeStringFromCLiteral(name);
    fonts->registerTypeface(family, FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright), fallback,
                            Valdi::BytesView(nullptr, bytes.data(), bytes.size()));
    if (invalidateCaller) {
        // Registration is lazy: force the borrowed source to become invalid
        // before the first font load, not merely after the native copy exists.
        std::fill(bytes.begin(), bytes.end(), 0);
        std::vector<uint8_t>().swap(bytes);
    }
    auto result = fonts->getFontWithFamilyNameAndStyle(family,
        FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright), kFontSize, 1, false);
    require(result.success(), std::string("Production registered custom-font load failed: ") + result.description());
    return result.moveValue();
}

constexpr int kShadowWidth = 512;
constexpr int kShadowHeight = 256;
constexpr float kShadowSigma = 3;

Ref<DisplayList> textShadowScene(const Ref<FontManager>& fonts, const Ref<Font>& font,
                                  float displayScale, float sigma, bool emoji = false,
                                  bool shadow = true) {
    auto resources = Valdi::makeShared<Resources>(fonts, displayScale, Valdi::ConsoleLogger::getLogger());
    auto layer = makeLayer<TextLayer>(resources);
    // A nonzero offset keeps radius-zero shadows visible through the actual
    // TextLayer policy. Multiplying offsets by displayScale preserves the same
    // local placement while its onDraw uses 1/displayScale for native glyphs.
    layer->setFrame(snap::drawing::Rect::makeXYWH(shadow ? 24 : 40, shadow ? 16 : 32, 180, 90));
    layer->setTextFont(font);
    layer->setText(Valdi::StringCache::makeStringFromCLiteral(emoji ? "\xF0\x9F\x98\x8A" : "H"));
    layer->setTextColor(shadow ? Color::transparent() : Color::white());
    if (shadow) layer->setTextShadow(Color::white(), sigma, 1, 16 * displayScale, 16 * displayScale);
    auto list = Valdi::makeShared<DisplayList>(snap::drawing::Size::make(256, 128), TimePoint());
    DrawMetrics metrics;
    layer->draw(*list, metrics);
    require(metrics.drawCacheMiss > 0, "Actual TextLayer did not record glyph/shadow content");
    // The TextLayer, resources and caller font references are not captured by
    // this returned list. Production picture/blob/deferred ownership is used.
    return list;
}

std::vector<uint8_t> shadowPixels(GPU& gpu, const Ref<DisplayList>& list, float replayScale,
                                   const tgfx::Rect* clip = nullptr) {
    auto surface = tgfx::Surface::Make(gpu.lock->get(), kShadowWidth, kShadowHeight,
        tgfx::ColorType::RGBA_8888, 1, false, 0, tgfx::ColorSpace::SRGB());
    require(surface != nullptr, "Cannot create real TextLayer shadow destination");
    auto* canvas = surface->getCanvas();
    canvas->clear(tgfx::Color::Transparent());
    if (clip) canvas->clipRect(*clip, false);
    DrawableSurfaceCanvas destination(surface.get(), kShadowWidth, kShadowHeight);
    list->draw(destination, kDisplayListAllPlaneIndexes, replayScale, replayScale, false);
    require(canvas->getSaveCount() == 0 && !canvas->hasError(), "TextLayer shadow replay leaked state or failed");
    auto info = tgfx::ImageInfo::Make(kShadowWidth, kShadowHeight,
        tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    std::vector<uint8_t> pixels(info.byteSize(), 0xcd);
    require(surface->readPixels(info, pixels.data()) && !canvas->hasError(), "Real TextLayer shadow readback failed");
    return pixels;
}

struct AlphaMoments {
    double varianceX;
    double varianceY;
    double centerX;
};

AlphaMoments alphaMoments(const std::vector<uint8_t>& pixels) {
    require(pixels.size() == static_cast<size_t>(kShadowWidth) * kShadowHeight * 4, "Invalid shadow readback size");
    double mass = 0, sumX = 0, sumY = 0, sumXX = 0, sumYY = 0;
    for (int y = 0; y < kShadowHeight; ++y) {
        for (int x = 0; x < kShadowWidth; ++x) {
            auto alpha = pixels[(static_cast<size_t>(y) * kShadowWidth + x) * 4 + 3];
            if (!alpha) continue;
            require(x > 2 && y > 2 && x < kShadowWidth - 3 && y < kShadowHeight - 3,
                    "Shadow moment oracle has clipped coverage at destination edge");
            double weight = alpha;
            double px = x, py = y;
            mass += weight;
            sumX += weight * px; sumY += weight * py;
            sumXX += weight * px * px; sumYY += weight * py * py;
        }
    }
    require(mass > 255, "Actual TextLayer shadow has no substantial glyph alpha coverage");
    auto meanX = sumX / mass;
    auto meanY = sumY / mass;
    return {sumXX / mass - meanX * meanX, sumYY / mass - meanY * meanY, meanX};
}

std::pair<double, double> addedShadowVariance(const std::vector<uint8_t>& blurred,
                                              const std::vector<uint8_t>& sharp) {
    const auto b = alphaMoments(blurred);
    const auto s = alphaMoments(sharp);
    auto x = b.varianceX - s.varianceX;
    auto y = b.varianceY - s.varianceY;
    // Convolution adds the blur kernel's variance to the actual glyph alpha
    // variance. Sigma 3 adds approximately 9 device-pixel squared per axis.
    // Bounds allow finite kernel tails/8-bit quantization, but reject no blur
    // and the approximately 36 added by a sigma-3 mask stretched 2x on replay.
    require(std::isfinite(x) && std::isfinite(y) && x >= 5 && x <= 14 && y >= 5 && y <= 14,
            "Fixed device sigma=3 violated: added variance x=" + std::to_string(x) + ", y=" + std::to_string(y));
    return {x, y};
}

void sameShadowVariance(const std::pair<double, double>& first, const std::pair<double, double>& second) {
    require(std::abs(first.first - second.first) <= 3 && std::abs(first.second - second.second) <= 3,
            "Device blur variance changed across recording/replay scale: " +
                std::to_string(first.first) + "/" + std::to_string(first.second) + " -> " +
                std::to_string(second.first) + "/" + std::to_string(second.second));
}

void addTextShadowChecks(TextCheckReport& report, const std::string& directory) {
    run(report, "text.shadow-device-radius-1x", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto sharp = textShadowScene(fonts, font, 1, 0);
        auto blurred = textShadowScene(fonts, font, 1, kShadowSigma);
        GPU gpu;
        auto variance = addedShadowVariance(shadowPixels(gpu, blurred, 1), shadowPixels(gpu, sharp, 1));
        return "Actual TextLayer sigma=3 added device variance=" +
            std::to_string(variance.first) + "/" + std::to_string(variance.second);
    });
    run(report, "text.shadow-device-radius-picture-replay-2x", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto sharp = textShadowScene(fonts, font, 1, 0);
        auto blurred = textShadowScene(fonts, font, 1, kShadowSigma);
        GPU gpu;
        auto one = addedShadowVariance(shadowPixels(gpu, blurred, 1), shadowPixels(gpu, sharp, 1));
        auto two = addedShadowVariance(shadowPixels(gpu, blurred, 2), shadowPixels(gpu, sharp, 2));
        sameShadowVariance(one, two);
        // Replay the same captured pictures again, after 2x cached glyph use.
        auto again = addedShadowVariance(shadowPixels(gpu, blurred, 1), shadowPixels(gpu, sharp, 1));
        sameShadowVariance(one, again);
        return std::string("Same real TextLayer pictures keep device sigma through 1x -> 2x -> 1x playback");
    });
    run(report, "text.shadow-device-radius-display-scale-2x", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto sharp = textShadowScene(fonts, font, 2, 0);
        auto blurred = textShadowScene(fonts, font, 2, kShadowSigma);
        GPU gpu;
        auto one = addedShadowVariance(shadowPixels(gpu, blurred, 1), shadowPixels(gpu, sharp, 1));
        auto two = addedShadowVariance(shadowPixels(gpu, blurred, 2), shadowPixels(gpu, sharp, 2));
        sameShadowVariance(one, two);
        return std::string("Actual displayScale=2 font layout and inverse recording matrix preserve device blur at both replay scales");
    });
    run(report, "text.shadow-playback-clip-preserves-interior-halo", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto sharp = textShadowScene(fonts, font, 1, 0);
        auto blurred = textShadowScene(fonts, font, 1, kShadowSigma);
        GPU gpu;
        auto shape = shadowPixels(gpu, sharp, 2);
        auto complete = shadowPixels(gpu, blurred, 2);
        int edge = static_cast<int>(std::floor(alphaMoments(shape).centerX));
        auto clip = tgfx::Rect::MakeLTRB(static_cast<float>(edge), 0, kShadowWidth, kShadowHeight);
        auto clipped = shadowPixels(gpu, blurred, 2, &clip);
        size_t interiorInk = 0;
        for (int y = 0; y < kShadowHeight; ++y) {
            for (int x = 0; x < kShadowWidth; ++x) {
                auto i = (static_cast<size_t>(y) * kShadowWidth + x) * 4 + 3;
                if (x < edge) require(clipped[i] == 0, "Deferred text shadow ignored destination clip");
                else {
                    require(std::abs(static_cast<int>(clipped[i]) - complete[i]) <= 1,
                            "Destination clip truncated contributing glyph alpha before device blur");
                    if (clipped[i]) ++interiorInk;
                }
            }
        }
        require(interiorInk > 0, "Clip oracle did not intersect actual glyph/shadow coverage");
        return std::string("Actual playback clip removes outside pixels without creating a false blurred edge inside");
    });
    run(report, "text.shadow-color-emoji-alpha-silhouette", [&] {
        auto fonts = manager();
        auto result = fonts->getEmojiFont(32, 1);
        require(result.success(), std::string("Cannot resolve real shadow emoji: ") + result.description());
        auto font = result.moveValue();
        auto mask = textShadowScene(fonts, font, 1, 0, true);
        auto color = textShadowScene(fonts, font, 1, 0, true, false);
        GPU gpu;
        auto actual = shadowPixels(gpu, mask, 1);
        auto reference = shadowPixels(gpu, color, 1);
        size_t inkCount = 0, colorCount = 0;
        for (size_t i = 0; i < actual.size(); i += 4) {
            require(std::abs(static_cast<int>(actual[i + 3]) - reference[i + 3]) <= 1,
                    "Text shadow did not retain actual color/bitmap emoji alpha");
            if (actual[i + 3]) ++inkCount;
            if (reference[i + 3] && (reference[i] != reference[i + 1] || reference[i + 1] != reference[i + 2])) ++colorCount;
            for (size_t channel = 0; channel < 3; ++channel) {
                require(std::abs(static_cast<int>(actual[i + channel]) - actual[i + 3]) <= 1,
                        "Emoji RGB leaked into the white alpha-only text shadow");
            }
        }
        require(inkCount > 0 && colorCount > 0, "Real color emoji positive control has no visible colored glyphs");
        return std::string("Production TextLayer shadow matches actual color glyph alpha, rather than a rectangle or outline");
    });
    run(report, "text.shadow-color-emoji-device-radius-replay-2x", [&] {
        auto fonts = manager();
        auto result = fonts->getEmojiFont(32, 1);
        require(result.success(), std::string("Cannot resolve actual emoji radius control: ") + result.description());
        auto font = result.moveValue();
        auto sharp = textShadowScene(fonts, font, 1, 0, true);
        auto blurred = textShadowScene(fonts, font, 1, kShadowSigma, true);
        GPU gpu;
        auto one = addedShadowVariance(shadowPixels(gpu, blurred, 1), shadowPixels(gpu, sharp, 1));
        auto two = addedShadowVariance(shadowPixels(gpu, blurred, 2), shadowPixels(gpu, sharp, 2));
        sameShadowVariance(one, two);
        return std::string("Real color/bitmap glyph alpha is resolved and blurred at final playback device scale");
    });
}

Ref<DisplayList> composedTextShadowScene(const Ref<FontManager>& fonts, const Ref<Font>& font,
                                         float sigma, float pictureOpacity, int groupDepth, bool emoji) {
    auto resources = Valdi::makeShared<Resources>(fonts, 1, Valdi::ConsoleLogger::getLogger());
    auto text = makeLayer<TextLayer>(resources);
    text->setFrame(snap::drawing::Rect::makeXYWH(24, 16, 180, 90));
    text->setTextFont(font);
    text->setText(Valdi::StringCache::makeStringFromCLiteral(emoji ? "\xF0\x9F\x98\x8A" : "H"));
    // Keep actual foreground and shadow draws in the leaf picture. A
    // transparent foreground can optimize the picture to a single operation
    // and miss RenderContext's multi-draw picture-opacity/offscreen route.
    text->setTextColor(Color::red());
    text->setTextShadow(Color::blue(), sigma, 1, 96, 16);
    text->setOpacity(pictureOpacity);
    Ref<Layer> content = text;
    for (int level = 0; level < groupDepth; ++level) {
        auto group = makeLayer<Layer>(resources);
        group->setFrame(snap::drawing::Rect::makeXYWH(0, 0, 256, 128));
        group->setOpacity(0.75f);
        // A visible nonoverlapping peer prevents a nominal group from being
        // merely a single picture whose opacity could be folded into it.
        auto peer = makeLayer<Layer>(resources);
        peer->setFrame(snap::drawing::Rect::makeXYWH(static_cast<float>(4 + 8 * level), 4, 6, 6));
        peer->setBackgroundColor(Color::green());
        group->addChild(peer);
        group->addChild(content);
        content = group;
    }
    auto list = Valdi::makeShared<DisplayList>(snap::drawing::Size::make(256, 128), TimePoint());
    DrawMetrics metrics;
    content->draw(*list, metrics);
    require(metrics.drawCacheMiss >= 1 + 2 * groupDepth,
            "Real TextLayer and nested opacity-group peers were not recorded");
    return list;
}

std::vector<uint8_t> composedTextShadowPixels(GPU& gpu, const Ref<DisplayList>& list,
                                               float scaleX, float scaleY, const tgfx::Rect* clip = nullptr) {
    auto surface = tgfx::Surface::Make(gpu.lock->get(), kShadowWidth, kShadowHeight,
        tgfx::ColorType::RGBA_8888, 1, false, 0, tgfx::ColorSpace::SRGB());
    require(surface != nullptr, "Cannot create actual composed TextLayer destination");
    auto* canvas = surface->getCanvas();
    canvas->clear(tgfx::Color::Transparent());
    if (clip) canvas->clipRect(*clip, false);
    DrawableSurfaceCanvas destination(surface.get(), kShadowWidth, kShadowHeight);
    list->draw(destination, kDisplayListAllPlaneIndexes, scaleX, scaleY, false);
    require(canvas->getSaveCount() == 0 && !canvas->hasError(), "Opacity/offscreen text replay failed or leaked canvas state");
    auto info = tgfx::ImageInfo::Make(kShadowWidth, kShadowHeight,
        tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    std::vector<uint8_t> result(info.byteSize(), 0xcd);
    require(surface->readPixels(info, result.data()) && !canvas->hasError(),
            "Actual composed TextLayer GPU readback failed");
    return result;
}

std::vector<uint8_t> composedShadowAlpha(const std::vector<uint8_t>& rgba, float scaleX, float scaleY,
                                         bool emoji, bool requireForeground = true) {
    require(rgba.size() == static_cast<size_t>(kShadowWidth) * kShadowHeight * 4, "Invalid composed text RGBA readback");
    std::vector<uint8_t> result(rgba.size(), 0);
    auto cut = static_cast<int>(96 * scaleX);
    auto foregroundLeft = static_cast<int>(std::floor(24 * scaleX));
    auto foregroundRight = static_cast<int>(std::ceil(72 * scaleX));
    auto foregroundTop = static_cast<int>(std::floor(16 * scaleY));
    auto foregroundBottom = static_cast<int>(std::ceil(96 * scaleY));
    size_t foreground = 0, shadow = 0;
    for (int y = 0; y < kShadowHeight; ++y) {
        for (int x = 0; x < kShadowWidth; ++x) {
            auto i = (static_cast<size_t>(y) * kShadowWidth + x) * 4;
            // Read an actually separated blue-shadow region. Color emoji can
            // contain blue in their foreground, so color-channel isolation
            // alone over the whole frame would be an invalid radius oracle.
            if (x >= cut) {
                require(rgba[i] == 0 && rgba[i + 1] == 0 &&
                            std::abs(static_cast<int>(rgba[i + 2]) - rgba[i + 3]) <= 1,
                        "Separated blue shadow contains foreground color or lost actual glyph alpha");
                result[i + 2] = result[i + 3] = rgba[i + 2];
                if (result[i + 3]) ++shadow;
            }
            if (x >= cut - 8 && x < cut + 8) {
                require(rgba[i + 3] == 0, "Radius oracle's foreground/shadow separation band contains coverage");
            }
            if (x >= foregroundLeft && x < foregroundRight &&
                y >= foregroundTop && y < foregroundBottom && rgba[i + 3]) {
                bool colored = rgba[i] != rgba[i + 1] || rgba[i + 1] != rgba[i + 2];
                bool red = rgba[i] > rgba[i + 1] && rgba[i] > rgba[i + 2];
                if (emoji ? colored : red) ++foreground;
            }
        }
    }
    if (requireForeground) require(foreground > 0, "Real composed picture lost its visible foreground positive control");
    require(shadow > 0, "Real composed picture has no separated blue glyph-shadow coverage");
    return result;
}

double shadowAlphaMass(const std::vector<uint8_t>& pixels) {
    uint64_t sum = 0;
    for (size_t i = 3; i < pixels.size(); i += 4) sum += pixels[i];
    require(sum > 0, "No actual alpha mass for composition opacity control");
    return static_cast<double>(sum);
}

struct ComposedTextScenes {
    Ref<DisplayList> sharp;
    Ref<DisplayList> blurred;
    Ref<DisplayList> opaqueSharp;
    float opacity;
    bool emoji;
};

ComposedTextScenes compositionScenes(const Ref<FontManager>& fonts, const Ref<Font>& font,
                                      float opacity, int groupDepth, bool emoji) {
    return {composedTextShadowScene(fonts, font, 0, opacity, groupDepth, emoji),
            composedTextShadowScene(fonts, font, kShadowSigma, opacity, groupDepth, emoji),
            composedTextShadowScene(fonts, font, 0, 1, 0, emoji),
            static_cast<float>(opacity * std::pow(0.75, groupDepth)), emoji};
}

std::pair<double, double> composedDeviceRadius(GPU& gpu, const ComposedTextScenes& scene,
                                               float scaleX, float scaleY) {
    auto sharp = composedShadowAlpha(composedTextShadowPixels(gpu, scene.sharp, scaleX, scaleY),
                                     scaleX, scaleY, scene.emoji);
    auto blurred = composedShadowAlpha(composedTextShadowPixels(gpu, scene.blurred, scaleX, scaleY),
                                       scaleX, scaleY, scene.emoji);
    auto opaque = composedShadowAlpha(composedTextShadowPixels(gpu, scene.opaqueSharp, scaleX, scaleY),
                                      scaleX, scaleY, scene.emoji);
    auto alphaRatio = shadowAlphaMass(sharp) / shadowAlphaMass(opaque);
    require(std::abs(alphaRatio - scene.opacity) <= std::max(0.02, 0.10 * scene.opacity),
            "Actual picture/group opacity was ignored or applied twice: got " +
                std::to_string(alphaRatio) + ", expected " + std::to_string(scene.opacity));
    return addedShadowVariance(blurred, sharp);
}

void addComposedTextShadowChecks(TextCheckReport& report, const std::string& directory) {
    run(report, "text.shadow-direct-nonuniform-device-radius", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto scene = compositionScenes(fonts, font, 1, 0, false);
        GPU gpu;
        auto variance = composedDeviceRadius(gpu, scene, 2, 1);
        return "Visible foreground + shadow, direct final scale(2,1): variance=" +
            std::to_string(variance.first) + "/" + std::to_string(variance.second);
    });
    run(report, "text.shadow-picture-opacity-half-nonuniform-device-radius", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto direct = compositionScenes(fonts, font, 1, 0, false);
        auto opacity = compositionScenes(fonts, font, 0.5f, 0, false);
        GPU gpu;
        auto reference = composedDeviceRadius(gpu, direct, 2, 1);
        auto first = composedDeviceRadius(gpu, opacity, 2, 1);
        sameShadowVariance(reference, first);
        // Same immutable multi-draw pictures must resolve independently for
        // each final transform, including after an offscreen texture was used.
        auto rotatedScale = composedDeviceRadius(gpu, opacity, 1, 2);
        sameShadowVariance(first, rotatedScale);
        auto again = composedDeviceRadius(gpu, opacity, 2, 1);
        sameShadowVariance(first, again);
        return std::string("Actual leaf TextLayer picture opacity .5 preserves device sigma through (2,1)->(1,2)->(2,1)");
    });
    run(report, "text.shadow-nested-opacity-offscreens-nonuniform-device-radius", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto direct = compositionScenes(fonts, font, 1, 0, false);
        auto nested = compositionScenes(fonts, font, 0.5f, 2, false);
        GPU gpu;
        sameShadowVariance(composedDeviceRadius(gpu, direct, 2, 1), composedDeviceRadius(gpu, nested, 2, 1));
        return std::string("Two real saveLayer groups (.75 each) plus leaf picture opacity .5 preserve final per-axis device radius");
    });
    run(report, "text.shadow-nested-opacity-final-clip-preserves-halo", [&] {
        auto fonts = manager();
        auto font = wrap(loadFace(directory), 32);
        auto nested = compositionScenes(fonts, font, 0.5f, 1, false);
        GPU gpu;
        composedDeviceRadius(gpu, nested, 2, 1);
        auto sharp = composedShadowAlpha(composedTextShadowPixels(gpu, nested.sharp, 2, 1), 2, 1, false);
        auto complete = composedShadowAlpha(composedTextShadowPixels(gpu, nested.blurred, 2, 1), 2, 1, false);
        auto edge = static_cast<int>(std::floor(alphaMoments(sharp).centerX));
        auto clip = tgfx::Rect::MakeLTRB(static_cast<float>(edge), 0, kShadowWidth, kShadowHeight);
        // This clip intentionally removes the foreground; its positive
        // control was checked in the full frame immediately above.
        auto clipped = composedShadowAlpha(composedTextShadowPixels(gpu, nested.blurred, 2, 1, &clip),
                                            2, 1, false, false);
        size_t kept = 0;
        for (int y = 0; y < kShadowHeight; ++y) {
            for (int x = 0; x < kShadowWidth; ++x) {
                auto i = (static_cast<size_t>(y) * kShadowWidth + x) * 4 + 3;
                if (x < edge) require(clipped[i] == 0, "Nested offscreen text shadow escaped final destination clip");
                else {
                    require(std::abs(static_cast<int>(clipped[i]) - complete[i]) <= 2,
                            "Nested opacity/offscreen clip cut contributing text-shadow alpha inside final clip");
                    if (clipped[i]) ++kept;
                }
            }
        }
        require(kept > 0, "Nested opacity clip did not retain actual glyph-shadow pixels");
        auto afterClip = composedShadowAlpha(composedTextShadowPixels(gpu, nested.blurred, 2, 1), 2, 1, false);
        require(afterClip == complete, "Final-clip replay contaminated the same nested offscreen picture's later unclipped shadow");
        return std::string("Nested picture/group offscreens preserve the final clip and the contributing outside-source blur halo");
    });
    run(report, "text.shadow-color-emoji-opacity-half-nonuniform-device-radius", [&] {
        auto fonts = manager();
        auto result = fonts->getEmojiFont(32, 1);
        require(result.success(), std::string("Cannot resolve actual composed emoji: ") + result.description());
        auto font = result.moveValue();
        auto direct = compositionScenes(fonts, font, 1, 0, true);
        auto opacity = compositionScenes(fonts, font, 0.5f, 0, true);
        GPU gpu;
        sameShadowVariance(composedDeviceRadius(gpu, direct, 2, 1), composedDeviceRadius(gpu, opacity, 2, 1));
        return std::string("Real visible color emoji plus separated alpha shadow retain device sigma under picture opacity .5");
    });
    run(report, "text.shadow-color-emoji-nested-offscreen-nonuniform-device-radius", [&] {
        auto fonts = manager();
        auto result = fonts->getEmojiFont(32, 1);
        require(result.success(), std::string("Cannot resolve actual nested emoji: ") + result.description());
        auto font = result.moveValue();
        auto direct = compositionScenes(fonts, font, 1, 0, true);
        auto nested = compositionScenes(fonts, font, 0.5f, 1, true);
        GPU gpu;
        sameShadowVariance(composedDeviceRadius(gpu, direct, 2, 1), composedDeviceRadius(gpu, nested, 2, 1));
        return std::string("Real color/bitmap glyph alpha survives nested saveLayer and picture-opacity transforms with fixed device radius");
    });
}
} // namespace

TextCheckReport runTextChecks(const std::string& fixtureDirectory) {
    TextCheckReport report;

    run(report, "text.fixture-real-variable-tables", [&] {
        auto face = loadFace(fixtureDirectory);
        require(face->unitsPerEm() > 0 && face->glyphsCount() > 0, "Fixture has no real font metrics/glyphs");
        auto hbFace = Harfbuzz::createFace(face);
        require(hb_ot_var_get_axis_count(hbFace.get()) == 13, "Pinned Roboto Flex must expose all 13 fvar axes");
        for (const auto tag : {HB_TAG('f', 'v', 'a', 'r'), HB_TAG('g', 'v', 'a', 'r'),
                               HB_TAG('H', 'V', 'A', 'R'), HB_TAG('G', 'S', 'U', 'B'),
                               HB_TAG('G', 'P', 'O', 'S')}) {
            auto table = face->copyTableData(tag);
            require(table != nullptr && !table->empty(), "Missing real fixture table " + axisName(tag));
        }
        auto weight = axisInfo(face, kWeight);
        auto width = axisInfo(face, kWidth);
        near(weight.min_value, 100, "Pinned wght minimum");
        near(weight.max_value, 1000, "Pinned wght maximum");
        near(width.min_value, 25, "Pinned wdth minimum");
        near(width.max_value, 151, "Pinned wdth maximum");
        return std::string("Pinned 1,775,480-byte fixture; 13 real axes and variation/layout tables");
    });

    run(report, "text.system-enumeration-default-metrics", [&] {
        auto fonts = manager();
        auto names = fonts->getAvailableFonts();
        require(!names.empty(), "Actual platform font enumeration is empty");
        auto result = fonts->getDefaultFont(24, 1);
        require(result.success(), std::string("Actual default font resolution failed: ") + result.description());
        auto font = result.moveValue();
        const auto& metrics = font->metrics();
        require(std::isfinite(metrics.ascent) && std::isfinite(metrics.descent) &&
                    metrics.descent > metrics.ascent && std::isfinite(metrics.lineSpacing) &&
                    metrics.lineSpacing > 0, "Invalid actual default font metrics");
        auto shaper = TextShaper::make(false);
        return "Enumerated " + std::to_string(names.size()) + " fonts; default Latin advance=" +
            std::to_string(advance(shape(*shaper, *font, U"Hello")));
    });

    struct AxisCase { uint32_t tag; float low; float high; };
    const AxisCase axes[] = {{kWeight, 100, 900}, {kWidth, 50, 150}, {kOpticalSize, 9, 100}};
    for (const auto& axis : axes) {
        run(report, "text.variable-coordinate-readback." + axisName(axis.tag), [&] {
            auto source = loadFace(fixtureDirectory);
            auto low = wrap(vary(source, {{axis.tag, axis.low}}));
            auto high = wrap(vary(source, {{axis.tag, axis.high}}));
            checkInstance(*low, axis.tag, axis.low);
            checkInstance(*high, axis.tag, axis.high);
            require(low->getFontId() != high->getFontId(), "Different variation instances share a FontId");
            return axisName(axis.tag) + " native/sized-HarfBuzz readback=" +
                std::to_string(axis.low) + "," + std::to_string(axis.high);
        });
    }

    run(report, "text.variable-harfbuzz-native-advance-and-coordinates", [&] {
        auto font = wrap(vary(loadFace(fixtureDirectory), {{kWeight, 750}, {kWidth, 65}, {kOpticalSize, 20}}));
        checkInstance(*font, kWeight, 750);
        checkInstance(*font, kWidth, 65);
        checkInstance(*font, kOpticalSize, 20);
        auto glyph = font->getSkValue().getGlyphID('H');
        require(glyph != 0, "Fixture has no H glyph");
        auto native = font->getSkValue().getAdvance(glyph);
        auto expected = static_cast<float>(std::floor(static_cast<double>(native) + 0.5));
        auto actual = static_cast<float>(hb_font_get_glyph_h_advance(font->getHBFont().get(), glyph)) / 65536.0f;
        near(actual, expected, "Public HB callback vs rounded drawing-instance advance", 1.0f / 65536);
        auto shaper = TextShaper::make(false);
        near(advance(shape(*shaper, *font, U"H")), expected, "Production single-glyph shaping advance");
        return "H native=" + std::to_string(native) + ", pre-HB rounded=" + std::to_string(actual);
    });

    run(report, "text.variable-width-shaped-advance", [&] {
        auto source = loadFace(fixtureDirectory);
        auto narrow = wrap(vary(source, {{kWidth, 50}}));
        auto wide = wrap(vary(source, {{kWidth, 150}}));
        auto shaper = TextShaper::make(false);
        auto small = advance(shape(*shaper, *narrow, kSample));
        auto large = advance(shape(*shaper, *wide, kSample));
        require(large > small, "wdth changes did not reach real shaped glyph advances");
        return "wdth 50 advance=" + std::to_string(small) + ", wdth 150 advance=" + std::to_string(large);
    });

    for (const auto& axis : axes) {
        run(report, "text.variable-layout-gpu-coverage." + axisName(axis.tag), [&] {
            auto low = instancePicture(fixtureDirectory, axis.tag, axis.low);
            auto high = instancePicture(fixtureDirectory, axis.tag, axis.high);
            GPU gpu;
            auto lowPixels = render(gpu, low);
            auto highPixels = render(gpu, high);
            auto changed = changedAlpha(lowPixels, highPixels);
            if (axis.tag == kWeight) {
                require(ink(highPixels) > ink(lowPixels), "Heavy instance did not produce more glyph coverage");
            }
            return axisName(axis.tag) + ": " + std::to_string(changed) +
                " changed alpha pixels after all source/layout owners were released";
        });
    }

    run(report, "text.variable-clone-independence-and-shaper-cache", [&] {
        auto source = vary(loadFace(fixtureDirectory), {{kWeight, 350}, {kWidth, 80}});
        auto original = wrap(source);
        checkInstance(*original, kWeight, 350, "source before clone");
        checkInstance(*original, kWidth, 80, "source before clone");
        auto shaper = TextShaper::make(true);
        auto before = shape(*shaper, *original, kSample);
        auto sibling = wrap(vary(source, {{kWeight, 900}}));
        checkInstance(*sibling, kWeight, 900, "sibling after clone");
        checkInstance(*sibling, kWidth, 80, "sibling inherited axis");
        checkInstance(*original, kWeight, 350, "source after clone");
        checkInstance(*original, kWidth, 80, "source after clone");
        shape(*shaper, *sibling, kSample);
        require(shape(*shaper, *original, kSample) == before, "Clone creation/use mutated source cached glyphs");
        shaper->clearCache();
        require(shape(*shaper, *original, kSample) == before, "Clone creation/use mutated source uncached glyphs");
        return std::string("Sibling preserves wdth; source coordinates and cold/warm glyphs remain unchanged");
    });

    run(report, "text.variable-clone-raster-cache-independence", [&] {
        auto fonts = manager();
        auto source = vary(loadFace(fixtureDirectory), {{kWeight, 100}, {kWidth, 100}});
        auto original = wrap(source);
        auto originalPicture = record(layout(fonts, original, kSampleUTF8));
        GPU gpu;
        auto before = render(gpu, originalPicture);
        auto sibling = wrap(vary(source, {{kWeight, 900}}));
        auto heavy = render(gpu, record(layout(fonts, sibling, kSampleUTF8)));
        changedAlpha(before, heavy);
        auto after = render(gpu, record(layout(fonts, original, kSampleUTF8)));
        require(after == before, "Drawing sibling instance changed source glyph raster/cache");
        return std::string("Distinct instance coverage; source RGBA readback identical before/after sibling rendering");
    });

    run(report, "text.custom-borrowed-bytes-lazy-load-and-export-lifetime", [&] {
        Ref<Font> font;
        {
            auto fonts = manager();
            font = registeredFont(fonts, fixtureDirectory, "TGFX Borrowed Fixture", false, true);
        }
        auto shaper = TextShaper::make(false);
        advance(shape(*shaper, *font, kSample));
        auto exported = font->typeface()->makeFontData();
        auto expected = fixtureBytes(fixtureDirectory);
        require(exported.getSource() != nullptr && exported.size() == expected.size() &&
                    std::equal(expected.begin(), expected.end(), exported.begin()),
                "Custom font did not export exact owned original bytes after caller/manager release");
        font = nullptr;
        require(std::equal(expected.begin(), expected.end(), exported.begin()),
                "Exported custom bytes did not outlive font/typeface");
        return std::string("Borrowed caller buffer destroyed before lazy load; exact exported bytes outlive font");
    });

    run(report, "text.custom-registered-fallback-precedes-system", [&] {
        auto fonts = manager();
        auto custom = registeredFont(fonts, fixtureDirectory, "TGFX Preferred Fallback", true);
        auto base = fonts->getDefaultFont(24, 1);
        require(base.success(), "Cannot resolve actual platform primary font");
        auto result = fonts->getCompatibleFont(base.value(), nullptr, 0, 'A');
        require(result.success(), std::string("Registered fallback lookup failed: ") + result.description());
        auto fallback = result.moveValue();
        require(fallback->typeface()->isCustom() &&
                    fallback->typeface()->familyName() == custom->typeface()->familyName() &&
                    fallback->typeface()->supportsCharacter('A'),
                "Native fallback overrode a covering explicitly registered fallback family");
        return std::string("Actual registered fallback selected ahead of system candidates");
    });

    run(report, "text.icu-bidi-segments-and-control-exclusion", [&] {
        auto shaper = TextShaper::make(false);
        const std::u32string input = U"abc \u202B\u0645\u0635\u0631\u202C xyz";
        std::vector<Character> characters(input.begin(), input.end());
        auto paragraphs = shaper->resolveParagraphs(characters.data(), characters.size(), false);
        require(!paragraphs.empty(), "Actual ICU bidi resolver returned no paragraph");
        std::vector<unsigned> coverage(characters.size(), 0);
        bool hasLTR = false;
        bool hasRTL = false;
        for (const auto& paragraph : paragraphs) {
            for (const auto& segment : paragraph.segments) {
                require(segment.start < segment.end && segment.end <= characters.size(), "Invalid bidi segment range");
                hasRTL |= segment.isRTL;
                hasLTR |= !segment.isRTL;
                for (size_t i = segment.start; i < segment.end; ++i) {
                    require(!Unicode::isBidiControl(characters[i]), "Bidi control leaked into shapeable segment");
                    ++coverage[i];
                }
            }
        }
        require(hasLTR && hasRTL, "Mixed text did not produce both visual directions");
        for (size_t i = 0; i < characters.size(); ++i) {
            require(coverage[i] == (Unicode::isBidiControl(characters[i]) ? 0u : 1u),
                    "Bidi resolution omitted or duplicated a non-control character");
        }
        return std::string("Real ICU mixed-script runs cover input once and exclude embedding controls");
    });

    run(report, "text.arabic-system-fallback-and-real-joining", [&] {
        auto fonts = manager();
        auto primary = registeredFont(fonts, fixtureDirectory, "TGFX Arabic Fallback Primary", false);
        require(!primary->typeface()->supportsCharacter(0x0633), "Fixture unexpectedly covers Arabic; fallback not exercised");
        const char* languages[] = {"ar"};
        auto result = fonts->getCompatibleFont(primary, languages, 1, 0x0633);
        require(result.success(), std::string("Real native Arabic fallback failed: ") + result.description());
        auto font = result.moveValue();
        const std::u32string text = U"\u0633\u0644\u0627\u0645";
        for (auto c : text) require(font->typeface()->supportsCharacter(c), "Selected native fallback lacks Arabic sample coverage");
        auto shaper = TextShaper::make(false);
        auto glyphs = shape(*shaper, *font, text, true);
        bool joined = glyphs.size() != text.size();
        if (!joined) {
            for (size_t i = 0; i < glyphs.size(); ++i) {
                joined |= glyphs[i].glyphID != font->getSkValue().getGlyphID(text[text.size() - 1 - i]);
            }
        }
        require(joined, "Arabic shaping returned only reversed nominal glyphs without real contextual joining");
        return font->getDescription() + "; joined advance=" + std::to_string(advance(glyphs));
    });

    run(report, "text.word-cache-cold-warm-and-clear-contract", [&] {
        auto font = wrap(vary(loadFace(fixtureDirectory), {{kWeight, 650}, {kWidth, 85}}));
        auto cached = TextShaper::make(true);
        auto uncached = TextShaper::make(false);
        const std::u32string text = U"office AVATAR office";
        auto reference = shape(*uncached, *font, text);
        require(shape(*cached, *font, text) == reference, "Cold word-cache output differs from real uncached shaping");
        require(shape(*cached, *font, text) == reference, "Warm word-cache output differs");
        cached->clearCache();
        require(shape(*cached, *font, text) == reference, "Cache clear changed shaped glyphs/positions");
        return std::string("Actual variable-instance glyph IDs, advances, offsets and character flags match");
    });

    run(report, "text.emoji-native-coverage-shaping-and-color-gpu", [&] {
        auto fonts = manager();
        auto result = fonts->getEmojiFont(kFontSize, 1);
        require(result.success(), std::string("Real native emoji font resolution failed: ") + result.description());
        auto font = result.moveValue();
        require(font->typeface()->isEmoji() && font->getSkValue().getTypeface()->hasColor() &&
                    font->typeface()->supportsCharacter(0x1f60a), "Actual emoji face has no required color/Unicode coverage");
        auto shaper = TextShaper::make(false);
        advance(shape(*shaper, *font, U"\U0001F60A"));
        auto content = record(layout(fonts, font, "\xF0\x9F\x98\x8A"));
        GPU gpu;
        auto pixels = render(gpu, content);
        ink(pixels);
        size_t colored = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i + 3] && (pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2])) ++colored;
        }
        require(colored > 0, "Actual color emoji blob rendered only monochrome/empty pixels");
        return font->getDescription() + "; color pixels=" + std::to_string(colored);
    });

    run(report, "text.collection-selected-face-standalone-sfnt-and-checksums", [&] {
        std::string diagnostics;
        auto first = collectionFace(fixtureDirectory, 0, &diagnostics);
        require(first->getGlyphID(0x0100) == 191 && first->getGlyphID(0x03a9) == 659,
                "Collection face 0 differs from independently recorded Roboto Flex cmap");
        auto selected = collectionFace(fixtureDirectory, 1);
        checkSelectedGlyphs(selected);
        auto font = wrap(selected, 32);
        auto bytes = font->typeface()->makeFontData();
        auto source = collectionBytes(fixtureDirectory, "NotoSans-Regular.ttf", 569208);
        validateSelectedSFNT(selected, bytes, be16(source.data() + 4));
        auto roundtrip = tgfx::Typeface::MakeFromBytes(bytes.data(), bytes.size());
        require(roundtrip != nullptr, "Actual TGFX cannot reopen selected standalone face export");
        checkSelectedGlyphs(roundtrip);
#if !defined(__APPLE__)
        auto originalStream = selected->openStream();
        require(originalStream != nullptr, "Original selected FreeType face lost its stream");
        char signature[4]{};
        require(originalStream->read(signature, sizeof(signature)) == sizeof(signature) &&
                    std::string(signature, sizeof(signature)) == "ttcf",
                "Export changed the underlying native collection stream API");
#endif
        return diagnostics + "; TTC face 1 preserved U+0100=194/U+03A9=373; every native table/checksum and standalone roundtrip verified";
    });

    run(report, "text.collection-selected-face-thorvg-font-load-and-glyphs", [&] {
        std::string diagnostics;
        auto selected = collectionFace(fixtureDirectory, 1, &diagnostics);
        checkSelectedGlyphs(selected);
        auto font = wrap(selected, 32);
        auto exported = font->typeface()->makeFontData();
        require(exported.size() >= 12 && be32(exported.data()) == 0x00010000,
                "No selected standalone SFNT for actual ThorVG handoff");
        auto fonts = manager();
        auto family = Valdi::StringCache::makeStringFromCLiteral("TGFXCollectionFace");
        fonts->registerTypeface(family, FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright), false, exported);
        auto resolved = fonts->getTypefaceWithName(family);
        require(resolved.success(), "Production FontManager could not load selected face export");
        checkSelectedGlyphs(resolved.value()->getSkValue());
        auto json = collectionBytes(fixtureDirectory, "collection-font.json");
        auto animation = LottieAnimatedImage::make(Ref<IFontManager>(fonts), json.data(), json.size());
        require(animation.success(), std::string("Actual ThorVG selected-face font load failed: ") + animation.description());
        exported = {};
        std::vector<uint8_t>().swap(json);
        GPU gpu;
        auto actual = pixelsForDraw(gpu, [&](NativeCanvas* canvas) {
            animation.value()->draw(canvas, snap::drawing::Rect::makeXYWH(0, 0, 128, 64),
                                     snap::drawing::Duration::fromSeconds(0.25), FittingSizeModeFill);
        });
        auto reference = pixelsForDraw(gpu, [&](NativeCanvas* canvas) {
            tgfx::Font native(selected, 32);
            tgfx::Paint paint;
            paint.setColor(tgfx::Color::White());
            canvas->drawSimpleText("HgQ\xC4\x80\xCE\xA9", 8, 44, native, paint);
        });
        matchingGlyphInteriors(actual, reference);
        return diagnostics + "; Real ThorVG font provider loaded exported TTC face; U+0100/U+03A9 raster matches native selected face";
    });

    addTextShadowChecks(report, fixtureDirectory);
    addComposedTextShadowChecks(report, fixtureDirectory);
    report.success = report.failed == 0 && report.passed == 32;
    return report;
}
