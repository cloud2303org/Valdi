#pragma once

#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/AnimatedImage.hpp"
#include "snap_drawing/cpp/Utils/Duration.hpp"
#include "snap_drawing/cpp/Utils/Geometry.hpp"
#include "valdi_core/cpp/Utils/Mutex.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/ThorvgMedia.hpp"
#else
#include "modules/svg/include/SkSVGDOM.h"
#endif

namespace snap::drawing {

class SVGAnimatedImage : public AnimatedImage {
public:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    explicit SVGAnimatedImage(std::shared_ptr<ThorvgMedia> media);
#else
    explicit SVGAnimatedImage(const sk_sp<SkSVGDOM>& dom);
#endif
    ~SVGAnimatedImage() override;

    Duration getCurrentTime() const override;
    const Duration& getDuration() const override;
    const Size& getSize() const override;
    double getFrameRate() const override;
    Valdi::Value getMetadata() const override;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    static Valdi::Result<Ref<SVGAnimatedImage>> make(const Valdi::Byte* data, size_t length,
                                                   const Ref<IFontManager>& fontManager = nullptr);
#else
    static Valdi::Result<Ref<SVGAnimatedImage>> make(const Valdi::Byte* data, size_t length);
#endif

    VALDI_CLASS_HEADER(SVGAnimatedImage)

protected:
    void doDraw(NativeCanvas* canvas,
                const Rect& drawBounds,
                const Duration& time,
                FittingSizeMode fittingSizeMode) override;

private:
    mutable Valdi::Mutex _mutex;
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    std::shared_ptr<ThorvgMedia> _media;
#else
    sk_sp<SkSVGDOM> _dom;
#endif
    Duration _duration;
    Duration _currentTime;
    Size _size;
};

} // namespace snap::drawing
