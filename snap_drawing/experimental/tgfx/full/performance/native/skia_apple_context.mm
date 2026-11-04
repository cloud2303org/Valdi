#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#import <Metal/Metal.h>

#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "valdi_core/cpp/Utils/Shared.hpp"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"

namespace snap::drawing::performance {

Valdi::Result<Ref<GrGraphicsContext>> makeSkiaOffscreenContext() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue) { return Valdi::Error("Skia: cannot create real Metal device/queue"); }
        // Same retained backend objects as MetalGraphicsContext::makeContext.
        GrMtlBackendContext backend;
        backend.fDevice.retain((__bridge void*)device);
        backend.fQueue.retain((__bridge void*)queue);
        auto options = Valdi::makeShared<GrGraphicsContextOptions>();
        auto context = GrDirectContexts::MakeMetal(backend, options->getGrContextOptions());
        if (!context) { return Valdi::Error("Skia: Ganesh MakeMetal failed"); }
        return Valdi::makeShared<GrGraphicsContext>(context, options);
    }
}

} // namespace snap::drawing::performance
#endif
