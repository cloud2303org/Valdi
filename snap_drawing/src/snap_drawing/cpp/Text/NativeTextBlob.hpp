#pragma once

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/TextBlob.h"
#include "tgfx/core/TextBlobBuilder.h"
#else
#include "include/core/SkTextBlob.h"
#endif

namespace snap::drawing {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
using NativeTextBlob = std::shared_ptr<tgfx::TextBlob>;
#else
using NativeTextBlob = sk_sp<SkTextBlob>;
#endif
} // namespace snap::drawing
