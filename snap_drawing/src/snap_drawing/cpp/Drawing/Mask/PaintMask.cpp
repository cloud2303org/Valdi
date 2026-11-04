//
//  PaintMask.cpp
//  snap_drawing-pc
//
//  Created by Simon Corsin on 10/20/22.
//

#include "snap_drawing/cpp/Drawing/Mask/PaintMask.hpp"

#include "valdi_core/cpp/Utils/Format.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkCanvas.h"

// This is a private header in Skia.
// This allows us to leverage the saveBehind API

class SkCanvasPriv {
public:
    // NOLINTNEXTLINE
    static int SaveBehind(SkCanvas* canvas, const SkRect* subset) {
        return canvas->only_axis_aligned_saveBehind(subset);
    }
};
#endif

namespace snap::drawing {

PaintMask::PaintMask(const Paint& paint, const Path& path, const Rect& rect)
    : _paint(paint), _path(path), _rect(rect) {}

PaintMask::~PaintMask() = default;

Rect PaintMask::getBounds() const {
    auto bounds = _path.getBounds();
    return bounds ? bounds.value() : _rect;
}

void PaintMask::prepare(NativeCanvas* canvas) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // The native record captures/clears only this mapped subset at playback.
    // Destination-dependent drawing outside it continues to see prior content.
    // Even a failed capture pushes a balanced save. The native context retains
    // a sticky render error, observable at the void draw/readback boundary.
    canvas->saveBehind(getBounds().getSkValue());
#else
    auto bounds = getBounds();
    SkCanvasPriv::SaveBehind(canvas, &bounds.getSkValue());
#endif
}

void PaintMask::apply(NativeCanvas* canvas) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    if (_path.isEmpty()) {
        _paint.drawRect(canvas, _rect.getSkValue());
    } else {
        _paint.drawPath(canvas, _path.getSkValue());
    }
#else
    if (_path.isEmpty()) {
        canvas->drawRect(_rect.getSkValue(), _paint.getSkValue());
    } else {
        canvas->drawPath(_path.getSkValue(), _paint.getSkValue());
    }
#endif

    canvas->restore();
}

String PaintMask::getDescription() {
    if (_path.isEmpty()) {
        return STRING_FORMAT("PaintMask with rect {}", _rect);
    } else {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        return STRING_FORMAT("PaintMask with path bounds {}", getBounds());
#else
        return STRING_FORMAT("PaintMask with path {}", _path);
#endif
    }
}

} // namespace snap::drawing
