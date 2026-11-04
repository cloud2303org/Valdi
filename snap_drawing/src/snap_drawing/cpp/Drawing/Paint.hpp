//
//  Paint.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#pragma once

#include "snap_drawing/cpp/Utils/Color.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/TgfxBridge.hpp"
#include "tgfx/core/Paint.h"
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

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
class Shader;
class MaskFilter;
#endif

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

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
    void setShader(const Shader& shader);
    void setMaskFilter(const MaskFilter& maskFilter);
    void setStrokeDashPattern(Scalar onLength, Scalar offLength);
    void setStrokeDotPattern(Scalar radius, Scalar advance);
#endif
};

} // namespace snap::drawing
