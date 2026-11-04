//
//  DrawingContext.cpp
//  valdi-skia-app
//
//  Created by Simon Corsin on 6/28/20.
//

#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Drawing/Surface/ExternalSurface.hpp"
#include "snap_drawing/cpp/Utils/BitmapUtils.hpp"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#endif
#include "snap_drawing/cpp/Utils/BorderRadius.hpp"
#include "snap_drawing/cpp/Utils/LazyPath.hpp"

#include <algorithm>

namespace snap::drawing {

DrawingContext::DrawingContext(Scalar width, Scalar height) : _drawBounds(Rect::makeXYWH(0, 0, width, height)) {}

DrawingContext::~DrawingContext() = default;

LayerContent DrawingContext::finish() {
    LayerContent content;
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
    if (_externalSurface != nullptr) {
        content.externalSurface = Valdi::makeShared<ExternalSurfaceSnapshot>(std::move(_externalSurface));
    }
#endif

    if (_canvas != nullptr) {
        content.picture = _recorder.finishRecordingAsPicture();
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        // TGFX returns nullptr for an empty recording. Clear the active canvas
        // in either case so repeated finish() and a fresh recording are coherent.
        _canvas = nullptr;
#endif
    }

    return content;
}

void DrawingContext::drawPaint(const Paint& paint, const BorderRadius& borderRadius, LazyPath& lazyPath) {
    drawPaint(paint, borderRadius, drawBounds(), lazyPath);
}

void DrawingContext::drawPaint(const Paint& paint,
                               const BorderRadius& borderRadius,
                               const Rect& targetRect,
                               LazyPath& lazyPath) {
    if (borderRadius.isEmpty()) {
        drawPaint(paint, targetRect);
    } else {
        if (lazyPath.update(targetRect.size())) {
            borderRadius.applyToPath(targetRect, lazyPath.path());
        }

        drawPaint(paint, lazyPath.path());
    }
}

void DrawingContext::drawPaint(const Paint& paint, const Path& path) {
    if (!path.isEmpty()) {
        canvas()->drawPath(path.getSkValue(), paint.getSkValue());
    }
}

void DrawingContext::drawPaint(const Paint& paint, const Rect& targetRect) {
    canvas()->drawRect(targetRect.getSkValue(), paint.getSkValue());
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
void DrawingContext::drawBitmap(const Valdi::Ref<Valdi::IBitmap>& bitmap, FittingSizeMode fittingSizeMode) {
    auto imageResult = Image::makeFromBitmap(bitmap, false);
    if (!imageResult) {
        return;
    }
    auto image = imageResult.moveValue();

    auto imageRect = Rect::makeLTRB(0, 0, static_cast<Scalar>(image->width()), static_cast<Scalar>(image->height()));
    auto targetRect = drawBounds().makeFittingSize(imageRect.size(), fittingSizeMode);

    drawImage(*image, imageRect, targetRect, nullptr);
}

void DrawingContext::drawImage(const Image& image, const Rect& imageRect, const Rect& targetRect, const Paint* paint) {
    canvas()->drawImageRect(image.getSkValue(),
                            imageRect.getSkValue(),
                            targetRect.getSkValue(),
                            SkSamplingOptions(),
                            paint != nullptr ? &paint->getSkValue() : nullptr,
                            SkCanvas::kStrict_SrcRectConstraint);
}
#endif

void DrawingContext::clipPath(const Path& path) {
    canvas()->clipPath(path.getSkValue(), true);
}

void DrawingContext::clipRect(const Rect& rect) {
    canvas()->clipRect(rect.getSkValue(), true);
}

void DrawingContext::concat(const Matrix& matrix) {
    canvas()->concat(matrix.getSkValue());
}

const Rect& DrawingContext::drawBounds() const {
    return _drawBounds;
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
void DrawingContext::drawExternalSurface(const Ref<ExternalSurface>& externalSurface) {
    SC_ASSERT(_externalSurface == nullptr);
    _externalSurface = externalSurface;
}
#endif

int DrawingContext::save() {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return canvas()->save() + 1;
#else
    return canvas()->save();
#endif
}

void DrawingContext::restore(int count) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto* target = canvas();
    // Translate the Skia count convention without overflowing for INT_MIN.
    // TGFX subtracts its requested count from an unsigned stack size.
    const int nativeCount = count <= 1 ? 0 : count - 1;
    target->restoreToCount(std::clamp(nativeCount, 0, target->getSaveCount()));
#else
    canvas()->restoreToCount(count);
#endif
}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
tgfx::Canvas* DrawingContext::canvas() {
#else
SkCanvas* DrawingContext::canvas() {
#endif
    if (_canvas == nullptr) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        // The original bounds are cull hints, not an implicit clip. TGFX's
        // recorder has no bounds argument; retain drawBounds() for callers.
        _canvas = _recorder.beginRecording();
#else
        _canvas = _recorder.beginRecording(_drawBounds.getSkValue());
#endif
    }

    return _canvas;
}

} // namespace snap::drawing
