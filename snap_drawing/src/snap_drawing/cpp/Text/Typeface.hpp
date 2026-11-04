//
//  Typeface.hpp
//  snap_drawing-macos
//
//  Created by Simon Corsin on 1/28/22.
//

#pragma once

#include "snap_drawing/cpp/Text/Character.hpp"
#include "snap_drawing/cpp/Text/FontMetrics.hpp"
#include "snap_drawing/cpp/Text/FontStyle.hpp"
#include "snap_drawing/cpp/Text/Harfbuzz.hpp"
#include "snap_drawing/cpp/Utils/Aliases.hpp"

#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/FlatMap.hpp"
#include "valdi_core/cpp/Utils/Mutex.hpp"

#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"

namespace Valdi {
class CharacterSet;
}

namespace snap::drawing {

class Typeface : public Valdi::SimpleRefCountable {
public:
    Typeface(NativeTypeface&& typeface, const String& familyName, bool isCustom);
    ~Typeface() override;

    uint32_t getId() const;

    const NativeTypeface& getSkValue() const;

    const String& familyName() const;
    const FontStyle& fontStyle() const;

    bool isCustom() const;

    bool hasSpaceInLigaturesOrKerning() const;

    bool isEmoji() const;

    bool supportsCharacter(Character character);

    // TGFX exports the selected face as a standalone SFNT for consumers such
    // as ThorVG, which cannot load a TTC container. Standalone source streams
    // keep their original bytes. Variable design coordinates are not baked
    // into outlines by this table export.
    Valdi::BytesView makeFontData();

    const HBFace& getHBFace() const;
    const HBFont& getHBFont() const;

    FontMetrics getFontMetrics(double fontSize);

private:
    mutable Valdi::Mutex _mutex;
#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && defined(__APPLE__)
    // The descriptor-adjusted CTTypeface has no TGFX backing stream. Keep the
    // original face alive through _typeface destruction to retain custom bytes
    // and export the original data.
    NativeTypeface _fontDataTypeface;
#endif
    NativeTypeface _typeface;
    Ref<Valdi::CharacterSet> _characterSet;
    String _familyName;
    FontStyle _fontStyle;
    bool _isCustom;
    bool _isEmoji;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    bool _hasSpaceInLigaturesOrKerning = false;
#endif
    HBFace _hbFace;
    HBFont _hbFont;
    Valdi::FlatMap<double, FontMetrics> _fontMetricsBySize;
};

} // namespace snap::drawing
