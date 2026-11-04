#pragma once

#include <memory>

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Canvas.h"
#include "tgfx/core/Data.h"
#include "tgfx/core/Font.h"
#include "tgfx/core/Image.h"
#include "tgfx/core/Picture.h"
#include "tgfx/core/Surface.h"
#include "tgfx/core/Typeface.h"
#else
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#endif

namespace snap::drawing {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
using NativeCanvas = tgfx::Canvas;
using NativeData = std::shared_ptr<tgfx::Data>;
using NativeDataObject = tgfx::Data;
using NativeFont = tgfx::Font;
using NativeTypeface = std::shared_ptr<tgfx::Typeface>;
using NativeImage = std::shared_ptr<tgfx::Image>;
using NativePicture = std::shared_ptr<tgfx::Picture>;
using NativePictureObject = tgfx::Picture;
using NativeSurface = tgfx::Surface;
#else
using NativeCanvas = SkCanvas;
using NativeData = sk_sp<SkData>;
using NativeDataObject = SkData;
using NativeFont = SkFont;
using NativeTypeface = sk_sp<SkTypeface>;
using NativeImage = sk_sp<SkImage>;
using NativePicture = sk_sp<SkPicture>;
using NativePictureObject = SkPicture;
using NativeSurface = SkSurface;
#endif
} // namespace snap::drawing
