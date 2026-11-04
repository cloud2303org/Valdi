//
//  FontFamilyWithStyleSet.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 12/12/22.
//

#pragma once

#include "snap_drawing/cpp/Text/FontFamily.hpp"
#include "valdi_core/cpp/Utils/FlatMap.hpp"

#include "snap_drawing/cpp/Text/NativeFontManager.hpp"

namespace snap::drawing {

class Typeface;

/**
 A FontFamily implementation that is backed by a Skia SkFontStyleSet instance.
 */
class FontFamilyWithStyleSet : public FontFamily {
public:
    FontFamilyWithStyleSet(const NativeFontStyleSetHandle& fontStyleSet, const String& familyName);
    ~FontFamilyWithStyleSet() override;

    Ref<Typeface> matchStyle(NativeFontManager& fontMgr, FontStyle fontStyle) override;

private:
    NativeFontStyleSetHandle _fontStyleSet;
    Valdi::FlatMap<uint32_t, Ref<Typeface>> _typefaces;
    Valdi::FlatMap<FontStyle, uint32_t> _fontIdByStyle;
};

} // namespace snap::drawing
