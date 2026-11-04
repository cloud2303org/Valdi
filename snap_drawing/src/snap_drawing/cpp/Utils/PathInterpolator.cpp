//
//  PathInterpolator.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 4/20/22.
//

#include "snap_drawing/cpp/Utils/PathInterpolator.hpp"

namespace snap::drawing {

PathInterpolator::PathInterpolator(const Path& path) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    _sourcePath = path;
    auto measure = tgfx::PathMeasure::MakeFrom(path.getSkValue());
    if (measure) {
        do {
            _totalLength += measure->getLength();
        } while (measure->nextContour());
    }
#else
    SkContourMeasureIter iter(path.getSkValue(), false);

    sk_sp<SkContourMeasure> measure;
    while ((measure = iter.next())) {
        _totalLength += measure->length();
        _contours.emplace_back(std::move(measure));
    }
#endif
}

PathInterpolator::~PathInterpolator() = default;

const Path& PathInterpolator::interpolate(Scalar start, Scalar end) {
    _interpolatedPath.reset();

    auto absoluteStart = start * _totalLength;
    auto absoluteEnd = end * _totalLength;

    Scalar currentStart = 0;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto measure = tgfx::PathMeasure::MakeFrom(_sourcePath.getSkValue());
    if (!measure) {
        return _interpolatedPath;
    }
    do {
        const auto currentLength = measure->getLength();
        const auto currentEnd = currentStart + currentLength;
        if (currentStart >= absoluteEnd) {
            break;
        }
        const auto relativeStart = std::clamp(absoluteStart - currentStart, 0.0f, currentLength);
        const auto relativeEnd = std::min(absoluteEnd - currentStart, currentLength);
        if (relativeStart < relativeEnd &&
            !measure->getSegment(relativeStart, relativeEnd, &_interpolatedPath.getSkValue())) {
            break;
        }
        currentStart = currentEnd;
    } while (measure->nextContour());
#else
    for (const auto& contour : _contours) {
        auto currentLength = contour->length();
        auto currentEnd = currentStart + currentLength;

        if (currentStart >= absoluteEnd) {
            break;
        }

        auto relativeStart = std::min(std::max(absoluteStart - currentStart, 0.0f), currentLength);
        auto relativeEnd = std::min(absoluteEnd - currentStart, currentLength);

        if (relativeStart != relativeEnd) {
            if (!contour->getSegment(relativeStart, relativeEnd, &_interpolatedPath.getSkValue(), true)) {
                break;
            }
        }

        currentStart = currentEnd;
    }
#endif

    return _interpolatedPath;
}

} // namespace snap::drawing
