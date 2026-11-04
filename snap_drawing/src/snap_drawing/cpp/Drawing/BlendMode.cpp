//
//  BlendMode.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/25/22.
//

#include "snap_drawing/cpp/Drawing/BlendMode.hpp"

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
tgfx::BlendMode toSkBlendMode(BlendMode blendMode) {
    switch (blendMode) {
        case BlendModeClear:
            return tgfx::BlendMode::Clear;
        case BlendModeSrc:
            return tgfx::BlendMode::Src;
        case BlendModeDst:
            return tgfx::BlendMode::Dst;
        case BlendModeSrcOver:
            return tgfx::BlendMode::SrcOver;
        case BlendModeDstOver:
            return tgfx::BlendMode::DstOver;
        case BlendModeSrcIn:
            return tgfx::BlendMode::SrcIn;
        case BlendModeDstIn:
            return tgfx::BlendMode::DstIn;
        case BlendModeSrcOut:
            return tgfx::BlendMode::SrcOut;
        case BlendModeDstOut:
            return tgfx::BlendMode::DstOut;
        case BlendModeSrcATop:
            return tgfx::BlendMode::SrcATop;
        case BlendModeDstATop:
            return tgfx::BlendMode::DstATop;
        case BlendModeXor:
            return tgfx::BlendMode::Xor;
        case BlendModePlus:
            return tgfx::BlendMode::PlusLighter;
        case BlendModeModulate:
            return tgfx::BlendMode::Modulate;
        case BlendModeScreen:
        case BlendModeLastCoeffMode:
            return tgfx::BlendMode::Screen;
        case BlendModeOverlay:
            return tgfx::BlendMode::Overlay;
        case BlendModeDarken:
            return tgfx::BlendMode::Darken;
        case BlendModeLighten:
            return tgfx::BlendMode::Lighten;
        case BlendModeColorDodge:
            return tgfx::BlendMode::ColorDodge;
        case BlendModeColorBurn:
            return tgfx::BlendMode::ColorBurn;
        case BlendModeHardLight:
            return tgfx::BlendMode::HardLight;
        case BlendModeSoftLight:
            return tgfx::BlendMode::SoftLight;
        case BlendModeDifference:
            return tgfx::BlendMode::Difference;
        case BlendModeExclusion:
            return tgfx::BlendMode::Exclusion;
        case BlendModeMultiply:
        case BlendModeLastSeparableMode:
            return tgfx::BlendMode::Multiply;
        case BlendModeHue:
            return tgfx::BlendMode::Hue;
        case BlendModeSaturation:
            return tgfx::BlendMode::Saturation;
        case BlendModeColor:
            return tgfx::BlendMode::Color;
        case BlendModeLuminosity:
            return tgfx::BlendMode::Luminosity;
    }
    return tgfx::BlendMode::SrcOver;
}
#else
SkBlendMode toSkBlendMode(BlendMode blendMode) {
    switch (blendMode) {
        case BlendModeClear:
            return SkBlendMode::kClear;
        case BlendModeSrc:
            return SkBlendMode::kSrc;
        case BlendModeDst:
            return SkBlendMode::kDst;
        case BlendModeSrcOver:
            return SkBlendMode::kSrcOver;
        case BlendModeDstOver:
            return SkBlendMode::kDstOver;
        case BlendModeSrcIn:
            return SkBlendMode::kSrcIn;
        case BlendModeDstIn:
            return SkBlendMode::kDstIn;
        case BlendModeSrcOut:
            return SkBlendMode::kSrcOut;
        case BlendModeDstOut:
            return SkBlendMode::kDstOut;
        case BlendModeSrcATop:
            return SkBlendMode::kSrcATop;
        case BlendModeDstATop:
            return SkBlendMode::kDstATop;
        case BlendModeXor:
            return SkBlendMode::kXor;
        case BlendModePlus:
            return SkBlendMode::kPlus;
        case BlendModeModulate:
            return SkBlendMode::kModulate;
        case BlendModeScreen:
            return SkBlendMode::kScreen;
        case BlendModeLastCoeffMode:
            return SkBlendMode::kLastCoeffMode;
        case BlendModeOverlay:
            return SkBlendMode::kOverlay;
        case BlendModeDarken:
            return SkBlendMode::kDarken;
        case BlendModeLighten:
            return SkBlendMode::kLighten;
        case BlendModeColorDodge:
            return SkBlendMode::kColorDodge;
        case BlendModeColorBurn:
            return SkBlendMode::kColorBurn;
        case BlendModeHardLight:
            return SkBlendMode::kHardLight;
        case BlendModeSoftLight:
            return SkBlendMode::kSoftLight;
        case BlendModeDifference:
            return SkBlendMode::kDifference;
        case BlendModeExclusion:
            return SkBlendMode::kExclusion;
        case BlendModeMultiply:
            return SkBlendMode::kMultiply;
        case BlendModeLastSeparableMode:
            return SkBlendMode::kLastSeparableMode;
        case BlendModeHue:
            return SkBlendMode::kHue;
        case BlendModeSaturation:
            return SkBlendMode::kSaturation;
        case BlendModeColor:
            return SkBlendMode::kColor;
        case BlendModeLuminosity:
            return SkBlendMode::kLuminosity;
    }
}
#endif

} // namespace snap::drawing
