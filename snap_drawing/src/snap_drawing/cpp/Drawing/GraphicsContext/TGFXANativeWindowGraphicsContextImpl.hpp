#pragma once

#if defined(__ANDROID__)
#include "snap_drawing/cpp/Drawing/GraphicsContext/ANativeWindowGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "tgfx/gpu/opengl/egl/EGLWindow.h"
#include "tgfx/core/Paint.h"
#include <algorithm>

namespace snap::drawing {
class ANativeWindowDrawableSurface : public DrawableSurface {
public:
    ANativeWindowDrawableSurface(ANativeWindow* nativeWindow,
        const Ref<ANativeWindowGraphicsContext>& manager)
        : _nativeWindow(nativeWindow), _manager(manager),
          _samples(std::max(1, manager->_displayParams.mSAASampleCount)) {
        ANativeWindow_acquire(_nativeWindow);
    }
    ~ANativeWindowDrawableSurface() override {
        if (_frame) {
            (void)_frame->get();
            if (_surface) {
                discardTGFXSurface(_surface.get());
                logTGFXCompletionError(
                    DrawableSurfaceCanvas(_surface.get(), _surface->width(), _surface->height()).validate(),
                    "Discarding failed EGL frame during cleanup");
            }
            _surface.reset();
            _displaySurface.reset();
            _frame.reset();
        }
        if (_context != nullptr) {
            auto lock = _context->lockContext();
            _surface.reset();
            _displaySurface.reset();
        }
        _window.reset();
        // EGLDevice must release its EGLSurface before the native window reference.
        _context = nullptr;
        ANativeWindow_release(_nativeWindow);
    }
    GraphicsContext* getGraphicsContext() override { return _manager.get(); }
    Valdi::Result<DrawableSurfaceCanvas> prepareCanvas() override {
        if (_context == nullptr) {
            // SurfaceView can report the same native window twice. Connect only
            // after DrawLooper has replaced and released the previous drawable.
            auto window = tgfx::EGLWindow::MakeFrom(
                _nativeWindow, nullptr, nullptr, !_manager->_displayParams.disableVsync);
            if (!window) { return Valdi::Error("Failed to create TGFX EGL native window"); }
            auto context = Valdi::makeShared<GrGraphicsContext>(window->getDevice(), _manager->_options);
            _manager->applyCacheLimit(context);
            {
                std::lock_guard<std::mutex> guard(_manager->_mutex);
                _manager->_devices.emplace_back(context->getDevice());
            }
            _window = std::move(window);
            _context = std::move(context);
        }
        if (!_frame) { _completionResult = Valdi::Void(); _frame = _context->lockContext(); }
        if (!*_frame) {
            _frame.reset();
            return Valdi::Error("Failed to lock native-window TGFX EGL device");
        }
        const auto width = ANativeWindow_getWidth(_nativeWindow);
        const auto height = ANativeWindow_getHeight(_nativeWindow);
        if (width <= 0 || height <= 0) {
            discardTGFXSurface(_surface.get());
            _surface.reset();
            _frame.reset();
            return Valdi::Error("Native window has no drawable area");
        }
        if (!_displaySurface || _displaySurface->width() != width || _displaySurface->height() != height) {
            discardTGFXSurface(_surface.get());
            _surface.reset();
            _displaySurface = tgfx::Surface::MakeFrom(_frame->get(), _window);
            if (!_displaySurface) {
                _frame.reset();
                return Valdi::Error("Failed to create TGFX EGL window surface");
            }
        }
        if (!_surface) {
            // Always stage user drawing offscreen, including non-MSAA frames.
            // Deferred native failures must be consumed before any window present.
            _surface = tgfx::Surface::Make(_frame->get(), width, height,
                tgfx::ColorType::RGBA_8888, _samples);
            if (!_surface) {
                _frame.reset();
                return Valdi::Error("Failed to create TGFX native-window MSAA target");
            }
        }
        DrawableSurfaceCanvas canvas(_surface.get(), _surface->width(), _surface->height());
        auto status = canvas.validate();
        if (!status) {
            _completionResult = status;
            _surface.reset();
            _frame.reset();
            return status.moveError();
        }
        return canvas;
    }
    void flush() override {
        logTGFXCompletionError(flushWithResult(), "TGFX legacy EGL frame flush");
    }
    Valdi::Result<Valdi::Void> flushWithResult() override {
        if (!_frame) { return _completionResult; }
        (void)_frame->get(); // Keep thread/context ownership checks at completion.
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
private:
    ANativeWindow* _nativeWindow;
    Ref<ANativeWindowGraphicsContext> _manager;
    std::shared_ptr<tgfx::EGLWindow> _window;
    Ref<GrGraphicsContext> _context;
    int _samples;
    std::shared_ptr<tgfx::Surface> _surface;
    std::shared_ptr<tgfx::Surface> _displaySurface;
    std::unique_ptr<TGFXContextLock> _frame;
    Valdi::Result<Valdi::Void> _completionResult = Valdi::Void();
};
ANativeWindowGraphicsContext::ANativeWindowGraphicsContext(const DisplayParams& params,
    const Ref<GrGraphicsContextOptions>& options) : _displayParams(params), _options(options) {}
ANativeWindowGraphicsContext::~ANativeWindowGraphicsContext() = default;
std::vector<Ref<GrGraphicsContext>> ANativeWindowGraphicsContext::liveContexts() {
    std::lock_guard<std::mutex> guard(_mutex);
    std::vector<Ref<GrGraphicsContext>> contexts;
    for (auto it = _devices.begin(); it != _devices.end();) {
        auto device = it->lock();
        if (device) { contexts.emplace_back(Valdi::makeShared<GrGraphicsContext>(std::move(device), _options)); ++it; }
        else { it = _devices.erase(it); }
    }
    return contexts;
}
void ANativeWindowGraphicsContext::applyCacheLimit(const Ref<GrGraphicsContext>& context) {
    // Never hold the manager mutex while waiting on a frame's device lock.
    auto lock = context->lockContext();
    std::optional<size_t> limit;
    { std::lock_guard<std::mutex> guard(_mutex); limit = _cacheLimit; }
    if (*lock && limit) { lock->get()->setCacheLimit(*limit); }
}
void ANativeWindowGraphicsContext::setResourceCacheLimit(size_t bytes) {
    { std::lock_guard<std::mutex> guard(_mutex); _cacheLimit = bytes; }
    for (const auto& context : liveContexts()) { applyCacheLimit(context); }
}
void ANativeWindowGraphicsContext::performCleanup(bool purgeScratch, std::chrono::seconds age) {
    for (const auto& context : liveContexts()) { context->performCleanup(purgeScratch, age); }
}
void ANativeWindowGraphicsContext::commit() {
    for (const auto& context : liveContexts()) { context->commit(); }
}
Ref<DrawableSurface> ANativeWindowGraphicsContext::createSurfaceForNativeWindow(ANativeWindow* nativeWindow) {
    if (!nativeWindow) { return nullptr; }
    return Valdi::makeShared<ANativeWindowDrawableSurface>(nativeWindow, Valdi::strongSmallRef(this));
}
int64_t ANativeWindowGraphicsContext::getMaxRenderTargetSize() {
    Ref<GrGraphicsContext> offscreen;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_offscreen == nullptr) {
            _offscreen = GrGraphicsContext::makeOffscreen(_options);
            if (_offscreen != nullptr) {
                _devices.emplace_back(_offscreen->getDevice());
            }
        }
        offscreen = _offscreen;
    }
    if (offscreen == nullptr) { return -1; }
    applyCacheLimit(offscreen);
    return offscreen->getMaxRenderTargetSize();
}
Ref<ANativeWindowGraphicsContext> ANativeWindowGraphicsContext::create(const DisplayParams& params,
    const Ref<GrGraphicsContextOptions>& options) {
    return Valdi::makeShared<ANativeWindowGraphicsContext>(params, options);
}
} // namespace snap::drawing
#endif
