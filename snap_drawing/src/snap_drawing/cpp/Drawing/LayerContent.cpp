//
//  LayerContent.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 2/1/22.
//

#include "snap_drawing/cpp/Drawing/LayerContent.hpp"

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
LayerContent::LayerContent(const std::shared_ptr<tgfx::Picture>& picture) : picture(picture) {
    if (picture) {
        bounds = Rect::fromTgfxValue(picture->getBounds());
    }
}
#ifdef SNAP_DRAWING_TGFX_COMPLETE
LayerContent::LayerContent(const std::shared_ptr<tgfx::Picture>& picture,
                           const Ref<ExternalSurfaceSnapshot>& externalSurface)
    : LayerContent(picture) {
    this->externalSurface = externalSurface;
}
#endif
#else
LayerContent::LayerContent(const sk_sp<SkPicture>& picture, const Ref<ExternalSurfaceSnapshot>& externalSurface)
    : picture(picture), externalSurface(externalSurface) {}
#endif
LayerContent::LayerContent() = default;
LayerContent::~LayerContent() = default;

bool LayerContent::isEmpty() const {
#if defined(SNAP_DRAWING_TGFX_EXPERIMENT) && !defined(SNAP_DRAWING_TGFX_COMPLETE)
    return picture == nullptr;
#else
    return picture == nullptr && externalSurface == nullptr;
#endif
}

void LayerContent::clear() {
    picture = nullptr;
#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || defined(SNAP_DRAWING_TGFX_COMPLETE)
    externalSurface = nullptr;
#endif
}

} // namespace snap::drawing
