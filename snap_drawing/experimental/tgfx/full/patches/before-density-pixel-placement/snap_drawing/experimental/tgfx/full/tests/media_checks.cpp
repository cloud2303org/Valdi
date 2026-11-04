#include "media_checks.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "media_checks requires the real complete TGFX production implementation"
#endif

#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "snap_drawing/cpp/Text/FontManager.hpp"
#include "snap_drawing/cpp/Text/Typeface.hpp"
#include "snap_drawing/cpp/Utils/AnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/Bitmap.hpp"
#include "snap_drawing/cpp/Utils/BitmapFactory.hpp"
#include "snap_drawing/cpp/Utils/Color.hpp"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/LottieAnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/SVGAnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/SVGUtils.hpp"
#include "snap_drawing/cpp/Utils/SkCodecAnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/ThorvgMedia.hpp"
#include "snap_drawing/cpp/Utils/TgfxMedia.hpp"
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"
#include "tgfx/core/Font.h"
#include "tgfx/core/Paint.h"
#include "tgfx/core/ImageCodec.h"
#include "tgfx/core/Pixmap.h"
#include "tgfx/core/PictureRecorder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

using namespace snap::drawing;

namespace {
using RGBA = std::array<int, 4>;
constexpr RGBA red{255, 0, 0, 255};
constexpr RGBA green{0, 255, 0, 255};
constexpr RGBA blue{0, 0, 255, 255};
constexpr RGBA white{255, 255, 255, 255};
constexpr RGBA clear{0, 0, 0, 0};

struct Check {
    bool passed = true;
    std::string details;

    bool expect(bool condition, const std::string& message) {
        if (!condition) {
            passed = false;
            if (!details.empty()) { details += "; "; }
            details += message;
        }
        return condition;
    }

    template<typename T>
    bool result(const Valdi::Result<T>& value, const char* operation) {
        return expect(static_cast<bool>(value),
                      value ? operation : std::string(operation) + ": " + value.error().toString());
    }
};

template<typename F>
void run(MediaTestReport& report, const std::string& name, F&& body) {
    Check check;
    body(check);
    report.checks.push_back({name, check.passed, check.details});
    if (check.passed) { ++report.passed; } else { ++report.failed; }
    report.details += std::string(check.passed ? "PASS " : "FAIL ") + name;
    if (!check.details.empty()) { report.details += ": " + check.details; }
    report.details += "\n";
}

Valdi::BytesView fixture(Check& check, const std::string& directory, const char* name) {
    const auto path = directory + "/" + name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!check.expect(stream.is_open(), "Cannot open required fixture: " + path)) { return {}; }
    const auto length = stream.tellg();
    if (!check.expect(length > 0 && length <= 4 * 1024 * 1024, "Invalid fixture length: " + path)) { return {}; }
    auto bytes = Valdi::makeShared<Valdi::Bytes>();
    bytes->resize(static_cast<size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(length));
    if (!check.expect(static_cast<bool>(stream), "Cannot read required fixture: " + path)) { return {}; }
    return Valdi::BytesView(bytes);
}

void pixel(Check& check, const std::vector<uint8_t>& pixels, int width, int x, int y,
           const RGBA& expected, int tolerance = 0) {
    const auto offset = (static_cast<size_t>(y) * width + x) * 4;
    if (!check.expect(offset + 4 <= pixels.size(), "Pixel assertion outside readback")) { return; }
    for (size_t c = 0; c < 4; ++c) {
        if (std::abs(static_cast<int>(pixels[offset + c]) - expected[c]) > tolerance) {
            check.expect(false, "Pixel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " +
                         std::to_string(c) + ": got " + std::to_string(pixels[offset + c]) +
                         ", expected " + std::to_string(expected[c]) + " +/- " + std::to_string(tolerance));
        }
    }
}

void number(Check& check, const Valdi::Value& metadata, const char* key, double expected,
            double tolerance = 0) {
    const auto value = metadata.getMapValue(key);
    check.expect(value.isNumber() && std::abs(value.toDouble() - expected) <= tolerance,
                 std::string("Metadata ") + key + ": got " + value.toString() +
                 ", expected " + std::to_string(expected));
}

void type(Check& check, const Valdi::Value& metadata, const char* expected) {
    const auto value = metadata.getMapValue("type");
    check.expect((value.isString() || value.isInternedString() || value.isStaticString()) &&
                     value.toString() == expected, "Unexpected metadata type: " + value.toString());
}

std::vector<uint8_t> bitmapPixels(Check& check, const Ref<Image>& image, int width, int height) {
    const Valdi::BitmapInfo info(width, height, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypeUnpremul,
                                 static_cast<size_t>(width) * 4);
    auto bitmap = image->toConvertedBitmap(info);
    if (!check.expect(bitmap != nullptr, "Production toConvertedBitmap returned null")) { return {}; }
    check.expect(bitmap->getInfo() == info, "Converted bitmap info/stride changed");
    auto* bytes = static_cast<const uint8_t*>(bitmap->lockBytes());
    if (!check.expect(bytes != nullptr, "Cannot lock actual converted bitmap")) { return {}; }
    std::vector<uint8_t> result(bytes, bytes + info.rowBytes * height);
    bitmap->unlockBytes();
    return result;
}

template<typename Draw>
std::vector<uint8_t> gpuPixels(Check& check, int width, int height, Draw&& draw) {
    auto context = GrGraphicsContext::makeOffscreen();
    if (!check.expect(context != nullptr, "Cannot create the real platform offscreen context")) { return {}; }
    auto lock = context->lockContext();
    if (!check.expect(static_cast<bool>(*lock), "Cannot lock the real TGFX device")) { return {}; }
    auto surface = tgfx::Surface::Make(lock->get(), width, height);
    if (!check.expect(surface != nullptr, "Cannot create the real TGFX surface")) { return {}; }
    surface->getCanvas()->clear();
    draw(surface->getCanvas());
    auto info = tgfx::ImageInfo::Make(width, height, tgfx::ColorType::RGBA_8888,
                                     tgfx::AlphaType::Premultiplied);
    std::vector<uint8_t> pixels(info.byteSize());
    if (!check.expect(surface->readPixels(info, pixels.data()), "Real TGFX surface readback failed")) { return {}; }
    return pixels;
}

std::vector<uint8_t> animationPixels(Check& check, const Ref<AnimatedImage>& animation,
                                     int width, int height, double seconds,
                                     FittingSizeMode mode = FittingSizeModeFill) {
    return gpuPixels(check, width, height, [&](NativeCanvas* canvas) {
        animation->draw(canvas, Rect::makeXYWH(0, 0, width, height), Duration::fromSeconds(seconds), mode);
    });
}

Ref<Image> loadImage(Check& check, const std::string& directory, const char* name) {
    auto bytes = fixture(check, directory, name);
    if (!check.passed) { return nullptr; }
    auto image = Image::make(bytes);
    if (!check.result(image, "Production Image::make")) { return nullptr; }
    return image.moveValue();
}

Ref<AnimatedImage> loadAnimation(Check& check, const std::string& directory, const char* name) {
    auto bytes = fixture(check, directory, name);
    if (!check.passed) { return nullptr; }
    auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
    if (!check.result(animation, "Production AnimatedImage::make")) { return nullptr; }
    // The factory accepts a raw span. It must finish copying/decoding before
    // return; all subsequent draws intentionally outlive this input owner.
    return animation.moveValue();
}

void staticChecks(MediaTestReport& report, const std::string& directory) {
    for (const char* name : {"pattern.png", "pattern.webp", "solid.jpg"}) {
        run(report, std::string("image.decode-pixels-metadata.") + name, [&](Check& check) {
            auto image = loadImage(check, directory, name);
            if (image == nullptr) { return; }
            const bool jpeg = std::string(name) == "solid.jpg";
            const int size = jpeg ? 16 : 2;
            check.expect(image->width() == size && image->height() == size, "Decoded dimensions changed");
            type(check, image->getMetadata(), "image");
            number(check, image->getMetadata(), "width", size);
            number(check, image->getMetadata(), "height", size);
            auto pixels = bitmapPixels(check, image, size, size);
            if (jpeg) {
                pixel(check, pixels, size, 8, 8, RGBA{200, 40, 80, 255}, 3);
            } else {
                pixel(check, pixels, 2, 0, 0, red);
                pixel(check, pixels, 2, 1, 0, green);
                pixel(check, pixels, 2, 0, 1, blue);
                pixel(check, pixels, 2, 1, 1,
                      std::string(name) == "pattern.webp" ? RGBA{255, 255, 255, 128} : white, 1);
            }
        });
    }
    for (const auto format : {EncodedImageFormatPNG, EncodedImageFormatJPG, EncodedImageFormatWebP}) {
        const auto name = format == EncodedImageFormatPNG ? "png" : format == EncodedImageFormatJPG ? "jpeg" : "webp";
        run(report, std::string("image.actual-encode-roundtrip.") + name, [&](Check& check) {
            auto image = loadImage(check, directory, "solid.jpg");
            if (image == nullptr) { return; }
            auto encoded = image->encode(format, 1.0);
            if (!check.result(encoded, "Production Image::encode")) { return; }
            auto bytes = encoded.moveValue();
            check.expect(bytes.size() > 12 && bytes.getSource() != nullptr, "Encoded output has no owned bytes");
            auto decoded = Image::make(bytes);
            if (!check.result(decoded, "Production encoded roundtrip")) { return; }
            auto pixels = bitmapPixels(check, decoded.value(), 16, 16);
            pixel(check, pixels, 16, 8, 8, RGBA{200, 40, 80, 255}, format == EncodedImageFormatJPG ? 5 : 3);
        });
    }
    run(report, "image.actual-resize-and-gpu-draw", [&](Check& check) {
        auto image = loadImage(check, directory, "solid.jpg");
        if (image == nullptr) { return; }
        auto resized = image->resized(8, 4);
        if (!check.expect(resized != nullptr, "Production resize failed")) { return; }
        check.expect(resized->width() == 8 && resized->height() == 4, "Resized dimensions changed");
        auto pixels = gpuPixels(check, 8, 4, [&](NativeCanvas* canvas) {
            DrawingContext drawing(8, 4);
            drawing.drawImage(*resized, Rect::makeXYWH(0, 0, 8, 4), Rect::makeXYWH(0, 0, 8, 4), nullptr);
            auto content = drawing.finish();
            if (check.expect(content.picture != nullptr, "Production DrawingContext did not record the image")) {
                canvas->drawPicture(content.picture);
            }
        });
        pixel(check, pixels, 8, 4, 2, RGBA{200, 40, 80, 255}, 3);
    });
    run(report, "image.encoded-bytes-outlive-image-and-worker", [&](Check& check) {
        auto image = loadImage(check, directory, "pattern.png");
        if (image == nullptr) { return; }
        auto encoded = image->toPNG();
        if (!check.result(encoded, "Production Image::toPNG")) { return; }
        auto bytes = encoded.moveValue();
        std::thread release([image = std::move(image)]() mutable { image = nullptr; });
        release.join();
        auto restored = Image::make(bytes);
        if (!check.result(restored, "Read retained encoded bytes after image release")) { return; }
        pixel(check, bitmapPixels(check, restored.value(), 2, 2), 2, 0, 0, red);
    });
    run(report, "surface.production-snapshot-outlives-surface-and-context", [&](Check& check) {
        auto image = loadImage(check, directory, "pattern.png");
        if (image == nullptr) { return; }
        Ref<Image> snapshot;
        {
            auto context = GrGraphicsContext::makeOffscreen();
            if (!check.expect(context != nullptr, "Cannot create actual snapshot device")) { return; }
            auto lock = context->lockContext();
            if (!check.expect(static_cast<bool>(*lock), "Cannot lock snapshot device")) { return; }
            auto surface = tgfx::Surface::Make(lock->get(), 2, 2);
            if (!check.expect(surface != nullptr, "Cannot allocate snapshot surface")) { return; }
            surface->getCanvas()->clear();
            surface->getCanvas()->drawImage(image->getSkValue());
            DrawableSurfaceCanvas canvas(surface.get(), 2, 2);
            auto result = canvas.snapshot();
            if (!check.result(result, "Production DrawableSurfaceCanvas::snapshot")) { return; }
            snapshot = result.moveValue();
        }
        image = nullptr;
        auto encoded = snapshot->toPNG();
        if (!check.result(encoded, "Production readback after original device/context destruction")) { return; }
        snapshot = nullptr;
        auto restored = Image::make(encoded.value());
        if (!check.result(restored, "Decode retained production snapshot output")) { return; }
        pixel(check, bitmapPixels(check, restored.value(), 2, 2), 2, 0, 0, red);
        pixel(check, bitmapPixels(check, restored.value(), 2, 2), 2, 1, 1, white);
    });
    run(report, "surface.texture-readback-owning-device-and-borrowed-context", [&](Check& check) {
        auto source = loadImage(check, directory, "pattern.png");
        if (source == nullptr) { return; }
        Ref<Image> retained;
        {
            auto context = GrGraphicsContext::makeOffscreen();
            if (!check.expect(context != nullptr, "Cannot create actual texture device")) { return; }
            auto lock = context->lockContext();
            if (!check.expect(static_cast<bool>(*lock), "Cannot lock texture device")) { return; }
            auto surface = tgfx::Surface::Make(lock->get(), 2, 2);
            if (!check.expect(surface != nullptr, "Cannot allocate texture surface")) { return; }
            surface->getCanvas()->clear();
            surface->getCanvas()->drawImage(source->getSkValue());
            auto texture = surface->makeImageSnapshot();
            if (!check.expect(texture != nullptr && texture->isTextureBacked(), "No real texture-backed snapshot")) {
                return;
            }
            auto encoded = Image::encodeSKImageToSKData(*texture, EncodedImageFormatPNG, 100, lock->get());
            check.expect(encoded != nullptr && encoded->size() > 12, "Borrowed-context production encoding failed");
            retained = Valdi::makeShared<Image>(texture, context->getDevice());
        }
        auto encoded = retained->toPNG();
        if (!check.result(encoded, "Owning-device production encoding after source surface release")) { return; }
        retained = nullptr;
        auto restored = Image::make(encoded.value());
        if (!check.result(restored, "Decode retained texture output")) { return; }
        pixel(check, bitmapPixels(check, restored.value(), 2, 2), 2, 0, 1, blue);
    });
}

void codecAnimationChecks(MediaTestReport& report, const std::string& directory) {
    for (const char* filename : {"disposal.gif", "disposal.apng", "disposal.webp"}) {
        run(report, std::string("animation.metadata-timing-disposal.") + filename, [&](Check& check) {
            auto animation = loadAnimation(check, directory, filename);
            if (animation == nullptr) { return; }
            const bool webp = std::string(filename) == "disposal.webp";
            const bool gif = std::string(filename) == "disposal.gif";
            check.expect(animation->getSize() == Size(4, 4), "Animation logical canvas changed");
            check.expect(std::abs(animation->getDuration().seconds() - 1) < 0.000001, "Encoded delays do not total 1s");
            check.expect(std::abs(animation->getFrameRate() - 4) < 0.000001, "Frame rate is not frame count/duration");
            const auto metadata = animation->getMetadata();
            type(check, metadata, "skcodec"); // Existing public metadata spelling, not a Skia decoder.
            number(check, metadata, "width", 4);
            number(check, metadata, "height", 4);
            number(check, metadata, "numberOfFrames", 4);
            number(check, metadata, "durationMs", 1000);
            for (const auto time : {0.0, 0.099}) {
                auto pixels = animationPixels(check, animation, 4, 4, time);
                pixel(check, pixels, 4, 0, 0, red);
                pixel(check, pixels, 4, 3, 3, red);
            }
            for (const auto time : {0.100, 0.299}) {
                auto pixels = animationPixels(check, animation, 4, 4, time);
                pixel(check, pixels, 4, 0, 0, green);
                pixel(check, pixels, 4, 3, 3, red);
            }
            for (const auto time : {0.300, 0.599}) {
                auto pixels = animationPixels(check, animation, 4, 4, time);
                pixel(check, pixels, 4, 0, 0, clear);
                pixel(check, pixels, 4, 3, 3, blue);
                pixel(check, pixels, 4, 3, 0, red);
            }
            for (const auto time : {0.600, 0.999, 1.0}) {
                auto pixels = animationPixels(check, animation, 4, 4, time);
                pixel(check, pixels, 4, 3, 3, webp ? blue : red);
                pixel(check, pixels, 4, 0, 0, webp ? RGBA{128, 128, 128, 128} : clear, 1);
                pixel(check, pixels, 4, 3, 0, webp ? red : gif ? white : RGBA{255, 128, 128, 255}, 1);
            }
            animationPixels(check, animation, 4, 4, -10);
            check.expect(animation->getCurrentTime() == Duration(), "Negative seek did not clamp to zero");
            animationPixels(check, animation, 4, 4, 100);
            check.expect(animation->getCurrentTime() == animation->getDuration(), "Late seek did not clamp to duration");
        });
    }
    run(report, "animation.direct-codec-factory-and-still-image", [&](Check& check) {
        auto bytes = fixture(check, directory, "disposal.gif");
        if (!check.passed) { return; }
        auto direct = SkCodecAnimatedImage::make(bytes);
        if (!check.result(direct, "Production SkCodecAnimatedImage::make")) { return; }
        number(check, direct.value()->getMetadata(), "numberOfFrames", 4);
        auto still = Image::make(bytes);
        if (!check.result(still, "Production still GIF Image::make")) { return; }
        pixel(check, bitmapPixels(check, still.value(), 4, 4), 4, 3, 3, red);
    });
    run(report, "animation.center-fit-letterboxing", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "disposal.gif");
        if (animation == nullptr) { return; }
        auto pixels = animationPixels(check, animation, 12, 4, 0.15, FittingSizeModeCenterScaleFit);
        pixel(check, pixels, 12, 0, 1, clear);
        pixel(check, pixels, 12, 11, 1, clear);
        pixel(check, pixels, 12, 4, 0, green);
        pixel(check, pixels, 12, 7, 3, red);
    });
    run(report, "animation.gif-interlace-row-order", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "interlaced.gif");
        if (animation == nullptr) { return; }
        auto pixels = animationPixels(check, animation, 4, 4, 0);
        pixel(check, pixels, 4, 1, 0, red);
        pixel(check, pixels, 4, 1, 1, green);
        pixel(check, pixels, 4, 1, 2, blue);
        pixel(check, pixels, 4, 1, 3, white);
    });
    run(report, "animation.apng-poster-is-not-a-frame", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "poster.apng");
        if (animation == nullptr) { return; }
        number(check, animation->getMetadata(), "numberOfFrames", 2);
        number(check, animation->getMetadata(), "durationMs", 300);
        auto first = animationPixels(check, animation, 4, 4, 0);
        pixel(check, first, 4, 0, 0, blue);
        pixel(check, first, 4, 3, 3, clear);
        auto second = animationPixels(check, animation, 4, 4, 0.15);
        pixel(check, second, 4, 0, 0, blue);
        pixel(check, second, 4, 3, 0, red);
        pixel(check, second, 4, 3, 3, clear);
        auto still = loadImage(check, directory, "poster.apng");
        if (still == nullptr) { return; }
        pixel(check, bitmapPixels(check, still, 4, 4), 4, 3, 3, RGBA{255, 255, 0, 255});
    });
}

// This tracker is only an input byte owner. All bitmap allocation, codecs,
// conversion, rendering, encoding, and release callbacks are production code.
class TrackedOwner final : public Valdi::SimpleRefCountable {
public:
    TrackedOwner(std::atomic<int>& released, std::vector<uint8_t> data)
        : bytes(std::move(data)), _released(released) {}
    ~TrackedOwner() override { _released.fetch_add(1); }
    std::vector<uint8_t> bytes;
private:
    std::atomic<int>& _released;
};

void ownershipChecks(MediaTestReport& report) {
    run(report, "ownership.no-copy-valdi-owner-last-native-release", [&](Check& check) {
        std::atomic<int> released{0};
        auto owner = Valdi::makeShared<TrackedOwner>(released, std::vector<uint8_t>{255, 0, 0, 255});
        Valdi::BytesView bytes(owner, owner->bytes.data(), owner->bytes.size());
        const Valdi::BitmapInfo info(1, 1, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 4);
        auto result = Image::makeFromPixelsData(info, bytes, false);
        if (!check.result(result, "Production no-copy raw image")) { return; }
        auto image = result.moveValue();
        NativeImage native = image->getSkValue();
        bytes = {};
        owner = nullptr;
        image = nullptr;
        check.expect(released.load() == 0, "No-copy Valdi owner released while native image remains alive");
        std::thread release([native = std::move(native)]() mutable { native.reset(); });
        release.join();
        check.expect(released.load() == 1, "Last native image release did not release its actual Valdi data owner");
    });
    run(report, "ownership.copy-is-independent-of-source", [&](Check& check) {
        std::array<uint8_t, 4> source{255, 0, 0, 255};
        const Valdi::BitmapInfo info(1, 1, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 4);
        auto result = Image::makeFromPixelsData(info, Valdi::BytesView(nullptr, source.data(), source.size()), true);
        if (!check.result(result, "Production copy raw image")) { return; }
        source.fill(0);
        pixel(check, bitmapPixels(check, result.value(), 1, 1), 1, 0, 0, red);
    });
    run(report, "ownership.actual-bitmap-dispose-with-no-copy-image", [&](Check& check) {
        auto result = BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888)->createBitmap(1, 1);
        if (!check.result(result, "Production BitmapFactory::createBitmap")) { return; }
        auto bitmap = result.moveValue();
        auto* pixels = static_cast<uint8_t*>(bitmap->lockBytes());
        if (!check.expect(pixels != nullptr, "Cannot lock production bitmap")) { return; }
        const std::array<uint8_t, 4> expected{255, 0, 0, 255};
        std::memcpy(pixels, expected.data(), 4);
        bitmap->unlockBytes();
        auto image = Image::makeFromBitmap(bitmap, false);
        if (!check.result(image, "Production no-copy Image::makeFromBitmap")) { return; }
        bitmap->dispose();
        auto* afterDispose = bitmap->lockBytes();
        check.expect(afterDispose == nullptr, "Disposed bitmap still exposes a new lock");
        if (afterDispose) { bitmap->unlockBytes(); }
        bitmap = nullptr;
        pixel(check, bitmapPixels(check, image.value(), 1, 1), 1, 0, 0, red);
    });
    run(report, "bitmap.padded-stride-and-draw-canaries", [&](Check& check) {
        auto source = Valdi::makeShared<Valdi::Bytes>();
        source->assign(32, 0xa5);
        const std::array<uint8_t, 8> row0{255, 0, 0, 255, 0, 255, 0, 255};
        const std::array<uint8_t, 8> row1{0, 0, 255, 255, 255, 255, 255, 255};
        std::memcpy(source->data(), row0.data(), 8);
        std::memcpy(source->data() + 16, row1.data(), 8);
        const Valdi::BitmapInfo input(2, 2, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 16);
        auto made = Image::makeFromPixelsData(input, Valdi::BytesView(source), false);
        if (!check.result(made, "Production padded raw image")) { return; }
        auto image = made.moveValue();
        std::vector<uint8_t> output(8 + 48 + 8, 0xcd);
        const Valdi::BitmapInfo destination(2, 2, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 24);
        image->draw(destination, output.data() + 8);
        check.expect(std::memcmp(output.data() + 8, row0.data(), 8) == 0, "Padded row 0 pixels changed");
        check.expect(std::memcmp(output.data() + 32, row1.data(), 8) == 0, "Padded row 1 pixels changed");
        for (size_t i = 0; i < output.size(); ++i) {
            const bool writtenPixel = (i >= 8 && i < 16) || (i >= 32 && i < 40);
            if (!writtenPixel && output[i] != 0xcd) {
                check.expect(false, "Draw wrote row padding or an outer canary at byte " + std::to_string(i));
                break;
            }
        }
        auto converted = image->toConvertedBitmap(destination);
        if (!check.expect(converted != nullptr, "Padded production bitmap conversion failed")) { return; }
        check.expect(converted->getInfo().rowBytes == 24, "Converted stride was not preserved");
        auto roundtrip = Image::makeFromBitmap(converted, true);
        if (!check.result(roundtrip, "Padded bitmap roundtrip")) { return; }
        auto compact = roundtrip.value()->getBitmap();
        if (!check.expect(compact != nullptr, "Compact roundtrip bitmap failed")) { return; }
        check.expect(compact->getInfo().rowBytes == 8, "getBitmap did not compact the source stride");
        check.expect((*source)[8] == 0xa5 && (*source)[24] == 0xa5, "Input padding was mutated");
    });
    run(report, "bitmap.f32-raw-copy-is-bit-exact", [&](Check& check) {
        // 1/3, 1+epsilon, negative zero and an HDR value expose accidental F16
        // normalization in the raw-copy boundary. This is not an HDR GPU claim.
        const std::array<uint32_t, 8> bits{0x3eaaaaab, 0x3f800001, 0x80000000, 0x3f800000,
                                          0x3fa00000, 0x3e000000, 0x3f000001, 0x3f800000};
        const Valdi::BitmapInfo info(2, 1, Valdi::ColorTypeRGBAF32, Valdi::AlphaTypePremul, 32);
        auto image = Image::makeFromPixelsData(
            info, Valdi::BytesView(nullptr, reinterpret_cast<const uint8_t*>(bits.data()), sizeof(bits)), true);
        if (!check.result(image, "Production F32 raw image")) { return; }
        auto bitmap = image.value()->getBitmap();
        if (!check.expect(bitmap != nullptr && bitmap->getInfo() == info, "F32 raw-copy info changed")) { return; }
        auto* pixels = bitmap->lockBytes();
        if (!check.expect(pixels != nullptr, "Cannot lock F32 output")) { return; }
        check.expect(std::memcmp(pixels, bits.data(), sizeof(bits)) == 0, "F32 raw-copy bits changed");
        bitmap->unlockBytes();
        std::array<uint8_t, 64> padded;
        padded.fill(0xcc);
        image.value()->draw(Valdi::BitmapInfo(2, 1, Valdi::ColorTypeRGBAF32, Valdi::AlphaTypePremul, 48),
                            padded.data() + 8);
        check.expect(std::memcmp(padded.data() + 8, bits.data(), sizeof(bits)) == 0, "F32 padded draw changed raw bits");
        check.expect(std::all_of(padded.begin(), padded.begin() + 8, [](uint8_t x) { return x == 0xcc; }) &&
                     std::all_of(padded.begin() + 40, padded.end(), [](uint8_t x) { return x == 0xcc; }),
                     "F32 raw draw overwrote canaries or padding");
    });
}

void vectorMediaChecks(MediaTestReport& report, const std::string& directory) {
    run(report, "lottie.actual-parser-two-frame-pixels-metadata", [&](Check& check) {
        auto input = fixture(check, directory, "moving-solid.json");
        if (!check.passed) { return; }
        auto result = LottieAnimatedImage::make(Ref<IFontManager>(), input.data(), input.size());
        if (!check.result(result, "Production LottieAnimatedImage::make")) { return; }
        Ref<AnimatedImage> animation = result.moveValue();
        input = {}; // ThorVG must not keep this caller's raw span.
        type(check, animation->getMetadata(), "lottie");
        number(check, animation->getMetadata(), "width", 32);
        number(check, animation->getMetadata(), "height", 16);
        number(check, animation->getMetadata(), "durationMs", 2000);
        check.expect(std::abs(animation->getDuration().seconds() - 2) < 0.001, "Lottie duration changed");
        check.expect(std::abs(animation->getFrameRate() - 10) < 0.001, "Lottie frame rate changed");
        auto first = animationPixels(check, animation, 32, 16, 0.25);
        pixel(check, first, 32, 8, 8, red, 1);
        pixel(check, first, 32, 24, 8, clear, 1);
        check.expect(std::abs(animation->getCurrentTime().seconds() - 0.25) < 0.000001, "Lottie current time changed");
        auto second = animationPixels(check, animation, 32, 16, 1.25);
        pixel(check, second, 32, 8, 8, clear, 1);
        pixel(check, second, 32, 24, 8, red, 1);
        check.expect(first != second, "Two actual Lottie times produced identical output");
        check.expect(std::abs(animation->getCurrentTime().seconds() - 1.25) < 0.000001, "Lottie seek did not advance");
        animationPixels(check, animation, 32, 16, -1);
        check.expect(animation->getCurrentTime() == Duration(), "Lottie negative seek did not clamp");
        animationPixels(check, animation, 32, 16, 20);
        check.expect(animation->getCurrentTime() == animation->getDuration(), "Lottie end seek did not clamp");
    });
    run(report, "lottie.animated-image-factory-routes-json-and-fit", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "moving-solid.json");
        if (animation == nullptr) { return; }
        type(check, animation->getMetadata(), "lottie");
        auto fit = animationPixels(check, animation, 64, 64, 0.25, FittingSizeModeCenterScaleFit);
        pixel(check, fit, 64, 16, 4, clear, 1);
        pixel(check, fit, 64, 16, 32, red, 1);
        pixel(check, fit, 64, 16, 60, clear, 1);
        auto fill = animationPixels(check, animation, 64, 64, 0.25, FittingSizeModeFill);
        pixel(check, fill, 64, 16, 20, red, 1);
        pixel(check, fill, 64, 48, 20, clear, 1);
    });
    run(report, "svg.production-size-preferred-raster-and-static-image", [&](Check& check) {
        auto bytes = fixture(check, directory, "pattern.svg");
        if (!check.passed) { return; }
        check.expect(Image::isSVG(bytes), "Actual SVG classifier rejected authored SVG");
        auto size = getSVGSize(bytes);
        if (!check.result(size, "Production getSVGSize")) { return; }
        check.expect(size.value() == std::make_pair(16, 8), "SVG intrinsic dimensions changed");
        auto bitmap = rasterizeSVG(bytes, BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888), 32, 0);
        if (!check.result(bitmap, "Production rasterizeSVG")) { return; }
        check.expect(bitmap.value()->getInfo().width == 32 && bitmap.value()->getInfo().height == 16,
                     "SVG preferred-width aspect ratio changed");
        auto image = Image::makeFromSVG(bytes, 32, 0);
        if (!check.result(image, "Production Image::makeFromSVG")) { return; }
        auto pixels = bitmapPixels(check, image.value(), 32, 16);
        pixel(check, pixels, 32, 4, 8, red, 1);
        pixel(check, pixels, 32, 28, 8, blue, 1);
        auto routed = Image::make(bytes);
        if (!check.result(routed, "Production Image::make SVG route")) { return; }
        check.expect(routed.value()->width() == 16 && routed.value()->height() == 8, "Static SVG factory dimensions changed");
    });
    run(report, "svg.actual-animated-api-fit-and-current-time", [&](Check& check) {
        auto bytes = fixture(check, directory, "pattern.svg");
        if (!check.passed) { return; }
        auto direct = SVGAnimatedImage::make(bytes.data(), bytes.size());
        if (!check.result(direct, "Production SVGAnimatedImage::make")) { return; }
        auto routed = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
        if (!check.result(routed, "Production AnimatedImage SVG route")) { return; }
        auto animation = routed.moveValue();
        bytes = {};
        type(check, animation->getMetadata(), "svg");
        number(check, animation->getMetadata(), "width", 16);
        number(check, animation->getMetadata(), "height", 8);
        check.expect(animation->getDuration() == Duration() && animation->getFrameRate() == 0,
                     "Static SVG acquired a synthesized animation timeline");
        auto pixels = animationPixels(check, animation, 32, 32, 0.75, FittingSizeModeCenterScaleFit);
        pixel(check, pixels, 32, 4, 4, clear, 1);
        pixel(check, pixels, 32, 4, 16, red, 1);
        pixel(check, pixels, 32, 28, 16, blue, 1);
        pixel(check, pixels, 32, 4, 28, clear, 1);
        check.expect(std::abs(animation->getCurrentTime().seconds() - 0.75) < 0.000001, "SVG current-time reporting changed");
    });
    run(report, "lottie.actual-custom-font-byte-provider-and-glyphs", [&](Check& check) {
        auto fontData = fixture(check, directory, "NotoSans-Regular.ttf");
        auto json = fixture(check, directory, "custom-font.json");
        if (!check.passed) { return; }
        auto fonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
        fonts->load();
        const auto alias = Valdi::StringBox::fromCString("ValdiMediaFixture");
        fonts->registerTypeface(alias, FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright), false, fontData);
        auto face = fonts->getTypefaceWithName(alias);
        if (!check.result(face, "Actual FontManager custom alias lookup")) { return; }
        check.expect(face.value()->isCustom(), "Custom-font alias fell back to a system typeface");
        auto exported = face.value()->makeFontData();
        check.expect(!exported.empty(), "Actual custom typeface cannot export font bytes");
        auto nativeFace = face.value()->getSkValue();
        if (!check.expect(nativeFace != nullptr, "Custom typeface has no real TGFX value")) { return; }
        auto animation = LottieAnimatedImage::make(Ref<IFontManager>(fonts), json.data(), json.size());
        if (!check.result(animation, "Production Lottie custom-font resolver")) { return; }
        fontData = {};
        json = {};
        Ref<AnimatedImage> text = animation.moveValue();
        auto actual = animationPixels(check, text, 128, 64, 0.25);
        auto reference = gpuPixels(check, 128, 64, [&](NativeCanvas* canvas) {
            tgfx::Font font(nativeFace, 32);
            tgfx::Paint paint;
            paint.setColor(Color::makeARGB(255, 255, 0, 0).getSkValue());
            canvas->drawSimpleText("HgQ", 8, 44, font, paint);
        });
        if (!check.expect(actual.size() == reference.size(), "Custom-font readback size mismatch") || !check.passed) { return; }
        size_t actualInk = 0, referenceInk = 0, interior = 0, matches = 0;
        std::array<int, 4> actualBounds{128, 64, -1, -1}, referenceBounds{128, 64, -1, -1};
        auto opaqueRed = [](const std::vector<uint8_t>& pixels, int x, int y) {
            const auto i = static_cast<size_t>(y * 128 + x) * 4;
            return pixels[i] > 240 && pixels[i + 1] < 8 && pixels[i + 2] < 8 && pixels[i + 3] > 240;
        };
        auto include = [](std::array<int, 4>& bounds, int x, int y) {
            bounds[0] = std::min(bounds[0], x); bounds[1] = std::min(bounds[1], y);
            bounds[2] = std::max(bounds[2], x); bounds[3] = std::max(bounds[3], y);
        };
        for (int y = 1; y < 63; ++y) {
            for (int x = 1; x < 127; ++x) {
                if (opaqueRed(actual, x, y)) { ++actualInk; include(actualBounds, x, y); }
                if (opaqueRed(reference, x, y)) { ++referenceInk; include(referenceBounds, x, y); }
                if (opaqueRed(reference, x, y) && opaqueRed(reference, x - 1, y) &&
                    opaqueRed(reference, x + 1, y) && opaqueRed(reference, x, y - 1) &&
                    opaqueRed(reference, x, y + 1)) {
                    ++interior;
                    if (opaqueRed(actual, x, y)) { ++matches; }
                }
            }
        }
        check.expect(referenceInk > 100 && actualInk > 100 && interior > 20, "Custom-font text rendered no substantial glyph coverage");
        check.expect(referenceInk && actualInk > referenceInk * 0.65 && actualInk < referenceInk * 1.35,
                     "ThorVG/TGFX same-font glyph coverage differs materially");
        for (size_t i = 0; i < 4; ++i) {
            check.expect(std::abs(actualBounds[i] - referenceBounds[i]) <= 3, "Custom-font baseline/bounds differ from actual TGFX typeface");
        }
        check.expect(interior && matches >= interior * 0.80, "Custom-font stable glyph interiors differ from the registered face");
    });
}

void errorChecks(MediaTestReport& report, const std::string& directory) {
    for (const char* name : {"truncated.png", "truncated.jpg", "truncated-still.webp"}) {
        run(report, std::string("errors.static-truncated.") + name, [&](Check& check) {
            auto bytes = fixture(check, directory, name);
            if (!check.passed) { return; }
            auto result = Image::make(bytes);
            // Deferred image creation may succeed after just a header parse.
            // A successful pixel decode of this truncated input must not.
            if (result) {
                check.expect(result.value()->getBitmap() == nullptr, "Truncated static input produced decoded pixels");
            } else {
                check.expect(!result.error().isEmpty(), "Static rejection had no diagnostic");
            }
        });
    }
    for (const char* name : {"not-image.bin", "truncated.gif", "truncated.webp", "truncated.apng", "bad-crc.apng"}) {
        run(report, std::string("errors.animated-malformed.") + name, [&](Check& check) {
            auto bytes = fixture(check, directory, name);
            if (!check.passed) { return; }
            auto result = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
            check.expect(!result, "Actual animated decoder accepted the malformed fixture");
            if (!result) { check.expect(!result.error().isEmpty(), "Animated rejection had no diagnostic"); }
        });
    }
    for (const char* name : {"malformed.json", "malformed.svg"}) {
        run(report, std::string("errors.vector-malformed.") + name, [&](Check& check) {
            auto bytes = fixture(check, directory, name);
            if (!check.passed) { return; }
            auto result = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
            check.expect(!result, "Actual vector parser accepted the incomplete fixture");
            if (!result) { check.expect(!result.error().isEmpty(), "Vector rejection had no diagnostic"); }
        });
    }
    run(report, "errors.static-invalid-empty-and-raw-buffer-boundaries", [&](Check& check) {
        auto invalid = fixture(check, directory, "not-image.bin");
        if (!check.passed) { return; }
        check.expect(!Image::make(invalid), "Actual static decoder accepted a non-image body");
        check.expect(!Image::make(Valdi::BytesView()), "Actual static decoder accepted empty bytes");
        check.expect(!AnimatedImage::make(nullptr, nullptr, 0), "Actual animated decoder accepted null/empty data");
        const std::array<uint8_t, 4> pixelData{255, 0, 0, 255};
        const Valdi::BytesView bytes(nullptr, pixelData.data(), pixelData.size());
        check.expect(!Image::makeFromPixelsData(
            Valdi::BitmapInfo(2, 2, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 8), bytes, true),
            "Actual raw-image API accepted a short source buffer");
        check.expect(!Image::makeFromPixelsData(
            Valdi::BitmapInfo(2, 1, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 4), bytes, false),
            "Actual raw-image API accepted an undersized stride");
        check.expect(!BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888)->createBitmap(-1, 2),
                     "Actual BitmapFactory accepted negative dimensions");
        check.expect(!Bitmap::make(Valdi::BitmapInfo(1, 2, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul,
                                                    std::numeric_limits<size_t>::max())),
                     "Actual Bitmap accepted overflowing allocation stride");
    });
    run(report, "errors.image-encode-invalid-quality", [&](Check& check) {
        auto image = loadImage(check, directory, "pattern.png");
        if (image == nullptr) { return; }
        check.expect(!image->encode(EncodedImageFormatPNG, std::numeric_limits<double>::quiet_NaN()),
                     "Actual encoder accepted non-finite quality");
    });
    run(report, "errors.original-payload-diagnostic-contract", [&](Check& check) {
        auto empty = Image::make(Valdi::BytesView());
        if (check.expect(!empty, "Empty Image input was accepted")) {
            check.expect(empty.error().getMessage() == "Empty payload (bytes=0 magic=)",
                         "Original empty Image payload message changed: " + empty.error().toString());
        }
        auto animated = AnimatedImage::make(nullptr, nullptr, 0);
        if (check.expect(!animated, "Empty AnimatedImage input was accepted")) {
            check.expect(animated.error().getMessage() == "Empty payload (bytes=0 magic=)",
                         "Original empty AnimatedImage payload message changed: " + animated.error().toString());
        }
        const std::array<uint8_t, 4> unknown{0x89, 'n', 'o', 't'};
        auto binary = Image::make(Valdi::BytesView(nullptr, unknown.data(), unknown.size()));
        if (check.expect(!binary, "Unknown binary image was accepted")) {
            check.expect(binary.error().getMessage() == "Unable to decode image (bytes=4 magic=896e6f74)",
                         "Original unknown-binary diagnostic changed: " + binary.error().toString());
        }
        const std::array<uint8_t, 16> pngHeader{137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        auto png = Image::make(Valdi::BytesView(nullptr, pngHeader.data(), pngHeader.size()));
        if (check.expect(!png, "Truncated PNG header was accepted")) {
            check.expect(png.error().getMessage() ==
                             "Unable to decode image (bytes=16 magic=89504e470d0a1a0a0000000d)",
                         "Original truncated-PNG label/length/magic changed: " + png.error().toString());
        }
    });
}

void jpegRecoveryChecks(MediaTestReport& report, const std::string& directory) {
    run(report, "jpeg.fatal-header-error-survives-and-valid-decode-follows", [&](Check& check) {
        // Exact source of backend_checks-run1's process exit: a 20-byte JPEG
        // truncated within the APP0 header, before any image/SOS is available.
        auto malformed = fixture(check, directory, "truncated.jpg");
        if (!check.passed) { return; }
        for (int repeat = 0; repeat < 3; ++repeat) {
            auto decoded = Image::make(malformed);
            check.expect(!decoded, "Fatal malformed JPEG header produced an Image");
        }
        auto valid = loadImage(check, directory, "solid.jpg");
        if (valid == nullptr) { return; }
        pixel(check, bitmapPixels(check, valid, 16, 16), 16, 8, 8, RGBA{200, 40, 80, 255}, 3);
    });
    for (const char* name : {"jpeg-truncated-after-sos.jpg", "jpeg-truncated-during-scanlines.jpg",
                              "jpeg-missing-eoi.jpg", "jpeg-missing-quant-tables.jpg"}) {
        run(report, std::string("jpeg.recover-after-valid-header.") + name, [&](Check& check) {
            auto input = fixture(check, directory, name);
            if (!check.passed) { return; }
            auto codec = tgfx::ImageCodec::MakeFrom(skDataFromBytes(input, DataConversionModeCopyIfUnsafe));
            if (!check.expect(codec != nullptr, "Regression fixture did not reach the valid-header decode boundary")) {
                return;
            }
            auto info = tgfx::ImageInfo::Make(codec->width(), codec->height(), tgfx::ColorType::RGBA_F16);
            std::vector<uint8_t> pixels(info.byteSize(), 0xa5);
            // F16 forces the production JPEG decoder to keep its Bitmap and
            // Pixmap conversion objects alive during scanline/finish calls.
            check.expect(!codec->readPixels(info, pixels.data()), "Incomplete/fatal JPEG decode reported success");
            auto valid = loadImage(check, directory, "solid.jpg");
            if (valid == nullptr) { return; }
            pixel(check, bitmapPixels(check, valid, 16, 16), 16, 8, 8, RGBA{200, 40, 80, 255}, 3);
        });
    }
    run(report, "jpeg.real-encode-error-survives-and-grown-output-remains-valid", [&](Check& check) {
        // 65536 exceeds the JPEG format/libjpeg dimension bound while remaining
        // a valid, small-memory TGFX Pixmap. jpeg_start_compress really raises
        // JERR_IMAGE_TOO_BIG; no mocked allocator or injected callback is used.
        auto oversizedInfo = tgfx::ImageInfo::Make(65536, 1, tgfx::ColorType::Gray_8, tgfx::AlphaType::Opaque);
        std::vector<uint8_t> oversized(oversizedInfo.byteSize(), 127);
        auto rejected = tgfx::ImageCodec::Encode(tgfx::Pixmap(oversizedInfo, oversized.data()),
                                                 tgfx::EncodedFormat::JPEG, 100);
        check.expect(rejected == nullptr, "Real oversized JPEG encode did not return failure");
        auto info = tgfx::ImageInfo::Make(128, 128, tgfx::ColorType::Gray_8, tgfx::AlphaType::Opaque);
        std::vector<uint8_t> pixels(info.byteSize());
        uint32_t state = 0x13275ace;
        for (auto& value : pixels) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            value = static_cast<uint8_t>(state);
        }
        auto encoded = tgfx::ImageCodec::Encode(tgfx::Pixmap(info, pixels.data()), tgfx::EncodedFormat::JPEG, 100);
        if (!check.expect(encoded != nullptr && encoded->size() > 4096,
                          "Valid encode after failure did not exercise the grown destination buffer")) { return; }
        auto decoded = Image::make(bytesFromSkData(encoded));
        if (!check.result(decoded, "Production decode after JPEG encoder recovery")) { return; }
        auto restored = bitmapPixels(check, decoded.value(), 128, 128);
        for (const auto point : {std::make_pair(16, 16), std::make_pair(64, 64), std::make_pair(111, 96)}) {
            const int gray = pixels[static_cast<size_t>(point.second) * 128 + point.first];
            pixel(check, restored, 128, point.first, point.second, RGBA{gray, gray, gray, 255}, 3);
        }
    });
}

void svgCompatibilityChecks(MediaTestReport& report) {
    run(report, "svg.incomplete-root-and-mismatched-elements-are-real-parse-errors", [&](Check& check) {
        const std::string missingClose =
            "<svg width=\"12\" height=\"10\" xmlns=\"http://www.w3.org/2000/svg\">"
            "<rect width=\"12\" height=\"10\" fill=\"#ff0000\"/>";
        auto bytes = Valdi::BytesView(nullptr, reinterpret_cast<const uint8_t*>(missingClose.data()), missingClose.size());
        check.expect(!Image::make(bytes), "Missing closing SVG root was accepted by Image");
        check.expect(!AnimatedImage::make(nullptr, bytes.data(), bytes.size()), "Missing SVG root was accepted by AnimatedImage");
        check.expect(!getSVGSize(bytes), "Missing SVG root was accepted by getSVGSize");
        const std::string mismatch = "<svg width=\"12\" height=\"10\"><g></svg>";
        check.expect(!Image::make(Valdi::BytesView(nullptr, reinterpret_cast<const uint8_t*>(mismatch.data()), mismatch.size())),
                     "Mismatched XML elements were accepted");
        const auto closed = missingClose + "</svg><!-- valid trailing comment with </svg> text -->";
        auto valid = Image::make(Valdi::BytesView(nullptr, reinterpret_cast<const uint8_t*>(closed.data()), closed.size()));
        if (!check.result(valid, "Actual parser must allow a complete SVG with a trailing comment")) { return; }
        pixel(check, bitmapPixels(check, valid.value(), 12, 10), 12, 6, 5, red, 1);
    });
    run(report, "svg.viewbox-only-requires-both-preferred-raster-axes", [&](Check& check) {
        const std::string source =
            "<svg viewBox=\"0 0 100 80\" xmlns=\"http://www.w3.org/2000/svg\">"
            "<rect width=\"100\" height=\"80\" fill=\"#0000ff\"/></svg>";
        auto bytes = Valdi::BytesView(nullptr, reinterpret_cast<const uint8_t*>(source.data()), source.size());
        const auto& factory = BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888);
        auto size = getSVGSize(bytes);
        if (!check.result(size, "Production viewBox-only intrinsic size query")) { return; }
        check.expect(size.value() == std::make_pair(0, 0), "ViewBox geometry became a fabricated intrinsic raster size");
        check.expect(!rasterizeSVG(bytes, factory), "ViewBox-only SVG rasterized without a preferred size");
        check.expect(!rasterizeSVG(bytes, factory, 50, 0), "ViewBox-only SVG fabricated a missing preferred height");
        check.expect(!rasterizeSVG(bytes, factory, 0, 40), "ViewBox-only SVG fabricated a missing preferred width");
        auto bitmap = rasterizeSVG(bytes, factory, 50, 40);
        if (!check.result(bitmap, "Production viewBox-only raster with both preferred dimensions")) { return; }
        check.expect(bitmap.value()->getInfo().width == 50 && bitmap.value()->getInfo().height == 40,
                     "Preferred raster dimensions changed");
        auto image = Image::makeFromBitmap(bitmap.value(), true);
        if (!check.result(image, "Production viewBox raster pixel image")) { return; }
        pixel(check, bitmapPixels(check, image.value(), 50, 40), 50, 25, 20, blue, 1);
    });
}

void mediaCapacityChecks(MediaTestReport& report, const std::string& directory) {
    for (const char* extension : {"gif", "webp", "apng"}) {
        for (const char* limit : {"canvas", "delta-timeline", "frame-count"}) {
            const std::string filename = std::string("capacity-") + limit + "." + extension;
            run(report, "capacity.actual-animation-rejects." + filename, [&](Check& check) {
                auto bytes = fixture(check, directory, filename.c_str());
                if (!check.passed) { return; }
                auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
                if (check.expect(!animation, "Production animated factory accepted an over-capacity timeline")) {
                    check.expect(animation.error().toString().find("Animated media capacity exceeded") != std::string::npos,
                                 "Capacity admission returned a different decoder error: " + animation.error().toString());
                }
                auto codec = SkCodecAnimatedImage::make(bytes);
                if (check.expect(!codec, "Production codec factory accepted an over-capacity timeline")) {
                    check.expect(codec.error().toString().find("Animated media capacity exceeded") != std::string::npos,
                                 "Direct codec did not report an explicit capacity error: " + codec.error().toString());
                }
            });
        }
        const std::string filename = std::string("capacity-small-control.") + extension;
        run(report, "capacity.valid-delta-control-still-decodes." + filename, [&](Check& check) {
            auto animation = loadAnimation(check, directory, filename.c_str());
            if (animation == nullptr) { return; }
            check.expect(animation->getSize() == Size(4, 4), "Small control dimensions changed");
            number(check, animation->getMetadata(), "numberOfFrames", 3);
            number(check, animation->getMetadata(), "durationMs", 300);
            auto pixels = animationPixels(check, animation, 4, 4, 0.250);
            pixel(check, pixels, 4, 0, 0, red);
            pixel(check, pixels, 4, 3, 3, clear);
        });
    }
    run(report, "capacity.apng-cannot-hide-extra-frames-behind-small-declared-count", [&](Check& check) {
        auto bytes = fixture(check, directory, "capacity-extra-frames.apng");
        if (!check.passed) { return; }
        auto animation = SkCodecAnimatedImage::make(bytes);
        if (check.expect(!animation, "APNG accepted additional undeclared frames")) {
            check.expect(animation.error().toString().find("more frames than declared") != std::string::npos,
                         "Extra APNG frames were not rejected at the incremental count guard: " +
                             animation.error().toString());
        }
    });
    run(report, "capacity.large-svg-size-query-and-preferred-raster-stay-small", [&](Check& check) {
        auto bytes = fixture(check, directory, "large-intrinsic.svg");
        if (!check.passed) { return; }
        auto size = getSVGSize(bytes);
        if (!check.result(size, "Large intrinsic SVG size query with bounded real validation render")) { return; }
        check.expect(size.value() == std::make_pair(20000, 20000), "Validation downscaled reported SVG dimensions");
        auto image = Image::makeFromSVG(bytes, 32, 32);
        if (!check.result(image, "Large intrinsic SVG with a real 32x32 preferred raster")) { return; }
        check.expect(image.value()->width() == 32 && image.value()->height() == 32,
                     "Preferred SVG raster dimensions changed");
        pixel(check, bitmapPixels(check, image.value(), 32, 32), 32, 16, 16, red, 1);
        auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
        if (!check.result(animation, "Large intrinsic SVG animated factory with bounded validation")) { return; }
        check.expect(animation.value()->getSize() == Size(20000, 20000), "SVG animation intrinsic dimensions changed");
        auto pixels = animationPixels(check, animation.value(), 32, 32, 0);
        pixel(check, pixels, 32, 16, 16, red, 1);
    });
}

void mediaAllocationAdmissionChecks(MediaTestReport& report, const std::string& directory) {
    run(report, "capacity.default-large-svg-rejected-before-bitmap-allocation", [&](Check& check) {
        auto bytes = fixture(check, directory, "large-intrinsic.svg");
        if (!check.passed) { return; }
        auto image = Image::make(bytes);
        if (check.expect(!image, "Default Image::make accepted a 20000x20000 SVG raster")) {
            check.expect(image.error().toString().find("SVG raster exceeds the 32 MiB pixel capacity") != std::string::npos,
                         "Default SVG load did not report the preallocation capacity error");
        }
        auto direct = Image::makeFromSVG(bytes);
        check.expect(!direct, "Default explicit SVG factory accepted an oversized raster");
        auto bitmap = rasterizeSVG(bytes, BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888));
        check.expect(!bitmap, "Default SVG bitmap factory path accepted an oversized raster");
        // The unchanged preceding case separately requires the 32x32 preferred
        // raster and the full 20000x20000 intrinsic size to remain supported.
    });
    run(report, "capacity.large-png-thumbnail-rejected-before-source-f16-decode", [&](Check& check) {
        auto bytes = fixture(check, directory, "capacity-static-source.png");
        if (!check.passed) { return; }
        auto codec = tgfx::ImageCodec::MakeFrom(skDataFromBytes(bytes, DataConversionModeNeverCopy));
        if (!check.expect(codec != nullptr, "Actual PNG codec must accept the complete oversized header")) { return; }
        check.expect(codec->width() == 20000 && codec->height() == 20000, "Oversized PNG header dimensions changed");
        auto image = Image::make(bytes);
        if (!check.result(image, "Actual lazy production Image::make must retain the declared PNG size")) { return; }
        check.expect(image.value()->width() == 20000 && image.value()->height() == 20000,
                     "Large source PNG was silently downscaled");
        const Valdi::BitmapInfo info(32, 32, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 32 * 4);
        check.expect(!tgfxMediaCanReadPixels(codec, info), "Oversized source passed aggregate decode admission");
        check.expect(image.value()->resized(32, 32) == nullptr, "Thumbnail resize accepted oversized source staging");
        check.expect(image.value()->toConvertedBitmap(info) == nullptr,
                     "Converted thumbnail accepted oversized source staging");
        std::array<uint8_t, 32 * 32 * 4> pixels;
        pixels.fill(0xa5);
        check.expect(!tgfxMediaReadPixels(codec, info, pixels.data()),
                     "Actual production read entered oversized PNG decoding");
        check.expect(std::all_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value == 0xa5; }),
                     "Rejected source decode modified caller pixels");
        auto encoded = image.value()->toPNG();
        check.expect(!encoded, "Large-source encode allocated an unadmitted output/decode buffer");
    });
    run(report, "capacity.large-and-padded-static-destinations-rejected-before-output-allocation", [&](Check& check) {
        auto bytes = fixture(check, directory, "pattern.png");
        if (!check.passed) { return; }
        auto image = Image::make(bytes);
        if (!check.result(image, "Actual ordinary PNG for destination-admission control")) { return; }
        const Valdi::BitmapInfo large(20000, 20000, Valdi::ColorTypeRGBA8888,
                                      Valdi::AlphaTypePremul, 20000 * 4);
        const Valdi::BitmapInfo padded(32, 32, Valdi::ColorTypeRGBA8888,
                                       Valdi::AlphaTypePremul, 5 * 1024 * 1024);
        check.expect(image.value()->resized(20000, 20000) == nullptr,
                     "Resize allocated oversized destination bitmap before admission");
        check.expect(image.value()->toConvertedBitmap(large) == nullptr,
                     "Conversion allocated oversized destination before admission");
        check.expect(image.value()->toConvertedBitmap(padded) == nullptr,
                     "Conversion ignored over-capacity caller row stride");
        auto small = image.value()->toConvertedBitmap(Valdi::BitmapInfo(32, 32, Valdi::ColorTypeRGBA8888,
                                                                       Valdi::AlphaTypePremul, 0));
        check.expect(small != nullptr && small->getInfo().rowBytes == 32 * 4,
                     "Ordinary conversion lost its supported minimum-stride normalization");
    });
    run(report, "capacity.static-f32-aggregate-staging-rejected-before-output-allocation", [&](Check& check) {
        auto bytes = fixture(check, directory, "pattern.png");
        if (!check.passed) { return; }
        auto codec = tgfx::ImageCodec::MakeFrom(skDataFromBytes(bytes, DataConversionModeNeverCopy));
        auto image = Image::make(bytes);
        if (!check.expect(codec != nullptr, "Actual small PNG codec unavailable") ||
            !check.result(image, "Actual small PNG for aggregate F32 control")) { return; }
        // Each 64 MiB F32 output and 32 MiB F16 stage individually fits.
        // Output + two F16 destinations already consume 128 MiB before source,
        // decoder staging, row pointers and workspace are counted.
        const Valdi::BitmapInfo info(2048, 2048, Valdi::ColorTypeRGBAF32,
                                     Valdi::AlphaTypeUnpremul, 2048 * 16);
        check.expect(!tgfxMediaCanReadPixels(codec, info), "F32 stages were checked independently instead of in aggregate");
        check.expect(image.value()->toConvertedBitmap(info) == nullptr,
                     "Aggregate F32 read allocated its output before rejecting staging");
    });
}

void mediaRawDestinationAdmissionCheck(MediaTestReport& report) {
    run(report, "capacity.exact-raw-copy-rejects-padded-destination-preserves-normal-copy", [&](Check& check) {
        const Valdi::BitmapInfo info(32, 32, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, 32 * 4);
        auto source = Valdi::makeShared<Valdi::Bytes>();
        source->resize(info.rowBytes * info.height);
        for (size_t offset = 0; offset < source->size(); offset += 4) {
            source->data()[offset] = static_cast<uint8_t>(offset / 4);
            source->data()[offset + 1] = 73;
            source->data()[offset + 2] = 19;
            source->data()[offset + 3] = 255;
        }
        auto image = Image::makeFromPixelsData(info, Valdi::BytesView(source), false);
        if (!check.result(image, "Actual raw RGBA/premul production image")) { return; }
        const Valdi::BitmapInfo padded(32, 32, Valdi::ColorTypeRGBA8888,
                                       Valdi::AlphaTypePremul, 5 * 1024 * 1024);
        check.expect(image.value()->toConvertedBitmap(padded) == nullptr,
                     "Matching raw shortcut admitted a 160 MiB padded destination");
        auto normal = image.value()->toConvertedBitmap(info);
        if (!check.expect(normal != nullptr, "Staging-free normal-stride raw conversion was rejected")) { return; }
        check.expect(normal->getInfo() == info, "Normal-stride raw copy changed bitmap format or stride");
        auto* pixels = normal->lockBytes();
        if (check.expect(pixels != nullptr, "Cannot lock the actual normal-stride raw result")) {
            check.expect(std::memcmp(pixels, source->data(), source->size()) == 0,
                         "Normal-stride raw conversion changed caller pixel bytes");
            normal->unlockBytes();
        }
    });
}

void compareTextPixels(Check& check, const std::vector<uint8_t>& actual,
                       const std::vector<uint8_t>& reference) {
    if (!check.expect(actual.size() == 128 * 64 * 4 && actual.size() == reference.size(),
                      "SVG font comparison has invalid readback dimensions")) { return; }
    auto ink = [](const std::vector<uint8_t>& pixels, int x, int y) {
        const auto offset = static_cast<size_t>(y * 128 + x) * 4;
        return pixels[offset] > 240 && pixels[offset + 1] < 8 && pixels[offset + 2] < 8 && pixels[offset + 3] > 240;
    };
    std::array<int, 4> a{128, 64, -1, -1}, b{128, 64, -1, -1};
    size_t actualInk = 0, referenceInk = 0, interior = 0, matches = 0;
    auto include = [](std::array<int, 4>& bounds, int x, int y) {
        bounds[0] = std::min(bounds[0], x); bounds[1] = std::min(bounds[1], y);
        bounds[2] = std::max(bounds[2], x); bounds[3] = std::max(bounds[3], y);
    };
    for (int y = 1; y < 63; ++y) {
        for (int x = 1; x < 127; ++x) {
            if (ink(actual, x, y)) { ++actualInk; include(a, x, y); }
            if (ink(reference, x, y)) { ++referenceInk; include(b, x, y); }
            if (ink(reference, x, y) && ink(reference, x - 1, y) && ink(reference, x + 1, y) &&
                ink(reference, x, y - 1) && ink(reference, x, y + 1)) {
                ++interior;
                if (ink(actual, x, y)) { ++matches; }
            }
        }
    }
    check.expect(actualInk > 100 && referenceInk > 100 && interior > 20,
                 "SVG text disappeared or has insufficient real glyph coverage (actual=" +
                 std::to_string(actualInk) + ", native=" + std::to_string(referenceInk) +
                 ", nativeInterior=" + std::to_string(interior) + ")");
    check.expect(actualInk > referenceInk * 0.65 && actualInk < referenceInk * 1.35,
                 "SVG selected-font coverage differs materially from the actual native face");
    for (size_t index = 0; index < 4; ++index) {
        check.expect(std::abs(a[index] - b[index]) <= 3, "SVG selected-font bounds/baseline differ from native text");
    }
    check.expect(interior && matches >= interior * 0.80, "SVG glyph interiors do not match the actual native selected font");
}

std::vector<uint8_t> nativeTextPixels(Check& check, const NativeTypeface& face, const char* text) {
    return gpuPixels(check, 128, 64, [&](NativeCanvas* canvas) {
        tgfx::Font font(face, 32);
        tgfx::Paint paint;
        paint.setColor(Color::makeARGB(255, 255, 0, 0).getSkValue());
        canvas->drawSimpleText(text, 8, 44, font, paint);
    });
}

Ref<Typeface> nativeArial(Check& check) {
    auto fonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
    fonts->load();
    auto selected = fonts->getTypefaceWithName(Valdi::StringBox::fromCString("Arial"));
    if (!check.result(selected, "Actual native FontManager Arial/default-face selection")) { return nullptr; }
    auto face = selected.moveValue();
    auto bytes = face->makeFontData();
    check.expect(bytes.size() >= 12 && std::memcmp(bytes.data(), "ttcf", 4) != 0,
                 "Native default font did not export its selected face as standalone SFNT");
    return face;
}

Ref<FontManager> notoManager(Check& check, const std::string& directory) {
    auto bytes = fixture(check, directory, "NotoSans-Regular.ttf");
    if (!check.passed) { return nullptr; }
    auto fonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
    fonts->load();
    fonts->registerTypeface(Valdi::StringBox::fromCString("ValdiMediaFixture"),
        FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright), false, bytes);
    return fonts;
}

Ref<AnimatedImage> unrelatedFontAsset(Check& check, const std::string& directory) {
    auto fonts = notoManager(check, directory);
    auto bytes = fixture(check, directory, "custom-font.json");
    if (fonts == nullptr || !check.passed) { return nullptr; }
    auto animation = LottieAnimatedImage::make(Ref<IFontManager>(fonts), bytes.data(), bytes.size());
    if (!check.result(animation, "Real unrelated Lottie asset must populate its own private font cache")) { return nullptr; }
    return animation.moveValue();
}

void svgFontResolutionChecks(MediaTestReport& report, const std::string& directory) {
    // Run this group FIRST: a cold-cache assertion must not be silently skipped
    // or satisfied by a previous media/text suite's process-global cache.
    run(report, "svg.text-native-default-from-clean-thorvg-cache", [&](Check& check) {
        const auto initialized = tvg::Initializer::init(0);
        if (!check.expect(initialized == tvg::Result::Success, "Cannot initialize actual ThorVG for the cold-cache probe")) { return; }
        struct Term { ~Term() { tvg::Initializer::term(); } } term;
        auto* probe = tvg::Text::gen();
        const auto cached = probe->font(nullptr);
        tvg::Paint::rel(probe);
        if (!check.expect(cached != tvg::Result::Success, "Cold SVG font case started with an available global ThorVG font")) { return; }
        auto face = nativeArial(check);
        auto bytes = fixture(check, directory, "svg-arial-text.svg");
        if (face == nullptr || !check.passed) { return; }
        // This actual public static factory supplies no IFontManager.
        auto image = Image::make(bytes);
        if (!check.result(image, "Public static SVG Arial text with an empty ThorVG font cache")) { return; }
        compareTextPixels(check, bitmapPixels(check, image.value(), 128, 64),
                          nativeTextPixels(check, face->getSkValue(), "Hello"));
    });
    run(report, "svg.text-native-default-ignores-unrelated-live-font-cache", [&](Check& check) {
        auto unrelated = unrelatedFontAsset(check, directory);
        auto face = nativeArial(check);
        auto bytes = fixture(check, directory, "svg-arial-text.svg");
        if (unrelated == nullptr || face == nullptr || !check.passed) { return; }
        auto* probe = tvg::Text::gen();
        const auto cached = probe->font(nullptr);
        tvg::Paint::rel(probe);
        check.expect(cached == tvg::Result::Success, "Unrelated actual asset did not establish a live cached-font control");
        auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
        if (!check.result(animation, "Public animated SVG default native font provider")) { return; }
        compareTextPixels(check, animationPixels(check, animation.value(), 128, 64, 0.25),
                          nativeTextPixels(check, face->getSkValue(), "Hello"));
    });
    run(report, "svg.text-two-asset-font-providers-preserve-same-alias-isolation", [&](Check& check) {
        auto firstFonts = notoManager(check, directory);
        auto secondBytes = fixture(check, directory, "Montserrat-Regular.ttf");
        auto bytes = fixture(check, directory, "svg-owned-text.svg");
        if (firstFonts == nullptr || !check.passed) { return; }
        auto secondFonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
        secondFonts->load();
        const auto alias = Valdi::StringBox::fromCString("ValdiMediaFixture");
        secondFonts->registerTypeface(alias, FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright),
                                      false, secondBytes);
        auto firstFace = firstFonts->getTypefaceWithName(alias);
        auto secondFace = secondFonts->getTypefaceWithName(alias);
        if (!check.result(firstFace, "First actual custom font lookup") ||
            !check.result(secondFace, "Second actual custom font lookup")) { return; }
        check.expect(firstFace.value()->isCustom() && secondFace.value()->isCustom(),
                     "SVG provider-isolation control fell back instead of using its bundled fonts");
        auto first = AnimatedImage::make(Ref<IFontManager>(firstFonts), bytes.data(), bytes.size());
        auto second = AnimatedImage::make(Ref<IFontManager>(secondFonts), bytes.data(), bytes.size());
        if (!check.result(first, "First asset-owned SVG font provider") ||
            !check.result(second, "Second asset-owned SVG font provider")) { return; }
        auto firstPixels = animationPixels(check, first.value(), 128, 64, 0.25);
        auto secondPixels = animationPixels(check, second.value(), 128, 64, 0.25);
        compareTextPixels(check, firstPixels, nativeTextPixels(check, firstFace.value()->getSkValue(), "QQQQ"));
        compareTextPixels(check, secondPixels, nativeTextPixels(check, secondFace.value()->getSkValue(), "QQQQ"));
        check.expect(firstPixels != secondPixels, "Distinct native providers collapsed to the same globally cached SVG face");
        check.expect(firstPixels == animationPixels(check, first.value(), 128, 64, 0.25),
                     "Loading the second same-named SVG face changed the first asset");
    });
    run(report, "svg.strict-font-resolver-rejection-is-a-real-load-failure", [&](Check& check) {
        auto unrelated = unrelatedFontAsset(check, directory);
        auto bytes = fixture(check, directory, "svg-arial-text.svg");
        if (unrelated == nullptr || !check.passed) { return; }
        auto* picture = tvg::Picture::gen();
        bool requested = false;
        const auto resolver = picture->resolver([&](tvg::Paint* paint, const char* source, void*) {
            requested = paint && paint->type() == tvg::Type::Text && std::string(source) == "name:Arial";
            return false; // A real host resolver explicitly rejects the requested asset.
        }, nullptr, false, true);
        check.expect(resolver == tvg::Result::Success, "Cannot install the actual strict SVG resolver");
        const auto loaded = picture->load(reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<uint32_t>(bytes.size()), "svg+xml", nullptr, true);
        check.expect(requested && loaded != tvg::Result::Success,
                     "Strict SVG font rejection returned a successful blank/cache-fallback Picture");
        tvg::Paint::rel(picture);
    });
}

struct ThorvgPaintRelease {
    void operator()(tvg::Paint* paint) const { if (paint) { paint->unref(); } }
};
using OwnedThorvgText = std::unique_ptr<tvg::Text, ThorvgPaintRelease>;
using OwnedThorvgPicture = std::unique_ptr<tvg::Picture, ThorvgPaintRelease>;

OwnedThorvgText ownedThorvgText() {
    auto* text = tvg::Text::gen();
    text->ref();
    return OwnedThorvgText(text);
}

OwnedThorvgPicture ownedThorvgPicture() {
    auto* picture = tvg::Picture::gen();
    picture->ref();
    return OwnedThorvgPicture(picture);
}

bool expectFontCacheEmpty(Check& check, const std::string& stage) {
    auto probe = ownedThorvgText();
    return check.expect(probe->font(nullptr) == tvg::Result::InsufficientCondition,
                        stage + ": an actual ThorVG font registration remains available");
}

bool expectFontLookup(Check& check, const char* name, bool available, const char* stage) {
    auto probe = ownedThorvgText();
    return check.expect((probe->font(name) == tvg::Result::Success) == available, stage);
}

std::vector<uint8_t> thorvgPaintPixels(Check& check, tvg::Paint* paint) {
    std::vector<uint32_t> packed(128 * 64);
    // Storage outlives the canvas target, including on failure returns.
    std::unique_ptr<tvg::SwCanvas> canvas(tvg::SwCanvas::gen());
    if (!check.expect(canvas != nullptr, "Actual ThorVG CPU canvas unavailable")) { return {}; }
    // ThorVG forbids moving a renderer-bound paint to another renderer even
    // after the previous canvas is destroyed. duplicate() retains the real
    // font loader and builds a renderer-independent paint for this canvas.
    auto* drawable = paint->duplicate();
    if (!check.expect(drawable != nullptr, "Actual retained paint duplicate failed")) { return {}; }
    struct ReleaseUnowned { tvg::Paint* paint; ~ReleaseUnowned() { tvg::Paint::rel(paint); } } release{drawable};
    if (!check.expect(canvas->target(packed.data(), 128, 128, 64, tvg::ColorSpace::ABGR8888) ==
                          tvg::Result::Success, "Actual ThorVG CPU target failed") ||
        !check.expect(canvas->add(drawable) == tvg::Result::Success, "Actual retained paint add failed") ||
        !check.expect(canvas->draw(true) == tvg::Result::Success, "Actual retained paint draw failed") ||
        !check.expect(canvas->sync() == tvg::Result::Success, "Actual retained paint sync failed")) { return {}; }
    std::vector<uint8_t> rgba(packed.size() * 4);
    size_t ink = 0;
    for (size_t index = 0; index < packed.size(); ++index) {
        for (size_t channel = 0; channel < 4; ++channel) {
            rgba[index * 4 + channel] = static_cast<uint8_t>(packed[index] >> (channel * 8));
        }
        if (rgba[index * 4 + 3] > 200) { ++ink; }
    }
    check.expect(ink > 100, "Retained real font paint contains no substantial glyph coverage");
    return rgba;
}

std::vector<uint8_t> thorvgTextPixels(Check& check, tvg::Text* text, const char* value) {
    check.expect(text->text(value) == tvg::Result::Success, "Retained Text cannot accept fresh glyphs");
    check.expect(text->size(32) == tvg::Result::Success, "Retained Text size failed");
    check.expect(text->fill(255, 0, 0) == tvg::Result::Success, "Retained Text fill failed");
    check.expect(text->transform(tvg::Matrix{1, 0, 8, 0, 1, 8, 0, 0, 1}) == tvg::Result::Success,
                 "Retained Text transform failed");
    return thorvgPaintPixels(check, text);
}

struct MemoryFontRegistration {
    const char* name;
    ~MemoryFontRegistration() { tvg::Text::load(name, nullptr, 0); }
};
struct FileFontRegistration {
    std::string path;
    ~FileFontRegistration() { tvg::Text::unload(path.c_str()); }
};

void thorvgFontLifetimeChecks(MediaTestReport& report, const std::string& directory) {
    // The original cold-cache group has already initialized the real runtime.
    // These checks never terminate/reinitialize ThorVG to clear registrations.
    run(report, "thorvg.memory-font-idempotent-copy-registration-and-unregister", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before memory registration")) { return; }
        auto bytes = fixture(check, directory, "NotoSans-Regular.ttf");
        if (!check.passed) { return; }
        constexpr const char* name = "ValdiRound3CopiedFont";
        MemoryFontRegistration cleanup{name};
        for (int index = 0; index < 32; ++index) {
            check.expect(tvg::Text::load(name, reinterpret_cast<const char*>(bytes.data()),
                static_cast<uint32_t>(bytes.size()), "ttf", true) == tvg::Result::Success,
                "Actual repeated copy registration failed");
        }
        bytes = {}; // The loader must own its actual copied bytes.
        expectFontLookup(check, name, true, "Copied font is unavailable after source release");
        check.expect(tvg::Text::unload(name) == tvg::Result::InsufficientCondition,
                     "Filename unload incorrectly removed a memory-font name");
        check.expect(tvg::Text::load(name, nullptr, 0) == tvg::Result::Success,
                     "Documented memory-font unregister failed");
        expectFontLookup(check, name, false, "Removed memory font is still selectable by name");
        check.expect(tvg::Text::load(name, nullptr, 0) == tvg::Result::InsufficientCondition,
                     "Repeated memory-font unregister unexpectedly succeeded");
        expectFontCacheEmpty(check, "After copied memory unregister");
    });
    run(report, "thorvg.memory-font-retained-paints-survive-borrow-replacement-and-unregister", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before retained-font control")) { return; }
        auto first = fixture(check, directory, "NotoSans-Regular.ttf");
        auto second = fixture(check, directory, "Montserrat-Regular.ttf");
        if (!check.passed) { return; }
        constexpr const char* name = "ValdiRound3BorrowedFont";
        MemoryFontRegistration cleanup{name};
        if (!check.expect(tvg::Text::load(name, reinterpret_cast<const char*>(first.data()),
                static_cast<uint32_t>(first.size()), "ttf", false) == tvg::Result::Success,
                "Actual borrowed registration failed")) { return; }
        auto retained = ownedThorvgText(), reference = ownedThorvgText();
        check.expect(retained->font(name) == tvg::Result::Success &&
                     reference->font(name) == tvg::Result::Success, "Cannot retain original font paints");
        thorvgTextPixels(check, retained.get(), "A"); // HgQ is requested only after detach.
        check.expect(tvg::Text::load(name, reinterpret_cast<const char*>(second.data()),
            static_cast<uint32_t>(second.size()), "ttf", true) == tvg::Result::Success,
            "Borrowed registration replacement failed");
        second = {}; // Only ThorVG's owned copy can back the replacement paint.
        auto replacement = ownedThorvgText();
        check.expect(replacement->font(name) == tvg::Result::Success, "Replacement face is unavailable");
        check.expect(tvg::Text::load(name, nullptr, 0) == tvg::Result::Success,
                     "Unregister with live old/new Text paints failed");
        expectFontLookup(check, name, false, "Unregistered face remains visible to new Text paints");
        expectFontCacheEmpty(check, "Detached paints must not remain in lookup");
        const auto oldPixels = thorvgTextPixels(check, retained.get(), "HgQ");
        const auto referencePixels = thorvgTextPixels(check, reference.get(), "HgQ");
        const auto newPixels = thorvgTextPixels(check, replacement.get(), "HgQ");
        check.expect(!oldPixels.empty() && oldPixels == referencePixels,
                     "Detached original Text lost its font or could not decode fresh glyphs");
        check.expect(!newPixels.empty() && oldPixels != newPixels,
                     "Same-name replacement changed old paints or collapsed distinct actual fonts");
        retained.reset(); reference.reset(); replacement.reset();
        expectFontCacheEmpty(check, "After last detached Text release");
    });
    run(report, "thorvg.filename-unload-preserves-live-paint-and-removes-idempotent-registration", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before file-font registration")) { return; }
        FileFontRegistration cleanup{directory + "/NotoSans-Regular.ttf"};
        for (int index = 0; index < 32; ++index) {
            check.expect(tvg::Text::load(cleanup.path.c_str()) == tvg::Result::Success,
                         "Actual repeated file registration failed");
        }
        auto retained = ownedThorvgText(), reference = ownedThorvgText();
        check.expect(retained->font("NotoSans-Regular") == tvg::Result::Success &&
                     reference->font("NotoSans-Regular") == tvg::Result::Success,
                     "Actual filename-derived font alias unavailable");
        check.expect(tvg::Text::load(cleanup.path.c_str(), nullptr, 0) == tvg::Result::InsufficientCondition,
                     "Memory unregister incorrectly removed a filename registration");
        check.expect(tvg::Text::unload(cleanup.path.c_str()) == tvg::Result::Success,
                     "Actual filename unload failed");
        expectFontLookup(check, "NotoSans-Regular", false, "Unloaded filename font remains selectable");
        check.expect(tvg::Text::unload(cleanup.path.c_str()) == tvg::Result::InsufficientCondition,
                     "Repeated filename unload unexpectedly succeeded");
        const auto actual = thorvgTextPixels(check, retained.get(), "HgQ");
        const auto expected = thorvgTextPixels(check, reference.get(), "HgQ");
        check.expect(!actual.empty() && actual == expected, "Live file-font paints did not survive unregister");
        retained.reset(); reference.reset();
        expectFontCacheEmpty(check, "After filename font release");
    });
    run(report, "thorvg.memory-name-and-filename-unregister-have-distinct-contracts", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before namespace control")) { return; }
        auto bytes = fixture(check, directory, "Montserrat-Regular.ttf");
        if (!check.passed) { return; }
        FileFontRegistration file{directory + "/NotoSans-Regular.ttf"};
        MemoryFontRegistration memory{file.path.c_str()};
        check.expect(tvg::Text::load(file.path.c_str()) == tvg::Result::Success, "File-font control load failed");
        check.expect(tvg::Text::load(memory.name, reinterpret_cast<const char*>(bytes.data()),
            static_cast<uint32_t>(bytes.size()), "ttf", true) == tvg::Result::Success,
            "Memory alias equal to a filename failed");
        check.expect(tvg::Text::unload(file.path.c_str()) == tvg::Result::Success, "File namespace unload failed");
        expectFontLookup(check, "NotoSans-Regular", false, "File registration survived filename unload");
        expectFontLookup(check, memory.name, true, "File unload removed independent memory alias");
        check.expect(tvg::Text::load(memory.name, nullptr, 0) == tvg::Result::Success, "Memory namespace unload failed");
        expectFontCacheEmpty(check, "After both namespace releases");
    });
    run(report, "svg.repeated-size-query-and-static-load-release-remove-private-fonts", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before repeated static SVG lifecycle")) { return; }
        auto bytes = fixture(check, directory, "svg-arial-text.svg");
        if (!check.passed) { return; }
        for (int index = 0; index < 16; ++index) {
            {
                auto size = getSVGSize(bytes);
                if (!check.result(size, "Repeated real text SVG size query")) { return; }
                check.expect(size.value() == std::make_pair(128, 64), "Repeated SVG size changed");
            }
            if (!expectFontCacheEmpty(check, "Immediately after SVG size query " + std::to_string(index))) { return; }
            {
                auto image = Image::make(bytes);
                if (!check.result(image, "Repeated static text SVG load")) { return; }
                auto pixels = bitmapPixels(check, image.value(), 128, 64);
                check.expect(!pixels.empty(), "Repeated static SVG readback failed");
            }
            if (!expectFontCacheEmpty(check, "After static SVG release " + std::to_string(index))) { return; }
        }
    });
    run(report, "svg.repeated-custom-provider-animated-release-removes-private-fonts", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before repeated provider SVG lifecycle")) { return; }
        auto fonts = notoManager(check, directory);
        auto bytes = fixture(check, directory, "svg-owned-text.svg");
        if (fonts == nullptr || !check.passed) { return; }
        for (int index = 0; index < 16; ++index) {
            {
                auto animation = AnimatedImage::make(Ref<IFontManager>(fonts), bytes.data(), bytes.size());
                if (!check.result(animation, "Repeated provider text SVG load")) { return; }
                auto probe = ownedThorvgText();
                check.expect(probe->font(nullptr) == tvg::Result::Success,
                             "Live provider SVG did not establish its private registration");
                auto pixels = animationPixels(check, animation.value(), 128, 64, 0.25);
                check.expect(!pixels.empty(), "Repeated provider SVG readback failed");
            }
            if (!expectFontCacheEmpty(check, "After provider SVG release " + std::to_string(index))) { return; }
        }
    });
    run(report, "thorvg.embedded-svg-fontface-repeated-load-release-cleans-memory-registration", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before embedded-font lifecycle")) { return; }
        auto bytes = fixture(check, directory, "svg-embedded-font.svg");
        if (!check.passed) { return; }
        for (int index = 0; index < 8; ++index) {
            auto retained = ownedThorvgText();
            {
                auto picture = ownedThorvgPicture();
                check.expect(picture->load(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<uint32_t>(bytes.size()), "svg+xml", nullptr, true) == tvg::Result::Success,
                    "Actual embedded @font-face SVG load failed");
                auto pixels = thorvgPaintPixels(check, picture.get());
                check.expect(!pixels.empty(), "Actual embedded @font-face glyph raster failed");
                expectFontLookup(check, "ValdiEmbeddedFixture", true, "Live embedded font registration missing");
                check.expect(retained->font("ValdiEmbeddedFixture") == tvg::Result::Success,
                             "Cannot retain embedded face beyond its parser lifetime");
            }
            if (!expectFontCacheEmpty(check, "After embedded SVG release " + std::to_string(index))) { return; }
            // Parser-owned base64 bytes have been freed. The detached loader's
            // copied data must still decode glyphs absent from the SVG's HgQ.
            check.expect(!thorvgTextPixels(check, retained.get(), "AW").empty(),
                         "Retained embedded font cannot render after SVG parser release");
            retained.reset();
            expectFontCacheEmpty(check, "After retained embedded Text release");
        }
    });
}

void nestedSvgFontChecks(MediaTestReport& report, const std::string& directory) {
    run(report, "svg.nested-data-uri-native-text-from-clean-cache", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before nested default SVG")) { return; }
        auto face = nativeArial(check);
        auto bytes = fixture(check, directory, "svg-nested-arial.svg");
        if (face == nullptr || !check.passed) { return; }
        {
            auto image = Image::make(bytes);
            if (!check.result(image, "Actual nested data-URI SVG default font load")) { return; }
            compareTextPixels(check, bitmapPixels(check, image.value(), 128, 64),
                              nativeTextPixels(check, face->getSkValue(), "Hello"));
        }
        expectFontCacheEmpty(check, "After nested default SVG release");
    });
    run(report, "svg.nested-default-text-ignores-unrelated-live-cache", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before nested unrelated-cache control")) { return; }
        auto unrelated = unrelatedFontAsset(check, directory);
        auto face = nativeArial(check);
        auto bytes = fixture(check, directory, "svg-nested-arial.svg");
        if (unrelated == nullptr || face == nullptr || !check.passed) { return; }
        auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
        if (!check.result(animation, "Nested default SVG with unrelated actual font")) { return; }
        compareTextPixels(check, animationPixels(check, animation.value(), 128, 64, 0.25),
                          nativeTextPixels(check, face->getSkValue(), "Hello"));
    });
    run(report, "svg.nested-same-family-two-native-providers-remain-isolated", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before nested provider control")) { return; }
        auto firstFonts = notoManager(check, directory);
        auto secondBytes = fixture(check, directory, "Montserrat-Regular.ttf");
        auto bytes = fixture(check, directory, "svg-nested-owned.svg");
        if (firstFonts == nullptr || !check.passed) { return; }
        auto secondFonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
        secondFonts->load();
        const auto alias = Valdi::StringBox::fromCString("ValdiMediaFixture");
        secondFonts->registerTypeface(alias, FontStyle(FontWidthNormal, FontWeightNormal, FontSlantUpright),
                                      false, secondBytes);
        auto firstFace = firstFonts->getTypefaceWithName(alias);
        auto secondFace = secondFonts->getTypefaceWithName(alias);
        if (!check.result(firstFace, "Nested first native provider") ||
            !check.result(secondFace, "Nested second native provider")) { return; }
        check.expect(firstFace.value()->isCustom() && secondFace.value()->isCustom(),
                     "Nested native provider control used fallback faces");
        auto first = AnimatedImage::make(Ref<IFontManager>(firstFonts), bytes.data(), bytes.size());
        auto second = AnimatedImage::make(Ref<IFontManager>(secondFonts), bytes.data(), bytes.size());
        if (!check.result(first, "Nested first asset-owned font load") ||
            !check.result(second, "Nested second asset-owned font load")) { return; }
        const auto firstPixels = animationPixels(check, first.value(), 128, 64, 0.25);
        const auto secondPixels = animationPixels(check, second.value(), 128, 64, 0.25);
        compareTextPixels(check, firstPixels, nativeTextPixels(check, firstFace.value()->getSkValue(), "QQQQ"));
        compareTextPixels(check, secondPixels, nativeTextPixels(check, secondFace.value()->getSkValue(), "QQQQ"));
        check.expect(firstPixels != secondPixels, "Nested SVG providers selected one global face");
        check.expect(firstPixels == animationPixels(check, first.value(), 128, 64, 0.25),
                     "Second nested provider changed the first live asset");
    });
    run(report, "svg.two-level-nested-default-text-inherits-owner-resolver", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before two-level nested SVG")) { return; }
        auto face = nativeArial(check);
        auto bytes = fixture(check, directory, "svg-nested-twice-arial.svg");
        if (face == nullptr || !check.passed) { return; }
        {
            auto image = Image::make(bytes);
            if (!check.result(image, "Actual two-level nested text SVG load")) { return; }
            compareTextPixels(check, bitmapPixels(check, image.value(), 128, 64),
                              nativeTextPixels(check, face->getSkValue(), "Hello"));
        }
        expectFontCacheEmpty(check, "After two-level nested SVG release");
    });
    for (const char* filename : {"svg-nested-arial.svg", "svg-nested-twice-arial.svg"}) {
        run(report, std::string("svg.nested-strict-rejection-propagates-to-outer-load.") + filename, [&](Check& check) {
            if (!expectFontCacheEmpty(check, "Before nested rejection control")) { return; }
            auto unrelated = unrelatedFontAsset(check, directory);
            auto bytes = fixture(check, directory, filename);
            if (unrelated == nullptr || !check.passed) { return; }
            auto probe = ownedThorvgText();
            check.expect(probe->font(nullptr) == tvg::Result::Success, "Unrelated cache control is absent");
            auto picture = ownedThorvgPicture();
            size_t requested = 0;
            check.expect(picture->resolver([&](tvg::Paint* paint, const char* source, void*) {
                if (paint && paint->type() == tvg::Type::Text && source && std::string(source) == "name:Arial") {
                    ++requested;
                }
                return false; // Actual host refusal must survive each nested loader.
            }, nullptr, false, true) == tvg::Result::Success, "Nested strict resolver installation failed");
            const auto loaded = picture->load(reinterpret_cast<const char*>(bytes.data()),
                static_cast<uint32_t>(bytes.size()), "svg+xml", nullptr, true);
            check.expect(requested == 1 && loaded != tvg::Result::Success,
                         "Nested font rejection became a successful outer SVG or skipped its owner resolver");
            picture.reset(); probe.reset(); unrelated = nullptr;
            expectFontCacheEmpty(check, "After failed nested SVG and unrelated asset release");
        });
    }
    run(report, "svg.nested-actual-font-loader-failure-propagates-with-unrelated-cache", [&](Check& check) {
        if (!expectFontCacheEmpty(check, "Before nested actual-loader failure")) { return; }
        auto unrelated = unrelatedFontAsset(check, directory);
        auto bytes = fixture(check, directory, "svg-nested-twice-arial.svg");
        auto malformedFont = fixture(check, directory, "not-image.bin");
        if (unrelated == nullptr || !check.passed) { return; }
        constexpr const char* name = "ValdiRound3RejectedFont";
        MemoryFontRegistration cleanup{name};
        size_t requested = 0;
        auto picture = ownedThorvgPicture();
        check.expect(picture->resolver([&](tvg::Paint* paint, const char*, void*) {
            if (!paint || paint->type() != tvg::Type::Text) { return false; }
            ++requested;
            const auto loaded = tvg::Text::load(name, reinterpret_cast<const char*>(malformedFont.data()),
                static_cast<uint32_t>(malformedFont.size()), "ttf", true);
            check.expect(loaded != tvg::Result::Success, "Actual SFNT loader accepted non-font bytes");
            return loaded == tvg::Result::Success &&
                   static_cast<tvg::Text*>(paint)->font(name) == tvg::Result::Success;
        }, nullptr, false, true) == tvg::Result::Success, "Actual failing-loader resolver installation failed");
        const auto loaded = picture->load(reinterpret_cast<const char*>(bytes.data()),
            static_cast<uint32_t>(bytes.size()), "svg+xml", nullptr, true);
        check.expect(requested == 1 && loaded != tvg::Result::Success,
                     "Nested actual font-loader failure returned outer load success or bypassed resolver");
        expectFontLookup(check, name, false, "Failed SFNT decode left a font registration");
        picture.reset(); unrelated = nullptr;
        expectFontCacheEmpty(check, "After actual nested-loader failure");
    });
}

void compareRasterPixels(Check& check, const std::vector<uint8_t>& actual,
                         const std::vector<uint8_t>& reference) {
    if (!check.expect(!actual.empty() && actual.size() == reference.size(), "Physical native-raster readback dimensions differ")) { return; }
    size_t differences = 0, referenceInk = 0;
    for (size_t index = 0; index < actual.size(); ++index) {
        if (std::abs(int(actual[index]) - int(reference[index])) > 1) { ++differences; }
        if (index % 4 == 3 && reference[index] > 200) { ++referenceInk; }
    }
    check.expect(referenceInk > 100, "Native sharp-detail reference contains no substantial coverage");
    check.expect(differences == 0, "Deferred SVG/Lottie differs from matching physical native raster in " +
                                   std::to_string(differences) + " channels (tolerance 1)");
}

void mediaDeferredRasterChecks(MediaTestReport& report, const std::string& directory) {
    for (const char* filename : {"sharp-detail.svg", "sharp-detail.json"}) {
        for (const int scale : {2, 3}) {
            run(report, std::string("media.detail-direct-physical-") + std::to_string(scale) + "x." + filename, [&](Check& check) {
                auto bytes = fixture(check, directory, filename);
                if (!check.passed) { return; }
                auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
                auto referenceMedia = ThorvgMedia::make(bytes.data(), bytes.size(), std::string(filename).find(".json") != std::string::npos);
                if (!check.result(animation, "Actual sharp-detail animated factory") ||
                    !check.result(referenceMedia, "Actual independent native-raster reference")) { return; }
                auto native = referenceMedia.value()->render(100 * scale, 100 * scale, Duration::fromSeconds(0.25));
                if (!check.result(native, "Matching physical native ThorVG raster")) { return; }
                auto actual = gpuPixels(check, 100 * scale, 100 * scale, [&](NativeCanvas* canvas) {
                    canvas->scale(scale, scale);
                    animation.value()->draw(canvas, Rect::makeXYWH(0, 0, 100, 100), Duration::fromSeconds(0.25), FittingSizeModeFill);
                    check.expect(!canvas->hasError(), "Physical media draw reported native render failure");
                });
                auto reference = gpuPixels(check, 100 * scale, 100 * scale, [&](NativeCanvas* canvas) {
                    canvas->drawImageRect(native.value(), tgfx::Rect::MakeWH(100 * scale, 100 * scale),
                        tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
                });
                compareRasterPixels(check, actual, reference);
            });
        }
        run(report, std::string("media.detail-picture-replay-retains-time-owner-final-transform.") + filename, [&](Check& check) {
            auto bytes = fixture(check, directory, filename);
            if (!check.passed) { return; }
            auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
            auto referenceMedia = ThorvgMedia::make(bytes.data(), bytes.size(), std::string(filename).find(".json") != std::string::npos);
            if (!check.result(animation, "Actual recorded media") || !check.result(referenceMedia, "Replay native reference")) { return; }
            tgfx::PictureRecorder recorder;
            auto* recording = recorder.beginRecording();
            recording->clipRect(tgfx::Rect::MakeWH(90, 140));
            animation.value()->draw(recording, Rect::makeXYWH(0, 0, 100, 140), Duration::fromSeconds(0.25),
                                     FittingSizeModeCenterScaleFit);
            auto picture = recorder.finishRecordingAsPicture();
            if (!check.expect(picture != nullptr, "Actual TGFX picture did not retain the deferred media operation")) { return; }
            // Advance the live media before replay, then drop the source span
            // and public owner. The picture must retain the earlier time/owner.
            animationPixels(check, animation.value(), 100, 100, 1.25);
            animation.value() = nullptr;
            bytes = {};
            for (const int scale : {2, 3}) {
                auto native = referenceMedia.value()->render(100 * scale, 100 * scale, Duration::fromSeconds(0.25));
                if (!check.result(native, "Replay reference at the final physical target size")) { return; }
                auto actual = gpuPixels(check, 100 * scale, 140 * scale, [&](NativeCanvas* canvas) {
                    canvas->clipRect(tgfx::Rect::MakeWH(100 * scale - 15, 140 * scale));
                    canvas->scale(scale, scale);
                    canvas->drawPicture(picture);
                    check.expect(!canvas->hasError(), "Deferred picture replay reported native render failure");
                });
                auto reference = gpuPixels(check, 100 * scale, 140 * scale, [&](NativeCanvas* canvas) {
                    canvas->clipRect(tgfx::Rect::MakeWH(100 * scale - 15, 140 * scale));
                    canvas->scale(scale, scale);
                    canvas->clipRect(tgfx::Rect::MakeWH(90, 140));
                    canvas->drawImageRect(native.value(), tgfx::Rect::MakeXYWH(0, 20, 100, 100),
                        tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
                });
                compareRasterPixels(check, actual, reference);
            }
        });
        for (const bool perspective : {false, true}) {
            run(report, std::string(perspective ? "media.detail-finite-perspective." : "media.detail-anisotropic-skew.") + filename,
                [&](Check& check) {
                    auto bytes = fixture(check, directory, filename);
                    if (!check.passed) { return; }
                    auto animation = AnimatedImage::make(nullptr, bytes.data(), bytes.size());
                    auto referenceMedia = ThorvgMedia::make(bytes.data(), bytes.size(), std::string(filename).find(".json") != std::string::npos);
                    if (!check.result(animation, "Transformed actual animation") || !check.result(referenceMedia, "Transformed native reference")) { return; }
                    // Skew columns have lengths 2 and sqrt(3^2 + .75^2).
                    // The finite perspective uses exact binary g=1/1024;
                    // its maximum Jacobian-column bounds yield 201x220.
                    const auto matrix = perspective ? tgfx::Matrix::MakeAll(2, 0, 0, 0, 2, 0, 1.0f / 1024, 0, 1)
                                                    : tgfx::Matrix::MakeAll(2, .75f, 0, 0, 3, 0);
                    auto native = referenceMedia.value()->render(perspective ? 201 : 200, perspective ? 220 : 310,
                                                                  Duration::fromSeconds(0.25));
                    if (!check.result(native, "Native raster matching the effective transformed sampling density")) { return; }
                    auto actual = gpuPixels(check, 300, 320, [&](NativeCanvas* canvas) {
                        canvas->concat(matrix);
                        animation.value()->draw(canvas, Rect::makeXYWH(0, 0, 100, 100), Duration::fromSeconds(0.25), FittingSizeModeFill);
                        check.expect(!canvas->hasError(), "Finite skew/perspective media draw reported failure");
                    });
                    auto reference = gpuPixels(check, 300, 320, [&](NativeCanvas* canvas) {
                        canvas->concat(matrix);
                        canvas->drawImageRect(native.value(), tgfx::Rect::MakeWH(100, 100),
                            tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
                    });
                    compareRasterPixels(check, actual, reference);
                });
        }
    }
    run(report, "media.deferred-invalid-transform-and-capacity-report-native-failure", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "sharp-detail.svg");
        if (animation == nullptr) { return; }
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        for (const auto& matrix : {tgfx::Matrix::MakeScale(nan, 2), tgfx::Matrix::MakeScale(1e10f),
                                  tgfx::Matrix::MakeAll(2, 0, 0, 0, 2, 0, -1.0f / 64, 0, 1)}) {
            auto context = GrGraphicsContext::makeOffscreen();
            if (!check.expect(context != nullptr, "Cannot create actual context for deferred failure checks")) { return; }
            auto lock = context->lockContext();
            if (!check.expect(static_cast<bool>(*lock), "Cannot lock actual deferred failure context")) { return; }
            auto surface = tgfx::Surface::Make(lock->get(), 4, 4);
            if (!check.expect(surface != nullptr, "Cannot create actual deferred failure surface")) { return; }
            auto* canvas = surface->getCanvas();
            canvas->setMatrix(matrix);
            animation->draw(canvas, Rect::makeXYWH(0, 0, 100, 100), Duration(), FittingSizeModeFill);
            check.expect(canvas->hasError(), "Invalid/non-finite/over-capacity media transform did not mark native failure");
            DrawableSurfaceCanvas checked(surface.get(), 4, 4);
            check.expect(!checked.snapshot(), "Production snapshot published pixels from a failed deferred media draw");
        }
    });
    run(report, "media.deferred-empty-final-clip-avoids-oversized-raster", [&](Check& check) {
        auto animation = loadAnimation(check, directory, "sharp-detail.svg");
        if (animation == nullptr) { return; }
        auto pixels = gpuPixels(check, 4, 4, [&](NativeCanvas* canvas) {
            canvas->clipRect(tgfx::Rect::MakeEmpty());
            canvas->scale(1e10f, 1e10f);
            animation->draw(canvas, Rect::makeXYWH(0, 0, 100, 100), Duration(), FittingSizeModeFill);
            check.expect(!canvas->hasError(), "Empty final clip triggered a needless over-capacity raster failure");
        });
        pixel(check, pixels, 4, 0, 0, clear);
    });
}

} // namespace

MediaTestReport runMediaChecks(const std::string& fixtureDirectory) {
    MediaTestReport report;
    svgFontResolutionChecks(report, fixtureDirectory);
    thorvgFontLifetimeChecks(report, fixtureDirectory);
    nestedSvgFontChecks(report, fixtureDirectory);
    staticChecks(report, fixtureDirectory);
    codecAnimationChecks(report, fixtureDirectory);
    ownershipChecks(report);
    vectorMediaChecks(report, fixtureDirectory);
    errorChecks(report, fixtureDirectory);
    jpegRecoveryChecks(report, fixtureDirectory);
    svgCompatibilityChecks(report);
    mediaCapacityChecks(report, fixtureDirectory);
    mediaAllocationAdmissionChecks(report, fixtureDirectory);
    mediaRawDestinationAdmissionCheck(report);
    mediaDeferredRasterChecks(report, fixtureDirectory);
    report.success = report.failed == 0 && report.passed > 0;
    return report;
}
