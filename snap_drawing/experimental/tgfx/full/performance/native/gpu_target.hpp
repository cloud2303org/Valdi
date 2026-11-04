#pragma once

#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"

#include <memory>

namespace snap::drawing::performance {

constexpr int kWidth = 360;
constexpr int kHeight = 640;
constexpr size_t kPixelBytes = static_cast<size_t>(kWidth) * kHeight * 4;

// All GPU objects are created, used and destroyed on the calling thread.
// Declaration order keeps surfaces alive only while their context is alive.
struct GPUTarget {
    Ref<GrGraphicsContext> graphics;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::unique_ptr<TGFXContextLock> lock;
    std::shared_ptr<NativeSurface> surface;
#else
    sk_sp<NativeSurface> surface;
#endif

    Valdi::Result<Valdi::Void> initialize();
    DrawableSurfaceCanvas canvas() const;
    // Owns tightly packed RGBA8888 premultiplied bytes. Completion, allocation,
    // transfer and backend validation are included in every timed frame.
    Valdi::Result<NativeData> readback() const;
};

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
// There is no makeOffscreen() on the production Skia GrGraphicsContext.
// These local platform adapters construct a real Ganesh context instead.
Valdi::Result<Ref<GrGraphicsContext>> makeSkiaOffscreenContext();
#endif

} // namespace snap::drawing::performance
