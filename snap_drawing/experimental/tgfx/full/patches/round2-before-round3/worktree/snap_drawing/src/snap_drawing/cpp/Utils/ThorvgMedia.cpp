#include "snap_drawing/cpp/Utils/ThorvgMedia.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Text/IFontManager.hpp"
#include "snap_drawing/cpp/Text/FontManager.hpp"
#include "snap_drawing/cpp/Text/Typeface.hpp"
#include "snap_drawing/cpp/Utils/TgfxMedia.hpp"
#include <expat.h>
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

namespace snap::drawing {
class ThorvgMediaRuntime {
public:
    ThorvgMediaRuntime() {
        std::promise<tvg::Result> initialized;
        auto ready = initialized.get_future();
        _thread = std::thread([this, &initialized] {
            const auto result = tvg::Initializer::init(0);
            initialized.set_value(result);
            for (;;) {
                std::function<void()> work;
                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    _condition.wait(lock, [this] { return _stopping || !_queue.empty(); });
                    if (_stopping && _queue.empty()) { break; }
                    work = std::move(_queue.front());
                    _queue.pop_front();
                }
                work();
            }
            if (result == tvg::Result::Success) { tvg::Initializer::term(); }
        });
        _initialization = ready.get();
    }

    ~ThorvgMediaRuntime() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
        }
        _condition.notify_one();
        _thread.join();
    }

    template<typename F>
    auto invoke(F&& function) -> decltype(function()) {
        using Return = decltype(function());
        auto work = std::make_shared<std::packaged_task<Return()>>(std::forward<F>(function));
        auto result = work->get_future();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _queue.emplace_back([work] { (*work)(); });
        }
        _condition.notify_one();
        return result.get();
    }

    bool ready() const { return _initialization == tvg::Result::Success; }

    static const std::shared_ptr<ThorvgMediaRuntime>& shared() {
        static const auto runtime = std::make_shared<ThorvgMediaRuntime>();
        return runtime;
    }

private:
    tvg::Result _initialization;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _condition;
    std::deque<std::function<void()>> _queue;
    bool _stopping = false;
};

namespace {
struct SvgValidation {
    XML_Parser parser = nullptr;
    size_t depth = 0;
    bool rootSeen = false;
    bool valid = true;
    bool intrinsicWidth = false;
    bool intrinsicHeight = false;
};

void XMLCALL svgStartElement(void* data, const XML_Char* name, const XML_Char** attributes) {
    auto& validation = *static_cast<SvgValidation*>(data);
    if (validation.depth == 0) {
        if (validation.rootSeen || std::strcmp(name, "svg") != 0) {
            validation.valid = false;
            XML_StopParser(validation.parser, XML_FALSE);
            return;
        }
        validation.rootSeen = true;
        for (auto attr = attributes; *attr != nullptr; attr += 2) {
            if (std::strcmp(attr[0], "width") != 0 && std::strcmp(attr[0], "height") != 0) { continue; }
            char* end = nullptr;
            const auto dimension = std::strtof(attr[1], &end);
            const bool absolute = end != attr[1] && std::isfinite(dimension) && dimension > 0 &&
                                  std::strchr(attr[1], '%') == nullptr;
            if (std::strcmp(attr[0], "width") == 0) { validation.intrinsicWidth = absolute; }
            else { validation.intrinsicHeight = absolute; }
        }
    }
    if (++validation.depth > 1024) {
        validation.valid = false;
        XML_StopParser(validation.parser, XML_FALSE);
    }
}

void XMLCALL svgEndElement(void* data, const XML_Char*) {
    auto& validation = *static_cast<SvgValidation*>(data);
    if (validation.depth > 0) { --validation.depth; }
}

bool validateSvg(const Valdi::Byte* data, size_t length, SvgValidation& validation) {
    auto parser = XML_ParserCreate(nullptr);
    if (parser == nullptr) { return false; }
    validation.parser = parser;
    XML_SetUserData(parser, &validation);
    XML_SetElementHandler(parser, svgStartElement, svgEndElement);
    // No external entity handler is installed. Expat does not fetch external
    // resources, and parameter entities are explicitly left disabled.
    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
    constexpr size_t chunkSize = 64 * 1024;
    bool parsed = true;
    for (size_t offset = 0; offset < length;) {
        const auto count = std::min(chunkSize, length - offset);
        const bool final = offset + count == length;
        if (XML_Parse(parser, reinterpret_cast<const char*>(data + offset), static_cast<int>(count),
                      final ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
            parsed = false;
            break;
        }
        offset += count;
    }
    XML_ParserFree(parser);
    validation.parser = nullptr;
    return parsed && validation.valid && validation.rootSeen && validation.depth == 0;
}

Valdi::Error thorvgError(const char* operation, tvg::Result result) {
    auto message = std::string("ThorVG ") + operation + " failed (result=" +
                   std::to_string(static_cast<int>(result)) + ")";
    return Valdi::Error(std::string_view(message));
}

constexpr size_t maxThorvgRasterBytes = 32 * 1024 * 1024;

struct MediaRasterPlan {
    int width = 0;
    int height = 0;
};

Valdi::Result<MediaRasterPlan> rasterPlan(NativeCanvas* canvas, const tgfx::Rect& fitted) {
    const auto& matrix = canvas->getMatrix();
    if (!matrix.isFinite()) { return Valdi::Error("Non-finite ThorVG playback matrix"); }
    const auto clip = canvas->getTotalClipBounds(); // Actual playback/device clip, not the recorder's clip.
    if (clip && clip->isEmpty()) { return MediaRasterPlan{}; }
    const double a = matrix[0], b = matrix[1], c = matrix[2];
    const double d = matrix[3], e = matrix[4], f = matrix[5];
    const double g = matrix[6], h = matrix[7], i = matrix[8];
    double minW = std::numeric_limits<double>::infinity();
    double left = minW, top = minW, right = -minW, bottom = -minW;
    for (const double x : {double(fitted.left), double(fitted.right)}) {
        for (const double y : {double(fitted.top), double(fitted.bottom)}) {
            const double w = g * x + h * y + i;
            if (!std::isfinite(w) || w <= 0) {
                return Valdi::Error("ThorVG perspective crosses or lies behind the projection plane");
            }
            minW = std::min(minW, w);
            const double px = (a * x + b * y + c) / w;
            const double py = (d * x + e * y + f) / w;
            if (!std::isfinite(px) || !std::isfinite(py)) {
                return Valdi::Error("Non-finite ThorVG projected bounds");
            }
            left = std::min(left, px); right = std::max(right, px);
            top = std::min(top, py); bottom = std::max(bottom, py);
        }
    }
    if (clip && (right <= clip->left || left >= clip->right ||
                 bottom <= clip->top || top >= clip->bottom)) { return MediaRasterPlan{}; }
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (determinant == 0) { return MediaRasterPlan{}; } // Collapsed geometry has no coverage.
    double scaleX, scaleY;
    if (g == 0 && h == 0) {
        // Column lengths retain rotation, anisotropic scaling and skew.
        scaleX = std::hypot(a, d) / i;
        scaleY = std::hypot(b, e) / i;
    } else {
        // Bound each Jacobian column across the entire fitted rectangle.
        // Its numerator is linear in the other axis; w is affine and positive,
        // so endpoint maxima and min corner w bound the continuous interior.
        auto maximum = [](double slope, double offset, double lo, double hi) {
            return std::max(std::abs(slope * lo + offset), std::abs(slope * hi + offset));
        };
        scaleX = std::hypot(maximum(a * h - g * b, a * i - g * c, fitted.top, fitted.bottom),
                            maximum(d * h - g * e, d * i - g * f, fitted.top, fitted.bottom)) / (minW * minW);
        scaleY = std::hypot(maximum(b * g - h * a, b * i - h * c, fitted.left, fitted.right),
                            maximum(e * g - h * d, e * i - h * f, fitted.left, fitted.right)) / (minW * minW);
    }
    const double width = std::ceil(double(fitted.width()) * scaleX);
    const double height = std::ceil(double(fitted.height()) * scaleY);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > (INT32_MAX >> 2) || height > (INT32_MAX >> 2)) {
        return Valdi::Error("Invalid ThorVG physical raster dimensions");
    }
    if (width > double(maxThorvgRasterBytes / 4) / height) {
        return Valdi::Error("ThorVG physical raster exceeds the 32 MiB pixel capacity");
    }
    return MediaRasterPlan{static_cast<int>(width), static_cast<int>(height)};
}

void rejectMediaDraw(NativeCanvas* canvas, const Valdi::Error& error) {
    Valdi::ConsoleLogger::getErrorLogger().log(Valdi::LogTypeError, error.toString());
    // Use the same public, replayable native failure seam as Paint's deferred
    // effects. The GPU owner supplies this API; media never edits TGFX.
    canvas->markRenderFailure();
}
} // namespace

ThorvgMedia::ThorvgMedia(std::shared_ptr<ThorvgMediaRuntime> runtime) : _runtime(std::move(runtime)) {
    static std::atomic<uint64_t> nextId{1};
    _id = nextId.fetch_add(1);
}

ThorvgMedia::~ThorvgMedia() {
    _runtime->invoke([this] {
        // Canvas has its own reference. Animation has another. Remove both
        // before unref'ing our independent reference and unloading fonts.
        if (_canvas) { _canvas->sync(); }
        _canvas.reset();
        _animation.reset();
        if (_picture) { _picture->unref(); _picture = nullptr; }
        for (const auto& font : _fonts) { tvg::Text::unload(font.second.c_str()); }
    });
}

bool ThorvgMedia::resolve(tvg::Paint* paint, const char* source) {
    if (!paint || !source) { return false; }
    if (paint->type() != tvg::Type::Text) {
        if (paint->type() != tvg::Type::Picture) {
            _assetError = "ThorVG requested an unsupported asset type";
            return false;
        }
        auto codec = tgfx::ImageCodec::MakeFrom(std::string(source));
        if (!codec) {
            _assetError = std::string("ThorVG could not resolve external image asset: ") + source;
            return false;
        }
        const bool swapped = tgfx::OrientationSwapsWidthHeight(codec->orientation());
        const int width = swapped ? codec->height() : codec->width();
        const int height = swapped ? codec->width() : codec->height();
        const Valdi::BitmapInfo info(width, height, Valdi::ColorTypeRGBA8888,
                                     Valdi::AlphaTypePremul, static_cast<size_t>(width) * 4);
        if (!tgfxMediaValidBitmapInfo(info)) {
            _assetError = "Invalid external image asset dimensions";
            return false;
        }
        std::vector<uint8_t> rgba(info.rowBytes * height);
        if (!tgfxMediaReadPixels(codec, info, rgba.data())) {
            _assetError = std::string("Cannot decode external image asset: ") + source;
            return false;
        }
        std::vector<uint32_t> packed(rgba.size() / 4);
        for (size_t i = 0; i < packed.size(); ++i) {
            packed[i] = uint32_t(rgba[i * 4]) | (uint32_t(rgba[i * 4 + 1]) << 8) |
                        (uint32_t(rgba[i * 4 + 2]) << 16) | (uint32_t(rgba[i * 4 + 3]) << 24);
        }
        const auto loaded = static_cast<tvg::Picture*>(paint)->load(
            packed.data(), width, height, tvg::ColorSpace::ABGR8888, true);
        if (loaded != tvg::Result::Success) {
            _assetError = thorvgError("external image load", loaded).toString();
            return false;
        }
        return true;
    }
    const std::string name = std::strncmp(source, "name:", 5) == 0 ? source + 5 : source;
    auto found = _fonts.find(name);
    if (found != _fonts.end()) {
        const auto applied = static_cast<tvg::Text*>(paint)->font(found->second.c_str());
        if (applied != tvg::Result::Success) {
            _assetError = thorvgError("private font selection", applied).toString();
            return false;
        }
        return true;
    }
    if (_fontManager == nullptr) {
        _assetError = "ThorVG asset has no native font provider";
        return false;
    }
    auto face = _fontManager->getTypefaceWithName(Valdi::StringCache::getGlobal().makeString(name));
    if (!face) {
        _assetError = face.error().toString();
        return false;
    }
    // Includes CoreText table and selected-TTC-face export. The view pins the
    // actual provider's bytes, including the default native font-manager path.
    auto providerBytes = face.value()->makeFontData();
    // Always use the selected-face SFNT export, including the native default
    // path. A raw openStream fallback could reintroduce a TTC container after
    // export failed, or select the wrong face from the collection.
    if (providerBytes.empty()) {
        _assetError = "No exportable selected-face bytes for ThorVG font: " + name;
        return false;
    }
    const auto* bytes = reinterpret_cast<const char*>(providerBytes.data());
    const auto length = providerBytes.size();
    if (length > std::numeric_limits<uint32_t>::max()) {
        _assetError = "Font bytes exceed ThorVG size limit";
        return false;
    }
    // ThorVG's font cache is process-wide. A private name prevents one
    // font manager/asset from replacing another asset's same-named font.
    const auto privateName = "valdi-thorvg-" + std::to_string(_id) + "-" + name;
    const auto result = tvg::Text::load(privateName.c_str(), bytes,
                                       static_cast<uint32_t>(length), nullptr, true);
    if (result != tvg::Result::Success) {
        _assetError = thorvgError("font load", result).toString();
        return false;
    }
    _fonts.emplace(name, privateName);
    const auto applied = static_cast<tvg::Text*>(paint)->font(privateName.c_str());
    if (applied != tvg::Result::Success) {
        _assetError = thorvgError("font selection", applied).toString();
        return false;
    }
    return true;
}

Valdi::Result<std::shared_ptr<ThorvgMedia>> ThorvgMedia::make(const Valdi::Byte* data,
                                                           size_t length,
                                                           bool lottie,
                                                           const Ref<IFontManager>& fontManager) {
    if (!data || !length || length > std::numeric_limits<uint32_t>::max()) {
        return Valdi::Error("Invalid ThorVG payload size");
    }
    constexpr size_t maxSvgSourceBytes = 64 * 1024 * 1024;
    if (!lottie && length > maxSvgSourceBytes) {
        return Valdi::Error("SVG source exceeds the 64 MiB decoding limit");
    }
    SvgValidation svg;
    if (!lottie && !validateSvg(data, length, svg)) {
        return Valdi::Error("Unable to decode SVG: invalid or incomplete XML structure");
    }
    auto runtime = ThorvgMediaRuntime::shared();
    if (!runtime->ready()) { return Valdi::Error("ThorVG initialization failed"); }
    auto media = std::shared_ptr<ThorvgMedia>(new ThorvgMedia(runtime));
    auto loaded = runtime->invoke([&]() -> Valdi::Result<Valdi::Void> {
        media->_fontManager = fontManager;
        if (media->_fontManager == nullptr) {
            // Static SVG factories have no provider parameter. Use an actual
            // asset-owned native FontManager, including its platform fallback
            // policy; never substitute ThorVG's process-global font cache.
            auto nativeFonts = Valdi::makeShared<FontManager>(Valdi::ConsoleLogger::getLogger(), false);
            nativeFonts->load();
            media->_fontManager = Ref<IFontManager>(nativeFonts);
        }
        if (lottie) {
            media->_animation.reset(tvg::Animation::gen());
            if (!media->_animation) { return Valdi::Error("Cannot create ThorVG animation"); }
            media->_picture = media->_animation->picture();
        } else {
            media->_picture = tvg::Picture::gen();
        }
        if (!media->_picture) { return Valdi::Error("Cannot create ThorVG picture"); }
        media->_picture->ref();
        auto result = media->_picture->resolver(
            [raw = media.get()](tvg::Paint* paint, const char* source, void*) { return raw->resolve(paint, source); },
            nullptr, false, true);
        if (result != tvg::Result::Success) { return thorvgError("asset resolver", result); }
        result = media->_picture->load(reinterpret_cast<const char*>(data),
                                       static_cast<uint32_t>(length), lottie ? "lottie+json" : "svg+xml",
                                       nullptr, true);
        if (result != tvg::Result::Success) {
            if (!media->_assetError.empty()) { return Valdi::Error(std::string_view(media->_assetError)); }
            return thorvgError("picture load", result);
        }
        float width = 0, height = 0;
        result = media->_picture->size(&width, &height);
        if (result != tvg::Result::Success) { return thorvgError("picture size", result); }
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            width > static_cast<double>(INT32_MAX >> 2) || height > static_cast<double>(INT32_MAX >> 2)) {
            return Valdi::Error("ThorVG picture has no valid intrinsic size");
        }
        media->_size = Size(width, height);
        media->_intrinsicSize = lottie ? media->_size
                                       : Size(svg.intrinsicWidth ? width : 0, svg.intrinsicHeight ? height : 0);
        if (media->_animation) {
            auto seconds = media->_animation->duration();
            auto frames = media->_animation->totalFrame();
            if (!std::isfinite(seconds) || !std::isfinite(frames) || seconds <= 0 || frames <= 0) {
                return Valdi::Error("ThorVG Lottie animation has no valid frame timeline");
            }
            media->_duration = Duration::fromSeconds(seconds);
            media->_frameRate = frames / seconds;
        }
        media->_canvas.reset(tvg::SwCanvas::gen());
        if (!media->_canvas) { return Valdi::Error("ThorVG software renderer was not built"); }
        return Valdi::Void();
    });
    if (!loaded) { return loaded.moveError(); }
    // Validate real rendering and asset resolution, rather than returning an
    // apparently loaded animation whose embedded loader could not render.
    // Loading/asset validation must not allocate at the document's intrinsic
    // size. Size queries and preferred-size renders may describe huge SVGs.
    // Still execute the real first frame, preserving aspect ratio and all
    // resolver errors, with an independently bounded validation target.
    constexpr double validationEdge = 32;
    const double scale = std::min(1.0, validationEdge /
        std::max(static_cast<double>(media->_size.width), static_cast<double>(media->_size.height)));
    const int validationWidth = std::clamp(static_cast<int>(std::ceil(media->_size.width * scale)), 1, 32);
    const int validationHeight = std::clamp(static_cast<int>(std::ceil(media->_size.height * scale)), 1, 32);
    auto firstFrame = media->render(validationWidth, validationHeight, Duration());
    if (!firstFrame) { return firstFrame.moveError(); }
    return media;
}

Valdi::Result<NativeImage> ThorvgMedia::renderOnWorker(int width, int height, const Duration& time) {
    const auto info = tgfx::ImageInfo::Make(width, height, tgfx::ColorType::RGBA_8888);
    if (info.isEmpty() || info.rowBytes() > std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
        return Valdi::Error("Invalid ThorVG raster dimensions");
    }
    if (info.byteSize() > maxThorvgRasterBytes) {
        return Valdi::Error("ThorVG raster exceeds the 32 MiB pixel capacity");
    }
    auto result = _canvas->sync();
    if (result != tvg::Result::Success) { return thorvgError("sync before target", result); }
    // The canvas target points into member storage, including on error returns.
    // Do not resize it until the previous draw is fully synchronized.
    _pixels.resize(info.byteSize() / sizeof(uint32_t));
    result = _canvas->target(_pixels.data(), static_cast<uint32_t>(width), width, height,
                                   tvg::ColorSpace::ABGR8888);
    if (result != tvg::Result::Success) { return thorvgError("target", result); }
    if (_canvas->paints().empty()) {
        result = _canvas->add(_picture);
        if (result != tvg::Result::Success) { return thorvgError("canvas add", result); }
    }
    if (_animation) {
        const float frame = static_cast<float>(std::clamp(time, Duration(), _duration).seconds() * _frameRate);
        const float last = std::nextafter(_animation->totalFrame(), 0.0f);
        result = _animation->frame(std::clamp(frame, 0.0f, last));
        // InsufficientCondition means the frame is unchanged, per the real API.
        if (result != tvg::Result::Success && result != tvg::Result::InsufficientCondition) {
            return thorvgError("animation frame", result);
        }
    }
    const tvg::Matrix matrix{width / _size.width, 0, 0, 0, height / _size.height, 0, 0, 0, 1};
    result = _picture->transform(matrix);
    if (result != tvg::Result::Success) { return thorvgError("picture transform", result); }
    result = _canvas->update();
    if (result != tvg::Result::Success) { return thorvgError("canvas update", result); }
    const auto drawn = _canvas->draw(true);
    const auto synced = _canvas->sync();
    if (!_assetError.empty()) { return Valdi::Error(std::string_view(_assetError)); }
    if (drawn != tvg::Result::Success) { return thorvgError("draw", drawn); }
    if (synced != tvg::Result::Success) { return thorvgError("sync", synced); }
    // ABGR8888 is a uint32 packing. Produce RGBA bytes explicitly on either
    // endian, then give TGFX an immutable copy independent of the next frame.
    std::vector<uint8_t> rgba(info.byteSize());
    for (size_t i = 0; i < _pixels.size(); ++i) {
        rgba[i * 4] = static_cast<uint8_t>(_pixels[i]);
        rgba[i * 4 + 1] = static_cast<uint8_t>(_pixels[i] >> 8);
        rgba[i * 4 + 2] = static_cast<uint8_t>(_pixels[i] >> 16);
        rgba[i * 4 + 3] = static_cast<uint8_t>(_pixels[i] >> 24);
    }
    _rasterCodec = tgfx::ImageCodec::MakeFrom(info, tgfx::Data::MakeWithCopy(rgba.data(), rgba.size()));
    auto image = _rasterCodec ? tgfx::Image::MakeFrom(_rasterCodec) : nullptr;
    if (!image) { return Valdi::Error("Cannot upload ThorVG pixels to TGFX image"); }
    return image;
}

Valdi::Result<NativeImage> ThorvgMedia::render(int width, int height, const Duration& time) {
    return _runtime->invoke([&] { return renderOnWorker(width, height, time); });
}

Valdi::Result<Valdi::Void> ThorvgMedia::rasterize(const Valdi::BitmapInfo& info, void* pixels) {
    return _runtime->invoke([&]() -> Valdi::Result<Valdi::Void> {
        auto image = renderOnWorker(info.width, info.height, Duration());
        if (!image) { return image.moveError(); }
        if (!tgfxMediaReadPixels(_rasterCodec, info, pixels)) {
            return Valdi::Error("Cannot convert ThorVG raster into bitmap pixels");
        }
        return Valdi::Void();
    });
}

void ThorvgMedia::draw(NativeCanvas* canvas, const Rect& bounds, const Duration& time, FittingSizeMode mode) {
    if (!canvas) { return; }
    auto fitted = bounds.makeFittingSize(_size, mode);
    if (fitted.width() <= 0 || fitted.height() <= 0) { return; }
    if (!std::isfinite(fitted.width()) || !std::isfinite(fitted.height()) ||
        !std::isfinite(fitted.left) || !std::isfinite(fitted.top) ||
        !std::isfinite(fitted.right) || !std::isfinite(fitted.bottom)) {
        rejectMediaDraw(canvas, Valdi::Error("Invalid ThorVG logical draw bounds"));
        return;
    }
    // The record owns the media and immutable draw arguments. Each playback
    // resolves its final matrix/clip, including transforms applied AFTER this
    // picture was recorded, rather than freezing a logical-resolution bitmap.
    canvas->drawDeferred(bounds.getSkValue(), 0,
        [media = shared_from_this(), bounds, time, mode](NativeCanvas* playback) {
            media->drawAtPlayback(playback, bounds, time, mode);
        });
}

void ThorvgMedia::drawAtPlayback(NativeCanvas* canvas, const Rect& bounds,
                                 const Duration& time, FittingSizeMode mode) {
    const auto fitted = bounds.makeFittingSize(_size, mode);
    auto plan = rasterPlan(canvas, fitted.getSkValue());
    if (!plan) { rejectMediaDraw(canvas, plan.error()); return; }
    if (plan.value().width == 0 || plan.value().height == 0) { return; }
    auto image = render(plan.value().width, plan.value().height, time);
    if (!image) {
        rejectMediaDraw(canvas, image.error());
        return;
    }
    const auto count = canvas->save();
    canvas->clipRect(bounds.getSkValue());
    canvas->drawImageRect(image.moveValue(), fitted.getSkValue(),
                          tgfx::SamplingOptions(tgfx::FilterMode::Linear, tgfx::MipmapMode::None));
    canvas->restoreToCount(count);
}
} // namespace snap::drawing
#endif
