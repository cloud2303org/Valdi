//
//  BoundingBoxHierarchy.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/22/22.
//

#include "snap_drawing/cpp/Utils/BoundingBoxHierarchy.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include <algorithm>
#else
#include "include/core/SkBBHFactory.h"
#endif

namespace snap::drawing {

BoundingBoxHierarchy::BoundingBoxHierarchy() = default;
BoundingBoxHierarchy::~BoundingBoxHierarchy() = default;

void BoundingBoxHierarchy::insert(const Rect& box) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _frames.emplace_back(box);
#else
    _frames.emplace_back(box.getSkValue());
    _rTree = nullptr;
#endif
}

bool BoundingBoxHierarchy::contains(const Rect& box) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // TGFX has no public spatial index; keep the intersection contract.
    return std::any_of(_frames.begin(), _frames.end(), [&box](const Rect& frame) {
        return !frame.intersection(box).isEmpty();
    });
#else
    if (_rTree == nullptr) {
        SkRTreeFactory factory;
        _rTree = factory();
        _rTree->insert(_frames.data(), static_cast<int>(_frames.size()));
    }

    _rTree->search(box.getSkValue(), &_rTreeOutput);
    if (_rTreeOutput.empty()) {
        return false;
    } else {
        _rTreeOutput.clear();
        return true;
    }
#endif
}

} // namespace snap::drawing
