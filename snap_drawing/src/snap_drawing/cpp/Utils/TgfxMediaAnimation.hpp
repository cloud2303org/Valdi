#pragma once
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"
#include "tgfx/core/ImageCodec.h"
#include "snap_drawing/cpp/Utils/Duration.hpp"
#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"
#include <cstddef>
#include <vector>

namespace snap::drawing {
// Per decode, including retained full RGBA frames, compositing/decoder canvas
// workspaces and encoded staging. These local admission limits do not represent
// a process-wide allocator or GPU cache limit.
inline constexpr size_t kTgfxMediaMaxDecodedBytes = 128 * 1024 * 1024;
inline constexpr size_t kTgfxMediaMaxEncodedBytes = 16 * 1024 * 1024;
inline constexpr size_t kTgfxMediaMaxFrames = 4096;
struct TgfxMediaFrame {
    NativeImage image;
    // Exclusive cumulative endpoint, converted once from the decoder timebase.
    Duration endTime;
    std::shared_ptr<tgfx::ImageCodec> codec;
};
struct TgfxMediaFrames {
    int width = 0;
    int height = 0;
    std::vector<TgfxMediaFrame> frames;
};
Valdi::Result<TgfxMediaFrames> tgfxMediaDecodeFrames(const Valdi::BytesView& data);
} // namespace snap::drawing
#endif
