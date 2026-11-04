//
//  Paint.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#include "snap_drawing/cpp/Drawing/Paint.hpp"
#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"
#include "snap_drawing/cpp/Drawing/Shader.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "tgfx/core/Canvas.h"
#include "tgfx/core/ColorFilter.h"
#include "tgfx/core/MaskFilter.h"
#include "tgfx/core/PathMeasure.h"
#else
#include "include/core/SkColorFilter.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#endif
#include <cmath>
#include <cstdint>
#include <utility>

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
namespace {
// Sk1DPathEffect::onFilterPath allows 100000 attempts per contour, then returns
// false. Check before producing geometry, rather than building that many ovals
// and discovering failure afterward. A whole-draw budget also prevents many
// individually acceptable contours from allocating unbounded stamp geometry.
constexpr uint32_t kMaxDotStampsPerContour = 100000;
constexpr uint32_t kMaxDotStampsPerDraw = 100000;
constexpr uint32_t kMaxDotContoursPerDraw = 100000;

struct DotPathPreflight {
    uint32_t stamps = 0;
    const char* failure = nullptr;
};

DotPathPreflight preflightDotPath(const tgfx::Path& path, Scalar advance) {
    if (!(advance > 0) || !std::isfinite(advance)) {
        return {0, "TGFX dot effect has an invalid stamp advance"};
    }
    auto measure = tgfx::PathMeasure::MakeFrom(path);
    if (!measure) {
        return {0, "TGFX dot effect could not measure its input path"};
    }
    uint32_t contours = 0;
    uint32_t stamps = 0;
    do {
        if (contours == kMaxDotContoursPerDraw) {
            return {0, "TGFX dot effect exceeds 100000 contours per draw"};
        }
        ++contours;
        const auto length = measure->getLength();
        if (!std::isfinite(length) || length < 0) {
            return {0, "TGFX dot effect has a non-finite or negative contour length"};
        }
        // Dividing in double avoids float overflow/underflow before checking
        // the bound. Convert to an integer only after the range is verified.
        const auto count = std::ceil(static_cast<double>(length) / static_cast<double>(advance));
        if (count > kMaxDotStampsPerContour) {
            return {0, "TGFX dot effect exceeds 100000 stamp attempts per contour"};
        }
        if (count > kMaxDotStampsPerDraw - stamps) {
            return {0, "TGFX dot effect exceeds 100000 total stamp attempts per draw"};
        }
        stamps += static_cast<uint32_t>(count);
    } while (measure->nextContour());
    return {stamps, nullptr};
}

void rejectDotPath(tgfx::Canvas* canvas, const char* failure) {
    Valdi::ConsoleLogger::getErrorLogger().log(Valdi::LogTypeError, failure);
    // This public native entry point must also retain an immutable failure
    // record on picture canvases. Reporting only the recorder's mutable status
    // would lose the failure at DrawingContext::finish()/picture replay.
    canvas->markRenderFailure();
}
} // namespace

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
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        _dashPathEffect = other._dashPathEffect;
        _emptyShader = other._emptyShader;
        _hasDotPattern = other._hasDotPattern;
        _dotRadius = other._dotRadius;
        _dotAdvance = other._dotAdvance;
        _maskFilter = other._maskFilter;
#endif
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

void Paint::setShader(const Shader& shader) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _emptyShader = shader._empty;
    auto nativeShader = shader.getSkValue();
    if (nativeShader) {
        // TGFX folds constant shaders into Paint::color, destroying the
        // independent paint alpha and making later setColor/setAlpha incorrect.
        // A real blend shader retains those semantics for every shader kind.
        nativeShader = tgfx::Shader::MakeBlend(
            tgfx::BlendMode::SrcOver, tgfx::Shader::MakeColorShader(tgfx::Color::Transparent()), nativeShader);
    }
    getSkValue().setShader(std::move(nativeShader));
#else
    getSkValue().setShader(shader.getSkValue());
#endif
}

void Paint::setMaskFilter(const MaskFilter& maskFilter) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _maskFilter = maskFilter;
    getSkValue().setMaskFilter(nullptr);
#else
    getSkValue().setMaskFilter(maskFilter.getSkValue());
#endif
}

void Paint::setStrokeDashPattern(Scalar onLength, Scalar offLength) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    const float intervals[] = {onLength, offLength};
    _dashPathEffect = tgfx::PathEffect::MakeDash(intervals, 2, 0, false);
    _hasDotPattern = false;
#else
    SkScalar intervals[] = {static_cast<SkScalar>(onLength), static_cast<SkScalar>(offLength)};
    getSkValue().setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));
#endif
}

void Paint::setStrokeDotPattern(Scalar radius, Scalar advance) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _dashPathEffect = nullptr;
    _hasDotPattern = advance > 0 && std::isfinite(advance);
    // An invalid/empty stamp still replaces the previous effect; it must not
    // turn into an ordinary unmodified stroke.
    _dotRadius = std::isfinite(radius) ? radius : 0;
    _dotAdvance = advance;
#else
    SkPath dot;
    dot.addCircle(0, 0, static_cast<SkScalar>(radius));
    getSkValue().setPathEffect(
        SkPath1DPathEffect::Make(dot, static_cast<SkScalar>(advance), 0, SkPath1DPathEffect::kTranslate_Style));
#endif
}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
void Paint::drawPath(tgfx::Canvas* canvas, const tgfx::Path& path) const {
    if (_emptyShader) {
        return;
    }
    // TGFX's draw ops reconstruct local coordinates with the inverse geometry
    // matrix before evaluating the brush. Applying the CTM to the shader here
    // would therefore apply it twice (also when replaying recorded pictures).
    auto nativePaint = getSkValue();
    if (_hasDotPattern) {
        // Unlike dash centerlines, a 1D translate effect emits filled copies of
        // the stamp even when the input paint uses a stroke.
        if (_dotRadius > 0) {
            const auto preflight = preflightDotPath(path, _dotAdvance);
            if (preflight.failure != nullptr) {
                rejectDotPath(canvas, preflight.failure);
                return;
            }
            // No output path or per-contour vector is allocated by preflight.
            // Re-measure only after the ENTIRE draw has passed both budgets.
            auto measure = tgfx::PathMeasure::MakeFrom(path);
            if (!measure) {
                rejectDotPath(canvas, "TGFX dot effect could not re-measure its input path");
                return;
            }
            tgfx::Path dots;
            auto remaining = preflight.stamps;
            do {
                const auto length = measure->getLength();
                const auto count = std::ceil(static_cast<double>(length) / static_cast<double>(_dotAdvance));
                if (!std::isfinite(count) || count < 0 || count > remaining) {
                    rejectDotPath(canvas, "TGFX dot effect measurement changed after preflight");
                    return;
                }
                const auto contourStamps = static_cast<uint32_t>(count);
                remaining -= contourStamps;
                // Each contour restarts at phase zero, and the endpoint is not
                // stamped twice on a closed contour. An integer index bounds
                // work even when floating-point distance addition would stall.
                for (uint32_t stamp = 0; stamp < contourStamps; ++stamp) {
                    const auto distance = static_cast<double>(stamp) * static_cast<double>(_dotAdvance);
                    tgfx::Point position = {};
                    tgfx::Point tangent = {};
                    if (measure->getPosTan(static_cast<float>(distance), &position, &tangent)) {
                        dots.addOval(tgfx::Rect::MakeLTRB(position.x - _dotRadius,
                                                         position.y - _dotRadius,
                                                         position.x + _dotRadius,
                                                         position.y + _dotRadius));
                    }
                }
            } while (measure->nextContour());
            nativePaint.setStyle(tgfx::PaintStyle::Fill);
            _maskFilter.drawPath(canvas, dots, nativePaint);
        }
        return;
    }
    if (_dashPathEffect && nativePaint.getStyle() == tgfx::PaintStyle::Stroke) {
        auto dashed = path;
        // This pin's MakeDash filters with a hairline stroke record. Keep our
        // actual stroke width/cap/join on the resulting centerline geometry.
        if (_dashPathEffect->filterPath(&dashed)) {
            _maskFilter.drawPath(canvas, dashed, nativePaint);
        }
        return;
    }
    // Skia's dash effect does not alter fill geometry.
    _maskFilter.drawPath(canvas, path, nativePaint);
}

void Paint::drawRect(tgfx::Canvas* canvas, const tgfx::Rect& rect) const {
    if (_emptyShader) {
        return;
    }
    if (_hasDotPattern || _dashPathEffect || _maskFilter.getSkValue()) {
        tgfx::Path path;
        path.addRect(rect);
        drawPath(canvas, path);
        return;
    }
    canvas->drawRect(rect, getSkValue());
}
#endif

} // namespace snap::drawing
