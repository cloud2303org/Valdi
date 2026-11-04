#include "gpu_target.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#ifndef SNAP_DRAWING_TGFX_COMPLETE
#error "The performance comparison requires the complete production TGFX backend"
#endif
#include "tgfx/core/ColorSpace.h"
#include "tgfx/core/ImageInfo.h"
#else
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#endif

namespace snap::drawing::performance {

Valdi::Result<Valdi::Void> GPUTarget::initialize() {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    graphics = GrGraphicsContext::makeOffscreen();
    if (graphics == nullptr) { return Valdi::Error("Cannot create the production TGFX offscreen GPU context"); }
    lock = graphics->lockContext();
    if (!lock || !*lock) { return Valdi::Error("Cannot lock the production TGFX GPU device"); }
    surface = tgfx::Surface::Make(lock->get(), kWidth, kHeight, tgfx::ColorType::RGBA_8888,
                                  1, false, 0, tgfx::ColorSpace::SRGB());
#else
    auto context = makeSkiaOffscreenContext();
    if (!context) { return context.moveError(); }
    graphics = context.moveValue();
    auto info = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType,
                                  kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    surface = SkSurfaces::RenderTarget(&graphics->getGrContext(), skgpu::Budgeted::kNo, info,
                                       1, kTopLeft_GrSurfaceOrigin, nullptr);
#endif
    if (!surface) { return Valdi::Error("Cannot allocate a real 360x640 RGBA8 GPU render target"); }
    return Valdi::Void();
}

DrawableSurfaceCanvas GPUTarget::canvas() const {
    return DrawableSurfaceCanvas(surface.get(), kWidth, kHeight);
}

Valdi::Result<NativeData> GPUTarget::readback() const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Checked production completion path includes deferred-error propagation
    // before and after flush/submit/readback. Never publish sentinel pixels.
    return canvas().readPixels(tgfx::ImageInfo::Make(kWidth, kHeight, tgfx::ColorType::RGBA_8888,
                                                    tgfx::AlphaType::Premultiplied));
#else
    auto pixels = SkData::MakeUninitialized(kPixelBytes);
    if (!pixels) { return Valdi::Error("Cannot allocate Skia GPU readback bytes"); }
    auto info = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    // SkSurface::readPixels synchronizes this GPU target; makeImageSnapshot()
    // alone would only retain a texture and would not be an equivalent boundary.
    if (!surface->readPixels(info, pixels->writable_data(), static_cast<size_t>(kWidth) * 4, 0, 0)) {
        return Valdi::Error("Skia GPU synchronous readPixels failed");
    }
    if (graphics->getGrContext().abandoned()) {
        return Valdi::Error("Skia GPU context was abandoned during readback");
    }
    return pixels;
#endif
}

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) && !defined(__APPLE__) && !defined(__ANDROID__)
Valdi::Result<Ref<GrGraphicsContext>> makeSkiaOffscreenContext() {
    return Valdi::Error("This source fixture supports Skia offscreen GPU contexts on Android and Apple only");
}
#endif

} // namespace snap::drawing::performance
