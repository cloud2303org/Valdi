#if defined(__ANDROID__) && !defined(SNAP_DRAWING_TGFX_EXPERIMENT)

#include "gpu_target.hpp"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"
#include "valdi_core/cpp/Utils/Shared.hpp"

#include <EGL/egl.h>

namespace snap::drawing::performance {
namespace {

// Pbuffer bootstrap follows ANativeWindowGraphicsContext's real EGL/Ganesh
// construction without introducing a window, swapchain, or raster fallback.
struct EGLState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLDisplay previousDisplay = eglGetCurrentDisplay();
    EGLContext previousContext = eglGetCurrentContext();
    EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
    EGLenum previousAPI = eglQueryAPI();
    bool current = false;

    ~EGLState() {
        if (current) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (context != EGL_NO_CONTEXT) { eglDestroyContext(display, context); }
        if (pbuffer != EGL_NO_SURFACE) { eglDestroySurface(display, pbuffer); }
        // EGL_DEFAULT_DISPLAY is process-shared (including Android's UI).
        // Destroy only our context/pbuffer; leave display lifetime to the host.
        eglBindAPI(previousAPI);
        if (current && previousDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext);
        }
    }
};

class OffscreenSkiaContext final : public GrGraphicsContext {
public:
    OffscreenSkiaContext(const sk_sp<GrDirectContext>& context, const Ref<GrGraphicsContextOptions>& options,
                        std::unique_ptr<EGLState> state)
        : GrGraphicsContext(context, options), _state(std::move(state)) {}

    ~OffscreenSkiaContext() override {
        // The base destructor runs after _state; release GPU resources while
        // this context's EGL owner is still current. The base's abandon is safe.
        getGrContext().releaseResourcesAndAbandonContext();
    }

private:
    std::unique_ptr<EGLState> _state;
};

} // namespace

Valdi::Result<Ref<GrGraphicsContext>> makeSkiaOffscreenContext() {
    auto state = std::make_unique<EGLState>();
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { return Valdi::Error("Skia: eglBindAPI failed"); }
    state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (state->display == EGL_NO_DISPLAY || !eglInitialize(state->display, nullptr, nullptr)) {
        return Valdi::Error("Skia: cannot initialize offscreen EGL display");
    }
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_STENCIL_SIZE, 8, EGL_SAMPLE_BUFFERS, 0, EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(state->display, attributes, &config, 1, &count) || count < 1) {
        return Valdi::Error("Skia: no RGBA8 offscreen EGL config");
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, contextAttributes);
    const EGLint pbufferAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    state->pbuffer = eglCreatePbufferSurface(state->display, config, pbufferAttributes);
    if (state->context == EGL_NO_CONTEXT || state->pbuffer == EGL_NO_SURFACE ||
        !eglMakeCurrent(state->display, state->pbuffer, state->pbuffer, state->context)) {
        return Valdi::Error("Skia: cannot create/make current offscreen EGL context");
    }
    state->current = true;
    auto options = Valdi::makeShared<GrGraphicsContextOptions>();
    auto interface = GrGLInterfaces::MakeEGL();
    if (!interface) { return Valdi::Error("Skia: GrGLInterfaces::MakeEGL failed"); }
    auto graphics = GrDirectContexts::MakeGL(std::move(interface), options->getGrContextOptions());
    if (!graphics) { return Valdi::Error("Skia: Ganesh MakeGL failed"); }
    Ref<GrGraphicsContext> result =
        Valdi::makeShared<OffscreenSkiaContext>(graphics, options, std::move(state));
    return result;
}

} // namespace snap::drawing::performance
#endif
