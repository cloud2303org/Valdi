//
//  SkCodecAnimatedImage.hpp
//  snap_drawing
//
//  Created by Ben Dodson on 6/20/24.
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
#include "snap_drawing/cpp/Utils/TgfxMediaAnimation.hpp"
#else
#include "include/codec/SkCodec.h"
#include "modules/skottie/include/Skottie.h"
#include "modules/skresources/src/SkAnimCodecPlayer.h"
#endif

namespace snap::drawing {

class Resources;
class DrawingContext;
class IFontManager;

class SkCodecAnimatedImage : public AnimatedImage {
public:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    explicit SkCodecAnimatedImage(TgfxMediaFrames frames);
#else
    explicit SkCodecAnimatedImage(std::unique_ptr<SkCodec> codec);
#endif
    ~SkCodecAnimatedImage() override;

    Duration getCurrentTime() const override;
    const Duration& getDuration() const override;
    const Size& getSize() const override;
    double getFrameRate() const override;
    Valdi::Value getMetadata() const override;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    static Valdi::Result<Ref<SkCodecAnimatedImage>> make(const Valdi::BytesView& data);
#else
    static Valdi::Result<Ref<SkCodecAnimatedImage>> make(std::unique_ptr<SkCodec> codec);
#endif

    VALDI_CLASS_HEADER(SkCodecAnimatedImage)

protected:
    void doDraw(NativeCanvas* canvas,
                const Rect& drawBounds,
                const Duration& time,
                FittingSizeMode fittingSizeMode) override;

private:
    mutable Valdi::Mutex _mutex;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    TgfxMediaFrames _frames;
#else
    std::unique_ptr<SkAnimCodecPlayer> _player;
#endif
    Duration _duration;
    Duration _currentTime;
    Size _size;
    size_t _numberOfFrames = 0;
    double _frameRate;
};

} // namespace snap::drawing
