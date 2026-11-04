#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "snap_drawing/cpp/Drawing/GraphicsContext/MetalGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "tgfx/gpu/metal/MetalDevice.h"
#include "tgfx/gpu/metal/MetalWindow.h"
#include <algorithm>

namespace snap::drawing {
std::shared_ptr<tgfx::Device> makeTGFXOffscreenMetalDevice() { return tgfx::MetalDevice::Make(); }
std::shared_ptr<tgfx::Device> MetalGraphicsContext::makeDevice(CFTypeRef device) {
    return device ? tgfx::MetalDevice::MakeFrom((__bridge id<MTLDevice>)device) : nullptr;
}
std::shared_ptr<tgfx::Window> MetalGraphicsContext::makeWindow(CFTypeRef layer,
    const std::shared_ptr<tgfx::Device>& device) {
    if (!device || !layer) { return nullptr; }
    CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)layer;
    auto metalDevice = std::static_pointer_cast<tgfx::MetalDevice>(device);
    if (metalLayer.device && metalLayer.device != metalDevice->metalDevice()) { return nullptr; }
    return tgfx::MetalWindow::MakeFrom(metalLayer, std::move(metalDevice));
}
MetalLayerSize MetalGraphicsContext::getMetalLayerSize(CFTypeRef layer) {
    CGSize size = ((__bridge CAMetalLayer*)layer).drawableSize;
    return {size.width, size.height};
}

class MetalTextureDrawableSurface : public DrawableSurface {
public:
    MetalTextureDrawableSurface(const Ref<MetalGraphicsContext>& context, CFTypeRef texture, int samples)
        : _context(context), _texture(CFRetain(texture)), _sampleCount(std::max(1, samples)) {}
    ~MetalTextureDrawableSurface() override {
        if (_frame) {
            (void)_frame->get();
            if (_surface) {
                discardTGFXSurface(_surface.get());
                logTGFXCompletionError(
                    DrawableSurfaceCanvas(_surface.get(), _surface->width(), _surface->height()).validate(),
                    "Discarding failed Metal texture frame during cleanup");
            }
            _surface.reset();
            _frame.reset();
        }
        { auto lock = _context->lockContext(); _surface.reset(); }
        CFRelease(_texture);
    }
    GraphicsContext* getGraphicsContext() override { return _context.get(); }
    Valdi::Result<DrawableSurfaceCanvas> prepareCanvas() override {
        if (!_frame) { _completionResult = Valdi::Void(); _frame = _context->lockContext(); }
        if (!*_frame) {
            _frame.reset();
            return Valdi::Error("Failed to lock TGFX Metal texture device");
        }
        if (!_surface) {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)_texture;
            tgfx::MetalTextureInfo info;
            info.texture = (__bridge const void*)texture;
            info.format = static_cast<unsigned>(texture.pixelFormat);
            tgfx::BackendTexture backend(info, static_cast<int>(texture.width), static_cast<int>(texture.height));
            _surface = tgfx::Surface::MakeFrom(_frame->get(), backend, tgfx::ImageOrigin::TopLeft, _sampleCount);
            if (!_surface) {
                _frame.reset();
                return Valdi::Error("Failed to wrap native MTLTexture in TGFX surface");
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
        logTGFXCompletionError(flushWithResult(), "TGFX legacy Metal texture flush");
    }
    Valdi::Result<Valdi::Void> flushWithResult() override {
        if (!_frame) { return _completionResult; }
        // Complete external-queue consumers' GPU writes before returning success.
        (void)_frame->get();
        _completionResult = DrawableSurfaceCanvas(_surface.get(), _surface->width(), _surface->height()).flush(true);
        if (!_completionResult) {
            discardTGFXSurface(_surface.get());
            _surface.reset();
        }
        _frame.reset();
        return _completionResult;
    }
private:
    Ref<MetalGraphicsContext> _context;
    CFTypeRef _texture;
    int _sampleCount;
    std::shared_ptr<tgfx::Surface> _surface;
    std::unique_ptr<TGFXContextLock> _frame;
    Valdi::Result<Valdi::Void> _completionResult = Valdi::Void();
};
Ref<DrawableSurface> MetalGraphicsContext::createSurfaceWrappingTexture(CFTypeRef textureRef, int sampleCount) {
    if (!textureRef || !getDevice()) { return nullptr; }
    id<MTLTexture> texture = (__bridge id<MTLTexture>)textureRef;
    auto requiredUsage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (texture.device != (__bridge id<MTLDevice>)_mtlDevice || texture.textureType != MTLTextureType2D ||
        (texture.usage & requiredUsage) != requiredUsage || texture.width == 0 || texture.height == 0) {
        return nullptr;
    }
    return Valdi::makeShared<MetalTextureDrawableSurface>(Valdi::strongSmallRef(this), textureRef, sampleCount);
}
} // namespace snap::drawing
#else
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#import "snap_drawing/cpp/Drawing/GraphicsContext/MetalGraphicsContext.hpp"

#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/mtl/SkSurfaceMetal.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"

namespace snap::drawing {

void MetalGraphicsContext::presentMetalDrawable(CFTypeRef mtlCommandBuffer, CFTypeRef mtlDrawableHandle) {
    id<CAMetalDrawable> currentDrawable = (__bridge id<CAMetalDrawable>)mtlDrawableHandle;
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)mtlCommandBuffer;

    if (currentDrawable) {
        [commandBuffer presentDrawable:currentDrawable];
    }
}

CFTypeRef MetalGraphicsContext::createCommandBuffer(CFTypeRef mtlCommandQueue) {
    id<MTLCommandBuffer> commandBuffer = [(__bridge id<MTLCommandQueue>)mtlCommandQueue commandBuffer];
    CFTypeRef rawCommandBuffer = (__bridge CFTypeRef)(commandBuffer);

    CFRetain(rawCommandBuffer);
    return rawCommandBuffer;
}

void MetalGraphicsContext::commitCommandBuffer(CFTypeRef mtlCommandBuffer) {
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)mtlCommandBuffer;
    [commandBuffer commit];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

snap::drawing::MetalLayerSize MetalGraphicsContext::getMetalLayerSize(CFTypeRef mtlLayer) {
    CAMetalLayer *metalLayer = (__bridge CAMetalLayer *)mtlLayer;

    CGSize drawableSize = metalLayer.drawableSize;

    return snap::drawing::MetalLayerSize(drawableSize.width, drawableSize.height);
}

#pragma clang diagnostic pop

sk_sp<GrDirectContext> MetalGraphicsContext::makeContext(void *mtlDevice, void *mtlCommandQueue, const GrContextOptions &options) {
    GrMtlBackendContext mtlBackendContext;
    mtlBackendContext.fDevice.retain(mtlDevice);
    mtlBackendContext.fQueue.retain(mtlCommandQueue);

    return GrDirectContexts::MakeMetal(mtlBackendContext, options);
}

sk_sp<SkSurface> MetalGraphicsContext::makeSurface(GrDirectContext *grContext, const void *mtlLayer, int sampleCount, SkColorType colorType, const void **mtlDrawableHandle) {
    return SkSurfaces::WrapCAMetalLayer(grContext, mtlLayer, kTopLeft_GrSurfaceOrigin, sampleCount, colorType, nullptr, nullptr, mtlDrawableHandle);
}

}

#endif // SNAP_DRAWING_TGFX_EXPERIMENT
