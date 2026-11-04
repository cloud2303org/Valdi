#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/Paint.hpp"
#include "snap_drawing/cpp/Utils/BorderRadius.hpp"
#include "snap_drawing/cpp/Utils/Color.hpp"
#include "snap_drawing/cpp/Utils/LazyPath.hpp"
#include "snap_drawing/cpp/Utils/Matrix.hpp"
#include "snap_drawing/cpp/Utils/Path.hpp"

#include "tgfx/core/ImageInfo.h"
#include "tgfx/core/Surface.h"
#include "tgfx/gpu/opengl/GLDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace snap::drawing;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename F>
void check(const char* name, F body, int& passed) {
    body();
    ++passed;
    std::cout << "PASS " << name << '\n';
}

template<typename F>
LayerContent record(F body) {
    DrawingContext drawing(64, 64);
    body(drawing);
    return drawing.finish();
}

Paint solid(Color color) {
    Paint paint;
    paint.setColor(color);
    return paint;
}

std::vector<uint8_t> render(tgfx::Context* context, const LayerContent& content) {
    auto surface = tgfx::Surface::Make(context, 64, 64);
    require(surface != nullptr, "TGFX could not create an offscreen GPU surface");
    surface->getCanvas()->clear();
    if (content.picture) {
        surface->getCanvas()->drawPicture(content.picture);
    }
    auto info = tgfx::ImageInfo::Make(64, 64, tgfx::ColorType::RGBA_8888,
                                    tgfx::AlphaType::Premultiplied);
    std::vector<uint8_t> bytes(info.byteSize());
    require(surface->readPixels(info, bytes.data()), "GPU readPixels failed");
    return bytes;
}

void pixel(const std::vector<uint8_t>& bytes, int x, int y, std::array<int, 4> expected,
           int tolerance = 0) {
    size_t offset = static_cast<size_t>((y * 64 + x) * 4);
    for (size_t component = 0; component != 4; ++component) {
        int actual = bytes.at(offset + component);
        if (std::abs(actual - expected[component]) > tolerance) {
            throw std::runtime_error("Unexpected GPU pixel at (" + std::to_string(x) + "," +
                                     std::to_string(y) + "), component " +
                                     std::to_string(component) + ": got " +
                                     std::to_string(actual) + ", expected " +
                                     std::to_string(expected[component]));
        }
    }
}

class CountConics final : public PathVisitor {
public:
    explicit CountConics(bool convert) : _convert(convert) {}
    bool shouldConvertConicToQuad() const override { return _convert; }
    void move(const Point&) override { ++moves; }
    void line(const Point&, const Point&) override {}
    void quad(const Point&, const Point&, const Point&) override { ++quads; }
    void conic(const Point&, const Point&, const Point&, Scalar) override { ++conics; }
    void cubic(const Point&, const Point&, const Point&, const Point&) override {}
    void close() override {}
    int moves = 0;
    int quads = 0;
    int conics = 0;
private:
    bool _convert;
};

void runGeometryChecks(int& passed) {
    check("actual-geometry-intersection", [] {
        Rect intersection = Rect::makeXYWH(0, 0, 20, 20).intersection(Rect::makeXYWH(10, 10, 20, 20));
        require(intersection == Rect::makeXYWH(10, 10, 10, 10), "Rectangle intersection changed");
    }, passed);
    check("paint-saved-skia-defaults", [] {
        Paint paint;
        require(paint.getColor() == Color::black(), "Default paint must be opaque black");
        require(paint.getStrokeWidth() == 0, "Default stroke width must remain zero");
        require(!paint.getSkValue().isAntiAlias(), "Default AA must remain disabled");
        require(paint.getSkValue().getStyle() == tgfx::PaintStyle::Fill, "Default style changed");
    }, passed);
    check("packed-argb-color-roundtrip", [] {
        for (uint32_t value : {0x00000000u, 0xffffffffu, 0x80402010u, 0x01020304u, 0xff11aaffu}) {
            Paint paint;
            paint.setColor(Color::make(value));
            require(paint.getColor().value == value, "Packed ARGB channels changed");
        }
    }, passed);
    check("matrix-affine-element-order", [] {
        Matrix matrix = Matrix::makeScaleTranslate(2, 3, 4, 5);
        matrix.setSkewX(6);
        matrix.setSkewY(7);
        Scalar affine[6] = {};
        require(matrix.toAffine(affine), "Affine matrix rejected");
        const Scalar expected[] = {2, 7, 6, 3, 4, 5};
        for (int index = 0; index != 6; ++index) {
            require(affine[index] == expected[index], "Affine output order changed");
        }
    }, passed);
    check("matrix-perspective-not-affine", [] {
        Matrix matrix;
        const float perspective[] = {1, 0, 0, 0, 1, 0, 0.1f, 0, 1};
        matrix.getSkValue().set9(perspective);
        Scalar affine[6] = {};
        require(!matrix.toAffine(affine), "Perspective was silently treated as affine");
    }, passed);
    check("actual-conic-visitor-and-conversion", [] {
        Path path;
        path.moveTo(0, 0);
        path.conicTo(10, 20, 20, 0, 0.707f);
        CountConics direct(false);
        path.visit(direct);
        require(direct.moves == 1 && direct.conics == 1, "Original conic was not visited");
        CountConics converted(true);
        path.visit(converted);
        require(converted.conics == 0 && converted.quads > 0, "Conic conversion did not emit quads");
    }, passed);
    check("empty-recording", [] {
        DrawingContext drawing(64, 64);
        require(drawing.finish().isEmpty(), "Empty recording gained content");
    }, passed);
}

void runGPUChecks(tgfx::Context* context, int& passed) {
    check("gpu-rect-and-picture-lifetime", [&] {
        auto content = record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(8, 8, 32, 24));
        });
        auto bytes = render(context, content);
        pixel(bytes, 12, 12, {255, 0, 0, 255});
        pixel(bytes, 2, 2, {0, 0, 0, 0});
    }, passed);
    check("gpu-half-alpha-composition", [&] {
        auto content = record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(0, 0, 64, 64));
            drawing.drawPaint(solid(Color::makeARGB(128, 255, 0, 0)), Rect::makeXYWH(8, 8, 24, 24));
        });
        pixel(render(context, content), 12, 12, {128, 0, 127, 255}, 1);
    }, passed);
    check("gpu-save-clip-transform-restore", [&] {
        auto content = record([](DrawingContext& drawing) {
            int saved = drawing.save();
            require(saved == 1, "Skia-facing first save token changed");
            drawing.clipRect(Rect::makeXYWH(0, 0, 20, 20));
            drawing.concat(Matrix::makeScaleTranslate(1, 1, 10, 0));
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(0, 0, 30, 20));
            drawing.restore(saved);
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(40, 0, 16, 16));
        });
        auto bytes = render(context, content);
        pixel(bytes, 12, 5, {255, 0, 0, 255});
        pixel(bytes, 25, 5, {0, 0, 0, 0});
        pixel(bytes, 45, 5, {0, 0, 255, 255});
    }, passed);
    check("gpu-path-clip", [&] {
        auto content = record([](DrawingContext& drawing) {
            Path triangle;
            triangle.moveTo(10, 10);
            triangle.lineTo(50, 10);
            triangle.lineTo(10, 50);
            triangle.close();
            drawing.clipPath(triangle);
            drawing.drawPaint(solid(Color::green()), Rect::makeXYWH(0, 0, 64, 64));
        });
        auto bytes = render(context, content);
        pixel(bytes, 15, 15, {0, 255, 0, 255});
        pixel(bytes, 45, 45, {0, 0, 0, 0});
    }, passed);
    check("gpu-rounded-border-and-lazy-path", [&] {
        auto content = record([](DrawingContext& drawing) {
            BorderRadius radius(10, 10, 10, 10, false, false, false, false);
            LazyPath path;
            drawing.drawPaint(solid(Color::red()), radius, Rect::makeXYWH(8, 8, 48, 48), path);
        });
        auto bytes = render(context, content);
        pixel(bytes, 9, 9, {0, 0, 0, 0});
        pixel(bytes, 30, 10, {255, 0, 0, 255});
        pixel(bytes, 30, 30, {255, 0, 0, 255});
    }, passed);
    check("gpu-conic-path", [&] {
        auto content = record([](DrawingContext& drawing) {
            Path path;
            path.moveTo(8, 32);
            path.conicTo(32, 0, 56, 32, 1);
            path.lineTo(56, 56);
            path.lineTo(8, 56);
            path.close();
            drawing.drawPaint(solid(Color::red()), path);
        });
        auto bytes = render(context, content);
        pixel(bytes, 32, 30, {255, 0, 0, 255});
        pixel(bytes, 32, 2, {0, 0, 0, 0});
    }, passed);
    check("gpu-path-boolean-intersection", [&] {
        auto content = record([](DrawingContext& drawing) {
            Path left;
            left.addRect(Rect::makeXYWH(8, 8, 32, 32), true);
            Path right;
            right.addRect(Rect::makeXYWH(24, 24, 32, 32), true);
            drawing.drawPaint(solid(Color::red()), left.intersection(right));
        });
        auto bytes = render(context, content);
        pixel(bytes, 28, 28, {255, 0, 0, 255});
        pixel(bytes, 12, 12, {0, 0, 0, 0});
    }, passed);
    check("gpu-restore-out-of-range", [&] {
        auto content = record([](DrawingContext& drawing) {
            drawing.save();
            drawing.restore(std::numeric_limits<int>::max());
            drawing.restore(0);
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(8, 8, 16, 16));
        });
        pixel(render(context, content), 12, 12, {255, 0, 0, 255});
    }, passed);
    check("gpu-empty-path-append-keeps-fill-rule", [&] {
        auto content = record([](DrawingContext& drawing) {
            Path source;
            source.addRect(Rect::makeXYWH(8, 8, 48, 48), true);
            source.addRect(Rect::makeXYWH(20, 20, 24, 24), true);
            Path destination;
            destination.getSkValue().setFillType(tgfx::PathFillType::EvenOdd);
            destination.addPath(source);
            drawing.drawPaint(solid(Color::red()), destination);
        });
        auto bytes = render(context, content);
        pixel(bytes, 12, 12, {255, 0, 0, 255});
        pixel(bytes, 32, 32, {0, 0, 0, 0});
    }, passed);
}
}

int main() {
    @autoreleasepool {
        int passed = 0;
        try {
            runGeometryChecks(passed);
            auto device = tgfx::GLDevice::Make();
            require(device != nullptr, "No real macOS OpenGL device; GPU checks did not run");
            auto* context = device->lockContext();
            require(context != nullptr, "Could not lock the OpenGL context");
            try {
                runGPUChecks(context, passed);
            } catch (...) {
                device->unlock();
                throw;
            }
            device->unlock();
            std::cout << "RESULT " << passed << " cases passed with real TGFX GPU rendering\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL after " << passed << " cases: " << error.what() << '\n';
            return 1;
        }
    }
}
