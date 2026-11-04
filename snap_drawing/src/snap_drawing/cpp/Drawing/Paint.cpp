//
//  Paint.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#include "snap_drawing/cpp/Drawing/Paint.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/ColorFilter.h"
#else
#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"
#include "snap_drawing/cpp/Drawing/Shader.hpp"

#include "include/core/SkColorFilter.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#endif
#include <utility>

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
Paint::Paint() {
    // TGFX defaults differ from Skia's baseline paint.
    getSkValue().setColor(tgfx::Color::Black());
    getSkValue().setAntiAlias(false);
    getSkValue().setStrokeWidth(0.0f);
    getSkValue().setStyle(tgfx::PaintStyle::Fill);
}
#else
Paint::Paint() = default;
#endif

Paint::Paint(const Paint& other) : Paint() {
    *this = other;
}

Paint::~Paint() = default;

Paint& Paint::operator=(const Paint& other) {
    if (&other != this) {
        getSkValue() = other.getSkValue();
    }

    return *this;
}

void Paint::setColor(Color color) {
    getSkValue().setColor(color.getSkValue());
}

Color Paint::getColor() const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return Color::fromTgfxValue(getSkValue().getColor());
#else
    return fromSkValue<Color>(getSkValue().getColor());
#endif
}

void Paint::setAlpha(float alpha) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().setAlpha(std::isnan(alpha) ? 0.0f : std::clamp(alpha, 0.0f, 1.0f));
#else
    getSkValue().setAlphaf(alpha);
#endif
}

void Paint::setBlendColorFilter(Color color) {
    if (color == Color::transparent()) {
        getSkValue().setColorFilter(nullptr);
    } else {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        getSkValue().setColorFilter(tgfx::ColorFilter::Blend(color.getSkValue(), tgfx::BlendMode::SrcATop));
#else
        getSkValue().setColorFilter(SkColorFilters::Blend(color.getSkValue(), SkBlendMode::kSrcATop));
#endif
    }
}

void Paint::setStroke(bool stroke) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().setStyle(stroke ? tgfx::PaintStyle::Stroke : tgfx::PaintStyle::Fill);
#else
    getSkValue().setStroke(stroke);
#endif
}

void Paint::setStrokeWidth(Scalar strokeWidth) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    if (!(strokeWidth >= 0.0f)) {
        return;
    }
#endif
    getSkValue().setStrokeWidth(strokeWidth);
}

Scalar Paint::getStrokeWidth() const {
    return getSkValue().getStrokeWidth();
}

void Paint::setStrokeJoin(PaintStrokeJoin strokeJoin) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().setLineJoin(static_cast<tgfx::LineJoin>(strokeJoin));
#else
    getSkValue().setStrokeJoin(static_cast<SkPaint::Join>(strokeJoin));
#endif
}

void Paint::setStrokeCap(PaintStrokeCap strokeCap) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().setLineCap(static_cast<tgfx::LineCap>(strokeCap));
#else
    getSkValue().setStrokeCap(static_cast<SkPaint::Cap>(strokeCap));
#endif
}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
void Paint::setBlendMode(tgfx::BlendMode blendMode) {
#else
void Paint::setBlendMode(SkBlendMode blendMode) {
#endif
    getSkValue().setBlendMode(blendMode);
}

void Paint::setAntiAlias(bool antialias) {
    getSkValue().setAntiAlias(antialias);
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
void Paint::setShader(const Shader& shader) {
    getSkValue().setShader(shader.getSkValue());
}

void Paint::setMaskFilter(const MaskFilter& maskFilter) {
    getSkValue().setMaskFilter(maskFilter.getSkValue());
}

void Paint::setStrokeDashPattern(Scalar onLength, Scalar offLength) {
    SkScalar intervals[] = {static_cast<SkScalar>(onLength), static_cast<SkScalar>(offLength)};
    getSkValue().setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));
}

void Paint::setStrokeDotPattern(Scalar radius, Scalar advance) {
    SkPath dot;
    dot.addCircle(0, 0, static_cast<SkScalar>(radius));
    getSkValue().setPathEffect(
        SkPath1DPathEffect::Make(dot, static_cast<SkScalar>(advance), 0, SkPath1DPathEffect::kTranslate_Style));
}
#endif

} // namespace snap::drawing
