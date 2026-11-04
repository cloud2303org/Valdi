#include "snap_drawing/cpp/Utils/TgfxMedia.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/BitmapUtils.hpp"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "tgfx/core/Color.h"
#include "tgfx/core/Pixmap.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace snap::drawing {
namespace {
// The pinned RawPixelCodec only supplies ImageBuffers directly for A8,
// RGBA8 and BGRA8. Its CPU conversion API supports more formats. Use the
// actual public base codec's native PixelBuffer path for those formats,
// while retaining the original codec for lossless CPU reads and data pins.
class MediaPixelCodec final : public tgfx::ImageCodec {
public:
    MediaPixelCodec(const tgfx::ImageInfo& info, std::shared_ptr<tgfx::ImageCodec> source)
        : tgfx::ImageCodec(info.width(), info.height(), tgfx::Orientation::TopLeft, info.colorSpace()),
          _info(info), _source(std::move(source)) {}
    bool isAlphaOnly() const override { return _source->isAlphaOnly(); }

protected:
    std::shared_ptr<tgfx::ImageBuffer> onMakeBuffer(bool tryHardware) const override {
        if (_info.alphaType() == tgfx::AlphaType::Premultiplied &&
            (_info.colorType() == tgfx::ColorType::ALPHA_8 ||
             _info.colorType() == tgfx::ColorType::RGBA_8888 ||
             _info.colorType() == tgfx::ColorType::BGRA_8888)) {
            return _source->makeBuffer(tryHardware);
        }
        return tgfx::ImageCodec::onMakeBuffer(tryHardware);
    }
    bool onReadPixels(tgfx::ColorType color, tgfx::AlphaType alpha, size_t stride,
                      std::shared_ptr<tgfx::ColorSpace> colorSpace, void* pixels) const override {
        return _source->readPixels(tgfx::ImageInfo::Make(width(), height(), color, alpha, stride,
                                                        std::move(colorSpace)), pixels);
    }

private:
    tgfx::ImageInfo _info;
    std::shared_ptr<tgfx::ImageCodec> _source;
};

std::shared_ptr<tgfx::ImageCodec> pixelCodec(const tgfx::ImageInfo& info,
                                            std::shared_ptr<tgfx::Data> data) {
    auto source = tgfx::ImageCodec::MakeFrom(info, std::move(data));
    return source ? std::make_shared<MediaPixelCodec>(info, std::move(source)) : nullptr;
}

// Conversion is numeric, not an ABI reinterpretation of either backend.
uint16_t floatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    uint32_t mantissa = bits & 0x7fffffu;
    int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    if (((bits >> 23) & 0xffu) == 255u) {
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        const uint32_t rounded = (mantissa + ((1u << (shift - 1)) - 1u) +
                                  ((mantissa >> shift) & 1u)) >> shift;
        return static_cast<uint16_t>(sign | rounded);
    }
    mantissa += 0xfffu + ((mantissa >> 13) & 1u);
    if (mantissa & 0x800000u) {
        mantissa = 0;
        if (++exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7c00u);
        }
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float halfToFloat(uint16_t value) {
    const float magnitude = (value & 0x7c00u) == 0x7c00u
                                ? ((value & 0x3ffu) ? std::numeric_limits<float>::quiet_NaN()
                                                   : std::numeric_limits<float>::infinity())
                                : std::ldexp(static_cast<float>((value & 0x3ffu) +
                                                                ((value & 0x7c00u) ? 1024u : 0u)),
                                             ((value >> 10) & 31u)
                                                 ? static_cast<int>((value >> 10) & 31u) - 25 : -24);
    return (value & 0x8000u) ? -magnitude : magnitude;
}

bool readOnContext(const NativeImage& image,
                   const Valdi::BitmapInfo& info,
                   void* pixels,
                   tgfx::Context* context) {
    // Materialize on the requested context before drawing. This also rejects
    // a direct texture image whose owning context does not match.
    auto texture = image->makeTextureImage(context);
    if (!texture) { return false; }
    // The pinned GPU PixelFormat supports 8-bit render targets only. Surface
    // readPixels performs real CPU conversion into Gray/A8/F16 destinations.
    auto surface = tgfx::Surface::Make(context, info.width, info.height, tgfx::ColorType::RGBA_8888);
    if (!surface) {
        return false;
    }
    surface->getCanvas()->clear(tgfx::Color::Transparent());
    surface->getCanvas()->drawImageRect(texture, tgfx::Rect::MakeWH(info.width, info.height),
                                        tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
    if (info.colorType != Valdi::ColorTypeRGBAF32) {
        return surface->readPixels(toSkiaImageInfo(info), pixels);
    }
    auto halfInfo = tgfx::ImageInfo::Make(info.width, info.height, tgfx::ColorType::RGBA_F16,
                                         info.alphaType == Valdi::AlphaTypeUnpremul
                                             ? tgfx::AlphaType::Unpremultiplied
                                             : (info.alphaType == Valdi::AlphaTypeOpaque
                                                    ? tgfx::AlphaType::Opaque : tgfx::AlphaType::Premultiplied));
    std::vector<uint16_t> halfPixels(halfInfo.byteSize() / sizeof(uint16_t));
    if (!surface->readPixels(halfInfo, halfPixels.data())) {
        return false;
    }
    for (int y = 0; y < info.height; ++y) {
        auto* row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * info.rowBytes;
        for (int x = 0; x < info.width * 4; ++x) {
            const auto value = halfToFloat(halfPixels[static_cast<size_t>(y) * info.width * 4 + x]);
            std::memcpy(row + static_cast<size_t>(x) * sizeof(float), &value, sizeof(value));
        }
    }
    return true;
}
} // namespace

std::shared_ptr<tgfx::Data> tgfxMediaData(const Valdi::BytesView& bytes, bool copy) {
    return skDataFromBytes(bytes, copy ? DataConversionModeAlwaysCopy : DataConversionModeNeverCopy);
}

Valdi::BytesView tgfxMediaBytes(const std::shared_ptr<tgfx::Data>& data) {
    return bytesFromSkData(data);
}

bool tgfxMediaValidBitmapInfo(const Valdi::BitmapInfo& info) {
    if (!tgfx::ImageInfo::IsValidSize(info.width, info.height) ||
        info.colorType == Valdi::ColorTypeUnknown ||
        (info.alphaType != Valdi::AlphaTypePremul && info.alphaType != Valdi::AlphaTypeUnpremul &&
         info.alphaType != Valdi::AlphaTypeOpaque)) {
        return false;
    }
    const auto bpp = Valdi::BitmapInfo::bytesPerPixelForColorType(info.colorType);
    return bpp && static_cast<size_t>(info.width) <= std::numeric_limits<size_t>::max() / bpp &&
           info.rowBytes >= static_cast<size_t>(info.width) * bpp &&
           info.rowBytes <= std::numeric_limits<size_t>::max() / static_cast<size_t>(info.height);
}

std::shared_ptr<tgfx::ImageCodec> tgfxMediaCodec(const Valdi::BitmapInfo& info,
                                               std::shared_ptr<tgfx::Data> data) {
    if (!tgfxMediaValidBitmapInfo(info) || !data || data->size() < info.rowBytes * info.height) {
        return nullptr;
    }
    if (info.colorType != Valdi::ColorTypeRGBAF32) {
        return pixelCodec(toSkiaImageInfo(info), std::move(data));
    }
    auto halfInfo = tgfx::ImageInfo::Make(info.width, info.height, tgfx::ColorType::RGBA_F16,
                                         info.alphaType == Valdi::AlphaTypeUnpremul
                                             ? tgfx::AlphaType::Unpremultiplied
                                             : (info.alphaType == Valdi::AlphaTypeOpaque
                                                    ? tgfx::AlphaType::Opaque : tgfx::AlphaType::Premultiplied));
    auto* converted = static_cast<uint16_t*>(std::malloc(halfInfo.byteSize()));
    if (!converted) {
        return nullptr;
    }
    for (int y = 0; y < info.height; ++y) {
        const auto* row = data->bytes() + static_cast<size_t>(y) * info.rowBytes;
        for (int x = 0; x < info.width * 4; ++x) {
            float value;
            std::memcpy(&value, row + static_cast<size_t>(x) * sizeof(float), sizeof(value));
            converted[static_cast<size_t>(y) * info.width * 4 + x] = floatToHalf(value);
        }
    }
    // Conversion needs new storage, but keep the original no-copy owner pinned
    // for the returned image's entire lifetime, including an IBitmap lock.
    auto* original = new std::shared_ptr<tgfx::Data>(std::move(data));
    auto halfData = tgfx::Data::MakeAdopted(converted, halfInfo.byteSize(),
        [](const void* pixels, void* context) {
            std::free(const_cast<void*>(pixels));
            delete static_cast<std::shared_ptr<tgfx::Data>*>(context);
        }, original);
    return pixelCodec(halfInfo, std::move(halfData));
}

NativeImage tgfxMediaImage(const Valdi::BitmapInfo& info, std::shared_ptr<tgfx::Data> data) {
    auto codec = tgfxMediaCodec(info, std::move(data));
    return codec ? tgfx::Image::MakeFrom(std::move(codec)) : nullptr;
}

namespace {
float mitchell(float x) {
    x = std::abs(x);
    if (x < 1) { return ((7.0f * x - 12.0f) * x * x + 16.0f / 3.0f) / 6.0f; }
    if (x < 2) { return (((-7.0f / 3.0f * x + 12.0f) * x - 20.0f) * x + 32.0f / 3.0f) / 6.0f; }
    return 0;
}
} // namespace

bool tgfxMediaCanReadPixels(const std::shared_ptr<tgfx::ImageCodec>& codec,
                           const Valdi::BitmapInfo& info) {
    if (!codec || !tgfxMediaValidBitmapInfo(info) ||
        !tgfx::ImageInfo::IsValidSize(codec->width(), codec->height())) { return false; }
    constexpr size_t capacity = 128 * 1024 * 1024;
    size_t remaining = capacity - 1024 * 1024;
    auto admit = [&](size_t width, size_t height, size_t bytesPerPixel) {
        if (!width || !height || !bytesPerPixel || width > remaining / bytesPerPixel) { return false; }
        const auto rowBytes = width * bytesPerPixel;
        if (height > remaining / rowBytes) { return false; }
        remaining -= rowBytes * height;
        return true;
    };
    const auto sourceWidth = static_cast<size_t>(codec->width());
    const auto sourceHeight = static_cast<size_t>(codec->height());
    const auto width = static_cast<size_t>(info.width);
    const auto height = static_cast<size_t>(info.height);
    // The PNG/other codecs can stage RGBA8 and row pointers while filling
    // source F16. Filtering retains source and destination F16 concurrently;
    // F32 output also needs an alpha-converted destination F16 buffer.
    return admit(info.rowBytes, height, 1) &&
           admit(sourceWidth, sourceHeight, 8) &&
           admit(sourceWidth, sourceHeight, 4) &&
           admit(sourceHeight, 1, sizeof(void*)) &&
           admit(width, height, 8) &&
           (info.colorType != Valdi::ColorTypeRGBAF32 || admit(width, height, 8));
}

bool tgfxMediaReadPixels(const std::shared_ptr<tgfx::ImageCodec>& codec,
                        const Valdi::BitmapInfo& info,
                        void* pixels) {
    if (!pixels || !tgfxMediaCanReadPixels(codec, info)) { return false; }
    // Decode through TGFX into premultiplied F16; numeric filtering never
    // interpolates unpremultiplied RGB across transparent edges.
    auto sourceInfo = tgfx::ImageInfo::Make(codec->width(), codec->height(), tgfx::ColorType::RGBA_F16,
                                           tgfx::AlphaType::Premultiplied, 0, tgfx::ColorSpace::SRGB());
    if (sourceInfo.isEmpty() ||
        sourceInfo.rowBytes() > std::numeric_limits<size_t>::max() / static_cast<size_t>(sourceInfo.height())) {
        return false;
    }
    std::vector<uint16_t> source(sourceInfo.byteSize() / sizeof(uint16_t));
    if (!codec->readPixels(sourceInfo, source.data())) { return false; }
    const auto orientation = codec->orientation();
    const bool swapped = tgfx::OrientationSwapsWidthHeight(orientation);
    const int orientedWidth = swapped ? codec->height() : codec->width();
    const int orientedHeight = swapped ? codec->width() : codec->height();
    auto sample = [&](int x, int y, int channel) {
        x = std::clamp(x, 0, orientedWidth - 1);
        y = std::clamp(y, 0, orientedHeight - 1);
        int sx = x, sy = y;
        switch (orientation) {
            case tgfx::Orientation::TopRight: sx = codec->width() - 1 - x; break;
            case tgfx::Orientation::BottomRight:
                sx = codec->width() - 1 - x; sy = codec->height() - 1 - y; break;
            case tgfx::Orientation::BottomLeft: sy = codec->height() - 1 - y; break;
            case tgfx::Orientation::LeftTop: sx = y; sy = x; break;
            case tgfx::Orientation::RightTop: sx = y; sy = codec->height() - 1 - x; break;
            case tgfx::Orientation::RightBottom:
                sx = codec->width() - 1 - y; sy = codec->height() - 1 - x; break;
            case tgfx::Orientation::LeftBottom: sx = codec->width() - 1 - y; sy = x; break;
            default: break;
        }
        return halfToFloat(source[(static_cast<size_t>(sy) * codec->width() + sx) * 4 + channel]);
    };
    auto scaledInfo = tgfx::ImageInfo::Make(info.width, info.height, tgfx::ColorType::RGBA_F16,
                                           tgfx::AlphaType::Premultiplied, 0, tgfx::ColorSpace::SRGB());
    std::vector<uint16_t> scaled(scaledInfo.byteSize() / sizeof(uint16_t));
    const bool sameSize = orientedWidth == info.width && orientedHeight == info.height;
    for (int y = 0; y < info.height; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * orientedHeight / info.height - 0.5f;
        const int iy = static_cast<int>(std::floor(sy));
        for (int x = 0; x < info.width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * orientedWidth / info.width - 0.5f;
            const int ix = static_cast<int>(std::floor(sx));
            for (int c = 0; c < 4; ++c) {
                float value = 0;
                if (sameSize) {
                    value = sample(x, y, c);
                } else {
                    for (int dy = -1; dy <= 2; ++dy) {
                        for (int dx = -1; dx <= 2; ++dx) {
                            value += sample(ix + dx, iy + dy, c) *
                                     mitchell(sx - (ix + dx)) * mitchell(sy - (iy + dy));
                        }
                    }
                }
                if (c == 3) { value = std::clamp(value, 0.0f, 1.0f); }
                scaled[(static_cast<size_t>(y) * info.width + x) * 4 + c] = floatToHalf(value);
            }
        }
    }
    tgfx::Pixmap pixmap(scaledInfo, scaled.data());
    if (info.colorType != Valdi::ColorTypeRGBAF32) {
        return pixmap.readPixels(toSkiaImageInfo(info).makeColorSpace(tgfx::ColorSpace::SRGB()), pixels);
    }
    auto convertedInfo = scaledInfo.makeAlphaType(info.alphaType == Valdi::AlphaTypeUnpremul
        ? tgfx::AlphaType::Unpremultiplied
        : info.alphaType == Valdi::AlphaTypeOpaque ? tgfx::AlphaType::Opaque : tgfx::AlphaType::Premultiplied);
    std::vector<uint16_t> converted(scaled.size());
    if (!pixmap.readPixels(convertedInfo, converted.data())) { return false; }
    for (int y = 0; y < info.height; ++y) {
        auto* row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * info.rowBytes;
        for (int x = 0; x < info.width * 4; ++x) {
            const auto value = halfToFloat(converted[static_cast<size_t>(y) * info.width * 4 + x]);
            std::memcpy(row + static_cast<size_t>(x) * sizeof(float), &value, sizeof(value));
        }
    }
    return true;
}

bool tgfxMediaReadPixels(const NativeImage& image,
                        const Valdi::BitmapInfo& info,
                        void* pixels,
                        tgfx::Context* context) {
    if (!image || !pixels || !tgfxMediaValidBitmapInfo(info)) {
        return false;
    }
    if (context) {
        return readOnContext(image, info, pixels, context);
    }
    // Use the GPU owner's platform seam: real Metal on Apple, EGL/OpenGL on
    // other hosts. Its recursive device lock also handles readback nested
    // inside composition without relocking TGFX's non-recursive Device.
    auto offscreen = GrGraphicsContext::makeOffscreen();
    if (offscreen == nullptr) { return false; }
    auto lock = offscreen->lockContext();
    return *lock && readOnContext(image, info, pixels, lock->get());
}
} // namespace snap::drawing
#endif
