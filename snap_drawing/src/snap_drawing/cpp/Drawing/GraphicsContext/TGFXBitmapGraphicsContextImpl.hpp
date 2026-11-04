#pragma once

#include "snap_drawing/cpp/Drawing/GraphicsContext/BitmapGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXUtils.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "tgfx/core/Buffer.h"
#include "tgfx/core/Data.h"
#include "tgfx/core/Paint.h"
#include <cstring>
#include <limits>
#include <vector>

namespace snap::drawing {
class BitmapDrawableSurface : public DrawableSurface {
public:
    BitmapDrawableSurface(const Ref<GrGraphicsContext>& context, const Valdi::Ref<Valdi::IBitmap>& bitmap,
        const Valdi::BitmapInfo& info, void* bytes)
        : _context(context), _bitmap(bitmap), _bitmapInfo(info), _bitmapBytes(bytes) {}
    ~BitmapDrawableSurface() override {
        if (_frame) {
            (void)_frame->get();
            if (_surface) {
                discardTGFXSurface(_surface.get());
                logTGFXCompletionError(
                    DrawableSurfaceCanvas(_surface.get(), _bitmapInfo.width, _bitmapInfo.height).validate(),
                    "Discarding failed bitmap frame during cleanup");
            }
            // Cleanup never publishes an unfinished or failed raster operation.
            unlockBitmap();
            _surface.reset();
            _frame.reset();
        }
        if (_context != nullptr) {
            auto lock = _context->lockContext();
            _surface.reset();
        }
    }
    GraphicsContext* getGraphicsContext() override { return _context.get(); }
    Valdi::Result<DrawableSurfaceCanvas> prepareCanvas() override {
        if (_frame) {
            (void)_frame->get(); // Enforce the preparing thread and active native context.
            DrawableSurfaceCanvas canvas(_surface.get(), _bitmapInfo.width, _bitmapInfo.height);
            auto status = canvas.validate();
            if (!status) {
                _completionResult = status;
                unlockBitmap();
                _surface.reset();
                _frame.reset();
                return status.moveError();
            }
            return canvas;
        }
        _completionResult = Valdi::Void();
        // The pinned GPU PixelFormat has neither F16 nor F32 storage. ColorType::RGBA_F16
        // exists for CPU metadata only and Surface::Make rejects it. Do not quantize silently.
        if (_bitmapInfo.colorType == Valdi::ColorTypeRGBAF16 ||
            _bitmapInfo.colorType == Valdi::ColorTypeRGBAF32) {
            return Valdi::Error("Pinned TGFX GPU backend cannot render F16/F32 bitmap surfaces");
        }
        auto info = toTGFXImageInfo(_bitmapInfo);
        if (info.isEmpty() || info.colorType() == tgfx::ColorType::Unknown ||
            info.rowBytes() > std::numeric_limits<size_t>::max() / static_cast<size_t>(info.height())) {
            return Valdi::Error("Invalid or unsupported TGFX bitmap format/stride/dimensions");
        }
        if (_context == nullptr) { return Valdi::Error("Failed to create TGFX offscreen device for bitmap rendering"); }
        _frame = _context->lockContext();
        if (!*_frame) {
            _frame.reset();
            return Valdi::Error("Failed to lock TGFX offscreen bitmap device");
        }
        // Gray output uses a color render target, then converts to gray on readback.
        auto targetType = info.colorType() == tgfx::ColorType::Gray_8 ? tgfx::ColorType::RGBA_8888 : info.colorType();
        if (!_surface) { _surface = tgfx::Surface::Make(_frame->get(), info.width(), info.height(), targetType); }
        if (!_surface) {
            _frame.reset();
            return Valdi::Error("Failed to create TGFX bitmap render target");
        }
        void* pixels = _bitmapBytes;
        if (_bitmap != nullptr) {
            pixels = _bitmap->lockBytes();
            _bitmapLocked = pixels != nullptr;
            if (!pixels) {
                discardTGFXSurface(_surface.get());
                _surface.reset();
                _frame.reset();
                return Valdi::Error("Failed to lock caller bitmap bytes for TGFX rendering");
            }
        } else if (!pixels) {
            if (_ownedPixels.empty()) { _ownedPixels.resize(info.byteSize(), 0); }
            pixels = _ownedPixels.data();
        }
        _destination = pixels;
        // Preserve existing content for damage-only rasterization. Copy the upload so
        // TGFX never retains an unowned pointer to a caller's mutable bitmap memory.
        auto data = tgfx::Data::MakeWithCopy(pixels, info.byteSize());
        auto image = data ? tgfx::Image::MakeFrom(info, std::move(data)) : nullptr;
        if (!image) {
            discardTGFXSurface(_surface.get());
            _surface.reset();
            unlockBitmap();
            _frame.reset();
            return Valdi::Error("Failed to upload bitmap pixels to TGFX");
        }
        tgfx::Paint paint;
        paint.setBlendMode(tgfx::BlendMode::Src);
        _surface->getCanvas()->drawImage(std::move(image), &paint);
        DrawableSurfaceCanvas canvas(_surface.get(), info.width(), info.height());
        auto status = canvas.validate();
        if (!status) {
            _completionResult = status;
            unlockBitmap();
            _surface.reset();
            _frame.reset();
            return status.moveError();
        }
        return canvas;
    }
    void flush() override {
        logTGFXCompletionError(flushWithResult(), "TGFX legacy bitmap flush");
    }
    Valdi::Result<Valdi::Void> flushWithResult() override {
        if (!_frame) { return _completionResult; }
        (void)_frame->get();
        // Read into owned staging pixels. Native readPixels may return true even
        // after deferred work sets hasError; no caller bytes are changed on failure.
        auto completed = [&]() -> Valdi::Result<NativeData> {
            if (!_surface || !_destination) { return Valdi::Error("TGFX bitmap has no completion destination"); }
            DrawableSurfaceCanvas canvas(_surface.get(), _bitmapInfo.width, _bitmapInfo.height);
            return canvas.readPixels(toTGFXImageInfo(_bitmapInfo));
        }();
        if (completed) {
            auto info = toTGFXImageInfo(_bitmapInfo);
            const auto* source = static_cast<const uint8_t*>(completed.value()->data());
            auto* destination = static_cast<uint8_t*>(_destination);
            for (int y = 0; y < info.height(); ++y) {
                const auto offset = static_cast<size_t>(y) * info.rowBytes();
                std::memcpy(destination + offset, source + offset, info.minRowBytes());
            }
        }
        unlockBitmap();
        _completionResult = completed ? Valdi::Result<Valdi::Void>(Valdi::Void())
                                      : Valdi::Result<Valdi::Void>(completed.moveError());
        if (!_completionResult) {
            discardTGFXSurface(_surface.get());
            _surface.reset();
        }
        _frame.reset();
        return _completionResult;
    }
private:
    void unlockBitmap() {
        if (_bitmapLocked) { _bitmap->unlockBytes(); _bitmapLocked = false; }
        _destination = nullptr;
    }
    Ref<GrGraphicsContext> _context;
    Valdi::Ref<Valdi::IBitmap> _bitmap;
    Valdi::BitmapInfo _bitmapInfo;
    void* _bitmapBytes;
    void* _destination = nullptr;
    bool _bitmapLocked = false;
    std::vector<uint8_t> _ownedPixels;
    std::shared_ptr<tgfx::Surface> _surface;
    std::unique_ptr<TGFXContextLock> _frame;
    Valdi::Result<Valdi::Void> _completionResult = Valdi::Void();
};
BitmapGraphicsContext::BitmapGraphicsContext() = default;
BitmapGraphicsContext::~BitmapGraphicsContext() = default;
BitmapGraphicsContext::BitmapGraphicsContext(const Ref<GrGraphicsContext>& context) : _context(context) {}
Ref<GrGraphicsContext> BitmapGraphicsContext::getGPUContext() {
    Ref<GrGraphicsContext> context;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_context == nullptr) { _context = GrGraphicsContext::makeOffscreen(); }
        context = _context;
    }
    if (context != nullptr) { applyCacheLimit(context); }
    return context;
}
void BitmapGraphicsContext::applyCacheLimit(const Ref<GrGraphicsContext>& context) {
    auto lock = context->lockContext();
    std::optional<size_t> limit;
    { std::lock_guard<std::mutex> guard(_mutex); limit = _cacheLimit; }
    if (*lock && limit) { lock->get()->setCacheLimit(*limit); }
}
void BitmapGraphicsContext::setResourceCacheLimit(size_t bytes) {
    Ref<GrGraphicsContext> context;
    { std::lock_guard<std::mutex> guard(_mutex); _cacheLimit = bytes; context = _context; }
    if (context != nullptr) { applyCacheLimit(context); }
}
void BitmapGraphicsContext::performCleanup(bool purgeScratch, std::chrono::seconds age) {
    Ref<GrGraphicsContext> context;
    { std::lock_guard<std::mutex> guard(_mutex); context = _context; }
    if (context != nullptr) { context->performCleanup(purgeScratch, age); }
}
Ref<DrawableSurface> BitmapGraphicsContext::createBitmapSurface(const Valdi::Ref<Valdi::IBitmap>& bitmap) {
    if (bitmap == nullptr) { return nullptr; }
    return Valdi::makeShared<BitmapDrawableSurface>(getGPUContext(), bitmap, bitmap->getInfo(), nullptr);
}
Ref<DrawableSurface> BitmapGraphicsContext::createBitmapSurface(const Valdi::BitmapInfo& info) {
    return Valdi::makeShared<BitmapDrawableSurface>(getGPUContext(), nullptr, info, nullptr);
}
Ref<DrawableSurface> BitmapGraphicsContext::createBitmapSurface(const Valdi::BitmapInfo& info, void* bytes) {
    return Valdi::makeShared<BitmapDrawableSurface>(getGPUContext(), nullptr, info, bytes);
}
Valdi::Result<Valdi::BytesView> BitmapGraphicsContext::bitmapToPNG(const Valdi::Ref<Valdi::IBitmap>& bitmap) {
    // Encoding the input bitmap does not require a GPU round trip or mutation of its pixels.
    auto image = Image::makeFromBitmap(bitmap, true);
    if (!image) { return image.moveError(); }
    return image.value()->toPNG();
}
} // namespace snap::drawing
