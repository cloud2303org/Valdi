#pragma once
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurface.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include "tgfx/gpu/Window.h"

namespace snap::drawing {
struct MetalLayerSize {
    double width;
    double height;
    constexpr MetalLayerSize(double width, double height) : width(width), height(height) {}
};
class MetalGraphicsContext;
class MetalDrawableSurface : public DrawableSurface {
public:
    MetalDrawableSurface(const Ref<MetalGraphicsContext>& context, CFTypeRef layer, int sampleCount);
    ~MetalDrawableSurface() override;
    GraphicsContext* getGraphicsContext() override;
    Valdi::Result<DrawableSurfaceCanvas> prepareCanvas() override;
    void flush() override;
    Valdi::Result<Valdi::Void> flushWithResult() override;
    CFTypeRef getMTLLayer() const;
private:
    Ref<MetalGraphicsContext> _context;
    CFTypeRef _mtlLayer;
    int _sampleCount;
    std::shared_ptr<tgfx::Window> _window;
    std::shared_ptr<tgfx::Surface> _surface;
    std::shared_ptr<tgfx::Surface> _displaySurface;
    std::unique_ptr<TGFXContextLock> _frame;
    Valdi::Result<Valdi::Void> _completionResult = Valdi::Void();
};
class MetalGraphicsContext : public GrGraphicsContext {
public:
    MetalGraphicsContext(CFTypeRef device, CFTypeRef queue, const Ref<GrGraphicsContextOptions>& options);
    ~MetalGraphicsContext() override;
    Ref<DrawableSurface> createSurfaceForMetalLayer(CFTypeRef layer, int sampleCount);
    // Retains the supplied id<MTLTexture>. It must belong to this MTLDevice and
    // have RenderTarget and ShaderRead usage; external queue work must be synchronized.
    Ref<DrawableSurface> createSurfaceWrappingTexture(CFTypeRef texture, int sampleCount = 1);
    void commit() override;
    CFTypeRef getMTLDevice() const;
    // Legacy host queue retained for compatibility. TGFX MetalDevice owns and uses
    // its own queue; no TGFX drawing or presentation is encoded on the host queue.
    CFTypeRef getMTLCommandQueue() const;
private:
    CFTypeRef _mtlDevice;
    CFTypeRef _mtlCommandQueue;
    friend class MetalDrawableSurface;
    static std::shared_ptr<tgfx::Device> makeDevice(CFTypeRef device);
    static std::shared_ptr<tgfx::Window> makeWindow(CFTypeRef layer, const std::shared_ptr<tgfx::Device>& device);
    static MetalLayerSize getMetalLayerSize(CFTypeRef layer);
};
} // namespace snap::drawing
