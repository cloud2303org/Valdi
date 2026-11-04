#pragma once

#if defined(__APPLE__)
#include "snap_drawing/cpp/Drawing/GraphicsContext/MetalGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "tgfx/core/Paint.h"
#include <algorithm>

namespace snap::drawing {
MetalDrawableSurface::MetalDrawableSurface(const Ref<MetalGraphicsContext>& context, CFTypeRef layer, int samples)
    : _context(context), _mtlLayer(CFRetain(layer)), _sampleCount(std::max(1, samples)),
      _window(MetalGraphicsContext::makeWindow(layer, context->getDevice())) {}
MetalDrawableSurface::~MetalDrawableSurface() {
    if (_frame) {
        (void)_frame->get();
        if (_surface) {
            discardTGFXSurface(_surface.get());
            logTGFXCompletionError(
                DrawableSurfaceCanvas(_surface.get(), _surface->width(), _surface->height()).validate(),
                "Discarding failed Metal frame during cleanup");
        }
        _surface.reset();
        _displaySurface.reset();
        _frame.reset();
    }
    {
        auto lock = _context->lockContext();
        _surface.reset();
        _displaySurface.reset();
        _window.reset();
    }
    CFRelease(_mtlLayer);
}
GraphicsContext* MetalDrawableSurface::getGraphicsContext() { return _context.get(); }
CFTypeRef MetalDrawableSurface::getMTLLayer() const { return _mtlLayer; }
Valdi::Result<DrawableSurfaceCanvas> MetalDrawableSurface::prepareCanvas() {
    if (!_window) { return Valdi::Error("Failed to create TGFX Metal window/device"); }
    if (!_frame) { _completionResult = Valdi::Void(); _frame = _context->lockContext(); }
    if (!*_frame) {
        _frame.reset();
        return Valdi::Error("Failed to lock TGFX Metal device");
    }
    auto size = MetalGraphicsContext::getMetalLayerSize(_mtlLayer);
    const auto width = static_cast<int>(size.width);
    const auto height = static_cast<int>(size.height);
    if (width <= 0 || height <= 0) {
        discardTGFXSurface(_surface.get());
        _surface.reset();
        _frame.reset();
        return Valdi::Error("Metal layer has no drawable area");
    }
    if (!_displaySurface || _displaySurface->width() != width || _displaySurface->height() != height) {
        discardTGFXSurface(_surface.get());
        _surface.reset();
        _displaySurface = tgfx::Surface::MakeFrom(_frame->get(), _window);
    }
    if (!_displaySurface) {
        _frame.reset();
        return Valdi::Error("Failed to create TGFX Metal display surface");
    }
    if (!_surface) {
        // Render to a sampleable offscreen texture, so snapshots work even when
        // CAMetalLayer.framebufferOnly is YES. Resolve MSAA here, then composite to
        // the actual drawable texture acquired and presented by MetalWindow.
        _surface = tgfx::Surface::Make(_frame->get(), width, height, tgfx::ColorType::BGRA_8888, _sampleCount);
    }
    if (!_surface) {
        _frame.reset();
        return Valdi::Error("Failed to create TGFX Metal offscreen render surface");
    }
    DrawableSurfaceCanvas canvas(_surface.get(), width, height);
    auto status = canvas.validate();
    if (!status) {
        _completionResult = status;
        _surface.reset();
        _frame.reset();
        return status.moveError();
    }
    return canvas;
}
void MetalDrawableSurface::flush() {
    logTGFXCompletionError(flushWithResult(), "TGFX legacy Metal frame flush");
}
Valdi::Result<Valdi::Void> MetalDrawableSurface::flushWithResult() {
    if (!_frame) { return _completionResult; }
    (void)_frame->get();
    _completionResult = presentTGFXSurface(_surface.get(), _displaySurface.get());
    if (!_completionResult) {
        discardTGFXSurface(_surface.get());
        discardTGFXSurface(_displaySurface.get());
        _surface.reset();
        if (_displaySurface && _displaySurface->getCanvas()->hasError()) { _displaySurface.reset(); }
    }
    _frame.reset();
    return _completionResult;
}
MetalGraphicsContext::MetalGraphicsContext(CFTypeRef device, CFTypeRef queue,
    const Ref<GrGraphicsContextOptions>& options)
    : GrGraphicsContext(makeDevice(device), options),
      _mtlDevice(device ? CFRetain(device) : nullptr), _mtlCommandQueue(queue ? CFRetain(queue) : nullptr) {}
MetalGraphicsContext::~MetalGraphicsContext() {
    commit();
    if (_mtlCommandQueue) { CFRelease(_mtlCommandQueue); }
    if (_mtlDevice) { CFRelease(_mtlDevice); }
}
Ref<DrawableSurface> MetalGraphicsContext::createSurfaceForMetalLayer(CFTypeRef layer, int sampleCount) {
    if (!layer) { return nullptr; }
    return Valdi::makeShared<MetalDrawableSurface>(Valdi::strongSmallRef(this), layer, sampleCount);
}
void MetalGraphicsContext::commit() { GrGraphicsContext::commit(); }
CFTypeRef MetalGraphicsContext::getMTLDevice() const { return _mtlDevice; }
CFTypeRef MetalGraphicsContext::getMTLCommandQueue() const { return _mtlCommandQueue; }
} // namespace snap::drawing
#endif
