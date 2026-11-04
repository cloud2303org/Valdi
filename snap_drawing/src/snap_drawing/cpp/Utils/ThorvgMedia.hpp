#pragma once
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"
#include "snap_drawing/cpp/Utils/Duration.hpp"
#include "snap_drawing/cpp/Utils/Geometry.hpp"
#include "snap_drawing/cpp/Text/IFontManager.hpp"
#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"
#include "tgfx/core/ImageCodec.h"
#include "thorvg.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace snap::drawing {
class ThorvgMediaRuntime;

class ThorvgMedia : public std::enable_shared_from_this<ThorvgMedia> {
public:
    ~ThorvgMedia();
    static Valdi::Result<std::shared_ptr<ThorvgMedia>> make(const Valdi::Byte* data,
                                                          size_t length,
                                                          bool lottie,
                                                          const Ref<IFontManager>& fontManager = nullptr);
    const Size& size() const { return _size; }
    // SVG viewBox geometry is not an intrinsic raster size. Missing/relative
    // root width or height remains zero until the caller supplies that axis.
    const Size& intrinsicSize() const { return _intrinsicSize; }
    Duration duration() const { return _duration; }
    double frameRate() const { return _frameRate; }
    Valdi::Result<NativeImage> render(int width, int height, const Duration& time);
    Valdi::Result<Valdi::Void> rasterize(const Valdi::BitmapInfo& info, void* pixels);
    void draw(NativeCanvas* canvas, const Rect& bounds, const Duration& time, FittingSizeMode mode);

private:
    explicit ThorvgMedia(std::shared_ptr<ThorvgMediaRuntime> runtime);
    Valdi::Result<NativeImage> renderOnWorker(int width, int height, const Duration& time);
    void drawAtPlayback(NativeCanvas* canvas, const Rect& bounds, const Duration& time, FittingSizeMode mode);
    bool resolve(tvg::Paint* paint, const char* source);

    // Every ThorVG API call, including destructors and global font registration,
    // runs on _runtime. Neither encoded input nor renderer-owned pixels escape it.
    std::shared_ptr<ThorvgMediaRuntime> _runtime;
    std::unique_ptr<tvg::Animation> _animation;
    tvg::Picture* _picture = nullptr;
    std::unique_ptr<tvg::SwCanvas> _canvas;
    std::vector<uint32_t> _pixels;
    std::shared_ptr<tgfx::ImageCodec> _rasterCodec;
    Ref<IFontManager> _fontManager;
    std::unordered_map<std::string, std::string> _fonts;
    std::string _assetError;
    uint64_t _id = 0;
    Size _size;
    Size _intrinsicSize;
    Duration _duration;
    double _frameRate = 0;
};
} // namespace snap::drawing
#endif
