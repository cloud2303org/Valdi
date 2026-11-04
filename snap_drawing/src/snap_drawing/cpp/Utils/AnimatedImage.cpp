#include "snap_drawing/cpp/Utils/AnimatedImage.hpp"
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/codec/SkCodec.h"
#endif
#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#include "snap_drawing/cpp/Utils/Image.hpp"
#include "snap_drawing/cpp/Utils/LottieAnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/SVGAnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/SkCodecAnimatedImage.hpp"
#include "valdi_core/cpp/Utils/JSONReader.hpp"

#include <string>
#include <string_view>

namespace snap::drawing {

AnimatedImage::AnimatedImage() = default;
AnimatedImage::~AnimatedImage() = default;

void AnimatedImage::draw(NativeCanvas* canvas,
                         const Rect& drawBounds,
                         const Duration& time,
                         FittingSizeMode fittingSizeMode) {
    doDraw(canvas, drawBounds, time, fittingSizeMode);
}

void AnimatedImage::drawInCanvas(const DrawableSurfaceCanvas& canvas,
                                 const Rect& drawBounds,
                                 const Duration& time,
                                 FittingSizeMode fittingSizeMode) {
    doDraw(canvas.getSkiaCanvas(), drawBounds, time, fittingSizeMode);
}

Valdi::Result<Ref<AnimatedImage>> AnimatedImage::make(const Ref<IFontManager>& fontManager,
                                                      const Valdi::Byte* data,
                                                      size_t length) {
    const Valdi::BytesView bytesView(nullptr, data, length);
    if (!data || length == 0) {
        const auto message = describeUndecodablePayload(bytesView, "Unsupported image format");
        return Valdi::Error(std::string_view(message));
    }
    Image::initializeCodecs();

    if constexpr (kLottieEnabled) {
        if (isJsonObject(data, length)) {
            return LottieAnimatedImage::make(fontManager, data, length).map<Ref<AnimatedImage>>();
        }
    }

    if (Image::isSVG(bytesView)) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        return SVGAnimatedImage::make(data, length, fontManager).map<Ref<AnimatedImage>>();
#else
        return SVGAnimatedImage::make(data, length).map<Ref<AnimatedImage>>();
#endif
    }

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    auto decoded = SkCodecAnimatedImage::make(bytesView);
    if (!decoded) {
        const auto message = describeUndecodablePayload(bytesView, "Unsupported image format");
        return decoded.error().rethrow(std::string_view(message));
    }
    return decoded.map<Ref<AnimatedImage>>();
#else
    auto skData = skDataFromBytes(bytesView, DataConversionModeAlwaysCopy);
    auto codec = SkCodec::MakeFromData(skData);
    if (codec == nullptr) {
        // The classifier below would call a Lottie payload a JSON body. That is correct only while
        // kLottieEnabled is true AND the JSON check precedes this branch, which routes Lottie away
        // before it can get here. Where the check is compiled out, name the real cause instead.
        if constexpr (!kLottieEnabled) {
            if (isJsonObject(data, length)) {
                const auto lottieMessage =
                    "Lottie payload in a build without Lottie support (" + describePayloadBytes(bytesView) + ")";
                return Valdi::Error(std::string_view(lottieMessage));
            }
        }

        const auto message = describeUndecodablePayload(bytesView, "Unsupported image format");
        return Valdi::Error(std::string_view(message));
    }
    return SkCodecAnimatedImage::make(std::move(codec)).map<Ref<AnimatedImage>>();
#endif
}

bool AnimatedImage::isJsonObject(const Valdi::Byte* data, size_t length) {
    // Fuzzy but efficient check for JSON, and if so assume Lottie
    Valdi::JSONReader reader(std::string_view(reinterpret_cast<const char*>(data), length));
    return reader.parseBeginObject();
}

} // namespace snap::drawing
