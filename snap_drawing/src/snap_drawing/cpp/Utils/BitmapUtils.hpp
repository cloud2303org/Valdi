//
//  BitmapUtils.hpp
//  snap_drawing-pc
//
//  Created by Simon Corsin on 1/24/22.
//

#pragma once

#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Data.h"
#include "tgfx/core/ImageInfo.h"
#else
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

#endif

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
Valdi::BitmapInfo toBitmapInfo(const tgfx::ImageInfo& info);
// Historical seam names; these return actual TGFX types in the TGFX build.
tgfx::ImageInfo toSkiaImageInfo(const Valdi::BitmapInfo& info);
tgfx::ColorType toSkiaColorType(Valdi::ColorType colorType);
Valdi::Result<std::shared_ptr<tgfx::Data>> bitmapToData(const Valdi::Ref<Valdi::IBitmap>& bitmap,
                                                     const Valdi::BitmapInfo& info,
                                                     bool copy);
#else
Valdi::BitmapInfo toBitmapInfo(const SkImageInfo& info);

SkImageInfo toSkiaImageInfo(const Valdi::BitmapInfo& info);
SkColorType toSkiaColorType(Valdi::ColorType colorType);

/**
 * Allocates the bitmap's pixels with `rowBytes` for every row, including the last, so the allocation covers
 * `Valdi::BitmapInfo::size()` (height * rowBytes). SkBitmap::tryAllocPixels() only allocates the minimum
 * width for the last row.
 */
bool tryAllocBitmapPixels(SkBitmap& bitmap, const SkImageInfo& info, size_t rowBytes);

Valdi::Result<sk_sp<SkData>> bitmapToData(const Valdi::Ref<Valdi::IBitmap>& bitmap,
                                          const Valdi::BitmapInfo& info,
                                          bool copy);

#endif

} // namespace snap::drawing
