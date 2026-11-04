//
//  LayerContent.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 2/1/22.
//

#include "snap_drawing/cpp/Drawing/LayerContent.hpp"

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
LayerContent::LayerContent(const std::shared_ptr<tgfx::Picture>& picture) : picture(picture) {}
#else
LayerContent::LayerContent(const sk_sp<SkPicture>& picture, const Ref<ExternalSurfaceSnapshot>& externalSurface)
    : picture(picture), externalSurface(externalSurface) {}
#endif
LayerContent::LayerContent() = default;
LayerContent::~LayerContent() = default;

bool LayerContent::isEmpty() const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return picture == nullptr;
#else
    return picture == nullptr && externalSurface == nullptr;
#endif
}

void LayerContent::clear() {
    picture = nullptr;
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
    externalSurface = nullptr;
#endif
}

} // namespace snap::drawing
