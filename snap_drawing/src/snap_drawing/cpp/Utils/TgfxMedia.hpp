#pragma once

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"
#include "tgfx/core/Data.h"
#include "tgfx/core/ImageCodec.h"
#include "tgfx/core/ImageInfo.h"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Utils/Bytes.hpp"

namespace snap::drawing {

std::shared_ptr<tgfx::Data> tgfxMediaData(const Valdi::BytesView& bytes, bool copy);
Valdi::BytesView tgfxMediaBytes(const std::shared_ptr<tgfx::Data>& data);
bool tgfxMediaValidBitmapInfo(const Valdi::BitmapInfo& info);
// Checks the 128 MiB aggregate CPU-read admission budget before allocating
// output or source/destination F16 staging. Includes destination stride,
// decoder RGBA staging/row pointers and a fixed 1 MiB workspace reserve.
bool tgfxMediaCanReadPixels(const std::shared_ptr<tgfx::ImageCodec>& codec,
                           const Valdi::BitmapInfo& info);

// RGBAF32 is stored losslessly by Bitmap. TGFX has no F32 color type, so transfers
// across this boundary use its real RGBA_F16 type, with explicit numeric conversion.
std::shared_ptr<tgfx::Image> tgfxMediaImage(const Valdi::BitmapInfo& info,
                                         std::shared_ptr<tgfx::Data> data);
std::shared_ptr<tgfx::ImageCodec> tgfxMediaCodec(const Valdi::BitmapInfo& info,
                                              std::shared_ptr<tgfx::Data> data);
bool tgfxMediaReadPixels(const std::shared_ptr<tgfx::ImageCodec>& codec,
                        const Valdi::BitmapInfo& info,
                        void* pixels);
bool tgfxMediaReadPixels(const NativeImage& image,
                        const Valdi::BitmapInfo& info,
                        void* pixels,
                        tgfx::Context* context = nullptr);

} // namespace snap::drawing
#endif
