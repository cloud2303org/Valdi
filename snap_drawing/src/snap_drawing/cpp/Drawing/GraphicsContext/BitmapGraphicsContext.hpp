//
//  BitmapGraphicsContext.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/20/22.
//

#pragma once

#include "snap_drawing/cpp/Drawing/GraphicsContext/GraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurface.hpp"

#include "valdi_core/cpp/Interfaces/IBitmap.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include <mutex>
#include <optional>
#endif

namespace snap::drawing {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
class GrGraphicsContext;
#endif

class BitmapGraphicsContext : public GraphicsContext {
public:
    BitmapGraphicsContext();
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    ~BitmapGraphicsContext() override;
    // Optional injection lets raster/composition reuse its already-owned GPU device.
    explicit BitmapGraphicsContext(const Ref<GrGraphicsContext>& context);
#endif

    void setResourceCacheLimit(size_t resourceCacheLimitBytes) override;

    void performCleanup(bool shouldPurgeScratchResources, std::chrono::seconds secondsNotUsed) override;

    /**
     Creates a surface that will draw directly into the given bitmap.
     */
    Ref<DrawableSurface> createBitmapSurface(const Valdi::Ref<Valdi::IBitmap>& bitmap);

    /**
     Creates a BitmapGraphicsContext that will allocate a bitmap from the given BitmapInfo
     and draw into it.
     */
    Ref<DrawableSurface> createBitmapSurface(const Valdi::BitmapInfo& bitmapInfo);

    /**
     Creates a BitmapGraphicsContext that will draw directly into the given bitmap bytes.
     */
    Ref<DrawableSurface> createBitmapSurface(const Valdi::BitmapInfo& bitmapInfo, void* bitmapBytes);

    /**
     Convenience method to create a PNG from a Bitmap
     */
    static Valdi::Result<Valdi::BytesView> bitmapToPNG(const Valdi::Ref<Valdi::IBitmap>& bitmap);

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::mutex _mutex;
    Ref<GrGraphicsContext> _context;
    std::optional<size_t> _cacheLimit;
    Ref<GrGraphicsContext> getGPUContext();
    void applyCacheLimit(const Ref<GrGraphicsContext>& context);
#endif
};

} // namespace snap::drawing
