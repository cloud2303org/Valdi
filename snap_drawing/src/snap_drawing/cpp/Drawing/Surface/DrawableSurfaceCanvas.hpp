//
//  DrawableSurfaceCanvas.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/20/22.
//

#pragma once

#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"

namespace snap::drawing {

class DrawableSurfaceCanvas {
public:
    DrawableSurfaceCanvas(NativeSurface* surface, int width, int height);

    // Borrowed until completion (including failed flushWithResult); TGFX callers
    // must stay on the preparing thread.
    NativeCanvas* getSkiaCanvas() const;
    NativeCanvas* getNativeCanvas() const { return _canvas; }
    NativeSurface* getNativeSurface() const { return _surface; }
    NativeSurface* getSkiaSurface() const;

    int getWidth() const;
    int getHeight() const;

    Valdi::Result<Ref<Image>> snapshot() const;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Completion is successful only if the destination has no sticky native
    // error before AND after deferred materialization/GPU work. The device must
    // remain locked on the calling thread for all these operations.
    Valdi::Result<Valdi::Void> validate() const;
    // Close and cancel the current unsubmitted batch without publishing it.
    // Already flushed unrelated batches are preserved. A flush is atomic across
    // surfaces; callers sharing a context must establish separate batch boundaries.
    void discard() const;
    Valdi::Result<Valdi::Void> flush(bool syncCpu = false) const;
    // Returns owned pixels only after readback and post-materialization checks.
    // Callers must not publish any pixels when this returns an error.
    Valdi::Result<NativeData> readPixels(const tgfx::ImageInfo& info) const;
    // GPU snapshot for same-device composition; unlike snapshot(), this requires
    // its device/context to remain alive. Checks the copy/materialization boundary.
    Valdi::Result<NativeImage> makeImageSnapshot() const;
#endif

private:
    NativeSurface* _surface;
    NativeCanvas* _canvas;
    int _width;
    int _height;
};

} // namespace snap::drawing
