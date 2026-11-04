//
//  DrawableSurfaceCanvas.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/20/22.
//

#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Buffer.h"
#include "tgfx/core/ImageInfo.h"
#include "tgfx/core/Pixmap.h"
#include "tgfx/core/SurfaceReadback.h"
#include "tgfx/gpu/Context.h"
#include <limits>
#endif

namespace snap::drawing {

DrawableSurfaceCanvas::DrawableSurfaceCanvas(NativeSurface* surface, int width, int height)
    : _surface(surface), _canvas(surface->getCanvas()), _width(width), _height(height) {}

NativeCanvas* DrawableSurfaceCanvas::getSkiaCanvas() const {
    return _canvas;
}

NativeSurface* DrawableSurfaceCanvas::getSkiaSurface() const {
    return _surface;
}

int DrawableSurfaceCanvas::getWidth() const {
    return _width;
}

int DrawableSurfaceCanvas::getHeight() const {
    return _height;
}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
Valdi::Result<Valdi::Void> DrawableSurfaceCanvas::validate() const {
    if (_canvas->hasError()) {
        discard();
        return Valdi::Error("TGFX canvas reported a native render failure");
    }
    return Valdi::Void();
}

void DrawableSurfaceCanvas::discard() const {
    auto* context = _surface->getContext();
    // flush() closes deferred compositors but never encodes/submits. Only the
    // returned identity is canceled; earlier unrelated recordings stay queued.
    auto recording = context->flush();
    if (recording) { context->discard(std::move(recording)); }
}

Valdi::Result<Valdi::Void> DrawableSurfaceCanvas::flush(bool syncCpu) const {
    auto status = validate();
    if (!status) { return status.moveError(); }
    auto* context = _surface->getContext();
    // Closing compositors can replay deferred images/pictures and set hasError.
    // Split flush from submit so a failure known at that point is never submitted.
    auto recording = context->flush();
    status = validate();
    if (!status) {
        // Releasing Recording does not remove its DrawingBuffer from the native
        // pending queue. Cancel this exact batch before dropping the token.
        context->discard(std::move(recording));
        return status.moveError();
    }
    if (recording || syncCpu) {
        context->submit(std::move(recording), syncCpu);
        status = validate();
        if (!status) { return status.moveError(); }
    }
    return Valdi::Void();
}

Valdi::Result<NativeData> DrawableSurfaceCanvas::readPixels(const tgfx::ImageInfo& info) const {
    auto status = validate();
    if (!status) { return status.moveError(); }
    if (info.isEmpty() || info.width() != _width || info.height() != _height ||
        _width != _surface->width() || _height != _surface->height() ||
        info.rowBytes() > std::numeric_limits<size_t>::max() / static_cast<size_t>(info.height())) {
        discard();
        return Valdi::Error("Invalid TGFX completion readback dimensions or stride");
    }
    tgfx::Buffer pixels;
    if (!pixels.alloc(info.byteSize())) {
        discard();
        return Valdi::Error("Failed to allocate TGFX completion readback pixels");
    }
    // Do not expose row padding from an uninitialized readback allocation.
    pixels.clear();
    // Surface::readPixels/SurfaceReadback::lockPixels can implicitly flush+submit.
    // Record the transfer explicitly, check materialization, then use our checked
    // flush to cancel a failed batch before mapping any pixels.
    auto readback = _surface->asyncReadPixels(tgfx::Rect::MakeWH(_width, _height));
    status = validate();
    if (!status) { return status.moveError(); }
    if (!readback) {
        discard();
        return Valdi::Error("Failed to create TGFX completion readback");
    }
    status = flush(true);
    if (!status) { return status.moveError(); }
    auto* context = _surface->getContext();
    auto* source = readback->lockPixels(context, _surface->origin() == tgfx::ImageOrigin::BottomLeft);
    status = validate();
    if (!status) {
        if (source) { readback->unlockPixels(context); }
        return status.moveError();
    }
    if (!source) {
        discard();
        return Valdi::Error("Failed to lock TGFX completion readback pixels");
    }
    const bool copied = tgfx::Pixmap(readback->info(), source).readPixels(info, pixels.data());
    readback->unlockPixels(context);
    status = validate();
    if (!status) { return status.moveError(); }
    if (!copied) { return Valdi::Error("Failed to read TGFX surface pixels"); }
    auto data = pixels.release();
    if (!data) { return Valdi::Error("Failed to retain TGFX completion readback pixels"); }
    return data;
}

Valdi::Result<NativeImage> DrawableSurfaceCanvas::makeImageSnapshot() const {
    auto status = validate();
    if (!status) { return status.moveError(); }
    auto image = _surface->makeImageSnapshot();
    status = validate();
    if (!status) { return status.moveError(); }
    if (!image) {
        discard();
        return Valdi::Error("Failed to create TGFX GPU surface snapshot");
    }
    return image;
}
#endif

Valdi::Result<Ref<Image>> DrawableSurfaceCanvas::snapshot() const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Image snapshots escape the drawable's device/frame lifetime (PNG encoding,
    // bitmap conversion and later composition can run on a different GPU device).
    // Read back while the preparing thread still owns the device lock and return
    // a TGFX image with independently owned immutable pixels.
    auto info = tgfx::ImageInfo::Make(_width, _height, tgfx::ColorType::RGBA_8888);
    auto pixels = readPixels(info);
    if (!pixels) { return pixels.moveError(); }
    auto snapshot = tgfx::Image::MakeFrom(info, pixels.moveValue());
    auto status = validate();
    if (!status) { return status.moveError(); }
#else
    auto snapshot = getSkiaSurface()->makeImageSnapshot();
#endif
    if (snapshot == nullptr) {
        return Valdi::Error("Failed to create snapshot");
    }

    return Ref<Image>(Valdi::makeShared<Image>(snapshot));
}

} // namespace snap::drawing
