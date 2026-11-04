//
//  Paint.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#pragma once

#include "snap_drawing/cpp/Utils/Color.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"
#include "snap_drawing/cpp/Utils/TgfxBridge.hpp"
#include "tgfx/core/Paint.h"
#include "tgfx/core/PathEffect.h"
#else
#include "snap_drawing/cpp/Utils/SkiaBridge.hpp"

#include "include/core/SkPaint.h"
#endif

namespace snap::drawing {

enum PaintStrokeCap {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    PaintStrokeCapButt = static_cast<int>(tgfx::LineCap::Butt),
    PaintStrokeCapRound = static_cast<int>(tgfx::LineCap::Round),
    PaintStrokeCapSquare = static_cast<int>(tgfx::LineCap::Square),
#else
    PaintStrokeCapButt = SkPaint::Cap::kButt_Cap,
    PaintStrokeCapRound = SkPaint::Cap::kRound_Cap,
    PaintStrokeCapSquare = SkPaint::Cap::kSquare_Cap,
#endif
};

enum PaintStrokeJoin {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    PaintStrokeJoinMiter = static_cast<int>(tgfx::LineJoin::Miter),
    PaintStrokeJoinRound = static_cast<int>(tgfx::LineJoin::Round),
    PaintStrokeJoinBevel = static_cast<int>(tgfx::LineJoin::Bevel),
#else
    PaintStrokeJoinMiter = SkPaint::Join::kMiter_Join,
    PaintStrokeJoinRound = SkPaint::Join::kRound_Join,
    PaintStrokeJoinBevel = SkPaint::Join::kBevel_Join,
#endif
};

class Shader;
class MaskFilter;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
class Paint : public WrappedTgfxValue<tgfx::Paint> {
#else
class Paint : public WrappedSkValue<SkPaint> {
#endif
public:
    Paint();
    Paint(const Paint& other);

    ~Paint();

    Paint& operator=(const Paint& other);

    Color getColor() const;
    void setColor(Color color);
    void setAlpha(float alpha);
    void setBlendColorFilter(Color color);
    void setStroke(bool stroke);

    void setStrokeWidth(Scalar strokeWidth);
    Scalar getStrokeWidth() const;

    void setStrokeCap(PaintStrokeCap strokeCap);
    void setStrokeJoin(PaintStrokeJoin strokeJoin);
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    void setBlendMode(tgfx::BlendMode blendMode);
#else
    void setBlendMode(SkBlendMode blendMode);
#endif
    void setAntiAlias(bool antialias);

    void setShader(const Shader& shader);
    void setMaskFilter(const MaskFilter& maskFilter);
    void setStrokeDashPattern(Scalar onLength, Scalar offLength);
    void setStrokeDotPattern(Scalar radius, Scalar advance);

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // TGFX Paint has no path effect. All path/rect call sites must use these
    // helpers to apply stored dash/dot geometry, blur coverage and local shaders.
    // Dot geometry is preflighted before materialization: at most 100000 stamp
    // attempts per contour and per draw, and at most 100000 measured contours.
    // Over-budget input reports a sticky, replayable native rendering failure;
    // it produces neither truncated dots nor an unmodified fill/stroke fallback.
    void drawPath(tgfx::Canvas* canvas, const tgfx::Path& path) const;
    void drawRect(tgfx::Canvas* canvas, const tgfx::Rect& rect) const;

private:
    std::shared_ptr<tgfx::PathEffect> _dashPathEffect;
    bool _emptyShader = false;
    bool _hasDotPattern = false;
    Scalar _dotRadius = 0;
    Scalar _dotAdvance = 0;
    MaskFilter _maskFilter;
#endif
};

} // namespace snap::drawing
