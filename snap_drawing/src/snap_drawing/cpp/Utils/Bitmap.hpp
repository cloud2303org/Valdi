//
//  Bitmap.hpp
//  snap_drawing
//
//  Created by Ramzy Jaber on 3/28/22.
//

#pragma once

#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "valdi_core/cpp/Utils/Bytes.hpp"
#include <mutex>
#else
#include "include/core/SkBitmap.h"
#endif

#include "snap_drawing/cpp/Utils/Aliases.hpp"

namespace snap::drawing {

class Bitmap : public Valdi::IBitmap {
public:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    Bitmap(const Valdi::BitmapInfo& info, Valdi::Ref<Valdi::Bytes> pixels);
#else
    explicit Bitmap(SkBitmap&& bitmap);
#endif

    ~Bitmap() override = default;

    void dispose() override;

    Valdi::BitmapInfo getInfo() const override;

    void* lockBytes() override;

    void unlockBytes() override;

    static Valdi::Result<Ref<Bitmap>> make(const Valdi::BitmapInfo& info);

private:
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    Valdi::BitmapInfo _info;
    Valdi::Ref<Valdi::Bytes> _pixels;
    Valdi::Ref<Valdi::Bytes> _lockedPixels;
    std::mutex _mutex;
    size_t _locks = 0;
#else
    SkBitmap _bitmap;
#endif
};

} // namespace snap::drawing
