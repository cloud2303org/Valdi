#pragma once

#include "snap_drawing/cpp/Text/Character.hpp"
#include "snap_drawing/cpp/Text/FontStyle.hpp"
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "valdi_core/cpp/Utils/FlatMap.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace snap::drawing {

// TGFX has no public font manager. This is a SnapDrawing platform bridge, with
// real tgfx::Typeface objects at every boundary.
class TgfxFontStyleSet {
public:
    struct Entry {
        FontStyle style{FontWidthNormal, FontWeightNormal, FontSlantUpright};
        std::string path;
        int collectionIndex = 0;
        NativeTypeface typeface;
        std::vector<std::pair<uint32_t, float>> variations;
        std::string supportedAxes;
        Valdi::FlatMap<FontStyle, NativeTypeface> instances;
        bool attemptedLoad = false;
    };

    explicit TgfxFontStyleSet(std::vector<Entry> entries);
    size_t count() const;
    NativeTypeface matchStyle(NativeFontStyle style);
    NativeTypeface matchCharacter(NativeFontStyle style, Character character);
    bool containsTypeface(uint32_t uniqueID);

private:
    std::mutex _mutex;
    std::vector<Entry> _entries;

    NativeTypeface load(size_t index);
    NativeTypeface resolveStyle(size_t index, NativeFontStyle style);
    std::vector<size_t> rank(FontStyle style) const;
};

class TgfxFontManager {
public:
    TgfxFontManager();
    ~TgfxFontManager();

    size_t countFamilies() const;
    std::string familyName(size_t index) const;
    const std::string& defaultFamilyName() const;
    std::shared_ptr<TgfxFontStyleSet> matchFamily(const char* name);
    NativeTypeface matchFamilyStyle(const char* name, NativeFontStyle style);
    NativeTypeface matchFamilyStyleCharacter(
        const char* name, NativeFontStyle style, const char** languages, size_t languageCount, Character character);
    NativeTypeface matchTypefaceStyle(const NativeTypeface& typeface, NativeFontStyle style);
    NativeTypeface matchEmoji(NativeFontStyle style, Character character);

    const std::vector<std::string>& unsupportedConfigurations() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

using NativeFontManager = TgfxFontManager;
using NativeFontManagerHandle = std::shared_ptr<TgfxFontManager>;
using NativeFontStyleSetHandle = std::shared_ptr<TgfxFontStyleSet>;

// Read numeric traits, not localized style-name strings. CoreText also supplies
// the selected coordinates of system variable fonts.
FontStyle getTgfxTypefaceStyle(const NativeTypeface& typeface);
#if !defined(__APPLE__)
NativeTypeface makeTgfxTypefaceWithStyle(const NativeTypeface& typeface, FontStyle style);
#endif

} // namespace snap::drawing
#else
#include "include/core/SkFontMgr.h"

namespace snap::drawing {
using NativeFontManager = SkFontMgr;
using NativeFontManagerHandle = sk_sp<SkFontMgr>;
using NativeFontStyleSetHandle = sk_sp<SkFontStyleSet>;
} // namespace snap::drawing
#endif
