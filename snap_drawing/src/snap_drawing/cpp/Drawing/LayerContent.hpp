//
//  LayerContent.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 2/1/22.
//

#pragma once

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Picture.h"
#include <memory>
#else
#include "include/core/SkPicture.h"
#include "snap_drawing/cpp/Drawing/Surface/ExternalSurface.hpp"
#include "snap_drawing/cpp/Utils/Aliases.hpp"
#endif

namespace snap::drawing {

/**
 LayerContent is a wrapper around a recorded list of draw commands through SkPicture,
 or a ExternalSurface reference. The LayerContent might hold the picture representing the
 background and result of the onDraw() call, or the foreground picture.
 It is created by the DrawingContext.
 */
struct LayerContent {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::shared_ptr<tgfx::Picture> picture;

    explicit LayerContent(const std::shared_ptr<tgfx::Picture>& picture);
#else
    sk_sp<SkPicture> picture;
    Ref<ExternalSurfaceSnapshot> externalSurface;

    LayerContent(const sk_sp<SkPicture>& picture, const Ref<ExternalSurfaceSnapshot>& externalSurface);
#endif
    LayerContent();
    ~LayerContent();

    bool isEmpty() const;
    void clear();
};

} // namespace snap::drawing
