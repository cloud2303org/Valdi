//
//  DrawingContext.hpp
//  valdi-skia-app
//
//  Created by Simon Corsin on 6/28/20.
//

#pragma once

#include "utils/base/NonCopyable.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#endif

#include "snap_drawing/cpp/Drawing/LayerContent.hpp"
#include "snap_drawing/cpp/Drawing/Paint.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
#include "snap_drawing/cpp/Utils/Image.hpp"
#endif
#include "snap_drawing/cpp/Utils/Matrix.hpp"
#include "snap_drawing/cpp/Utils/Path.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Canvas.h"
#include "tgfx/core/PictureRecorder.h"
#else
#include "include/core/SkCanvas.h"
#include "include/core/SkPictureRecorder.h"
#endif

#include <optional>

namespace snap::drawing {

class BorderRadius;
class LazyPath;
#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
class ExternalSurface;
#endif

class DrawingContext : public snap::NonCopyable {
public:
    DrawingContext(Scalar width, Scalar height);
    ~DrawingContext();

    /**
     Complete the rendering. This must be called after prepare() has been called.
     */
    LayerContent finish();

    /**
     Draw the paint to the context with the given border radius.
     */
    void drawPaint(const Paint& paint, const BorderRadius& borderRadius, LazyPath& lazyPath);

    /**
     Draw the paint to the context with the given border radius at the given targetRect.
     */
    void drawPaint(const Paint& paint, const BorderRadius& borderRadius, const Rect& targetRect, LazyPath& lazyPath);

    /**
     Draw the paint as a rect to the context at the given targetRect.
     */
    void drawPaint(const Paint& paint, const Rect& targetRect);

    /**
     Draw the paint with the given path
     */
    void drawPaint(const Paint& paint, const Path& path);

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
    /**
     Draw the bitmap into the current draw bounds, scaling the bitmap with the given FittingSizeMode
     */
    void drawBitmap(const Valdi::Ref<Valdi::IBitmap>& bitmap, FittingSizeMode fittingSizeMode);

    /**
     Draw the image into the given target rect
     */
    void drawImage(const Image& image, const Rect& imageRect, const Rect& targetRect, const Paint* paint);

    void drawExternalSurface(const Ref<ExternalSurface>& externalSurface);
#endif

    void clipPath(const Path& path);

    void clipRect(const Rect& rect);

    void concat(const Matrix& matrix);

    int save();

    void restore(int count);

    const Rect& drawBounds() const;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    tgfx::Canvas* canvas();
#else
    SkCanvas* canvas();
#endif

private:
    Rect _drawBounds;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    tgfx::PictureRecorder _recorder;
    tgfx::Canvas* _canvas = nullptr;
#else
    SkPictureRecorder _recorder;
    SkCanvas* _canvas = nullptr;
#endif
#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
    Ref<ExternalSurface> _externalSurface;
#endif
};

} // namespace snap::drawing
