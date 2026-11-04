//
//  FontFamilyWithStyleSet.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 12/12/22.
//

#include "snap_drawing/cpp/Text/FontFamilyWithStyleSet.hpp"
#include "snap_drawing/cpp/Text/Typeface.hpp"

#include "snap_drawing/cpp/Text/NativeFontManager.hpp"

namespace snap::drawing {

FontFamilyWithStyleSet::FontFamilyWithStyleSet(const NativeFontStyleSetHandle& fontStyleSet, const String& familyName)
    : FontFamily(familyName), _fontStyleSet(fontStyleSet) {}
FontFamilyWithStyleSet::~FontFamilyWithStyleSet() = default;

Ref<Typeface> FontFamilyWithStyleSet::matchStyle(NativeFontManager& fontMgr, FontStyle fontStyle) {
    const auto& it = _fontIdByStyle.find(fontStyle);
    if (it != _fontIdByStyle.end()) {
        if (it->second == 0) {
            return nullptr;
        }

        return _typefaces[it->second];
    }

#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && defined(__APPLE__)
    auto matchedTypeface = fontMgr.matchFamilyStyle(getFamilyName().getCStr(), fontStyle.getSkValue());
#else
    NativeTypeface matchedTypeface(_fontStyleSet->matchStyle(fontStyle.getSkValue()));
#endif
    if (matchedTypeface == nullptr) {
        _fontIdByStyle[fontStyle] = 0;
        return nullptr;
    }

    auto id = matchedTypeface->uniqueID();
    _fontIdByStyle[fontStyle] = id;

    const auto& typefaceId = _typefaces.find(id);
    if (typefaceId != _typefaces.end()) {
        return typefaceId->second;
    }

    auto typeface = Valdi::makeShared<Typeface>(std::move(matchedTypeface), getFamilyName(), false);
    _typefaces[id] = typeface;

    return typeface;
}

} // namespace snap::drawing
