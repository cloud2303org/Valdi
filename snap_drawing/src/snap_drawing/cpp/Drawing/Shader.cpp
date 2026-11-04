//
//  Shader.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/25/22.
//

#include "snap_drawing/cpp/Drawing/Shader.hpp"
#include "snap_drawing/cpp/Utils/Image.hpp"
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTileMode.h"
#endif

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
static tgfx::SamplingOptions makeSamplingOptions(FilterQuality quality) {
    switch (quality) {
        case FilterQualityNone:
            return tgfx::SamplingOptions(tgfx::FilterMode::Nearest, tgfx::MipmapMode::None);
        case FilterQualityLow:
            return tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None);
        case FilterQualityMedium:
            return tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::Linear);
        case FilterQualityHigh:
            // The cubic factory below samples the original image texel grid.
            return tgfx::SamplingOptions(tgfx::FilterMode::Nearest, tgfx::MipmapMode::None);
    }
    return tgfx::SamplingOptions(tgfx::FilterMode::Nearest, tgfx::MipmapMode::None);
}
#else
static SkSamplingOptions makeSamplingOptions(FilterQuality quality) {
    switch (quality) {
        case FilterQualityNone:
            return SkSamplingOptions();
        case FilterQualityLow:
            return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
        case FilterQualityMedium:
            return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
        case FilterQualityHigh:
            return SkSamplingOptions(SkCubicResampler::Mitchell());
    }
}
#endif

Shader::Shader() : Shader(nullptr) {}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
Shader::Shader(std::shared_ptr<tgfx::Shader> shader, bool empty)
    : WrappedTgfxValue<std::shared_ptr<tgfx::Shader>>(std::move(shader)), _empty(empty) {}
#else
Shader::Shader(sk_sp<SkShader> skValue) : WrappedSkValue<sk_sp<SkShader>>(std::move(skValue)) {}
#endif
Shader::~Shader() = default;

Shader Shader::withLocalMatrix(const Matrix& localMatrix) const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return Shader(getSkValue() ? getSkValue()->makeWithMatrix(localMatrix.getSkValue()) : nullptr, _empty);
#else
    return Shader(getSkValue()->makeWithLocalMatrix(localMatrix.getSkValue()));
#endif
}

Shader Shader::makeEmpty() {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Empty means no drawing, including under Src/Clear or a color filter that
    // generates alpha. Paint's drawing helpers preserve that distinction from a
    // transparent color shader while still retaining a real TGFX shader object.
    return Shader(tgfx::Shader::MakeColorShader(tgfx::Color::Transparent()), true);
#else
    return Shader(SkShaders::Empty());
#endif
}

Shader Shader::makeColor(Color color) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return Shader(tgfx::Shader::MakeColorShader(color.getSkValue()));
#else
    return Shader(SkShaders::Color(color.getSkValue()));
#endif
}

Shader Shader::makeBlend(BlendMode mode, Shader dst, Shader src) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    if (mode == BlendModeDst) {
        return dst;
    }
    if (mode == BlendModeSrc) {
        return src;
    }
    auto empty = mode != BlendModeClear && (dst._empty || src._empty);
    return Shader(tgfx::Shader::MakeBlend(toSkBlendMode(mode), dst.getSkValue(), src.getSkValue()), empty);
#else
    return Shader(SkShaders::Blend(toSkBlendMode(mode), dst.getSkValue(), src.getSkValue()));
#endif
}

Shader Shader::makeImage(const Ref<Image>& image, const Matrix* localMatrix, FilterQuality filterQuality) {
    auto samplingOptions = makeSamplingOptions(filterQuality);
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto shader = filterQuality == FilterQualityHigh
                      ? tgfx::Shader::MakeCubicImageShader(image->getSkValue())
                      : tgfx::Shader::MakeImageShader(
                            image->getSkValue(), tgfx::TileMode::Clamp, tgfx::TileMode::Clamp, samplingOptions);
    if (shader && localMatrix) {
        shader = shader->makeWithMatrix(localMatrix->getSkValue());
    }
    return Shader(std::move(shader));
#else
    const auto* skMatrix = localMatrix != nullptr ? &localMatrix->getSkValue() : nullptr;
    return Shader(image->getSkValue()->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, samplingOptions, skMatrix));
#endif
}

} // namespace snap::drawing
