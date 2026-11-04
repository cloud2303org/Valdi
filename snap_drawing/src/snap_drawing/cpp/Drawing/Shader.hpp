//
//  Shader.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/25/22.
//

#pragma once

#include "snap_drawing/cpp/Drawing/BlendMode.hpp"
#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/Color.hpp"
#include "snap_drawing/cpp/Utils/Matrix.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/TgfxBridge.hpp"
#include "tgfx/core/Shader.h"
#else
#include "snap_drawing/cpp/Utils/SkiaBridge.hpp"
#include "include/core/SkShader.h"
#endif

namespace snap::drawing {

class Image;

enum FilterQuality {
    FilterQualityNone,   //!< fastest but lowest quality, typically nearest-neighbor
    FilterQualityLow,    //!< typically bilerp
    FilterQualityMedium, //!< typically bilerp + mipmaps for down-scaling
    FilterQualityHigh,   //!< slowest but highest quality, typically bicubic or better
};

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
class Shader : public WrappedTgfxValue<std::shared_ptr<tgfx::Shader>> {
#else
class Shader : public WrappedSkValue<sk_sp<SkShader>> {
#endif
public:
    Shader();
    ~Shader();

    Shader withLocalMatrix(const Matrix& localMatrix) const;

    static Shader makeEmpty();
    static Shader makeColor(Color color);
    static Shader makeBlend(BlendMode mode, Shader dst, Shader src);
    static Shader makeImage(const Ref<Image>& image, const Matrix* localMatrix, FilterQuality filterQuality);

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    explicit Shader(std::shared_ptr<tgfx::Shader> shader, bool empty = false);
    bool _empty = false;
    friend class Paint;
#else
    explicit Shader(sk_sp<SkShader> skValue);
#endif
    friend class LinearGradient;
    friend class RadialGradient;
};

} // namespace snap::drawing
