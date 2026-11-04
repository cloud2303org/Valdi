#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/SkCodecAnimatedImage.hpp"
#include <algorithm>

namespace snap::drawing {
SkCodecAnimatedImage::SkCodecAnimatedImage(TgfxMediaFrames frames)
    : _frames(std::move(frames)), _size(_frames.width, _frames.height) {
    _numberOfFrames = _frames.frames.size();
    _duration = _frames.frames.empty() ? Duration() : _frames.frames.back().endTime;
    _frameRate = _duration.seconds() > 0 ? _numberOfFrames / _duration.seconds() : 0;
}
SkCodecAnimatedImage::~SkCodecAnimatedImage() = default;
Duration SkCodecAnimatedImage::getCurrentTime() const {
    std::lock_guard<Valdi::Mutex> lock(_mutex);
    return _currentTime;
}
const Duration& SkCodecAnimatedImage::getDuration() const { return _duration; }
const Size& SkCodecAnimatedImage::getSize() const { return _size; }
double SkCodecAnimatedImage::getFrameRate() const { return _frameRate; }
Valdi::Value SkCodecAnimatedImage::getMetadata() const {
    return Valdi::Value()
        .setMapValue("type", Valdi::Value(Valdi::StringBox::fromCString("skcodec")))
        .setMapValue("width", Valdi::Value(static_cast<int32_t>(_size.width)))
        .setMapValue("height", Valdi::Value(static_cast<int32_t>(_size.height)))
        .setMapValue("numberOfFrames", Valdi::Value(static_cast<int32_t>(_numberOfFrames)))
        .setMapValue("durationMs", Valdi::Value(static_cast<int32_t>(_duration.milliseconds())));
}
void SkCodecAnimatedImage::doDraw(NativeCanvas* canvas, const Rect& bounds,
                                  const Duration& time, FittingSizeMode mode) {
    std::lock_guard<Valdi::Mutex> lock(_mutex);
    _currentTime = std::clamp(time, Duration(), _duration);
    if (!canvas || _frames.frames.empty()) { return; }
    auto frame = std::upper_bound(_frames.frames.begin(), _frames.frames.end(), _currentTime,
        [](const Duration& current, const TgfxMediaFrame& candidate) { return current < candidate.endTime; });
    if (frame == _frames.frames.end()) { frame = _frames.frames.end() - 1; }
    const auto saved = canvas->save();
    canvas->clipRect(bounds.getSkValue());
    canvas->drawImageRect(frame->image, bounds.makeFittingSize(_size, mode).getSkValue(),
                          tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
    canvas->restoreToCount(saved);
}
Valdi::Result<Ref<SkCodecAnimatedImage>> SkCodecAnimatedImage::make(const Valdi::BytesView& data) {
    auto frames = tgfxMediaDecodeFrames(data);
    if (!frames) { return frames.moveError(); }
    return Valdi::makeShared<SkCodecAnimatedImage>(frames.moveValue());
}
VALDI_CLASS_IMPL(SkCodecAnimatedImage)
} // namespace snap::drawing
#else
//
//  SkCodecAnimatedImage.cpp
//  snap_drawing
//
//  Created by Ben Dodson on 06/19/24.
//

#include "snap_drawing/cpp/Utils/SkCodecAnimatedImage.hpp"
#include "include/core/SkCanvas.h"
#include "snap_drawing/cpp/Drawing/DrawingContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"

namespace snap::drawing {

SkCodecAnimatedImage::~SkCodecAnimatedImage() = default;

const Duration& SkCodecAnimatedImage::getDuration() const {
    return _duration;
}

Duration SkCodecAnimatedImage::getCurrentTime() const {
    std::lock_guard<Valdi::Mutex> lock(_mutex);
    return _currentTime;
}

const Size& SkCodecAnimatedImage::getSize() const {
    return _size;
}

double SkCodecAnimatedImage::getFrameRate() const {
    return _frameRate;
}

SkCodecAnimatedImage::SkCodecAnimatedImage(std::unique_ptr<SkCodec> codec)
    : _size(Size(codec->dimensions().width(), codec->dimensions().height())) {
    SkImageInfo imageInfo = codec->getInfo().makeColorType(kN32_SkColorType);
    std::vector<SkCodec::FrameInfo> frameInfos = codec->getFrameInfo();
    const auto totalFrames = frameInfos.size();
    long totalAnimationDurationMs = 0u;
    for (size_t i = 0; i < totalFrames; i++) {
        totalAnimationDurationMs += frameInfos[i].fDuration;
    }
    _numberOfFrames = totalFrames;
    _duration = Duration::fromMilliseconds(totalAnimationDurationMs);
    _frameRate = _frameRate = codec->getFrameCount() / _duration.seconds();
    _player = std::make_unique<SkAnimCodecPlayer>(std::move(codec));
}

Valdi::Value SkCodecAnimatedImage::getMetadata() const {
    return Valdi::Value()
        .setMapValue("type", Valdi::Value(Valdi::StringBox::fromCString("skcodec")))
        .setMapValue("width", Valdi::Value(static_cast<int32_t>(_size.width)))
        .setMapValue("height", Valdi::Value(static_cast<int32_t>(_size.height)))
        .setMapValue("numberOfFrames", Valdi::Value(static_cast<int32_t>(_numberOfFrames)))
        .setMapValue("durationMs", Valdi::Value(static_cast<int32_t>(_duration.milliseconds())));
}

void SkCodecAnimatedImage::doDraw(SkCanvas* canvas,
                                  const Rect& drawBounds,
                                  const Duration& time,
                                  FittingSizeMode fittingSizeMode) {
    std::lock_guard<Valdi::Mutex> lock(_mutex);
    _currentTime = std::clamp(time, Duration(), _duration);

    _player->seek(_currentTime.milliseconds());
    const auto& image = _player->getFrame();
    const SkRect srcR = SkRect::MakeWH(_size.width, _size.height);
    canvas->drawImageRect(image,
                          srcR,
                          drawBounds.getSkValue(),
                          SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                          nullptr,
                          SkCanvas::kStrict_SrcRectConstraint);
}

Valdi::Result<Ref<SkCodecAnimatedImage>> SkCodecAnimatedImage::make(std::unique_ptr<SkCodec> codec) {
    return Valdi::makeShared<SkCodecAnimatedImage>(std::move(codec));
}

VALDI_CLASS_IMPL(SkCodecAnimatedImage)

} // namespace snap::drawing

#endif
