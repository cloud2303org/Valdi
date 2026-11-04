#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/Bitmap.hpp"
#include "snap_drawing/cpp/Utils/TgfxMedia.hpp"
#include <algorithm>
#include <limits>

namespace snap::drawing {
Bitmap::Bitmap(const Valdi::BitmapInfo& info, Valdi::Ref<Valdi::Bytes> pixels)
    : _info(info), _pixels(std::move(pixels)) {}

Valdi::BitmapInfo Bitmap::getInfo() const { return _info; }

void* Bitmap::lockBytes() {
    std::lock_guard<std::mutex> guard(_mutex);
    if (_pixels == nullptr) { return nullptr; }
    if (_locks++ == 0) { _lockedPixels = _pixels; }
    return _lockedPixels->data();
}

void Bitmap::unlockBytes() {
    std::lock_guard<std::mutex> guard(_mutex);
    if (_locks && --_locks == 0) { _lockedPixels = nullptr; }
}

void Bitmap::dispose() {
    std::lock_guard<std::mutex> guard(_mutex);
    // A no-copy image owns a lock; disposal cannot invalidate its pinned data.
    _pixels = nullptr;
}

Valdi::Result<Ref<Bitmap>> Bitmap::make(const Valdi::BitmapInfo& info) {
    auto normalizedInfo = info;
    const auto bytesPerPixel = Valdi::BitmapInfo::bytesPerPixelForColorType(info.colorType);
    if (info.width <= 0 || bytesPerPixel == 0 ||
        static_cast<size_t>(info.width) > std::numeric_limits<size_t>::max() / bytesPerPixel) {
        return Valdi::Error("Invalid bitmap size, stride or format");
    }
    normalizedInfo.rowBytes = std::max(info.rowBytes, static_cast<size_t>(info.width) * bytesPerPixel);
    if (!tgfxMediaValidBitmapInfo(normalizedInfo)) {
        return Valdi::Error("Invalid bitmap size, stride or format");
    }
    auto bytes = Valdi::makeShared<Valdi::Bytes>();
    bytes->resize(normalizedInfo.rowBytes * static_cast<size_t>(normalizedInfo.height));
    return Valdi::makeShared<Bitmap>(normalizedInfo, std::move(bytes));
}
} // namespace snap::drawing
#else
//
//  Bitmap.cpp
//  snap_drawing
//
//  Created by Ramzy Jaber on 3/28/22.
//

#include "snap_drawing/cpp/Utils/Bitmap.hpp"
#include "snap_drawing/cpp/Utils/BitmapUtils.hpp"

namespace snap::drawing {

Bitmap::Bitmap(SkBitmap&& bitmap) : _bitmap(std::move(bitmap)) {}

Valdi::BitmapInfo Bitmap::getInfo() const {
    const auto& pixmap = _bitmap.pixmap();
    auto info = toBitmapInfo(pixmap.info());
    // The pixels may have been allocated with a stride wider than the minimum.
    info.rowBytes = pixmap.rowBytes();
    return info;
}

void* Bitmap::lockBytes() {
    const auto& pixmap = _bitmap.pixmap();
    return pixmap.writable_addr();
}

void Bitmap::unlockBytes() {}

void Bitmap::dispose() {
    _bitmap = SkBitmap();
}

Valdi::Result<Ref<Bitmap>> Bitmap::make(const Valdi::BitmapInfo& info) {
    SkBitmap skBitmap;
    auto imageInfo = toSkiaImageInfo(info);
    if (!tryAllocBitmapPixels(skBitmap, imageInfo, std::max(info.rowBytes, imageInfo.minRowBytes()))) {
        return Valdi::Error("Failed to allocate pixels for bitmap");
    }

    return Valdi::makeShared<Bitmap>(std::move(skBitmap));
}

} // namespace snap::drawing

#endif
