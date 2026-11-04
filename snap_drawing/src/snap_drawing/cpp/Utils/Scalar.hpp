//
//  Scalar.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#pragma once

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkScalar.h"
#endif
#include <cstddef>
#include <cmath>

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
using Scalar = float;
#else
using Scalar = SkScalar;
#endif

inline Scalar pixelsToScalar(int pixels, float pointScale) {
    return static_cast<Scalar>(pixels) / pointScale;
}

template<size_t length>
static inline bool scalarsEqual(const Scalar* left, const Scalar* right) {
    // Use epsilon-based comparison to handle floating-point precision issues.
    // Without this, tiny differences (e.g., 10.499999 vs 10.500001) from different
    // transformation paths incorrectly trigger damage detection, requiring larger
    // damage rect margins to compensate.
    constexpr Scalar epsilon = 0.0001f; // ~1/10000th of a pixel

    for (size_t i = 0; i < length; i++) {
        if (std::abs(left[i] - right[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

static inline Scalar sanitizeScalarFromScale(Scalar value, Scalar scale) {
    return roundf(value * scale) / scale;
}

} // namespace snap::drawing
