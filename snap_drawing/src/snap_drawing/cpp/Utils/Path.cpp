//
//  Path.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 1/28/22.
//

#include "snap_drawing/cpp/Utils/Path.hpp"
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkStream.h"
#include "include/pathops/SkPathOps.h"
#endif
#include "snap_drawing/cpp/Utils/Matrix.hpp"

namespace snap::drawing {

Path::Path() = default;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
Path::Path(Path&& other) noexcept : WrappedTgfxValue<tgfx::Path>(std::move(other.getSkValue())) {}

Path::Path(const Path& other) noexcept : WrappedTgfxValue<tgfx::Path>(other.getSkValue()) {}
#else
Path::Path(Path&& other) noexcept : WrappedSkValue<SkPath>(std::move(other.getSkValue())) {}

Path::Path(const Path& other) noexcept : WrappedSkValue<SkPath>(other.getSkValue()) {}
#endif

Path::~Path() = default;

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
static SkPathDirection getPathDirection(bool clockwise) {
    return clockwise ? SkPathDirection::kCW : SkPathDirection::kCCW;
}
#endif

Path& Path::operator=(Path&& other) noexcept {
    if (&other != this) {
        getSkValue() = std::move(other.getSkValue());
    }

    return *this;
}

Path& Path::operator=(const Path& other) noexcept {
    getSkValue() = other.getSkValue();
    return *this;
}

bool Path::operator==(const Path& other) const {
    return getSkValue() == other.getSkValue();
}

bool Path::operator!=(const Path& other) const {
    return getSkValue() != other.getSkValue();
}

void Path::addRoundRect(const Rect& bounds, Scalar radii[8], bool clockwise) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    const std::array<tgfx::Point, 4> corners = {{
        tgfx::Point::Make(radii[0], radii[1]),
        tgfx::Point::Make(radii[2], radii[3]),
        tgfx::Point::Make(radii[4], radii[5]),
        tgfx::Point::Make(radii[6], radii[7]),
    }};
    getSkValue().addRoundRect(bounds.getSkValue(), corners, !clockwise);
#else
    getSkValue().addRoundRect(bounds.getSkValue(), &radii[0], getPathDirection(clockwise));
#endif
}

void Path::addRoundRect(const Rect& bounds, Scalar rx, Scalar ry, bool clockwise) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().addRoundRect(bounds.getSkValue(), rx, ry, !clockwise);
#else
    getSkValue().addRoundRect(bounds.getSkValue(), rx, ry, getPathDirection(clockwise));
#endif
}

bool Path::isEmpty() const {
    return getSkValue().isEmpty();
}

void Path::reset() {
    getSkValue().reset();
}

void Path::moveTo(Scalar x, Scalar y) {
    getSkValue().moveTo(x, y);
}

void Path::lineTo(Scalar x, Scalar y) {
    getSkValue().lineTo(x, y);
}

void Path::quadTo(Scalar x0, Scalar y0, Scalar x1, Scalar y1) {
    getSkValue().quadTo(x0, y0, x1, y1);
}

void Path::conicTo(Scalar x0, Scalar y0, Scalar x1, Scalar y1, Scalar weight) {
    getSkValue().conicTo(x0, y0, x1, y1, weight);
}

void Path::cubicTo(Scalar x0, Scalar y0, Scalar x1, Scalar y1, Scalar x2, Scalar y2) {
    getSkValue().cubicTo(x0, y0, x1, y1, x2, y2);
}

void Path::close() {
    getSkValue().close();
}

void Path::addRect(const Rect& bounds, bool clockwise) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().addRect(bounds.getSkValue(), !clockwise);
#else
    getSkValue().addRect(bounds.getSkValue(), getPathDirection(clockwise));
#endif
}

void Path::addOval(const Rect& bounds, bool clockwise) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().addOval(bounds.getSkValue(), !clockwise);
#else
    getSkValue().addOval(bounds.getSkValue(), getPathDirection(clockwise));
#endif
}

void Path::arcTo(const Rect& oval, Scalar startAngle, Scalar sweepAngle) {
    getSkValue().addArc(oval.getSkValue(), startAngle, sweepAngle);
}

void Path::addPath(const Path& path) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // TGFX copies the source fill rule when the destination is empty.
    const auto fillType = getSkValue().getFillType();
#endif
    getSkValue().addPath(path.getSkValue());
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    getSkValue().setFillType(fillType);
#endif
}

void Path::transform(const Matrix& matrix) {
    getSkValue().transform(matrix.getSkValue());
}

Path Path::intersection(const Path& other) const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    Path output(*this);
    // TGFX mutates the left operand and exposes no result/success boolean.
    output.getSkValue().addPath(other.getSkValue(), tgfx::PathOp::Intersect);
#else
    Path output;
    Op(getSkValue(), other.getSkValue(), kIntersect_SkPathOp, &output.getSkValue());
#endif

    output.getSkValue().setFillType(getSkValue().getFillType());
    return output;
}

std::optional<Rect> Path::getBounds() const {
    if (isEmpty()) {
        return std::nullopt;
    }
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    return Rect::fromTgfxValue(getSkValue().getBounds());
#else
    return {fromSkValue<Rect>(getSkValue().getBounds())};
#endif
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
std::string Path::toString() const {
    SkDynamicMemoryWStream stream;
    getSkValue().dump(&stream, true);

    auto bytes = stream.bytesWritten();

    std::string output;
    output.resize(bytes);

    stream.copyTo(output.data());

    return output;
}
#endif

void Path::visit(PathVisitor& visitor) const {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    for (const auto& segment : getSkValue()) {
        const auto* points = segment.points;
        switch (segment.verb) {
            case tgfx::PathVerb::Move:
                visitor.move(Point::fromTgfxValue(points[0]));
                break;
            case tgfx::PathVerb::Line:
                visitor.line(Point::fromTgfxValue(points[0]), Point::fromTgfxValue(points[1]));
                break;
            case tgfx::PathVerb::Quad:
                visitor.quad(Point::fromTgfxValue(points[0]),
                             Point::fromTgfxValue(points[1]),
                             Point::fromTgfxValue(points[2]));
                break;
            case tgfx::PathVerb::Conic: {
                if (visitor.shouldConvertConicToQuad()) {
                    const auto quads = tgfx::Path::ConvertConicToQuads(
                        points[0], points[1], points[2], segment.conicWeight, 1);
                    // Degenerate conics need not produce the requested two quads.
                    for (size_t i = 0; i + 2 < quads.size(); i += 2) {
                        visitor.quad(Point::fromTgfxValue(quads[i]),
                                     Point::fromTgfxValue(quads[i + 1]),
                                     Point::fromTgfxValue(quads[i + 2]));
                    }
                } else {
                    visitor.conic(Point::fromTgfxValue(points[0]),
                                  Point::fromTgfxValue(points[1]),
                                  Point::fromTgfxValue(points[2]),
                                  segment.conicWeight);
                }
                break;
            }
            case tgfx::PathVerb::Cubic:
                visitor.cubic(Point::fromTgfxValue(points[0]),
                              Point::fromTgfxValue(points[1]),
                              Point::fromTgfxValue(points[2]),
                              Point::fromTgfxValue(points[3]));
                break;
            case tgfx::PathVerb::Close:
                visitor.close();
                break;
            case tgfx::PathVerb::Done:
                return;
        }
    }
#else
    SkPath::Iter it(getSkValue(), false);
    SkPoint points[4];
    SkPoint quads[5];

    for (;;) {
        auto verb = it.next(points);

        switch (verb) {
            case SkPath::kMove_Verb:
                visitor.move(fromSkValue<Point>(points[0]));
                break;
            case SkPath::kLine_Verb:
                visitor.line(fromSkValue<Point>(points[0]), fromSkValue<Point>(points[1]));
                break;
            case SkPath::kQuad_Verb:
                visitor.quad(
                    fromSkValue<Point>(points[0]), fromSkValue<Point>(points[1]), fromSkValue<Point>(points[2]));
                break;
            case SkPath::kConic_Verb: {
                if (visitor.shouldConvertConicToQuad()) {
                    SkPath::ConvertConicToQuads(points[0], points[1], points[2], it.conicWeight(), quads, 1);

                    visitor.quad(
                        fromSkValue<Point>(quads[0]), fromSkValue<Point>(quads[1]), fromSkValue<Point>(quads[2]));
                    visitor.quad(
                        fromSkValue<Point>(quads[2]), fromSkValue<Point>(quads[3]), fromSkValue<Point>(quads[4]));
                } else {
                    visitor.conic(fromSkValue<Point>(points[0]),
                                  fromSkValue<Point>(points[1]),
                                  fromSkValue<Point>(points[2]),
                                  it.conicWeight());
                }
            } break;
            case SkPath::kCubic_Verb:
                visitor.cubic(fromSkValue<Point>(points[0]),
                              fromSkValue<Point>(points[1]),
                              fromSkValue<Point>(points[2]),
                              fromSkValue<Point>(points[3]));
                break;
            case SkPath::kClose_Verb:
                visitor.close();
                break;
            case SkPath::kDone_Verb:
                return;
        }
    }
#endif
}

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
std::ostream& operator<<(std::ostream& os, const Path& path) {
    return os << path.toString();
}
#endif

} // namespace snap::drawing
