//
//  Image.hpp
//  valdi-desktop-apple
//
//  Created by Simon Corsin on 7/7/20.
//

#pragma once

#include "valdi_core/cpp/Attributes/ImageFilter.hpp"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Resources/LoadedAsset.hpp"
#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"
#include "valdi_core/cpp/Utils/ValdiObject.hpp"

#include "snap_drawing/cpp/Utils/Aliases.hpp"

#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Data.h"
#include "tgfx/core/ImageCodec.h"
#include "tgfx/gpu/Device.h"
#include <optional>
#else
#include "include/core/SkData.h"
#endif

namespace snap::drawing {

enum EncodedImageFormat { EncodedImageFormatJPG, EncodedImageFormatPNG, EncodedImageFormatWebP };

class Image : public Valdi::LoadedAsset {
public:
    explicit Image(const NativeImage& skImage);
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Capture the owning device for texture readback after the caller releases
    // its canvas lock. Never retain a borrowed tgfx::Context across threads.
    Image(const NativeImage& image, std::shared_ptr<tgfx::Device> readbackDevice);
#endif

    ~Image() override;

    int width() const;
    int height() const;

    Valdi::Value getMetadata() const override;

    static void initializeCodecs();

    /**
     Resize the image to the given width/height.
     This will read the entire bitmap of the Image and scale it
     in software, this is an expensive operation.
     */
    Ref<Image> resized(int width, int height) const;

    /**
     Draw the image into the given bytes buffer conforming to the given bitmap info,
     scaling pixels if necessary.
     */
    void draw(const Valdi::BitmapInfo& bitmapInfo, void* bytes);

    /**
     Returns a new Image which will be displayed with the given filter when drawn
     */
    Ref<Image> withFilter(const Ref<Valdi::ImageFilter>& filter);

    const Ref<Valdi::ImageFilter>& getFilter() const;

    Valdi::Result<Valdi::BytesView> toPNG() const;

    Valdi::Result<Valdi::BytesView> encode(EncodedImageFormat format, double qualityRatio) const;

    const NativeImage& getSkValue() const;

    Valdi::Ref<Valdi::IBitmap> getBitmap();

    Valdi::Ref<Valdi::IBitmap> toConvertedBitmap(const Valdi::BitmapInfo& bitmapInfo);

    /**
     Make an Image from bytes representing an encoded image, like in PNG or JPG format.
     */
    static Valdi::Result<Ref<Image>> make(const Valdi::BytesView& data);

    static bool isSVG(const Valdi::BytesView& data);

    static Valdi::Result<Ref<Image>> makeFromSVG(const Valdi::BytesView& data,
                                                 int preferredWidth = 0,
                                                 int preferredHeight = 0);

    /**
     Make an Image with the raw pixels data in the format specified in the BitmapInfo.
     If shouldCopy is false, the returned Image will use the bytes from the attached BytesView
     and it will retain its source if it has one.
     Otherwise, the bytes will be copied.
     */
    static Valdi::Result<Ref<Image>> makeFromPixelsData(const Valdi::BitmapInfo& bitmapInfo,
                                                        const Valdi::BytesView& pixelsData,
                                                        bool shouldCopy);

    /**
     Make an Image with the raw pixels data provided from the given Bitmap.
     If shouldCopy is false, the returned Image will retain the Bitmap and use its underlying
     buffer directly. Otherwise, the bytes will be copied.
     */
    static Valdi::Result<Ref<Image>> makeFromBitmap(const Valdi::Ref<Valdi::IBitmap>& bitmap, bool shouldCopy);

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Texture-backed images must be read on their owning (already locked) context.
    static std::shared_ptr<tgfx::Data> encodeSKImageToSKData(tgfx::Image& image,
                                                          EncodedImageFormat format,
                                                          int quality,
                                                          tgfx::Context* context = nullptr);
#else
    static sk_sp<SkData> encodeSKImageToSKData(SkImage& image, EncodedImageFormat format, int quality);
#endif

    VALDI_CLASS_HEADER(Image)

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // The image/proxies must be destroyed before their device.
    std::shared_ptr<tgfx::Device> _readbackDevice;
#endif
    NativeImage _skImage;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // Retain the real TGFX codec for CPU bitmap operations. Images supplied by
    // a GPU surface instead use the context-based readback seam.
    std::shared_ptr<tgfx::ImageCodec> _codec;
    std::optional<Valdi::BitmapInfo> _bitmapInfo;
    NativeData _rawPixelData;
    std::optional<Valdi::BitmapInfo> _rawPixelInfo;
    bool readPixels(const Valdi::BitmapInfo& info, void* pixels) const;
#endif
    Ref<Image> _sourceImage;
    Ref<Valdi::ImageFilter> _filter;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    static Valdi::Result<Ref<Image>> makeFromPixelsData(const Valdi::BitmapInfo& bitmapInfo,
                                                      const std::shared_ptr<tgfx::Data>& pixelsData);
#else
    static Valdi::Result<Ref<Image>> makeFromPixelsData(const Valdi::BitmapInfo& bitmapInfo,
                                                      const sk_sp<SkData>& pixelsData);
#endif
};

} // namespace snap::drawing
