#include "effects_checks.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "effects_checks requires the actual complete experimental TGFX backend"
#endif

#include "snap_drawing/cpp/Drawing/BlendMode.hpp"
#include "snap_drawing/cpp/Drawing/BoxShadow.hpp"
#include "snap_drawing/cpp/Drawing/Composition/CompositionState.hpp"
#include "snap_drawing/cpp/Drawing/DisplayList/DisplayList.hpp"
#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/LinearGradient.hpp"
#include "snap_drawing/cpp/Drawing/Mask/CompositeMask.hpp"
#include "snap_drawing/cpp/Drawing/Mask/PaintMask.hpp"
#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"
#include "snap_drawing/cpp/Drawing/Paint.hpp"
#include "snap_drawing/cpp/Drawing/RadialGradient.hpp"
#include "snap_drawing/cpp/Drawing/Shader.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "snap_drawing/cpp/Utils/BorderRadius.hpp"
#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/Matrix.hpp"
#include "snap_drawing/cpp/Utils/Path.hpp"
#include "valdi_core/cpp/Utils/Shared.hpp"

#include "tgfx/core/ColorSpace.h"
#include "tgfx/core/Data.h"
#include "tgfx/core/ImageInfo.h"
#include "tgfx/core/PictureRecorder.h"
#include "tgfx/core/Surface.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace snap::drawing;

constexpr int kSize = 128;
using Pixel = std::array<int, 4>; // RGBA, premultiplied, byte channels.

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

struct Check {
    std::string name;
    std::function<void(tgfx::Context*)> body;
    bool releasesHarnessLock = false;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string describe(const Pixel& value) {
    return "(" + std::to_string(value[0]) + "," + std::to_string(value[1]) + "," +
           std::to_string(value[2]) + "," + std::to_string(value[3]) + ")";
}

Pixel at(const Frame& frame, int x, int y) {
    require(x >= 0 && y >= 0 && x < frame.width && y < frame.height, "Sample is outside readback bounds");
    auto offset = static_cast<size_t>((y * frame.width + x) * 4);
    return {frame.pixels.at(offset), frame.pixels.at(offset + 1),
            frame.pixels.at(offset + 2), frame.pixels.at(offset + 3)};
}

void pixel(const Frame& frame, int x, int y, Pixel expected, int tolerance = 0) {
    auto actual = at(frame, x, y);
    for (size_t channel = 0; channel < expected.size(); ++channel) {
        if (std::abs(actual[channel] - expected[channel]) > tolerance) {
            auto message = "Pixel (" + std::to_string(x) + "," + std::to_string(y) +
                           ") RGBA=" + describe(actual) + ", expected " + describe(expected) +
                           ", tolerance=" + std::to_string(tolerance);
            // Diagnostics only: preserve the original assertion and tolerance.
            // Neighbor probes help distinguish a shifted shader boundary from
            // absent geometry, a uniform color, or the wrong blur support.
            const std::pair<int, int> offsets[] = {{-8, 0}, {8, 0}, {0, -8}, {0, 8}};
            for (const auto& offset : offsets) {
                const auto px = x + offset.first;
                const auto py = y + offset.second;
                if (px >= 0 && py >= 0 && px < frame.width && py < frame.height) {
                    message += "; probe(" + std::to_string(px) + "," + std::to_string(py) +
                               ")=" + describe(at(frame, px, py));
                }
            }
            throw std::runtime_error(message);
        }
    }
}

void alphaBetween(const Frame& frame, int x, int y, int low, int high, const char* reason) {
    auto value = at(frame, x, y);
    require(value[3] >= low && value[3] <= high,
            std::string(reason) + " at (" + std::to_string(x) + "," + std::to_string(y) +
                "), RGBA=" + describe(value) + ", expected alpha in [" + std::to_string(low) +
                "," + std::to_string(high) + "]");
}

void framesNear(const Frame& first, const Frame& second, int tolerance) {
    require(first.width == second.width && first.height == second.height, "Readback dimensions differ");
    for (int y = 0; y < first.height; ++y) {
        for (int x = 0; x < first.width; ++x) {
            pixel(first, x, y, at(second, x, y), tolerance);
        }
    }
}

Paint solid(Color color) {
    Paint paint;
    paint.setColor(color);
    return paint;
}

Paint stroke(Color color, Scalar width) {
    auto paint = solid(color);
    paint.setStroke(true);
    paint.setStrokeWidth(width);
    paint.setStrokeCap(PaintStrokeCapButt);
    return paint;
}

template<typename Body>
LayerContent record(Body body, Scalar width = kSize, Scalar height = kSize) {
    DrawingContext drawing(width, height);
    body(drawing);
    // Return only the real LayerContent: its recorder, paint, gradients, masks,
    // source paths and input image wrappers have died before picture replay.
    return drawing.finish();
}

std::shared_ptr<tgfx::Surface> makeSurface(tgfx::Context* context, int width, int height) {
    require(context != nullptr, "No locked TGFX context");
    auto surface = tgfx::Surface::Make(
        context, width, height, tgfx::ColorType::RGBA_8888, 1, false, 0, tgfx::ColorSpace::SRGB());
    require(surface != nullptr, "Surface::Make failed for RGBA8 offscreen target");
    surface->getCanvas()->clear(tgfx::Color::Transparent());
    return surface;
}

Frame readback(const std::shared_ptr<tgfx::Surface>& surface) {
    require(!surface->getCanvas()->hasError(), "Native TGFX render failure before GPU readback");
    auto info = tgfx::ImageInfo::Make(surface->width(), surface->height(),
                                    tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    Frame result{surface->width(), surface->height(), std::vector<uint8_t>(info.byteSize(), 0xcd)};
    // readPixels synchronizes the GPU; no timer or asynchronous screenshot is
    // accepted as evidence. Sentinel bytes are never interpreted on failure.
    require(surface->readPixels(info, result.pixels.data()), "Surface::readPixels failed");
    require(!surface->getCanvas()->hasError(), "Native TGFX render failure during GPU readback");
    return result;
}

Frame render(tgfx::Context* context, const LayerContent& content) {
    auto surface = makeSurface(context, kSize, kSize);
    // A transparent/no-op shader can legitimately be optimized to an empty
    // picture. The expected pixels, not the number of recorded commands, decide
    // whether that scene is correct. Positive controls still require coverage.
    if (content.picture) {
        surface->getCanvas()->drawPicture(content.picture);
    }
    require(surface->getCanvas()->getSaveCount() == 0, "Picture replay leaked canvas saves");
    return readback(surface);
}

Frame render(tgfx::Context* context, const DisplayList& list, int width = kSize, int height = kSize) {
    auto surface = makeSurface(context, width, height);
    DrawableSurfaceCanvas canvas(surface.get(), width, height);
    // Exercise DisplayList's bounds/viewport, opacity-group, picture ownership
    // and mask replay decisions, rather than reimplementing its visitor here.
    list.draw(canvas, kDisplayListAllPlaneIndexes, true);
    require(surface->getCanvas()->getSaveCount() == 0, "DisplayList replay leaked canvas saves");
    return readback(surface);
}

LayerContent rectangle(Color color, const Rect& bounds) {
    return record([&](DrawingContext& drawing) { drawing.drawPaint(solid(color), bounds); });
}

Ref<Image> rgbaImage(int width, int height, const std::vector<uint8_t>& bytes) {
    auto info = tgfx::ImageInfo::Make(width, height, tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    require(bytes.size() == info.byteSize(), "Invalid constant input image fixture size");
    auto image = tgfx::Image::MakeFrom(info, tgfx::Data::MakeWithCopy(bytes.data(), bytes.size()));
    require(image != nullptr, "Could not create immutable input image");
    return Valdi::makeShared<Image>(image);
}

void addGradientChecks(std::vector<Check>& checks) {
    checks.push_back({"gradient-linear-uneven-stops-and-clamp", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::green(), Color::blue()});
            gradient.setLocations({0, 0.25f, 1});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(16, 16, 100, 32));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(8, 16, 116, 32));
        }));
        pixel(frame, 10, 24, {255, 0, 0, 255});
        pixel(frame, 28, 24, {128, 128, 0, 255}, 2); // t=.125, midway through first interval.
        pixel(frame, 78, 24, {0, 128, 128, 255}, 2); // t=.625, midway through second interval.
        pixel(frame, 120, 24, {0, 0, 255, 255});
        pixel(frame, 4, 24, {0, 0, 0, 0});
    }});
    checks.push_back({"gradient-linear-mismatched-locations-use-even-spacing", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::blue()});
            gradient.setLocations({0, 0.25f, 1});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(16.5f, 16, 100, 32));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 101, 32));
        }));
        pixel(frame, 66, 24, {128, 0, 128, 255}, 2);
    }});
    checks.push_back({"gradient-linear-hard-stops-and-partial-endpoint-stops", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.setColors({Color::red(), Color::red(), Color::blue(), Color::blue()});
            gradient.setLocations({0, 0.5f, 0.5f, 1});
            gradient.update(Rect::makeXYWH(16.5f, 8, 100, 32));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 8, 101, 32));
            gradient.setColors({Color::red(), Color::blue()});
            gradient.setLocations({0.25f, 0.75f});
            gradient.update(Rect::makeXYWH(16.5f, 56, 100, 32));
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 56, 101, 32));
        }));
        pixel(frame, 60, 24, {255, 0, 0, 255});
        pixel(frame, 72, 24, {0, 0, 255, 255});
        pixel(frame, 20, 72, {255, 0, 0, 255});
        pixel(frame, 66, 72, {128, 0, 128, 255}, 2);
        pixel(frame, 110, 72, {0, 0, 255, 255});
    }});
    checks.push_back({"gradient-unpremultiplied-stops-premultiplied-readback", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::makeARGB(0, 255, 0, 0), Color::blue()});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(16.5f, 16, 100, 32));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 101, 32));
        }));
        pixel(frame, 66, 24, {64, 0, 64, 128}, 2);
    }});

    struct OrientationCase {
        const char* name;
        LinearGradientOrientation orientation;
        int startX, startY, endX, endY;
    };
    const OrientationCase orientations[] = {
        {"top-bottom", LinearGradientOrientationTopBottom, 64, 20, 64, 107},
        {"top-right-bottom-left", LinearGradientOrientationTopRightBottomLeft, 107, 20, 20, 107},
        {"right-left", LinearGradientOrientationRightLeft, 107, 64, 20, 64},
        {"bottom-right-top-left", LinearGradientOrientationBottomRightTopLeft, 107, 107, 20, 20},
        {"bottom-top", LinearGradientOrientationBottomTop, 64, 107, 64, 20},
        {"bottom-left-top-right", LinearGradientOrientationBottomLeftTopRight, 20, 107, 107, 20},
        {"left-right", LinearGradientOrientationLeftRight, 20, 64, 107, 64},
        {"top-left-bottom-right", LinearGradientOrientationTopLeftBottomRight, 20, 20, 107, 107},
    };
    for (const auto& item : orientations) {
        checks.push_back({std::string("gradient-orientation-") + item.name, [item](tgfx::Context* context) {
            auto frame = render(context, record([&](DrawingContext& drawing) {
                LinearGradient gradient;
                gradient.setColors({Color::red(), Color::blue()});
                gradient.setOrientation(item.orientation);
                gradient.update(Rect::makeXYWH(16, 16, 96, 96));
                Paint paint;
                gradient.applyToPaint(paint);
                drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 96, 96));
            }));
            auto start = at(frame, item.startX, item.startY);
            auto end = at(frame, item.endX, item.endY);
            require(start[0] > 230 && start[1] == 0 && start[2] < 25 && start[3] == 255,
                    "Wrong gradient start: " + describe(start));
            require(end[0] < 25 && end[1] == 0 && end[2] > 230 && end[3] == 255,
                    "Wrong gradient end: " + describe(end));
            pixel(frame, 8, 8, {0, 0, 0, 0});
        }});
    }
    checks.push_back({"gradient-local-shader-follows-canvas-transform", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::red(), Color::blue(), Color::blue()});
            gradient.setLocations({0, 0.5f, 0.5f, 1});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(0, 0, 32, 16));
            Paint paint;
            gradient.applyToPaint(paint);
            auto saved = drawing.save();
            drawing.concat(Matrix::makeScaleTranslate(2, 2, 16, 16));
            drawing.drawPaint(paint, Rect::makeXYWH(0, 0, 32, 16));
            drawing.restore(saved);
            drawing.drawPaint(solid(Color::green()), Rect::makeXYWH(0, 0, 8, 8));
        }));
        pixel(frame, 24, 24, {255, 0, 0, 255});
        pixel(frame, 72, 24, {0, 0, 255, 255});
        pixel(frame, 4, 4, {0, 255, 0, 255});
        pixel(frame, 88, 24, {0, 0, 0, 0});
    }});
    checks.push_back({"gradient-draw-rounded-path-and-degenerate-clamp", [](tgfx::Context* context) {
        auto rounded = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::blue()});
            gradient.draw(drawing, BorderRadius::makeOval(24, false));
        }));
        pixel(rounded, 0, 0, {0, 0, 0, 0});
        alphaBetween(rounded, 64, 4, 255, 255, "Rounded gradient interior missing");
        auto degenerate = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::blue()});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(40, 20, 0, 80));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 96, 96));
        }));
        pixel(degenerate, 64, 64, {0, 0, 255, 255});
    }});
    checks.push_back({"gradient-radial-stops-and-center", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            RadialGradient gradient;
            gradient.setColors({Color::white(), Color::red(), Color::black()});
            gradient.setLocations({0, 0.5f, 1});
            gradient.update(Rect::makeXYWH(16.5f, 16.5f, 96, 96));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 97, 97));
        }));
        pixel(frame, 64, 64, {255, 255, 255, 255}, 2);
        pixel(frame, 76, 64, {255, 128, 128, 255}, 3);
        pixel(frame, 88, 64, {255, 0, 0, 255}, 3);
        pixel(frame, 112, 64, {0, 0, 0, 255}, 3);
    }});
    checks.push_back({"gradient-radial-ellipse-local-matrix", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            RadialGradient gradient;
            gradient.setColors({Color::white(), Color::black()});
            gradient.update(Rect::makeXYWH(16.5f, 24.5f, 96, 48));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 24, 97, 49));
        }));
        pixel(frame, 64, 48, {255, 255, 255, 255}, 2);
        pixel(frame, 88, 48, {128, 128, 128, 255}, 3);
        pixel(frame, 64, 60, {128, 128, 128, 255}, 3);
        pixel(frame, 112, 48, {0, 0, 0, 255}, 3);
        pixel(frame, 64, 72, {0, 0, 0, 255}, 3);
    }});
    checks.push_back({"gradient-radial-update-keeps-recorded-shader-alive", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            RadialGradient gradient;
            gradient.setColors({Color::red(), Color::black()});
            gradient.update(Rect::makeXYWH(16.5f, 16.5f, 32, 32));
            Paint paint;
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 33, 33));
            gradient.setColors({Color::green(), Color::blue()});
            gradient.setLocations({0, 1});
            gradient.update(Rect::makeXYWH(80.5f, 64.5f, 32, 32));
            gradient.applyToPaint(paint);
            drawing.drawPaint(paint, Rect::makeXYWH(80, 64, 33, 33));
        }));
        pixel(frame, 32, 32, {255, 0, 0, 255}, 2);
        pixel(frame, 96, 80, {0, 255, 0, 255}, 2);
        pixel(frame, 112, 80, {0, 0, 255, 255}, 3);
    }});
}

void addShaderChecks(std::vector<Check>& checks) {
    checks.push_back({"shader-color-keeps-paint-color-and-alpha-independent", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            Paint paint = solid(Color::makeARGB(64, 0, 255, 0));
            paint.setShader(Shader::makeColor(Color::makeARGB(128, 255, 0, 0)));
            require(paint.getColor() == Color::makeARGB(64, 0, 255, 0), "setShader changed the paint color");
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 32, 16));
            paint.setAlpha(0.5f);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 48, 32, 16));
            paint.setColor(Color::green());
            drawing.drawPaint(paint, Rect::makeXYWH(16, 80, 32, 16));
        }));
        pixel(frame, 24, 24, {32, 0, 0, 32}, 1);
        pixel(frame, 24, 56, {64, 0, 0, 64}, 1);
        pixel(frame, 24, 88, {128, 0, 0, 128}, 1);
    }});
    checks.push_back({"shader-empty-does-not-clear-destination-under-src", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(0, 0, kSize, kSize));
            Paint empty;
            empty.setShader(Shader::makeEmpty().withLocalMatrix(Matrix::makeScale(2, 2)));
            empty.setBlendMode(toSkBlendMode(BlendModeSrc));
            Paint copy(empty);
            drawing.drawPaint(copy, Rect::makeXYWH(16, 16, 64, 64));
            Path path;
            path.addOval(Rect::makeXYWH(24, 24, 48, 48), true);
            drawing.drawPaint(copy, path);
        }));
        pixel(frame, 40, 40, {0, 0, 255, 255});
        pixel(frame, 96, 96, {0, 0, 255, 255});
    }});
    checks.push_back({"shader-blend-color-filter-and-clear-filter", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            Paint paint;
            paint.setShader(Shader::makeColor(Color::makeARGB(128, 0, 0, 255)));
            paint.setBlendColorFilter(Color::red());
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 32, 32));
            paint.setBlendColorFilter(Color::transparent());
            drawing.drawPaint(paint, Rect::makeXYWH(64, 16, 32, 32));
        }));
        pixel(frame, 24, 24, {128, 0, 0, 128}, 1);
        pixel(frame, 72, 24, {0, 0, 128, 128}, 1);
    }});
    checks.push_back({"shader-image-clamp-local-matrix-composition-and-canvas-transform", [](tgfx::Context* context) {
        auto content = record([](DrawingContext& drawing) {
            auto image = rgbaImage(2, 2, {255, 0, 0, 255, 0, 255, 0, 255,
                                          0, 0, 255, 255, 255, 255, 255, 255});
            auto local = Matrix::makeScaleTranslate(16, 16, 16, 16);
            auto shader = Shader::makeImage(image, &local, FilterQualityNone)
                              .withLocalMatrix(Matrix::makeScaleTranslate(1, 1, 32, 16));
            Paint paint;
            paint.setShader(shader);
            drawing.concat(Matrix::makeScaleTranslate(1, 1, 8, 8));
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 96, 96));
        });
        auto frame = render(context, content);
        pixel(frame, 60, 44, {255, 0, 0, 255});
        pixel(frame, 76, 44, {0, 255, 0, 255});
        pixel(frame, 60, 60, {0, 0, 255, 255});
        pixel(frame, 76, 60, {255, 255, 255, 255});
        pixel(frame, 44, 44, {255, 0, 0, 255}); // Clamp outside the image, inside draw geometry.
        pixel(frame, 16, 16, {0, 0, 0, 0});
    }});
    checks.push_back({"shader-image-low-quality-bilinear", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto image = rgbaImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255});
            auto local = Matrix::makeScaleTranslate(32, 32, 16.5f, 16.5f);
            Paint paint;
            paint.setShader(Shader::makeImage(image, &local, FilterQualityLow));
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 65, 33));
        }));
        pixel(frame, 32, 32, {255, 0, 0, 255}, 1);
        pixel(frame, 48, 32, {128, 128, 0, 255}, 2);
        pixel(frame, 64, 32, {0, 255, 0, 255}, 1);
    }});
    checks.push_back({"shader-image-high-quality-mitchell-center", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto image = rgbaImage(4, 1, {255, 0, 0, 255, 0, 255, 0, 255,
                                          0, 0, 255, 255, 255, 255, 255, 255});
            auto local = Matrix::makeScaleTranslate(16, 16, 16.5f, 16.5f);
            Paint paint;
            paint.setShader(Shader::makeImage(image, &local, FilterQualityHigh));
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 65, 17));
        }));
        // Independent Skia FilterQualityHigh contract: Mitchell's weights at
        // a texel center are (1/18, 8/9, 1/18). This must FAIL for a bilinear
        // fallback; support cannot be claimed from a nearest/bilinear control.
        pixel(frame, 40, 24, {14, 227, 14, 255}, 2);
    }});
}

// Fixed expected RGBA values are the oracle. No production blend/gradient,
// blur, dash or path-measure algorithm is copied into this fixture.
struct BlendCase {
    const char* name;
    BlendMode mode;
    tgfx::BlendMode native;
    bool translucent;
    Pixel expected;
};

void addBlendChecks(std::vector<Check>& checks) {
    const BlendCase cases[] = {
        {"clear", BlendModeClear, tgfx::BlendMode::Clear, true, {0, 0, 0, 0}},
        {"src", BlendModeSrc, tgfx::BlendMode::Src, true, {85, 0, 0, 85}},
        {"dst", BlendModeDst, tgfx::BlendMode::Dst, true, {0, 0, 170, 170}},
        {"src-over", BlendModeSrcOver, tgfx::BlendMode::SrcOver, true, {85, 0, 113, 198}},
        {"dst-over", BlendModeDstOver, tgfx::BlendMode::DstOver, true, {28, 0, 170, 198}},
        {"src-in", BlendModeSrcIn, tgfx::BlendMode::SrcIn, true, {57, 0, 0, 57}},
        {"dst-in", BlendModeDstIn, tgfx::BlendMode::DstIn, true, {0, 0, 57, 57}},
        {"src-out", BlendModeSrcOut, tgfx::BlendMode::SrcOut, true, {28, 0, 0, 28}},
        {"dst-out", BlendModeDstOut, tgfx::BlendMode::DstOut, true, {0, 0, 113, 113}},
        {"src-atop", BlendModeSrcATop, tgfx::BlendMode::SrcATop, true, {57, 0, 113, 170}},
        {"dst-atop", BlendModeDstATop, tgfx::BlendMode::DstATop, true, {28, 0, 57, 85}},
        {"xor", BlendModeXor, tgfx::BlendMode::Xor, true, {28, 0, 113, 142}},
        {"plus", BlendModePlus, tgfx::BlendMode::PlusLighter, true, {85, 0, 170, 255}},
        {"modulate", BlendModeModulate, tgfx::BlendMode::Modulate, true, {0, 0, 0, 57}},
        {"screen", BlendModeScreen, tgfx::BlendMode::Screen, false, {208, 144, 208, 255}},
        {"last-coeff-alias", BlendModeLastCoeffMode, tgfx::BlendMode::Screen, false, {208, 144, 208, 255}},
        {"overlay", BlendModeOverlay, tgfx::BlendMode::Overlay, false, {96, 33, 161, 255}},
        {"darken", BlendModeDarken, tgfx::BlendMode::Darken, false, {64, 32, 64, 255}},
        {"lighten", BlendModeLighten, tgfx::BlendMode::Lighten, false, {192, 128, 192, 255}},
        {"color-dodge", BlendModeColorDodge, tgfx::BlendMode::ColorDodge, false, {255, 146, 255, 255}},
        {"color-burn", BlendModeColorBurn, tgfx::BlendMode::ColorBurn, false, {1, 0, 4, 255}},
        {"hard-light", BlendModeHardLight, tgfx::BlendMode::HardLight, false, {161, 32, 96, 255}},
        {"soft-light", BlendModeSoftLight, tgfx::BlendMode::SoftLight, false, {96, 80, 168, 255}},
        {"difference", BlendModeDifference, tgfx::BlendMode::Difference, false, {128, 96, 128, 255}},
        {"exclusion", BlendModeExclusion, tgfx::BlendMode::Exclusion, false, {160, 128, 160, 255}},
        {"multiply", BlendModeMultiply, tgfx::BlendMode::Multiply, false, {48, 16, 48, 255}},
        {"last-separable-alias", BlendModeLastSeparableMode, tgfx::BlendMode::Multiply, false, {48, 16, 48, 255}},
        {"hue", BlendModeHue, tgfx::BlendMode::Hue, false, {203, 75, 100, 255}},
        {"saturation", BlendModeSaturation, tgfx::BlendMode::Saturation, false, {51, 131, 211, 255}},
        {"color", BlendModeColor, tgfx::BlendMode::Color, false, {224, 64, 96, 255}},
        {"luminosity", BlendModeLuminosity, tgfx::BlendMode::Luminosity, false, {32, 96, 160, 255}},
    };
    static_assert(sizeof(cases) / sizeof(cases[0]) == 31, "Every public blend enum, including markers, needs a case");
    for (const auto& item : cases) {
        checks.push_back({std::string("blend-paint-and-shader-") + item.name, [item](tgfx::Context* context) {
            require(toSkBlendMode(item.mode) == item.native, "Public blend mapped to the wrong native enum");
            const auto destination = item.translucent ? Color::makeARGB(170, 0, 0, 255)
                                                      : Color::makeARGB(255, 64, 128, 192);
            const auto source = item.translucent ? Color::makeARGB(85, 255, 0, 0)
                                                : Color::makeARGB(255, 192, 32, 64);
            auto painted = render(context, record([&](DrawingContext& drawing) {
                drawing.drawPaint(solid(destination), Rect::makeXYWH(16, 16, 64, 64));
                Paint paint = solid(source);
                paint.setBlendMode(toSkBlendMode(item.mode));
                drawing.drawPaint(paint, Rect::makeXYWH(32, 32, 64, 64));
            }));
            pixel(painted, 48, 48, item.expected, 3);
            pixel(painted, 24, 24,
                  item.translucent ? Pixel{0, 0, 170, 170} : Pixel{64, 128, 192, 255}, 1);
            pixel(painted, 112, 112, {0, 0, 0, 0});
            auto shaded = render(context, record([&](DrawingContext& drawing) {
                Paint paint;
                paint.setShader(Shader::makeBlend(item.mode, Shader::makeColor(destination), Shader::makeColor(source)));
                drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 80, 80));
            }));
            pixel(shaded, 48, 48, item.expected, 3);
            pixel(shaded, 112, 112, {0, 0, 0, 0});
        }});
    }
}

void addGeometryEffectChecks(std::vector<Check>& checks) {
    checks.push_back({"effects-dash-copy-construction-assignment-and-replacement", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 4);
            original.setStrokeDashPattern(8, 8);
            Paint constructed(original);
            Paint assigned = stroke(Color::green(), 12);
            assigned.setStrokeDotPattern(5, 20);
            assigned = original;
            original.setStrokeDotPattern(2, 10);
            original.setColor(Color::blue());
            original.setStrokeWidth(14);
            Path path;
            path.moveTo(8, 24);
            path.lineTo(104, 24);
            drawing.drawPaint(constructed, path);
            path.reset();
            path.moveTo(8, 56);
            path.lineTo(104, 56);
            drawing.drawPaint(assigned, path);
            path.reset();
            path.moveTo(8, 88);
            path.lineTo(104, 88);
            drawing.drawPaint(original, path);
        }));
        for (int y : {24, 56}) {
            pixel(frame, 10, y, {255, 0, 0, 255});
            pixel(frame, 20, y, {0, 0, 0, 0});
            pixel(frame, 26, y, {255, 0, 0, 255});
            pixel(frame, 36, y, {0, 0, 0, 0});
        }
        pixel(frame, 8, 88, {0, 0, 255, 255});
        pixel(frame, 13, 88, {0, 0, 0, 0});
        pixel(frame, 8, 91, {0, 0, 0, 0}); // A stamp is filled, not stroked with width 14.
    }});
    checks.push_back({"effects-dash-rect-helper-and-fill-style", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto paint = stroke(Color::red(), 4);
            paint.setStrokeDashPattern(12, 12);
            drawing.drawPaint(paint, Rect::makeXYWH(16, 16, 72, 72));
            Paint fill(paint);
            fill.setStroke(false);
            drawing.drawPaint(fill, Rect::makeXYWH(16, 100, 72, 16));
        }));
        pixel(frame, 20, 16, {255, 0, 0, 255});
        pixel(frame, 32, 16, {0, 0, 0, 0});
        pixel(frame, 88, 20, {255, 0, 0, 255});
        pixel(frame, 88, 32, {0, 0, 0, 0});
        pixel(frame, 48, 48, {0, 0, 0, 0});
        pixel(frame, 32, 108, {255, 0, 0, 255});
    }});
    checks.push_back({"effects-copied-dash-conic-and-contour-phase-reset", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 3);
            original.setStrokeDashPattern(8, 8);
            Paint paint(original);
            original.setStrokeDashPattern(1, 1000);
            Path path;
            // A quarter-circle conic, radius 56, centered at (76,76).
            path.moveTo(20, 76);
            path.conicTo(20, 20, 76, 20, 0.70710678118f);
            path.moveTo(20, 108);
            path.lineTo(108, 108);
            drawing.drawPaint(paint, path);
        }));
        pixel(frame, 20, 72, {255, 0, 0, 255});
        pixel(frame, 21, 64, {0, 0, 0, 0});
        pixel(frame, 23, 56, {255, 0, 0, 255});
        pixel(frame, 27, 49, {0, 0, 0, 0});
        pixel(frame, 24, 108, {255, 0, 0, 255});
        pixel(frame, 32, 108, {0, 0, 0, 0});
        pixel(frame, 64, 76, {0, 0, 0, 0}); // Not the conic's endpoint chord or a filled contour.
    }});
    checks.push_back({"effects-dots-assignment-multiple-contours-and-filled-stamps", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 14);
            original.setStrokeDotPattern(3, 16);
            Paint paint;
            paint.setStrokeDashPattern(1, 1000);
            paint = original;
            original.setStrokeDotPattern(8, 32);
            original.setColor(Color::green());
            Path path;
            path.moveTo(12, 24);
            path.lineTo(108, 24);
            path.moveTo(20, 64);
            path.lineTo(116, 64);
            drawing.drawPaint(paint, path);
        }));
        pixel(frame, 12, 24, {255, 0, 0, 255});
        pixel(frame, 28, 24, {255, 0, 0, 255});
        pixel(frame, 20, 24, {0, 0, 0, 0});
        pixel(frame, 20, 64, {255, 0, 0, 255});
        pixel(frame, 36, 64, {255, 0, 0, 255});
        pixel(frame, 28, 64, {0, 0, 0, 0});
        pixel(frame, 12, 28, {0, 0, 0, 0});
    }});
    checks.push_back({"effects-dots-measure-conic-arc-instead-of-endpoint-chord", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 18);
            // Half the analytic quarter-circle length: pi * 56 / 4.
            // This scalar is an independent fixture, not a test PathMeasure.
            original.setStrokeDotPattern(4, 43.98229715f);
            Paint paint(original);
            Path path;
            path.moveTo(20, 76);
            path.conicTo(20, 20, 76, 20, 0.70710678118f);
            drawing.drawPaint(paint, path);
        }));
        pixel(frame, 20, 76, {255, 0, 0, 255});
        pixel(frame, 36, 36, {255, 0, 0, 255}); // Circle's 45-degree position.
        pixel(frame, 48, 48, {0, 0, 0, 0}); // Midpoint of the straight endpoint chord.
        pixel(frame, 36, 42, {0, 0, 0, 0}); // Radius stays 4 despite stroke width 18.
    }});
    checks.push_back({"effects-dots-cubic-and-quadratic-interiors", [](tgfx::Context* context) {
        auto cubic = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 20);
            original.setStrokeDotPattern(2, 1);
            Paint paint(original);
            Path path;
            path.moveTo(16, 96);
            path.cubicTo(32, 16, 96, 16, 112, 96);
            drawing.drawPaint(paint, path);
        }));
        pixel(cubic, 64, 36, {255, 0, 0, 255});
        pixel(cubic, 64, 70, {0, 0, 0, 0});
        pixel(cubic, 64, 96, {0, 0, 0, 0});
        auto quadratic = render(context, record([](DrawingContext& drawing) {
            auto original = stroke(Color::red(), 20);
            original.setStrokeDotPattern(2, 1);
            Paint paint;
            paint = original;
            Path path;
            path.moveTo(16, 96);
            path.quadTo(64, 0, 112, 96);
            drawing.drawPaint(paint, path);
        }));
        pixel(quadratic, 64, 48, {255, 0, 0, 255});
        pixel(quadratic, 64, 70, {0, 0, 0, 0});
        pixel(quadratic, 64, 96, {0, 0, 0, 0});
    }});
    checks.push_back({"effects-empty-dot-stamp-replaces-dash-without-unmodified-stroke", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(0, 0, kSize, kSize));
            auto paint = stroke(Color::red(), 6);
            paint.setStrokeDashPattern(8, 8);
            paint.setStrokeDotPattern(0, 16);
            Path path;
            path.moveTo(16, 32);
            path.lineTo(112, 32);
            drawing.drawPaint(paint, path);
        }));
        pixel(frame, 20, 32, {0, 0, 255, 255});
        pixel(frame, 28, 32, {0, 0, 255, 255});
        pixel(frame, 64, 32, {0, 0, 255, 255});
    }});
}

void requireBoundedDotGovernorControl() {
    // Run this BEFORE every adversarial input. With the exact old Paint this
    // recorder materializes at most 100001 ovals, then this assertion FAILS.
    // The recording is destroyed without GPU playback. Consequently the old
    // source never reaches the later 10M/16M-stamp inputs.
    //
    // This is a real production Paint draw, not a cloned count calculation or
    // a test-only production getter. The native recording canvas exposes the
    // same sticky error used by ordinary destinations.
    tgfx::PictureRecorder recorder;
    auto* canvas = recorder.beginRecording();
    auto paint = stroke(Color::red(), 14);
    paint.setStrokeDotPattern(1, 1);
    tgfx::Path path;
    path.moveTo(16, 24);
    path.lineTo(100017, 24); // Exactly 100001 units at advance 1.
    paint.drawPath(canvas, path);
    require(canvas->hasError(),
            "Bounded 100001-stamp control did not reject the draw; adversarial inputs were NOT executed");
    require(canvas->getSaveCount() == 0, "Dot governor control leaked canvas saves");
}

Frame rejectedDotReadback(const std::shared_ptr<tgfx::Surface>& surface) {
    // Diagnostic native readback only: this does not change the production
    // rule that a destination with hasError() is a failed render. We inspect
    // pixels solely to reject partial dots or an unmodified geometry fallback.
    require(surface->getCanvas()->hasError(), "Rejected dot draw did not report native failure");
    auto info = tgfx::ImageInfo::Make(surface->width(), surface->height(),
                                    tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    Frame frame{surface->width(), surface->height(), std::vector<uint8_t>(info.byteSize(), 0xcd)};
    require(surface->readPixels(info, frame.pixels.data()), "Could not inspect rejected dot draw's native pixels");
    require(surface->getCanvas()->hasError(), "Dot governor failure was cleared by readback");
    return frame;
}

void requireRejectedDotPicture(tgfx::Context* context, const Paint& paint, const Path& path) {
    // Exercise the normal DrawingContext -> LayerContent -> native picture
    // replay boundary, including Paint's copy ownership after the recorder dies.
    auto content = record([&](DrawingContext& drawing) { drawing.drawPaint(paint, path); });
    require(content.picture != nullptr, "Dot rejection was lost as an empty/successful recording");
    auto surface = makeSurface(context, kSize, kSize);
    solid(Color::blue()).drawRect(surface->getCanvas(), tgfx::Rect::MakeWH(128, 128));
    const auto before = readback(surface);
    surface->getCanvas()->drawPicture(content.picture);
    require(surface->getCanvas()->hasError(), "Recorded dot rejection did not fail its replay destination");
    require(surface->getCanvas()->getSaveCount() == 0, "Rejected dot picture leaked canvas saves");
    framesNear(rejectedDotReadback(surface), before, 0);
}

void addDotGovernorChecks(std::vector<Check>& checks) {
    checks.push_back({"effects-dot-governor-tiny-advance-rejected-before-materialization",
                      [](tgfx::Context* context) {
        requireBoundedDotGovernorControl();
        auto original = stroke(Color::red(), 14);
        original.setStrokeDotPattern(3, 0.0001f);
        Paint paint(original);
        original.setStrokeDotPattern(1, 16);
        Path path;
        path.moveTo(16, 24);
        path.lineTo(1040, 24); // Old unchecked expansion: about 10.24 million circles.
        requireRejectedDotPicture(context, paint, path);
    }});
    checks.push_back({"effects-dot-governor-large-contour-rejected-before-materialization",
                      [](tgfx::Context* context) {
        requireBoundedDotGovernorControl();
        auto paint = stroke(Color::red(), 14);
        paint.setStrokeDotPattern(3, 1);
        Path path;
        path.moveTo(0, 64);
        path.lineTo(16777216, 64); // Exactly 2^24 units, not a synthetic measured length.
        requireRejectedDotPicture(context, paint, path);
    }});
    checks.push_back({"effects-dot-governor-total-budget-preflights-all-contours",
                      [](tgfx::Context* context) {
        requireBoundedDotGovernorControl();
        auto original = stroke(Color::red(), 14);
        original.setStrokeDotPattern(3, 1);
        Paint paint;
        paint = original;
        original.setStrokeDashPattern(8, 8);
        Path path;
        path.moveTo(16, 24);
        path.lineTo(80, 24); // Valid prefix must not be emitted on later rejection.
        path.moveTo(16, 64);
        path.lineTo(60016, 64);
        path.moveTo(16, 96);
        path.lineTo(60016, 96);
        // Each contour is individually below Skia's 100000 limit, but the
        // complete draw requires 120064 stamps. An aggregate governor matters.
        requireRejectedDotPicture(context, paint, path);
    }});
    checks.push_back({"effects-dot-governor-small-advance-within-budget-retains-filled-contour-stamps",
                      [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            drawing.concat(Matrix::makeScaleTranslate(100000, 100000, 0, 0));
            auto original = stroke(Color::red(), 14);
            original.setStrokeDotPattern(0.000025f, 0.0001f);
            Paint paint(original);
            original.setStrokeDotPattern(1, 1);
            Path path;
            path.moveTo(0.0002f, 0.0004f);
            path.lineTo(0.00116f, 0.0004f);
            path.moveTo(0.00025f, 0.0008f);
            path.lineTo(0.00075f, 0.0008f);
            drawing.drawPaint(paint, path);
        }));
        pixel(frame, 20, 40, {255, 0, 0, 255});
        pixel(frame, 30, 40, {255, 0, 0, 255});
        pixel(frame, 25, 40, {0, 0, 0, 0});
        pixel(frame, 20, 44, {0, 0, 0, 0}); // Fill stamps still ignore the large stroke width.
        pixel(frame, 25, 80, {255, 0, 0, 255});
        pixel(frame, 35, 80, {255, 0, 0, 255});
        pixel(frame, 30, 80, {0, 0, 0, 0});
    }});
    checks.push_back({"effects-dot-governor-exact-total-limit-allows-multiple-contours",
                      [](tgfx::Context*) {
        // The accepted boundary is intentionally a recording-only check.
        // It prepares 100000 real circles but never submits that geometry to
        // the GPU; public picture bounds witness the first/last filled stamps.
        tgfx::PictureRecorder recorder;
        auto* canvas = recorder.beginRecording();
        auto paint = stroke(Color::red(), 14);
        paint.setStrokeDotPattern(1, 1);
        tgfx::Path path;
        path.moveTo(16, 24);
        path.lineTo(50016, 24);
        path.moveTo(16, 64);
        path.lineTo(50016, 64);
        paint.drawPath(canvas, path);
        require(!canvas->hasError(), "Exactly 100000 total stamps must fit the documented budget");
        require(canvas->getSaveCount() == 0, "Accepted dot boundary leaked canvas saves");
        auto picture = recorder.finishRecordingAsPicture();
        require(picture != nullptr, "Accepted dot boundary produced no geometry");
        const auto bounds = picture->getBounds();
        require(bounds == tgfx::Rect::MakeLTRB(15, 23, 50016, 65),
                "Accepted boundary lost filled stamps, contour phase reset, or endpoint exclusion");
    }});
    checks.push_back({"effects-dot-governor-rejection-survives-opaque-fill-and-picture-replay",
                      [](tgfx::Context* context) {
        requireBoundedDotGovernorControl();
        tgfx::PictureRecorder recorder;
        auto* canvas = recorder.beginRecording();
        auto original = stroke(Color::red(), 14);
        original.setStrokeDotPattern(3, 1);
        Paint paint(original);
        tgfx::Path path;
        path.moveTo(16, 24);
        path.lineTo(100017, 24); // Bounded input even if this particular guard regresses.
        paint.drawPath(canvas, path);
        require(canvas->hasError(), "Dot rejection did not fail its recording canvas");
        canvas->clear(tgfx::Color::Green()); // A visual discard must not erase the failure record.
        require(canvas->hasError(), "Opaque fill cleared the recording's dot rejection");
        auto picture = recorder.finishRecordingAsPicture();
        require(picture != nullptr, "Opaque fill discarded the complete rejected recording");
        auto failed = makeSurface(context, kSize, kSize);
        auto healthy = makeSurface(context, kSize, kSize);
        failed->getCanvas()->drawPicture(picture);
        require(failed->getCanvas()->hasError(), "Opaque fill dropped the replayable dot rejection");
        require(failed->getCanvas()->getSaveCount() == 0, "Replayed dot rejection leaked canvas saves");
        require(!healthy->getCanvas()->hasError(), "Dot rejection contaminated an independent surface");
        solid(Color::blue()).drawRect(healthy->getCanvas(), tgfx::Rect::MakeWH(128, 128));
        pixel(readback(healthy), 32, 24, {0, 0, 255, 255});
    }});
}

void addBlurChecks(std::vector<Check>& checks) {
    struct BlurCase {
        const char* name;
        BlurStyle style;
    };
    const BlurCase cases[] = {
        {"normal", BlurStyleNormal}, {"solid", BlurStyleSolid},
        {"outer", BlurStyleOuter}, {"inner", BlurStyleInner},
    };
    for (const auto& item : cases) {
        checks.push_back({std::string("blur-style-") + item.name, [item](tgfx::Context* context) {
            auto frame = render(context, record([&](DrawingContext& drawing) {
                Paint paint = solid(Color::white());
                paint.setAntiAlias(true);
                paint.setMaskFilter(MaskFilter::makeBlur(item.style, 4));
                drawing.drawPaint(paint, Rect::makeXYWH(40, 40, 48, 48));
            }));
            if (item.style == BlurStyleOuter) {
                pixel(frame, 64, 64, {0, 0, 0, 0}, 1);
                pixel(frame, 41, 64, {0, 0, 0, 0}, 1);
            } else {
                pixel(frame, 64, 64, {255, 255, 255, 255}, 3);
                if (item.style == BlurStyleSolid) {
                    pixel(frame, 41, 64, {255, 255, 255, 255}, 2);
                } else {
                    alphaBetween(frame, 41, 64, 100, 235, "Missing soft inside edge");
                }
            }
            if (item.style == BlurStyleInner) {
                pixel(frame, 36, 64, {0, 0, 0, 0});
            } else {
                alphaBetween(frame, 36, 64, 8, 120, "Missing finite outside blur halo");
                auto halo = at(frame, 36, 64);
                pixel(frame, 36, 64, {halo[3], halo[3], halo[3], halo[3]}, 2);
            }
            pixel(frame, 16, 64, {0, 0, 0, 0});
            pixel(frame, 112, 64, {0, 0, 0, 0});
        }});
    }
    checks.push_back({"blur-operates-on-coverage-without-smearing-shader-rgb", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            LinearGradient gradient;
            gradient.setColors({Color::red(), Color::red(), Color::blue(), Color::blue()});
            gradient.setLocations({0, 0.5f, 0.5f, 1});
            gradient.setOrientation(LinearGradientOrientationLeftRight);
            gradient.update(Rect::makeXYWH(40, 40, 48, 48));
            Paint paint;
            gradient.applyToPaint(paint);
            paint.setMaskFilter(MaskFilter::makeBlur(BlurStyleNormal, 4));
            drawing.drawPaint(paint, Rect::makeXYWH(40, 40, 48, 48));
        }));
        pixel(frame, 60, 64, {255, 0, 0, 255}, 3);
        pixel(frame, 68, 64, {0, 0, 255, 255}, 3);
        alphaBetween(frame, 36, 64, 8, 120, "Shader-colored outside coverage was dropped");
        auto halo = at(frame, 36, 64);
        pixel(frame, 36, 64, {halo[3], 0, 0, halo[3]}, 2);
    }});
    checks.push_back({"blur-copy-assignment-retains-style-and-reset-is-independent", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            Paint original = solid(Color::red());
            original.setMaskFilter(MaskFilter::makeBlur(BlurStyleOuter, 3));
            Paint assigned;
            assigned = original;
            original.setMaskFilter(MaskFilter());
            drawing.drawPaint(assigned, Rect::makeXYWH(24, 40, 24, 24));
            drawing.drawPaint(original, Rect::makeXYWH(80, 40, 24, 24));
        }));
        pixel(frame, 36, 52, {0, 0, 0, 0}, 1);
        alphaBetween(frame, 20, 52, 5, 120, "Copied outer blur was lost");
        pixel(frame, 92, 52, {255, 0, 0, 255});
        pixel(frame, 76, 52, {0, 0, 0, 0});
    }});
    checks.push_back({"blur-default-respects-ctm-and-explicit-false-stays-device-sized", [](tgfx::Context* context) {
        auto scene = [](bool respect, bool useDefault) {
            return record([&](DrawingContext& drawing) {
                drawing.concat(Matrix::makeScale(2, 2));
                Paint paint = solid(Color::white());
                paint.setMaskFilter(useDefault ? MaskFilter::makeBlur(BlurStyleNormal, 3)
                                               : MaskFilter::makeBlur(BlurStyleNormal, 3, respect));
                drawing.drawPaint(paint, Rect::makeXYWH(20, 20, 24, 24));
            });
        };
        auto defaultBlur = render(context, scene(true, true));
        auto scaled = render(context, scene(true, false));
        auto fixed = render(context, scene(false, false));
        framesNear(defaultBlur, scaled, 1);
        auto scaledAlpha = at(scaled, 34, 64)[3];
        auto fixedAlpha = at(fixed, 34, 64)[3];
        require(scaledAlpha > fixedAlpha + 15,
                "respectCTM did not enlarge the halo: true=" + std::to_string(scaledAlpha) +
                    ", false=" + std::to_string(fixedAlpha));
        alphaBetween(fixed, 64, 64, 250, 255, "Fixed-size blur center missing");
    }});
    checks.push_back({"blur-respect-ctm-false-picture-replay", [](tgfx::Context* context) {
        auto local = record([](DrawingContext& drawing) {
            Paint paint = solid(Color::white());
            paint.setMaskFilter(MaskFilter::makeBlur(BlurStyleNormal, 3, false));
            drawing.drawPaint(paint, Rect::makeXYWH(20, 20, 24, 24));
        });
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.pushContext(Matrix::makeScale(2, 2), 1, 1, true);
        list.appendLayerContent(local, 1);
        list.popContext();
        auto replayed = render(context, list);
        auto direct = render(context, record([](DrawingContext& drawing) {
            drawing.concat(Matrix::makeScale(2, 2));
            Paint paint = solid(Color::white());
            paint.setMaskFilter(MaskFilter::makeBlur(BlurStyleNormal, 3, false));
            drawing.drawPaint(paint, Rect::makeXYWH(20, 20, 24, 24));
        }));
        // With respectCTM=false the same device geometry must retain the same
        // device sigma regardless of whether scale is introduced before or
        // after recording. A pre-resolved mask that scales on replay FAILS.
        pixel(replayed, 34, 64, at(direct, 34, 64), 6);
        pixel(replayed, 36, 64, at(direct, 36, 64), 6);
        pixel(replayed, 64, 64, at(direct, 64, 64), 3);
    }});
    checks.push_back({"box-shadow-offset-double-sigma-and-recorder-bounds-bleed", [](tgfx::Context* context) {
        auto frame = render(context, record([](DrawingContext& drawing) {
            BoxShadow shadow;
            shadow.setOffset(Size::make(40, 24));
            shadow.setColor(Color::red());
            shadow.setBlurAmount(4); // The public BoxShadow API uses sigma=2*amount.
            shadow.draw(drawing, BorderRadius());
        }, 32, 32));
        alphaBetween(frame, 56, 40, 190, 255, "Shadow body missing at requested offset");
        alphaBetween(frame, 30, 40, 6, 90, "Shadow did not use doubled sigma or was clipped to recording bounds");
        pixel(frame, 16, 16, {0, 0, 0, 0});
        auto halo = at(frame, 30, 40);
        pixel(frame, 30, 40, {halo[3], 0, 0, halo[3]}, 2);
    }});
}

void addCompositionChecks(std::vector<Check>& checks) {
    checks.push_back({"display-list-group-opacity-once-across-overlapping-pictures", [](tgfx::Context* context) {
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.pushContext(Matrix(), 0.5f, 1, true);
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(8, 8, 64, 64)), 1);
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(40, 8, 64, 64)), 1);
        list.popContext();
        auto frame = render(context, list);
        pixel(frame, 16, 24, {128, 0, 0, 128}, 1);
        pixel(frame, 48, 24, {0, 0, 128, 128}, 1);
        pixel(frame, 96, 24, {0, 0, 128, 128}, 1);
        pixel(frame, 120, 24, {0, 0, 0, 0});
    }});
    checks.push_back({"display-list-picture-opacity-once-for-overlapping-primitives", [](tgfx::Context* context) {
        auto content = record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(8, 8, 64, 64));
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(40, 8, 64, 64));
        });
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(content, 0.5f);
        auto frame = render(context, list);
        pixel(frame, 16, 24, {128, 0, 0, 128}, 1);
        pixel(frame, 48, 24, {0, 0, 128, 128}, 1);
        pixel(frame, 96, 24, {0, 0, 128, 128}, 1);
    }});
    checks.push_back({"composition-nested-opacity-transform-clip-and-display-list-restore", [](tgfx::Context* context) {
        const auto translation = Matrix::makeScaleTranslate(1, 1, 16, 16);
        const auto scale = Matrix::makeScale(2, 2);
        auto state = CompositionState().pushContext(0.5f, translation).pushContext(0.5f, scale);
        require(state.getAbsoluteOpacity() == 0.25f, "CompositionState lost nested opacity");
        require(state.getAbsoluteRect(Rect::makeXYWH(8, 8, 16, 16)) == Rect::makeXYWH(32, 32, 32, 32),
                "CompositionState composed transforms incorrectly");
        state.clipRect(24, 24);
        require(state.getAbsoluteClippedRect(Rect::makeXYWH(16, 16, 16, 16)) == Rect::makeLTRB(48, 48, 64, 64),
                "CompositionState did not intersect absolute clip bounds");
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.pushContext(translation, 0.5f, 1, true);
        list.pushContext(scale, 0.5f, 2, true);
        list.appendClipRect(24, 24);
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(8, 8, 32, 32)), 0.5f);
        list.popContext();
        list.popContext();
        list.appendLayerContent(rectangle(Color::green(), Rect::makeXYWH(4, 4, 8, 8)), 1);
        auto frame = render(context, list);
        pixel(frame, 40, 40, {0, 0, 32, 32}, 1); // .5 group * .5 group * .5 picture.
        pixel(frame, 72, 40, {0, 0, 0, 0}); // Absolute clip ends at x=64.
        pixel(frame, 8, 8, {0, 255, 0, 255}); // Restored transform, clip and opacity.
    }});
    checks.push_back({"display-list-zero-opacity-restores-following-content", [](tgfx::Context* context) {
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.pushContext(Matrix::makeScaleTranslate(1, 1, 32, 32), 0, 1, true);
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.popContext();
        list.appendLayerContent(rectangle(Color::green(), Rect::makeXYWH(4, 4, 8, 8)), 1);
        auto frame = render(context, list);
        pixel(frame, 64, 64, {0, 0, 255, 255});
        pixel(frame, 8, 8, {0, 255, 0, 255});
    }});
    checks.push_back({"display-list-partial-viewport-translates-at-native-scale", [](tgfx::Context* context) {
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.setViewport(Rect::makeXYWH(40, 24, 32, 32));
        auto content = record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(40, 24, 16, 32));
            drawing.drawPaint(solid(Color::blue()), Rect::makeXYWH(56, 24, 16, 32));
        });
        list.appendLayerContent(content, 1);
        auto frame = render(context, list, 32, 32);
        pixel(frame, 4, 4, {255, 0, 0, 255});
        pixel(frame, 20, 4, {0, 0, 255, 255});
        pixel(frame, 4, 28, {255, 0, 0, 255});
        pixel(frame, 28, 28, {0, 0, 255, 255});
    }});
    checks.push_back({"display-list-opacity-group-retains-decoration-beyond-layer-bounds", [](tgfx::Context* context) {
        auto content = record([](DrawingContext& drawing) {
            drawing.drawPaint(solid(Color::red()), Rect::makeXYWH(0, 0, 48, 48));
        }, 24, 24);
        require(content.bounds == Rect::makeXYWH(0, 0, 24, 24), "Recording bounds metadata changed");
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.pushContext(Matrix::makeScaleTranslate(1, 1, 16, 16), 0.5f, 1, true);
        list.appendLayerContent(content, 1);
        list.popContext();
        auto frame = render(context, list);
        pixel(frame, 24, 24, {128, 0, 0, 128}, 1);
        pixel(frame, 56, 56, {128, 0, 0, 128}, 1); // Beyond layer hint, within destination.
        pixel(frame, 72, 56, {0, 0, 0, 0});
    }});
    checks.push_back({"mask-rect-partial-alpha-preserves-preexisting-background", [](tgfx::Context* context) {
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        Paint maskPaint = solid(Color::makeARGB(128, 255, 255, 255));
        maskPaint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        auto mask = Valdi::makeShared<PaintMask>(maskPaint, Path(), Rect::makeXYWH(32, 32, 32, 32));
        require(mask->getBounds() == Rect::makeXYWH(32, 32, 32, 32), "Rect mask bounds changed");
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(16, 16, 80, 80)), 1);
        list.appendApplyMask(mask.get());
        mask = nullptr; // DisplayList must retain both operation references itself.
        auto frame = render(context, list);
        pixel(frame, 48, 48, {127, 0, 128, 255}, 2);
        pixel(frame, 24, 48, {255, 0, 0, 255});
        pixel(frame, 104, 48, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-oval-path-bounds-override-fallback-rect", [](tgfx::Context* context) {
        Path path;
        path.addOval(Rect::makeXYWH(32, 32, 64, 64), true);
        Paint maskPaint = solid(Color::white());
        maskPaint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        auto mask = Valdi::makeShared<PaintMask>(maskPaint, path, Rect::makeXYWH(0, 0, 4, 4));
        require(mask->getBounds() == Rect::makeXYWH(32, 32, 64, 64), "Path bounds did not override the fallback rect");
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(16, 16, 96, 96)), 1);
        list.appendApplyMask(mask.get());
        auto frame = render(context, list);
        pixel(frame, 64, 64, {0, 0, 255, 255});
        pixel(frame, 34, 34, {255, 0, 0, 255}); // Inside bounds but outside the oval.
        pixel(frame, 24, 64, {255, 0, 0, 255});
        pixel(frame, 120, 64, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-composite-reverse-unwind-and-union-bounds", [](tgfx::Context* context) {
        Paint paint = solid(Color::makeARGB(128, 255, 255, 255));
        paint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        std::vector<Ref<IMask>> masks;
        masks.emplace_back(Valdi::makeShared<PaintMask>(paint, Path(), Rect::makeXYWH(24, 24, 48, 64)));
        masks.emplace_back(Valdi::makeShared<PaintMask>(paint, Path(), Rect::makeXYWH(48, 40, 48, 64)));
        auto mask = Valdi::makeShared<CompositeMask>(std::move(masks));
        require(mask->getBounds() == Rect::makeLTRB(24, 24, 96, 104), "Composite mask union bounds changed");
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(16, 16, 96, 96)), 1);
        list.appendApplyMask(mask.get());
        mask = nullptr;
        auto frame = render(context, list);
        pixel(frame, 32, 32, {127, 0, 128, 255}, 2);
        pixel(frame, 88, 56, {127, 0, 128, 255}, 2);
        pixel(frame, 56, 56, {63, 0, 192, 255}, 2);
        pixel(frame, 24, 108, {255, 0, 0, 255});
        pixel(frame, 120, 64, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-gradient-alpha-controls-content-without-coloring-background", [](tgfx::Context* context) {
        LinearGradient gradient;
        gradient.setColors({Color::black(), Color::transparent()});
        gradient.setOrientation(LinearGradientOrientationLeftRight);
        gradient.update(Rect::makeXYWH(32.5f, 32.5f, 64, 64));
        Paint paint;
        gradient.applyToPaint(paint);
        paint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        auto mask = Valdi::makeShared<PaintMask>(paint, Path(), Rect::makeXYWH(32.5f, 32.5f, 64, 64));
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(16, 16, 96, 96)), 1);
        list.appendApplyMask(mask.get());
        auto frame = render(context, list);
        pixel(frame, 40, 64, {32, 0, 223, 255}, 2);
        pixel(frame, 88, 64, {223, 0, 32, 255}, 2);
        pixel(frame, 24, 64, {255, 0, 0, 255});
        pixel(frame, 120, 64, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-local-bounds-follow-display-list-context-transform", [](tgfx::Context* context) {
        Paint paint = solid(Color::white());
        paint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        auto mask = Valdi::makeShared<PaintMask>(paint, Path(), Rect::makeXYWH(16, 16, 32, 32));
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::blue(), Rect::makeXYWH(0, 0, kSize, kSize)), 1);
        list.pushContext(Matrix::makeScaleTranslate(1, 1, 16, 16), 1, 1, true);
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(8, 8, 80, 80)), 1);
        list.appendApplyMask(mask.get());
        list.popContext();
        auto frame = render(context, list);
        require(mask->getBounds() == Rect::makeXYWH(16, 16, 32, 32), "Replay mutated local mask bounds");
        pixel(frame, 40, 40, {0, 0, 255, 255});
        pixel(frame, 24, 40, {255, 0, 0, 255});
        pixel(frame, 72, 40, {255, 0, 0, 255});
        pixel(frame, 16, 16, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-bounded-save-behind-preserves-outside-destination-blends", [](tgfx::Context* context) {
        Paint paint = solid(Color::white());
        paint.setBlendMode(toSkBlendMode(BlendModeDstOut));
        auto mask = Valdi::makeShared<PaintMask>(paint, Path(), Rect::makeXYWH(72, 72, 32, 32));
        auto behind = record([](DrawingContext& drawing) {
            Paint green = solid(Color::green());
            green.setBlendMode(toSkBlendMode(BlendModeDstOver));
            drawing.drawPaint(green, Rect::makeXYWH(0, 0, 112, 112));
        });
        DisplayList list(Size::make(kSize, kSize), TimePoint());
        list.appendLayerContent(rectangle(Color::red(), Rect::makeXYWH(8, 8, 32, 32)), 1);
        list.appendPrepareMask(mask.get());
        list.appendLayerContent(behind, 1);
        list.appendApplyMask(mask.get());
        auto frame = render(context, list);
        // Outside the saved subset, DstOver must still see the existing red.
        // Whole-canvas saveLayer isolation makes this pixel green and FAILS.
        pixel(frame, 16, 16, {255, 0, 0, 255});
        pixel(frame, 48, 16, {0, 255, 0, 255});
        pixel(frame, 80, 80, {0, 0, 0, 0});
        pixel(frame, 120, 120, {0, 0, 0, 0});
    }});
}

// These exercise the native mechanisms through production Paint/PaintMask and
// public Canvas APIs. Deliberately unbalanced deferred callbacks are a public
// contract violation with deterministic error reporting, not an injected flag.
void addDestinationChecks(std::vector<Check>& checks) {
    checks.push_back({"mask-bounded-capture-nested-overlap-scaled-nonzero-origin", [](tgfx::Context* context) {
        auto surface = makeSurface(context, kSize, kSize);
        auto* canvas = surface->getCanvas();
        solid(Color::red()).drawRect(canvas, tgfx::Rect::MakeXYWH(0, 0, 64, 128));
        solid(Color::blue()).drawRect(canvas, tgfx::Rect::MakeXYWH(64, 0, 64, 128));
        Paint erase = solid(Color::white());
        erase.setBlendMode(toSkBlendMode(BlendModeDstOut));
        PaintMask first(erase, Path(), Rect::makeXYWH(8, 12, 24, 20));
        PaintMask second(erase, Path(), Rect::makeXYWH(20, 20, 24, 20));
        canvas->translate(20, 10);
        canvas->scale(2, 1.5f);
        first.prepare(canvas); // device (36,28)-(84,58).
        auto green = solid(Color::green());
        green.setBlendMode(toSkBlendMode(BlendModeDstOver));
        green.drawRect(canvas, tgfx::Rect::MakeWH(64, 80));
        second.prepare(canvas); // device (60,40)-(108,70), overlapping first.
        solid(Color::white()).drawRect(canvas, tgfx::Rect::MakeXYWH(20, 20, 24, 20));
        second.apply(canvas);
        auto intermediate = readback(surface);
        pixel(intermediate, 48, 36, {0, 255, 0, 255});
        pixel(intermediate, 72, 48, {0, 255, 0, 255});
        pixel(intermediate, 100, 48, {0, 0, 255, 255});
        pixel(intermediate, 24, 36, {255, 0, 0, 255});
        first.apply(canvas);
        require(canvas->getSaveCount() == 0, "Nested scaled saveBehind leaked saves");
        auto restored = readback(surface);
        pixel(restored, 48, 36, {255, 0, 0, 255});
        pixel(restored, 72, 48, {0, 0, 255, 255});
        pixel(restored, 100, 48, {0, 0, 255, 255});
        pixel(restored, 24, 36, {255, 0, 0, 255});
    }});
    checks.push_back({"mask-bounded-capture-clipped-fractional-subset-and-empty-save", [](tgfx::Context* context) {
        auto surface = makeSurface(context, kSize, kSize);
        auto* canvas = surface->getCanvas();
        solid(Color::red()).drawRect(canvas, tgfx::Rect::MakeWH(128, 64));
        solid(Color::blue()).drawRect(canvas, tgfx::Rect::MakeXYWH(0, 64, 128, 64));
        canvas->save();
        canvas->clipRect(tgfx::Rect::MakeXYWH(40, 24, 48, 72), false);
        Paint erase = solid(Color::white());
        erase.setBlendMode(toSkBlendMode(BlendModeDstOut));
        PaintMask mask(erase, Path(), Rect::makeXYWH(24.25f, 48.25f, 95.5f, 71.5f));
        mask.prepare(canvas);
        solid(Color::green()).drawRect(canvas, tgfx::Rect::MakeWH(128, 128));
        mask.apply(canvas);
        auto frame = readback(surface);
        pixel(frame, 48, 56, {255, 0, 0, 255}); // restored inside rounded capture.
        pixel(frame, 48, 80, {0, 0, 255, 255});
        pixel(frame, 48, 32, {0, 255, 0, 255}); // new drawing outside capture.
        pixel(frame, 32, 56, {255, 0, 0, 255}); // outside original clip.
        pixel(frame, 96, 80, {0, 0, 255, 255});
        PaintMask empty(erase, Path(), Rect::makeXYWH(200, 200, 16, 16));
        empty.prepare(canvas);
        empty.apply(canvas);
        canvas->restore();
        require(!canvas->hasError(), "A clipped-empty saveBehind is not a rendering failure");
        require(canvas->getSaveCount() == 0, "Clipped/empty saveBehind leaked saves");
        framesNear(readback(surface), frame, 0);
    }});
    checks.push_back({"mask-bounded-capture-bottom-left-target-origin", [](tgfx::Context* context) {
        auto storage = makeSurface(context, kSize, kSize);
        auto texture = storage->getBackendTexture();
        require(texture.isValid(), "No backend texture for bottom-left target regression");
        auto surface = tgfx::Surface::MakeFrom(context, texture, tgfx::ImageOrigin::BottomLeft,
                                              1, 0, tgfx::ColorSpace::SRGB());
        require(surface != nullptr, "Could not wrap bottom-left GPU target");
        auto* canvas = surface->getCanvas();
        solid(Color::red()).drawRect(canvas, tgfx::Rect::MakeWH(128, 48));
        solid(Color::blue()).drawRect(canvas, tgfx::Rect::MakeXYWH(0, 48, 128, 80));
        Paint erase = solid(Color::white());
        erase.setBlendMode(toSkBlendMode(BlendModeDstOut));
        PaintMask mask(erase, Path(), Rect::makeXYWH(24, 32, 48, 48));
        mask.prepare(canvas);
        solid(Color::green()).drawRect(canvas, tgfx::Rect::MakeXYWH(24, 32, 48, 48));
        mask.apply(canvas);
        require(canvas->getSaveCount() == 0, "Bottom-left mask leaked saves");
        auto frame = readback(surface);
        pixel(frame, 40, 40, {255, 0, 0, 255});
        pixel(frame, 40, 64, {0, 0, 255, 255});
        pixel(frame, 8, 40, {255, 0, 0, 255});
        pixel(frame, 88, 64, {0, 0, 255, 255});
    }});
    checks.push_back({"mask-small-captures-do-not-retain-target-sized-textures", [](tgfx::Context* context) {
        // Purge previous cases' unused textures before establishing the baseline.
        // Public resource accounting includes the copies instantiated by encode.
        // The default cache is ample for the broken 16 * 4 MiB implementation;
        // the resource-expiration window cannot expire these in one submission.
        context->flushAndSubmit(true);
        context->purgeResourcesUntilMemoryTo(0);
        auto surface = makeSurface(context, 1024, 1024);
        surface->getCanvas()->clear(tgfx::Color::Red());
        context->flushAndSubmit(true);
        const auto baseline = context->memoryUsage();
        Paint erase = solid(Color::white());
        erase.setBlendMode(toSkBlendMode(BlendModeDstOut));
        for (int i = 0; i < 16; ++i) {
            const auto x = static_cast<float>(32 + i * 40);
            PaintMask mask(erase, Path(), Rect::makeXYWH(x, 96, 24, 24));
            mask.prepare(surface->getCanvas());
            solid(Color::blue()).drawRect(surface->getCanvas(),
                                         tgfx::Rect::MakeXYWH(x, 96.0f, 24.0f, 24.0f));
            mask.apply(surface->getCanvas());
        }
        // All captures are queued together before submission; no per-mask
        // submit/release can disguise a full-target allocation.
        context->flushAndSubmit(true);
        const auto total = context->memoryUsage();
        const auto growth = total > baseline ? total - baseline : 0;
        require(growth < 4u * 1024u * 1024u,
                "16 small masks retained " + std::to_string(growth) +
                    " extra GPU bytes; budget is less than one 1024x1024 RGBA target");
        require(surface->getCanvas()->getSaveCount() == 0, "Capture allocation scene leaked saves");
        auto frame = readback(surface);
        pixel(frame, 40, 104, {255, 0, 0, 255});
        pixel(frame, 640, 104, {255, 0, 0, 255});
        pixel(frame, 800, 800, {255, 0, 0, 255});
    }});
    checks.push_back({"render-error-independent-existing-and-new-surfaces", [](tgfx::Context* context) {
        auto failed = makeSurface(context, kSize, kSize);
        auto existing = makeSurface(context, kSize, kSize);
        failed->getCanvas()->drawDeferred(tgfx::Rect::MakeWH(16, 16), 0, [](tgfx::Canvas* canvas) {
            solid(Color::red()).drawRect(canvas, tgfx::Rect::MakeWH(16, 16));
            canvas->save(); // Real callback contract violation; native code must unwind.
        });
        require(failed->getCanvas()->hasError(), "Deferred save imbalance was not reported");
        require(failed->getCanvas()->getSaveCount() == 0, "Failure left the canvas unbalanced");
        require(!existing->getCanvas()->hasError(), "Unrelated existing surface inherited a failure");
        auto createdAfter = makeSurface(context, kSize, kSize);
        solid(Color::blue()).drawRect(existing->getCanvas(), tgfx::Rect::MakeWH(128, 128));
        solid(Color::green()).drawRect(createdAfter->getCanvas(), tgfx::Rect::MakeWH(128, 128));
        pixel(readback(existing), 8, 8, {0, 0, 255, 255});
        pixel(readback(createdAfter), 8, 8, {0, 255, 0, 255});
        require(failed->getCanvas()->hasError(), "Rendering another surface cleared sticky failure");
    }});
    checks.push_back({"render-error-offscreen-child-reaches-only-active-ancestor", [](tgfx::Context* context) {
        auto ancestor = makeSurface(context, kSize, kSize);
        auto unrelated = makeSurface(context, kSize, kSize);
        auto* canvas = ancestor->getCanvas();
        canvas->saveLayerAlpha(0.5f);
        canvas->saveLayerAlpha(0.5f);
        canvas->drawDeferred(tgfx::Rect::MakeWH(32, 32), 0, [](tgfx::Canvas* child) {
            solid(Color::red()).drawRect(child, tgfx::Rect::MakeWH(32, 32));
            child->save();
        });
        solid(Color::blue()).drawRect(canvas, tgfx::Rect::MakeXYWH(40, 0, 16, 16));
        canvas->restore(); // A multi-draw layer requires real PictureImage offscreen playback.
        solid(Color::blue()).drawRect(canvas, tgfx::Rect::MakeXYWH(64, 0, 16, 16));
        canvas->restore(); // Propagate through a second offscreen ancestor as well.
        solid(Color::green()).drawRect(unrelated->getCanvas(), tgfx::Rect::MakeWH(128, 128));
        context->flushAndSubmit(true); // Closes both destinations' pending compositors.
        require(ancestor->getCanvas()->hasError(), "Offscreen child failure did not reach its destination");
        require(!unrelated->getCanvas()->hasError(), "Closing another compositor contaminated this surface");
        require(canvas->getSaveCount() == 0, "Failing child layer leaked parent saves");
        pixel(readback(unrelated), 8, 8, {0, 255, 0, 255});
    }});
    checks.push_back({"render-error-immutable-picture-replay-does-not-retain-destination-status",
                      [](tgfx::Context* context) {
        tgfx::PictureRecorder recorder;
        auto* recording = recorder.beginRecording();
        recording->drawDeferred(tgfx::Rect::MakeWH(16, 16), 0, [](tgfx::Canvas* canvas) {
            solid(Color::green()).drawRect(canvas, tgfx::Rect::MakeWH(16, 16));
            if (canvas->getMatrix().getTranslateX() > 32) {
                canvas->save(); // Only the translated replay violates the callback contract.
            }
        });
        auto picture = recorder.finishRecordingAsPicture();
        require(picture != nullptr, "Deferred picture recording failed");
        auto first = makeSurface(context, kSize, kSize);
        auto second = makeSurface(context, kSize, kSize);
        first->getCanvas()->translate(48, 0);
        first->getCanvas()->drawPicture(picture);
        require(first->getCanvas()->hasError(), "Translated picture replay did not signal its failure");
        second->getCanvas()->drawPicture(picture);
        require(!second->getCanvas()->hasError(), "Picture carried the first destination's failure");
        pixel(readback(second), 8, 8, {0, 255, 0, 255});
        pixel(readback(second), 56, 8, {0, 0, 0, 0});
    }});
    checks.push_back({"render-error-sticky-status-survives-serialized-device-thread-transfer",
                      [](tgfx::Context*) {
        // Device::lockContext explicitly supports serialized ownership across
        // threads. The harness releases its lock for this case: makeOffscreen
        // may resolve the SAME registered native Metal device.
        auto graphics = GrGraphicsContext::makeOffscreen();
        require(graphics != nullptr, "Thread-transfer check could not create its GPU device");
        std::shared_ptr<tgfx::Surface> failed;
        std::shared_ptr<tgfx::Surface> healthy;
        {
            auto lock = graphics->lockContext();
            require(lock != nullptr && static_cast<bool>(*lock), "Initial device lock failed");
            try {
                failed = makeSurface(lock->get(), kSize, kSize);
                healthy = makeSurface(lock->get(), kSize, kSize);
                failed->getCanvas()->drawDeferred(tgfx::Rect::MakeWH(8, 8), 0, [](tgfx::Canvas* canvas) {
                    canvas->save();
                });
                solid(Color::blue()).drawRect(healthy->getCanvas(), tgfx::Rect::MakeWH(128, 128));
                lock->get()->flushAndSubmit(true);
                require(failed->getCanvas()->hasError(), "Initial thread lost its destination failure");
                require(!healthy->getCanvas()->hasError(), "Initial thread contaminated its healthy destination");
            } catch (...) {
                failed.reset();
                healthy.reset();
                throw;
            }
        }
        std::exception_ptr failure;
        std::thread next([&] {
            try {
                auto lock = graphics->lockContext();
                require(lock != nullptr && static_cast<bool>(*lock), "Transferred device lock failed");
                try {
                    require(failed->getCanvas()->hasError(), "Transferred failed surface lost sticky status");
                    require(!healthy->getCanvas()->hasError(), "Transferred healthy surface acquired a failure");
                    pixel(readback(healthy), 8, 8, {0, 0, 255, 255});
                    auto fresh = makeSurface(lock->get(), kSize, kSize);
                    require(!fresh->getCanvas()->hasError(), "Fresh surface inherited transferred failure");
                    solid(Color::green()).drawRect(fresh->getCanvas(), tgfx::Rect::MakeWH(128, 128));
                    pixel(readback(fresh), 8, 8, {0, 255, 0, 255});
                } catch (...) {
                    failure = std::current_exception();
                }
                failed.reset();
                healthy.reset(); // Release GPU resources while this thread owns the lock.
            } catch (...) {
                failure = std::current_exception();
            }
        });
        next.join();
        // Also cover lock-acquisition failure without destroying GPU resources
        // on an unlocked thread.
        if (failed || healthy) {
            auto lock = graphics->lockContext();
            require(lock != nullptr && static_cast<bool>(*lock), "Cleanup device lock failed");
            failed.reset();
            healthy.reset();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }, true});
}

void appendResult(EffectsCheckReport& report, const std::string& name, bool passed, const std::string& details) {
    report.checks.push_back({name, passed, details});
    if (passed) {
        ++report.passed;
    } else {
        ++report.failed;
    }
    report.details += std::string(passed ? "PASS " : "FAIL ") + name;
    if (!details.empty()) {
        report.details += ": " + details;
    }
    report.details += "\n";
}
} // namespace

EffectsCheckReport runEffectsChecks() {
    std::vector<Check> checks;
    addGradientChecks(checks);
    addShaderChecks(checks);
    addBlendChecks(checks);
    addGeometryEffectChecks(checks);
    addDotGovernorChecks(checks);
    addBlurChecks(checks);
    addCompositionChecks(checks);
    addDestinationChecks(checks);

    EffectsCheckReport report;
    Ref<GrGraphicsContext> graphics;
    std::unique_ptr<TGFXContextLock> lock;
    std::string setupFailure;
    try {
        graphics = GrGraphicsContext::makeOffscreen();
        require(graphics != nullptr, "GrGraphicsContext::makeOffscreen returned no GPU device");
        lock = graphics->lockContext();
        require(lock != nullptr && static_cast<bool>(*lock), "TGFXContextLock could not acquire its context");
        auto frame = render(lock->get(), rectangle(Color::makeARGB(128, 255, 0, 0),
                                                  Rect::makeXYWH(16, 16, 32, 32)));
        pixel(frame, 24, 24, {128, 0, 0, 128}, 1);
        pixel(frame, 8, 8, {0, 0, 0, 0});
        appendResult(report, "backend-offscreen-lock-rgba8-readback", true, "");
    } catch (const std::exception& error) {
        setupFailure = error.what();
        appendResult(report, "backend-offscreen-lock-rgba8-readback", false, setupFailure);
    } catch (...) {
        setupFailure = "Unknown offscreen setup/readback exception";
        appendResult(report, "backend-offscreen-lock-rgba8-readback", false, setupFailure);
    }
    for (const auto& check : checks) {
        if (!setupFailure.empty()) {
            appendResult(report, check.name, false, "Not executed because backend prerequisite failed: " + setupFailure);
            continue;
        }
        try {
            if (check.releasesHarnessLock) {
                lock.reset();
            } else if (!lock) {
                lock = graphics->lockContext();
                require(lock != nullptr && static_cast<bool>(*lock), "Could not reacquire harness device lock");
            }
            check.body(check.releasesHarnessLock ? nullptr : lock->get());
            appendResult(report, check.name, true, "");
        } catch (const std::exception& error) {
            appendResult(report, check.name, false, error.what());
        } catch (...) {
            appendResult(report, check.name, false, "Unknown exception");
        }
    }
    report.success = report.failed == 0 && report.passed == static_cast<int>(checks.size()) + 1;
    report.details += "TOTAL " + std::to_string(report.passed + report.failed) + ", PASS " +
                      std::to_string(report.passed) + ", FAIL " + std::to_string(report.failed) + "\n";
    return report;
}
