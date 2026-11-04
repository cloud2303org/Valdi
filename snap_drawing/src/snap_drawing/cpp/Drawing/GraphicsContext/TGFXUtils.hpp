#pragma once
#include "tgfx/core/ImageInfo.h"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"

namespace snap::drawing {
inline tgfx::ColorType toTGFXColorType(Valdi::ColorType type) {
    switch (type) {
        case Valdi::ColorTypeRGBA8888: return tgfx::ColorType::RGBA_8888;
        case Valdi::ColorTypeBGRA8888: return tgfx::ColorType::BGRA_8888;
        case Valdi::ColorTypeAlpha8: return tgfx::ColorType::ALPHA_8;
        case Valdi::ColorTypeGray8: return tgfx::ColorType::Gray_8;
        case Valdi::ColorTypeRGBAF16: return tgfx::ColorType::RGBA_F16;
        default: return tgfx::ColorType::Unknown;
    }
}
inline tgfx::ImageInfo toTGFXImageInfo(const Valdi::BitmapInfo& info) {
    auto alpha = tgfx::AlphaType::Premultiplied;
    if (info.alphaType == Valdi::AlphaTypeOpaque) { alpha = tgfx::AlphaType::Opaque; }
    if (info.alphaType == Valdi::AlphaTypeUnpremul) { alpha = tgfx::AlphaType::Unpremultiplied; }
    return tgfx::ImageInfo::Make(info.width, info.height, toTGFXColorType(info.colorType), alpha, info.rowBytes);
}
} // namespace snap::drawing
