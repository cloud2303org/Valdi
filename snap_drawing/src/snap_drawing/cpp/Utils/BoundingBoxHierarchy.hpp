//
//  BoundingBoxHierarchy.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/22/22.
//

#pragma once

#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/Geometry.hpp"

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkRefCnt.h"
#endif

#include <vector>

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
class SkBBoxHierarchy;
#endif

namespace snap::drawing {

class BoundingBoxHierarchy : public Valdi::SimpleRefCountable {
public:
    BoundingBoxHierarchy();
    ~BoundingBoxHierarchy() override;

    void insert(const Rect& box);
    bool contains(const Rect& box);

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::vector<Rect> _frames;
#else
    std::vector<SkRect> _frames;
    std::vector<int> _rTreeOutput;
    sk_sp<SkBBoxHierarchy> _rTree;
#endif
};

} // namespace snap::drawing
