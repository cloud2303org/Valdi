#pragma once

#include "snap_drawing/cpp/Utils/Scalar.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/TgfxBridge.hpp"
#include "tgfx/core/Canvas.h"
#include "tgfx/core/ImageFilter.h"
#else
#include "snap_drawing/cpp/Utils/SkiaBridge.hpp"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#endif

namespace snap::drawing {

enum BlurStyle {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    BlurStyleNormal,
    BlurStyleSolid,
    BlurStyleOuter,
    BlurStyleInner,
#else
    BlurStyleNormal = kNormal_SkBlurStyle, //!< fuzzy inside and outside
    BlurStyleSolid = kSolid_SkBlurStyle,   //!< solid inside, fuzzy outside
    BlurStyleOuter = kOuter_SkBlurStyle,   //!< nothing inside, fuzzy outside
    BlurStyleInner = kInner_SkBlurStyle,   //!< fuzzy inside, nothing outside
#endif
};

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
// TGFX's MaskFilter has only MakeShader, not a blur factory. Keep the real
// Gaussian image filter and build a geometry-only alpha mask at drawing time.
class MaskFilter : public WrappedTgfxValue<std::shared_ptr<tgfx::ImageFilter>> {
#else
class MaskFilter : public WrappedSkValue<sk_sp<SkMaskFilter>> {
#endif
public:
    MaskFilter();
    ~MaskFilter();

    static MaskFilter makeBlur(BlurStyle blurStyle, Scalar sigma, bool respectCTM = true);

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    BlurStyle _style = BlurStyleNormal;
    Scalar _sigma = 0;
    bool _respectCTM = true;

    MaskFilter(BlurStyle style, Scalar sigma, bool respectCTM, std::shared_ptr<tgfx::ImageFilter> filter);
    void drawPath(tgfx::Canvas* canvas, const tgfx::Path& path, const tgfx::Paint& paint) const;
    void drawResolvedPath(tgfx::Canvas* canvas, const tgfx::Path& path, const tgfx::Paint& paint) const;
    friend class Paint;
#else
    explicit MaskFilter(sk_sp<SkMaskFilter> skValue);
#endif
};

} // namespace snap::drawing
