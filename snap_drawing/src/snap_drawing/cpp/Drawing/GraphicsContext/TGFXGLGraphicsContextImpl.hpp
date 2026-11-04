#pragma once

#if !defined(__APPLE__)
#include "snap_drawing/cpp/Drawing/GraphicsContext/GLGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXUtils.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "tgfx/gpu/GPU.h"
#include "tgfx/gpu/opengl/GLDevice.h"

namespace snap::drawing {
GLDrawableSurface::GLDrawableSurface(const Ref<GLGraphicsContext>& context,
    const tgfx::BackendTexture& texture, int sampleCount, Valdi::ColorType /*colorType*/, bool owned,
    std::shared_ptr<tgfx::Texture> owner)
    : _context(context), _backendTexture(texture), _texture(std::move(owner)),
      _sampleCount(sampleCount), _owned(owned) {}
GLDrawableSurface::~GLDrawableSurface() {
    if (_frame) {
        (void)_frame->get();
        if (_surface) {
            discardTGFXSurface(_surface.get());
            logTGFXCompletionError(
                DrawableSurfaceCanvas(_surface.get(), getTextureWidth(), getTextureHeight()).validate(),
                "Discarding failed GL texture frame during cleanup");
        }
        _surface.reset();
        _frame.reset();
    }
    auto lock = _context->lockContext();
    _surface.reset();
    _texture.reset();
}
GraphicsContext* GLDrawableSurface::getGraphicsContext() { return _context.get(); }
Valdi::Result<DrawableSurfaceCanvas> GLDrawableSurface::prepareCanvas() {
    if (!_frame) { _completionResult = Valdi::Void(); _frame = _context->lockContext(); }
    if (!*_frame) {
        _frame.reset();
        return Valdi::Error("Failed to lock TGFX GL device");
    }
    if (_owned && !_texture) {
        _texture = _frame->get()->gpu()->importBackendTexture(_backendTexture,
            tgfx::TextureUsage::TEXTURE_BINDING | tgfx::TextureUsage::RENDER_ATTACHMENT, true);
        if (!_texture) {
            _frame.reset();
            return Valdi::Error("Failed to adopt TGFX GL texture");
        }
    }
    if (!_surface) {
        _surface = tgfx::Surface::MakeFrom(_frame->get(), _backendTexture,
            tgfx::ImageOrigin::BottomLeft, _sampleCount);
        if (!_surface) {
            _frame.reset();
            return Valdi::Error("Failed to wrap GL texture in TGFX surface");
        }
    }
    DrawableSurfaceCanvas canvas(_surface.get(), getTextureWidth(), getTextureHeight());
    auto status = canvas.validate();
    if (!status) {
        _completionResult = status;
        _surface.reset();
        _frame.reset();
        return status.moveError();
    }
    return canvas;
}
void GLDrawableSurface::flush() {
    logTGFXCompletionError(flushWithResult(), "TGFX legacy GL texture flush");
}
Valdi::Result<Valdi::Void> GLDrawableSurface::flushWithResult() {
    if (!_frame) { return _completionResult; }
    (void)_frame->get();
    _completionResult = DrawableSurfaceCanvas(_surface.get(), getTextureWidth(), getTextureHeight()).flush(true);
    if (!_completionResult) {
        discardTGFXSurface(_surface.get());
        _surface.reset();
    }
    _frame.reset();
    return _completionResult;
}
void GLDrawableSurface::flushSurface(tgfx::Surface* surface) {
    if (surface) {
        auto completed = DrawableSurfaceCanvas(surface, surface->width(), surface->height()).flush(true);
        logTGFXCompletionError(completed, "TGFX legacy GL surface flush");
    }
}
unsigned GLDrawableSurface::getTextureId() const {
    tgfx::GLTextureInfo info;
    return _backendTexture.getGLTextureInfo(&info) ? info.id : 0;
}
int GLDrawableSurface::getTextureWidth() const { return _backendTexture.width(); }
int GLDrawableSurface::getTextureHeight() const { return _backendTexture.height(); }
GLGraphicsContext::GLGraphicsContext(std::shared_ptr<tgfx::Device> device,
    const Ref<GrGraphicsContextOptions>& options) : GrGraphicsContext(std::move(device), options) {}
GLGraphicsContext::~GLGraphicsContext() = default;
Valdi::Result<Ref<GLGraphicsContext>> GLGraphicsContext::create(const Ref<GrGraphicsContextOptions>& options) {
    auto nativeContext = tgfx::GLDevice::CurrentNativeHandle();
    if (!nativeContext) { return Valdi::Error("A current native GL context is required for texture sharing"); }
    // Give TGFX an owned context in the caller's texture share group. In particular,
    // do not reuse a borrowed context whose GL state the caller can change between
    // frames: this TGFX revision has no public resetContext/resetGLState API.
    // On Android GLDevice::Make creates a real EGLDevice with a pbuffer surface;
    // its lock switches to that context and restores the caller's EGL state.
    auto device = tgfx::GLDevice::Make(nativeContext);
    if (!device) { return Valdi::Error("Failed to create shared TGFX GL device"); }
    return Valdi::makeShared<GLGraphicsContext>(std::move(device), options);
}
Ref<GLDrawableSurface> GLGraphicsContext::createSurface(int width, int height,
    Valdi::ColorType colorType, int sampleCount) {
    auto lock = lockContext();
    if (!*lock) { return nullptr; }
    tgfx::PixelFormat format;
    switch (toTGFXColorType(colorType)) {
        case tgfx::ColorType::RGBA_8888: format = tgfx::PixelFormat::RGBA_8888; break;
        case tgfx::ColorType::BGRA_8888: format = tgfx::PixelFormat::BGRA_8888; break;
        case tgfx::ColorType::ALPHA_8: format = tgfx::PixelFormat::ALPHA_8; break;
        case tgfx::ColorType::Gray_8: format = tgfx::PixelFormat::GRAY_8; break;
        default: return nullptr;
    }
    tgfx::TextureDescriptor descriptor(width, height, format, true, 1,
        tgfx::TextureUsage::TEXTURE_BINDING | tgfx::TextureUsage::RENDER_ATTACHMENT);
    auto texture = lock->get()->gpu()->createTexture(descriptor);
    if (!texture) { return nullptr; }
    auto backend = texture->getBackendTexture();
    if (!backend.isValid()) { return nullptr; }
    // Publish texture allocation to consumers in the host's shared GL context.
    lock->get()->gpu()->queue()->waitUntilCompleted();
    return Valdi::makeShared<GLDrawableSurface>(Valdi::strongSmallRef(this), backend, sampleCount,
                                               colorType, true, std::move(texture));
}
Ref<GLDrawableSurface> GLGraphicsContext::createSurfaceWrappingTexture(unsigned textureId,
    int width, int height, Valdi::ColorType colorType, int sampleCount, unsigned format, unsigned target) {
    tgfx::GLTextureInfo info;
    info.id = textureId;
    info.format = format;
    info.target = target;
    tgfx::BackendTexture texture(info, width, height);
    if (!texture.isValid() || !textureId) { return nullptr; }
    return Valdi::makeShared<GLDrawableSurface>(Valdi::strongSmallRef(this), texture, sampleCount, colorType, false);
}
} // namespace snap::drawing

#endif // non-Apple GL backend; Apple TGFX builds use Metal
