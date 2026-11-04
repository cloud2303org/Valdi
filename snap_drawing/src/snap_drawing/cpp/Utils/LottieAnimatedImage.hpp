//
//  LottieScene.hpp
//  snap_drawing
//
//  Created by Simon Corsin on 7/22/22.
//

#pragma once

#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/AnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/Duration.hpp"
#include "snap_drawing/cpp/Utils/Geometry.hpp"

#include "valdi_core/cpp/Resources/LoadedAsset.hpp"

#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/Mutex.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/ThorvgMedia.hpp"
#else
#include "modules/skottie/include/Skottie.h"
#endif

namespace snap::drawing {

class Resources;
class DrawingContext;
class IFontManager;

class LottieAnimatedImage : public AnimatedImage {
public:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    explicit LottieAnimatedImage(std::shared_ptr<ThorvgMedia> media);
#else
    explicit LottieAnimatedImage(const sk_sp<skottie::Animation>& animation);
#endif
    ~LottieAnimatedImage() override;

    Duration getCurrentTime() const override;
    const Duration& getDuration() const override;
    const Size& getSize() const override;
    double getFrameRate() const override;
    Valdi::Value getMetadata() const override;

    static Valdi::Result<Ref<LottieAnimatedImage>> make(const Ref<Resources>& resources,
                                                        const Valdi::Byte* data,
                                                        size_t length);

    static Valdi::Result<Ref<LottieAnimatedImage>> make(const Ref<IFontManager>& fontManager,
                                                        const Valdi::Byte* data,
                                                        size_t length);

    VALDI_CLASS_HEADER(LottieAnimatedImage)

protected:
    void doDraw(NativeCanvas* canvas,
                const Rect& drawBounds,
                const Duration& time,
                FittingSizeMode fittingSizeMode) override;

private:
    mutable Valdi::Mutex _mutex;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::shared_ptr<ThorvgMedia> _media;
#elif defined(SNAP_DRAWING_LOTTIE_ENABLED)
    sk_sp<skottie::Animation> _animation;
#endif
    Duration _duration;
    Duration _currentTime;
    Size _size;
    double _frameRate;
};

} // namespace snap::drawing
