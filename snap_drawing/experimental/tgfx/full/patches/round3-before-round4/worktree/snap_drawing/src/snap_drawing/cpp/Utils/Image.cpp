#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/Bitmap.hpp"
#include "snap_drawing/cpp/Utils/BitmapFactory.hpp"
#include "snap_drawing/cpp/Utils/BitmapUtils.hpp"
#include "snap_drawing/cpp/Utils/SVGUtils.hpp"
#include "snap_drawing/cpp/Utils/TgfxMedia.hpp"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#include "snap_drawing/cpp/Utils/TgfxMediaAnimation.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "tgfx/core/ImageCodec.h"
#include "tgfx/core/Pixmap.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace snap::drawing {
Image::Image(const NativeImage& image) : _skImage(image) {}
Image::Image(const NativeImage& image, std::shared_ptr<tgfx::Device> device)
    : _readbackDevice(std::move(device)), _skImage(image) {}
Image::~Image() = default;
void Image::initializeCodecs() {} // TGFX codec registration is selected by its build options.

Valdi::Value Image::getMetadata() const {
    return Valdi::Value()
        .setMapValue("type", Valdi::Value(Valdi::StringBox::fromCString("image")))
        .setMapValue("width", Valdi::Value(static_cast<int32_t>(width())))
        .setMapValue("height", Valdi::Value(static_cast<int32_t>(height())));
}
const NativeImage& Image::getSkValue() const { return _skImage; }
int Image::width() const { return _skImage->width(); }
int Image::height() const { return _skImage->height(); }
Valdi::Result<Valdi::BytesView> Image::toPNG() const { return encode(EncodedImageFormatPNG, 1.0); }

std::shared_ptr<tgfx::Data> Image::encodeSKImageToSKData(tgfx::Image& image,
                                                      EncodedImageFormat format,
                                                      int quality,
                                                      tgfx::Context* context) {
    // makeScaled at identical dimensions returns the image's actual shared owner.
    auto retainedImage = image.makeScaled(image.width(), image.height());
    Valdi::BitmapInfo info(image.width(), image.height(), Valdi::ColorTypeRGBA8888,
                           Valdi::AlphaTypeUnpremul, static_cast<size_t>(image.width()) * 4);
    if (!tgfxMediaValidBitmapInfo(info)) { return nullptr; }
    std::vector<uint8_t> pixels(info.rowBytes * info.height);
    if (!tgfxMediaReadPixels(retainedImage, info, pixels.data(), context)) { return nullptr; }
    tgfx::EncodedFormat nativeFormat;
    switch (format) {
        case EncodedImageFormatPNG: nativeFormat = tgfx::EncodedFormat::PNG; break;
        case EncodedImageFormatJPG: nativeFormat = tgfx::EncodedFormat::JPEG; break;
        case EncodedImageFormatWebP: nativeFormat = tgfx::EncodedFormat::WEBP; break;
        default: return nullptr;
    }
    return tgfx::ImageCodec::Encode(tgfx::Pixmap(toSkiaImageInfo(info), pixels.data()),
                                    nativeFormat, std::clamp(quality, 0, 100));
}

Valdi::Result<Valdi::BytesView> Image::encode(EncodedImageFormat format, double qualityRatio) const {
    if (!std::isfinite(qualityRatio)) { return Valdi::Error("Invalid image quality"); }
    std::shared_ptr<tgfx::Data> encoded;
    const auto quality = static_cast<int>(std::round(100 * std::clamp(qualityRatio, 0.0, 1.0)));
    if (_codec || _readbackDevice) {
        Valdi::BitmapInfo info(width(), height(), Valdi::ColorTypeRGBA8888, Valdi::AlphaTypeUnpremul,
                               static_cast<size_t>(width()) * 4);
        std::vector<uint8_t> pixels(info.rowBytes * info.height);
        if (!readPixels(info, pixels.data())) { return Valdi::Error("Cannot read TGFX pixels"); }
        tgfx::EncodedFormat nativeFormat;
        switch (format) {
            case EncodedImageFormatPNG: nativeFormat = tgfx::EncodedFormat::PNG; break;
            case EncodedImageFormatJPG: nativeFormat = tgfx::EncodedFormat::JPEG; break;
            case EncodedImageFormatWebP: nativeFormat = tgfx::EncodedFormat::WEBP; break;
            default: return Valdi::Error("Unknown encoded image format");
        }
        encoded = tgfx::ImageCodec::Encode(tgfx::Pixmap(toSkiaImageInfo(info), pixels.data()), nativeFormat, quality);
    } else {
        encoded = encodeSKImageToSKData(*_skImage, format, quality);
    }
    if (!encoded) { return Valdi::Error("Cannot read or encode TGFX image"); }
    return tgfxMediaBytes(encoded);
}

Ref<Image> Image::resized(int width, int height) const {
    const auto color = _bitmapInfo ? _bitmapInfo->colorType : Valdi::ColorTypeRGBA8888;
    const auto alpha = _bitmapInfo ? _bitmapInfo->alphaType : Valdi::AlphaTypePremul;
    Valdi::BitmapInfo info(width, height, color, alpha,
                           width > 0 ? static_cast<size_t>(width) *
                                           Valdi::BitmapInfo::bytesPerPixelForColorType(color) : 0);
    auto bitmap = Bitmap::make(info);
    if (!bitmap) { return nullptr; }
    auto* pixels = bitmap.value()->lockBytes();
    auto success = readPixels(info, pixels);
    bitmap.value()->unlockBytes();
    if (!success) { return nullptr; }
    auto image = makeFromBitmap(bitmap.moveValue(), false);
    return image ? image.moveValue() : nullptr;
}
void Image::draw(const Valdi::BitmapInfo& info, void* bytes) {
    const auto success = readPixels(info, bytes);
    if (!success) {
        Valdi::ConsoleLogger::getErrorLogger().log(Valdi::LogTypeError, "Cannot draw TGFX image into bitmap bytes");
    }
}
Ref<Image> Image::withFilter(const Ref<Valdi::ImageFilter>& filter) {
    auto image = Valdi::makeShared<Image>(_skImage);
    image->_filter = filter;
    image->_codec = _codec;
    image->_bitmapInfo = _bitmapInfo;
    image->_rawPixelData = _rawPixelData;
    image->_rawPixelInfo = _rawPixelInfo;
    image->_readbackDevice = _readbackDevice;
    image->_sourceImage = Valdi::strongSmallRef(this);
    return image;
}
const Ref<Valdi::ImageFilter>& Image::getFilter() const { return _filter; }

Valdi::Result<Ref<Image>> Image::make(const Valdi::BytesView& data) {
    if (!data.data() || data.empty()) {
        const auto message = describeUndecodablePayload(data, "Unable to decode image");
        return Valdi::Error(std::string_view(message));
    }
    if (isSVG(data)) { return makeFromSVG(data); }
    // Source-less encoded input has no owner to pin. Copy it before lazy decode.
    auto encoded = skDataFromBytes(data, DataConversionModeCopyIfUnsafe);
    auto codec = tgfx::ImageCodec::MakeFrom(encoded);
    auto native = codec ? tgfx::Image::MakeFrom(codec) : nullptr;
    if (!native) {
        // giflib also supplies still GIF on platforms without a native GIF codec.
        auto decoded = tgfxMediaDecodeFrames(data);
        if (!decoded) {
            const auto message = describeUndecodablePayload(data, "Unable to decode image");
            return decoded.error().rethrow(std::string_view(message));
        }
        auto first = std::move(decoded.value().frames.front());
        auto image = Valdi::makeShared<Image>(first.image);
        image->_codec = std::move(first.codec);
        return image;
    }
    auto image = Valdi::makeShared<Image>(native);
    image->_codec = std::move(codec);
    return image;
}
bool Image::isSVG(const Valdi::BytesView& data) { return snap::drawing::isSVG(data); }
Valdi::Result<Ref<Image>> Image::makeFromSVG(const Valdi::BytesView& data, int width, int height) {
    auto bitmap = rasterizeSVG(data, BitmapFactory::getInstance(Valdi::ColorTypeRGBA8888), width, height);
    if (!bitmap) { return bitmap.moveError(); }
    return makeFromBitmap(bitmap.moveValue(), false);
}
Valdi::Result<Ref<Image>> Image::makeFromPixelsData(const Valdi::BitmapInfo& info,
                                                  const Valdi::BytesView& data,
                                                  bool copy) {
    if (!tgfxMediaValidBitmapInfo(info) || !data.data() || data.size() < info.rowBytes * info.height) {
        return Valdi::Error("Invalid raw image size, stride or pixel buffer");
    }
    return makeFromPixelsData(info, tgfxMediaData(data, copy));
}
Valdi::Result<Ref<Image>> Image::makeFromPixelsData(const Valdi::BitmapInfo& info,
                                                  const std::shared_ptr<tgfx::Data>& data) {
    auto codec = tgfxMediaCodec(info, data);
    auto image = codec ? tgfx::Image::MakeFrom(codec) : nullptr;
    if (!image) { return Valdi::Error("Unable to create TGFX image from pixels"); }
    auto result = Valdi::makeShared<Image>(image);
    result->_codec = std::move(codec);
    result->_bitmapInfo = info;
    result->_bitmapInfo->rowBytes = static_cast<size_t>(info.width) *
                                   Valdi::BitmapInfo::bytesPerPixelForColorType(info.colorType);
    result->_rawPixelInfo = info;
    result->_rawPixelData = data;
    return result;
}
Valdi::Result<Ref<Image>> Image::makeFromBitmap(const Valdi::Ref<Valdi::IBitmap>& bitmap, bool copy) {
    if (bitmap == nullptr) { return Valdi::Error("Missing bitmap"); }
    const auto info = bitmap->getInfo();
    auto data = bitmapToData(bitmap, info, copy);
    if (!data) { return data.moveError(); }
    return makeFromPixelsData(info, data.value());
}
Valdi::Ref<Valdi::IBitmap> Image::getBitmap() {
    if (_bitmapInfo) { return toConvertedBitmap(*_bitmapInfo); }
    return toConvertedBitmap(Valdi::BitmapInfo(width(), height(), Valdi::ColorTypeRGBA8888,
                                              Valdi::AlphaTypePremul, static_cast<size_t>(width()) * 4));
}
Valdi::Ref<Valdi::IBitmap> Image::toConvertedBitmap(const Valdi::BitmapInfo& info) {
    auto bitmap = Bitmap::make(info);
    if (!bitmap) { return nullptr; }
    auto output = bitmap.moveValue();
    auto* pixels = output->lockBytes();
    bool success = readPixels(output->getInfo(), pixels);
    output->unlockBytes();
    return success ? Valdi::Ref<Valdi::IBitmap>(output) : nullptr;
}

bool Image::readPixels(const Valdi::BitmapInfo& info, void* pixels) const {
    if (!pixels || !tgfxMediaValidBitmapInfo(info)) { return false; }
    if (_rawPixelData && _rawPixelInfo && _rawPixelInfo->width == info.width &&
        _rawPixelInfo->height == info.height && _rawPixelInfo->colorType == info.colorType &&
        _rawPixelInfo->alphaType == info.alphaType) {
        const auto bytes = static_cast<size_t>(info.width) *
                           Valdi::BitmapInfo::bytesPerPixelForColorType(info.colorType);
        for (int y = 0; y < info.height; ++y) {
            std::memcpy(static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * info.rowBytes,
                        _rawPixelData->bytes() + static_cast<size_t>(y) * _rawPixelInfo->rowBytes, bytes);
        }
        return true;
    }
    if (_codec) { return tgfxMediaReadPixels(_codec, info, pixels); }
    if (_readbackDevice) {
        GrGraphicsContext readback(_readbackDevice, nullptr);
        auto lock = readback.lockContext();
        return *lock && tgfxMediaReadPixels(_skImage, info, pixels, lock->get());
    }
    return tgfxMediaReadPixels(_skImage, info, pixels);
}
VALDI_CLASS_IMPL(Image)
} // namespace snap::drawing
#else
//
//  Image.cpp
//  valdi-desktop-apple
//
//  Created by Simon Corsin on 7/7/20.
//

#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"

#include "snap_drawing/cpp/Utils/Bitmap.hpp"
#include "snap_drawing/cpp/Utils/BitmapFactory.hpp"
#include "snap_drawing/cpp/Utils/SVGUtils.hpp"

#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Utils/ValueTypedArray.hpp"

#include "snap_drawing/cpp/Utils/BitmapUtils.hpp"

#include "include/codec/SkEncodedImageFormat.h"
#include "include/codec/SkGifDecoder.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkWebpDecoder.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "modules/svg/include/SkSVGDOM.h"
#include "src/image/SkImage_Base.h"

#include <algorithm>
#include <cmath>

namespace snap::drawing {

Image::Image(const sk_sp<SkImage>& skImage) : _skImage(skImage) {}

Image::~Image() = default;

void Image::initializeCodecs() {
    static std::once_flag flag;
    std::call_once(flag, [&]() {
        SkCodecs::Register(SkGifDecoder::Decoder());
        SkCodecs::Register(SkJpegDecoder::Decoder());
        SkCodecs::Register(SkPngDecoder::Decoder());
        SkCodecs::Register(SkWebpDecoder::Decoder());
    });
}

Valdi::Value Image::getMetadata() const {
    return Valdi::Value()
        .setMapValue("type", Valdi::Value(Valdi::StringBox::fromCString("image")))
        .setMapValue("width", Valdi::Value(static_cast<int32_t>(_skImage->width())))
        .setMapValue("height", Valdi::Value(static_cast<int32_t>(_skImage->height())));
}

const sk_sp<SkImage>& Image::getSkValue() const {
    return _skImage;
}

int Image::width() const {
    return _skImage->width();
}

int Image::height() const {
    return _skImage->height();
}

Valdi::Result<Valdi::BytesView> Image::toPNG() const {
    return encode(EncodedImageFormatPNG, 1.0);
}

static bool encodeImage(SkWStream* dst, const SkPixmap& src, EncodedImageFormat format, int quality) {
    switch (format) {
        case EncodedImageFormatJPG: {
            SkJpegEncoder::Options opts;
            opts.fQuality = quality;
            return SkJpegEncoder::Encode(dst, src, opts);
        }
        case EncodedImageFormatPNG: {
            SkPngEncoder::Options opts;
            return SkPngEncoder::Encode(dst, src, opts);
        }
        case EncodedImageFormatWebP: {
            SkWebpEncoder::Options opts;
            if (quality == 100) {
                opts.fCompression = SkWebpEncoder::Compression::kLossless;
                // Note: SkEncodeImage treats 0 quality as the lowest quality
                // (greatest compression) and 100 as the highest quality (least
                // compression). For kLossy, this matches libwebp's
                // interpretation, so it is passed directly to libwebp. But
                // with kLossless, libwebp always creates the highest quality
                // image. In this case, fQuality is reinterpreted as how much
                // effort (time) to put into making a smaller file. This API
                // does not provide a way to specify this value (though it can
                // be specified by using SkWebpEncoder::Encode) so we have to
                // pick one arbitrarily. This value matches that chosen by
                // blink::ImageEncoder::ComputeWebpOptions as well
                // WebPConfigInit.
                opts.fQuality = 75;
            } else {
                opts.fCompression = SkWebpEncoder::Compression::kLossy;
                opts.fQuality = quality;
            }
            return SkWebpEncoder::Encode(dst, src, opts);
        }
    }
}

sk_sp<SkData> Image::encodeSKImageToSKData(SkImage& image, EncodedImageFormat format, int quality) {
    Image::initializeCodecs();
    SkBitmap bm;
    if (!as_IB(&image)->getROPixels(nullptr, &bm)) {
        return nullptr;
    }

    SkPixmap pixmap;
    if (!bm.peekPixels(&pixmap)) {
        return nullptr;
    }

    SkDynamicMemoryWStream stream;
    if (!encodeImage(&stream, pixmap, format, quality)) {
        return nullptr;
    }
    return stream.detachAsData();
}

Valdi::Result<Valdi::BytesView> Image::encode(EncodedImageFormat format, double qualityRatio) const {
    auto quality = static_cast<int>(round(100.0 * qualityRatio));

    auto encoded = Image::encodeSKImageToSKData(*_skImage, format, quality);
    if (encoded == nullptr) {
        return Valdi::Error("Cannot encode image");
    }

    return bytesFromSkData(encoded);
}

Ref<Image> Image::resized(int width, int height) const {
    SkBitmap bitmap;

    SkImageInfo imageInfo =
        SkImageInfo::Make(width, height, _skImage->colorType(), _skImage->alphaType(), _skImage->refColorSpace());
    bitmap.allocPixels(imageInfo, imageInfo.minRowBytes());

    _skImage->scalePixels(bitmap.pixmap(), SkSamplingOptions(SkCubicResampler::Mitchell()));

    bitmap.setImmutable();

    auto skImage = SkImages::RasterFromBitmap(bitmap);
    return Ref<Image>(Valdi::makeShared<Image>(skImage));
}

void Image::draw(const Valdi::BitmapInfo& bitmapInfo, void* bytes) {
    SkPixmap pixmap(toSkiaImageInfo(bitmapInfo), bytes, bitmapInfo.rowBytes);
    _skImage->scalePixels(pixmap, SkSamplingOptions(SkCubicResampler::Mitchell()));
}

Ref<Image> Image::withFilter(const Ref<Valdi::ImageFilter>& filter) {
    auto copiedImage = Valdi::makeShared<Image>(_skImage);
    copiedImage->_filter = filter;
    copiedImage->_sourceImage = Valdi::strongSmallRef(this);
    return copiedImage;
}

const Ref<Valdi::ImageFilter>& Image::getFilter() const {
    return _filter;
}

Valdi::Result<Ref<Image>> Image::make(const Valdi::BytesView& data) {
    if (isSVG(data)) {
        return makeFromSVG(data);
    }

    Image::initializeCodecs();
    auto skData = skDataFromBytes(data, DataConversionModeNeverCopy);

    auto skImage = SkImages::DeferredFromEncodedData(skData);

    if (skImage == nullptr) {
        const auto message = describeUndecodablePayload(data, "Unable to decode image");
        return Valdi::Error(std::string_view(message));
    }

    return Ref<Image>(Valdi::makeShared<Image>(skImage));
}

bool Image::isSVG(const Valdi::BytesView& data) {
    return snap::drawing::isSVG(data);
}

Valdi::Result<Ref<Image>> Image::makeFromSVG(const Valdi::BytesView& data, int preferredWidth, int preferredHeight) {
    auto bitmap = rasterizeSVG(
        data, BitmapFactory::getInstance(Valdi::ColorType::ColorTypeRGBA8888), preferredWidth, preferredHeight);
    if (!bitmap) {
        return bitmap.moveError();
    }

    return makeFromBitmap(bitmap.moveValue(), false);
}

Valdi::Result<Ref<Image>> Image::makeFromPixelsData(const Valdi::BitmapInfo& bitmapInfo,
                                                    const Valdi::BytesView& pixelsData,
                                                    bool shouldCopy) {
    return makeFromPixelsData(
        bitmapInfo,
        skDataFromBytes(pixelsData, shouldCopy ? DataConversionModeAlwaysCopy : DataConversionModeNeverCopy));
}

Valdi::Result<Ref<Image>> Image::makeFromPixelsData(const Valdi::BitmapInfo& bitmapInfo,
                                                    const sk_sp<SkData>& pixelsData) {
    auto imageInfo = snap::drawing::toSkiaImageInfo(bitmapInfo);

    auto skImage = SkImages::RasterFromData(imageInfo, pixelsData, bitmapInfo.rowBytes);

    if (skImage == nullptr) {
        return Valdi::Error("Unable to create image");
    }

    return Ref<Image>(Valdi::makeShared<Image>(skImage));
}

Valdi::Result<Ref<Image>> Image::makeFromBitmap(const Valdi::Ref<Valdi::IBitmap>& bitmap, bool shouldCopy) {
    auto info = bitmap->getInfo();

    auto data = bitmapToData(bitmap, info, shouldCopy);
    if (!data) {
        return data.moveError();
    }

    return makeFromPixelsData(info, data.value());
}

Valdi::Ref<Valdi::IBitmap> Image::getBitmap() {
    auto imageInfo = _skImage->imageInfo();
    if (imageInfo.colorType() == kUnknown_SkColorType) {
        return nullptr;
    }

    SkBitmap bitmap;
    bitmap.allocPixels(_skImage->imageInfo(), imageInfo.minRowBytes());
    auto pixmap = bitmap.pixmap();
    _skImage->readPixels(imageInfo, pixmap.writable_addr(), imageInfo.minRowBytes(), 0, 0);

    return Valdi::makeShared<snap::drawing::Bitmap>(std::move(bitmap));
}

Valdi::Ref<Valdi::IBitmap> Image::toConvertedBitmap(const Valdi::BitmapInfo& bitmapInfo) {
    if (bitmapInfo.width < 0 || bitmapInfo.height < 0 || _skImage->colorType() == kUnknown_SkColorType) {
        return nullptr;
    }

    auto imageInfo = toSkiaImageInfo(bitmapInfo).makeColorSpace(SkColorSpace::MakeSRGB());
    SkBitmap bitmap;
    if (!tryAllocBitmapPixels(bitmap, imageInfo, std::max(bitmapInfo.rowBytes, imageInfo.minRowBytes()))) {
        return nullptr;
    }

    if (!_skImage->scalePixels(bitmap.pixmap(), SkSamplingOptions(SkCubicResampler::Mitchell()))) {
        return nullptr;
    }

    return Valdi::makeShared<snap::drawing::Bitmap>(std::move(bitmap));
}

VALDI_CLASS_IMPL(Image)

} // namespace snap::drawing

#endif
