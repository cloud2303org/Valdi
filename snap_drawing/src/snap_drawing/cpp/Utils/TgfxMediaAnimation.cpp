#include "snap_drawing/cpp/Utils/TgfxMediaAnimation.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#include "tgfx/core/ImageCodec.h"
#include <gif_lib.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace snap::drawing {
namespace {
using Pixels = std::vector<uint8_t>;
bool validDimensions(int width, int height) {
    return tgfx::ImageInfo::IsValidSize(width, height) &&
           static_cast<size_t>(width) <= std::numeric_limits<size_t>::max() / 4 / static_cast<size_t>(height);
}

Valdi::Error capacityError() {
    return Valdi::Error("Animated media capacity exceeded (128 MiB decoded/staging budget, 16 MiB input, 4096 frames)");
}

struct FrameBudget {
    size_t available = 0;
    size_t frameBytes = 0;
    bool admits(size_t frames) const {
        return frameBytes != 0 && frames <= kTgfxMediaMaxFrames && frames <= available / frameBytes;
    }
};

Valdi::Result<FrameBudget> makeFrameBudget(int width, int height, size_t encodedBytes,
                                          size_t workspaceCanvases) {
    if (!validDimensions(width, height)) { return Valdi::Error("Invalid animation canvas dimensions"); }
    if (encodedBytes > kTgfxMediaMaxEncodedBytes) { return capacityError(); }
    const size_t canvasBytes = static_cast<size_t>(width) * height * 4;
    // Charge six source spans for PNG reconstruction, Data copies and vector
    // growth, plus 1 MiB for fixed decoder/row state. Each retained canvas also
    // carries 4 KiB of image/codec/frame-vector metadata allowance.
    size_t available = kTgfxMediaMaxDecodedBytes - 1024 * 1024;
    if (encodedBytes > available / 6) { return capacityError(); }
    available -= encodedBytes * 6;
    if (canvasBytes > available / workspaceCanvases) { return capacityError(); }
    available -= canvasBytes * workspaceCanvases;
    if (canvasBytes > kTgfxMediaMaxDecodedBytes - 4096) { return capacityError(); }
    return FrameBudget{available, canvasBytes + 4096};
}

Valdi::Result<Valdi::Void> appendFrame(TgfxMediaFrames& output, const FrameBudget& budget,
                 const uint8_t* pixels, const Duration& endTime,
                 std::shared_ptr<tgfx::ColorSpace> colorSpace = tgfx::ColorSpace::SRGB()) {
    if (!budget.admits(output.frames.size() + 1)) { return capacityError(); }
    auto info = tgfx::ImageInfo::Make(output.width, output.height, tgfx::ColorType::RGBA_8888,
                                     tgfx::AlphaType::Premultiplied, 0, std::move(colorSpace));
    auto codec = tgfx::ImageCodec::MakeFrom(info, tgfx::Data::MakeWithCopy(pixels, info.byteSize()));
    auto image = codec ? tgfx::Image::MakeFrom(codec) : nullptr;
    if (!image) { return Valdi::Error("Cannot create decoded animation frame image"); }
    // The decoder supplies the cumulative endpoint in its encoded timebase.
    // Adding separately converted second durations rounds exact 300/600 ms
    // boundaries upward and makes a seek at that boundary select the old frame.
    output.frames.push_back({std::move(image), endTime, std::move(codec)});
    return Valdi::Void();
}

struct GifInput { const Valdi::BytesView& data; size_t offset = 0; };
int readGif(GifFileType* file, GifByteType* destination, int count) {
    auto* input = static_cast<GifInput*>(file->UserData);
    if (count <= 0) { return 0; }
    auto size = std::min(static_cast<size_t>(count), input->data.size() - input->offset);
    std::memcpy(destination, input->data.data() + input->offset, size);
    input->offset += size;
    return static_cast<int>(size);
}
struct GifDeleter {
    void operator()(GifFileType* file) const {
        int error = 0;
        DGifCloseFile(file, &error);
    }
};

bool consumeGifExtension(GifFileType* gif, GraphicsControlBlock& control) {
    int code = 0;
    GifByteType* block = nullptr;
    if (DGifGetExtension(gif, &code, &block) != GIF_OK) { return false; }
    if (code == GRAPHICS_EXT_FUNC_CODE &&
        (!block || DGifExtensionToGCB(block[0], block + 1, &control) != GIF_OK)) { return false; }
    while (block) {
        if (DGifGetExtensionNext(gif, &block) != GIF_OK) { return false; }
    }
    return true;
}

struct GifScan {
    TgfxMediaFrames output;
    size_t count = 0;
};

Valdi::Result<GifScan> scanGif(const Valdi::BytesView& data, bool decodePixels) {
    GifInput input{data};
    int error = 0;
    std::unique_ptr<GifFileType, GifDeleter> gif(DGifOpen(&input, readGif, &error));
    if (!gif || !validDimensions(gif->SWidth, gif->SHeight)) {
        return Valdi::Error("Unable to decode GIF animation with giflib");
    }
    // No DGifSlurp or DGifGetImageDesc: neither indexed rasters nor SavedImages
    // are retained by giflib. Header/code-block scans use its fixed LZW state.
    // Three canvases reserve composition, restore-previous and the streamed row.
    auto capacity = makeFrameBudget(gif->SWidth, gif->SHeight, data.size(), 3);
    if (!capacity) { return capacity.moveError(); }
    const auto budget = capacity.value();
    if (!budget.admits(1)) { return capacityError(); }
    GifScan scan;
    auto& output = scan.output;
    output.width = gif->SWidth;
    output.height = gif->SHeight;
    Pixels canvas;
    if (decodePixels) { canvas.resize(static_cast<size_t>(output.width) * output.height * 4); }
    auto background = [&](const GraphicsControlBlock& control) {
        std::array<uint8_t, 4> color{};
        if (control.TransparentColor == NO_TRANSPARENT_COLOR && gif->SColorMap &&
            gif->SBackGroundColor >= 0 && gif->SBackGroundColor < gif->SColorMap->ColorCount) {
            const auto& rgb = gif->SColorMap->Colors[gif->SBackGroundColor];
            color = {rgb.Red, rgb.Green, rgb.Blue, 255};
        }
        return color;
    };
    GraphicsControlBlock control{};
    control.TransparentColor = NO_TRANSPARENT_COLOR;
    int64_t elapsedMilliseconds = 0;
    for (;;) {
        GifRecordType record = UNDEFINED_RECORD_TYPE;
        if (DGifGetRecordType(gif.get(), &record) != GIF_OK) {
            return Valdi::Error("Incomplete GIF record stream");
        }
        if (record == TERMINATE_RECORD_TYPE) { break; }
        if (record == EXTENSION_RECORD_TYPE) {
            if (!consumeGifExtension(gif.get(), control)) { return Valdi::Error("Invalid GIF extension"); }
            continue;
        }
        if (record != IMAGE_DESC_RECORD_TYPE || DGifGetImageHeader(gif.get()) != GIF_OK) {
            return Valdi::Error("Invalid GIF image header");
        }
        const auto& rect = gif->Image;
        const auto* palette = rect.ColorMap ? rect.ColorMap : gif->SColorMap;
        if (!palette || rect.Left < 0 || rect.Top < 0 || rect.Width <= 0 || rect.Height <= 0 ||
            rect.Left > output.width - rect.Width || rect.Top > output.height - rect.Height) {
            return Valdi::Error("Invalid GIF frame rectangle or palette");
        }
        if (!budget.admits(++scan.count)) { return capacityError(); }
        if (!decodePixels) {
            // Metadata preflight consumes compressed subblocks through giflib,
            // without allocating a row, full canvas or decompressed image.
            GifByteType* block = nullptr;
            int codeSize = 0;
            if (DGifGetCode(gif.get(), &codeSize, &block) != GIF_OK) {
                return Valdi::Error("Invalid GIF compressed blocks");
            }
            while (block) {
                if (DGifGetCodeNext(gif.get(), &block) != GIF_OK) {
                    return Valdi::Error("Incomplete GIF compressed blocks");
                }
            }
        } else {
            const auto clearColor = background(control);
            if (scan.count == 1) {
                for (size_t i = 0; i < canvas.size(); i += 4) {
                    std::copy(clearColor.begin(), clearColor.end(), canvas.begin() + i);
                }
            }
            Pixels previous;
            if (control.DisposalMode == DISPOSE_PREVIOUS) { previous = canvas; }
            std::vector<GifPixelType> row(static_cast<size_t>(rect.Width));
            auto drawRow = [&](int y) {
                if (DGifGetLine(gif.get(), row.data(), rect.Width) != GIF_OK) { return false; }
                for (int x = 0; x < rect.Width; ++x) {
                    const int index = row[x];
                    if (index == control.TransparentColor) { continue; }
                    if (index >= palette->ColorCount) { return false; }
                    const auto& color = palette->Colors[index];
                    const auto offset = (static_cast<size_t>(y + rect.Top) * output.width + x + rect.Left) * 4;
                    canvas[offset] = color.Red;
                    canvas[offset + 1] = color.Green;
                    canvas[offset + 2] = color.Blue;
                    canvas[offset + 3] = 255;
                }
                return true;
            };
            if (rect.Interlace) {
                // The same four row passes used by real DGifSlurp, without
                // allocating and retaining its full indexed raster.
                constexpr int offsets[] = {0, 4, 2, 1};
                constexpr int steps[] = {8, 8, 4, 2};
                for (int pass = 0; pass < 4; ++pass) {
                    for (int y = offsets[pass]; y < rect.Height; y += steps[pass]) {
                        if (!drawRow(y)) { return Valdi::Error("Cannot decode GIF interlaced row"); }
                    }
                }
            } else {
                for (int y = 0; y < rect.Height; ++y) {
                    if (!drawRow(y)) { return Valdi::Error("Cannot decode GIF row or color index"); }
                }
            }
            // Keep zero-duration frames and exact integer cumulative timing.
            elapsedMilliseconds += static_cast<int64_t>(control.DelayTime) * 10;
            auto appended = appendFrame(output, budget, canvas.data(),
                Duration::fromMilliseconds(static_cast<double>(elapsedMilliseconds)));
            if (!appended) { return appended.moveError(); }
            if (control.DisposalMode == DISPOSE_PREVIOUS) {
                canvas = std::move(previous);
            } else if (control.DisposalMode == DISPOSE_BACKGROUND) {
                for (int y = 0; y < rect.Height; ++y) {
                    for (int x = 0; x < rect.Width; ++x) {
                        const auto offset = (static_cast<size_t>(y + rect.Top) * output.width + x + rect.Left) * 4;
                        std::copy(clearColor.begin(), clearColor.end(), canvas.begin() + offset);
                    }
                }
            }
        }
        control = {};
        control.TransparentColor = NO_TRANSPARENT_COLOR;
    }
    if (scan.count == 0) { return Valdi::Error("GIF contains no frames"); }
    return scan;
}

Valdi::Result<TgfxMediaFrames> decodeGif(const Valdi::BytesView& data) {
    auto preflight = scanGif(data, false);
    if (!preflight) { return preflight.moveError(); }
    auto decoded = scanGif(data, true);
    if (!decoded) { return decoded.moveError(); }
    if (decoded.value().count != preflight.value().count) { return Valdi::Error("GIF frame count changed during decode"); }
    return std::move(decoded.value().output);
}

Valdi::Result<TgfxMediaFrames> decodeWebP(const Valdi::BytesView& data) {
    WebPData input{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&input), WebPDemuxDelete);
    if (!demux) { return Valdi::Error("Unable to demux WebP animation"); }
    const auto width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    const auto height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    const auto frameCount = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    if (width > INT32_MAX || height > INT32_MAX || frameCount == 0) {
        return Valdi::Error("Invalid WebP animation metadata");
    }
    // WebPAnimDecoderNew allocates TWO full RGBA canvases before returning.
    // Admit the entire retained timeline before calling it, with another six
    // canvas-equivalents reserved for the native pixel/transform/row workspace.
    auto capacity = makeFrameBudget(static_cast<int>(width), static_cast<int>(height), data.size(), 8);
    if (!capacity) { return capacity.moveError(); }
    const auto budget = capacity.value();
    if (!budget.admits(frameCount)) { return capacityError(); }
    demux.reset(); // Do not keep two copies of demux frame/chunk metadata.
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) { return Valdi::Error("Incompatible libwebpdemux ABI"); }
    options.color_mode = MODE_rgbA; // Actual premultiplied RGBA, composited by libwebpdemux.
    options.use_threads = 0;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
        WebPAnimDecoderNew(&input, &options), WebPAnimDecoderDelete);
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info) ||
        info.canvas_width > INT32_MAX || info.canvas_height > INT32_MAX ||
        !validDimensions(static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height)) ||
        info.canvas_width != width || info.canvas_height != height || info.frame_count != frameCount) {
        return Valdi::Error("Unable to decode WebP animation");
    }
    TgfxMediaFrames output;
    output.width = static_cast<int>(info.canvas_width);
    output.height = static_cast<int>(info.canvas_height);
    auto colorSpace = tgfx::ColorSpace::SRGB();
    WebPChunkIterator profile{};
    if (WebPDemuxGetChunk(WebPAnimDecoderGetDemuxer(decoder.get()), "ICCP", 1, &profile)) {
        auto parsed = tgfx::ColorSpace::MakeFromICC(profile.chunk.bytes, profile.chunk.size);
        if (parsed) { colorSpace = std::move(parsed); }
        WebPDemuxReleaseChunkIterator(&profile);
    }
    int previousTime = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
        uint8_t* pixels = nullptr;
        int timestamp = 0;
        // libwebp returns the cumulative frame endpoint in milliseconds.
        // Preserve it directly rather than subtracting and re-adding seconds.
        if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) || timestamp < previousTime || !pixels) {
            return Valdi::Error("Unable to decode WebP animation frame");
        }
        auto appended = appendFrame(output, budget, pixels, Duration::fromMilliseconds(timestamp), colorSpace);
        if (!appended) { return appended.moveError(); }
        previousTime = timestamp;
    }
    if (output.frames.size() != info.frame_count || output.frames.empty()) {
        return Valdi::Error("Incomplete WebP animation");
    }
    return output;
}

uint32_t read32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}
void write32(Pixels& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value >> 24));
    data.push_back(static_cast<uint8_t>(value >> 16));
    data.push_back(static_cast<uint8_t>(value >> 8));
    data.push_back(static_cast<uint8_t>(value));
}
void chunk(Pixels& png, const char* type, const uint8_t* bytes, size_t length) {
    write32(png, static_cast<uint32_t>(length));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    if (length) { png.insert(png.end(), bytes, bytes + length); }
    // PNG chunks are bounded to uint32_t by their format.
    auto crc = crc32(0, png.data() + start, 4);
    if (length) { crc = crc32(crc, png.data() + start + 4, static_cast<uInt>(length)); }
    write32(png, static_cast<uint32_t>(crc));
}

struct PngFrame {
    uint32_t width = 0, height = 0, left = 0, top = 0;
    uint16_t delayNumerator = 0, delayDenominator = 100;
    uint8_t dispose = 0, blend = 0;
    Pixels compressed;
};

Valdi::Result<TgfxMediaFrames> decodePng(const Valdi::BytesView& data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    std::array<uint8_t, 13> header{};
    Pixels sharedChunks;
    std::vector<PngFrame> frames;
    uint32_t expectedFrames = 0, sequence = 0;
    bool gotHeader = false, gotData = false, ended = false;
    int width = 0, height = 0;
    FrameBudget budget;
    for (size_t offset = 8; offset < data.size();) {
        if (data.size() - offset < 12) { return Valdi::Error("Truncated PNG chunk"); }
        const auto length = read32(bytes + offset);
        if (length > data.size() - offset - 12) { return Valdi::Error("Truncated PNG chunk data"); }
        const auto* type = bytes + offset + 4;
        const auto* body = bytes + offset + 8;
        auto crc = crc32(0, type, 4);
        crc = crc32(crc, body, length);
        if (static_cast<uint32_t>(crc) != read32(body + length)) { return Valdi::Error("Invalid PNG chunk CRC"); }
        auto is = [&](const char* name) { return std::memcmp(type, name, 4) == 0; };
        if (is("IHDR")) {
            if (gotHeader || offset != 8 || length != 13) { return Valdi::Error("Invalid PNG header"); }
            std::copy(body, body + 13, header.begin());
            const auto w = read32(body), h = read32(body + 4);
            if (w > INT32_MAX || h > INT32_MAX || !validDimensions(static_cast<int>(w), static_cast<int>(h))) {
                return Valdi::Error("Invalid PNG dimensions");
            }
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            // Composite + restore-previous + frame pixels + TGFX/libpng
            // conversion storage, row pointers and decoder row workspace.
            auto capacity = makeFrameBudget(width, height, data.size(), 8);
            if (!capacity) { return capacity.moveError(); }
            budget = capacity.value();
            if (!budget.admits(1)) { return capacityError(); }
            gotHeader = true;
        } else if (!gotHeader) {
            return Valdi::Error("PNG has no header");
        } else if (is("acTL")) {
            if (gotData || expectedFrames || length != 8 || read32(body) == 0) {
                return Valdi::Error("Invalid APNG animation control");
            }
            expectedFrames = read32(body);
            if (!budget.admits(expectedFrames)) { return capacityError(); }
        } else if (is("fcTL")) {
            if (!expectedFrames || length != 26 || read32(body) != sequence++) {
                return Valdi::Error("Invalid APNG frame sequence");
            }
            if (!frames.empty() && frames.back().compressed.empty()) {
                return Valdi::Error("APNG frame has no pixels");
            }
            if (frames.size() >= expectedFrames) { return Valdi::Error("APNG contains more frames than declared"); }
            if (!budget.admits(frames.size() + 1)) { return capacityError(); }
            PngFrame frame;
            frame.width = read32(body + 4); frame.height = read32(body + 8);
            frame.left = read32(body + 12); frame.top = read32(body + 16);
            frame.delayNumerator = static_cast<uint16_t>((body[20] << 8) | body[21]);
            frame.delayDenominator = static_cast<uint16_t>((body[22] << 8) | body[23]);
            frame.dispose = body[24]; frame.blend = body[25];
            if (!frame.width || !frame.height || frame.width > static_cast<uint32_t>(width) ||
                frame.height > static_cast<uint32_t>(height) || frame.left > width - frame.width ||
                frame.top > height - frame.height || frame.dispose > 2 || frame.blend > 1 ||
                (!gotData && (frame.width != static_cast<uint32_t>(width) ||
                              frame.height != static_cast<uint32_t>(height) || frame.left || frame.top))) {
                return Valdi::Error("Invalid APNG frame rectangle or operation");
            }
            frames.push_back(std::move(frame));
        } else if (is("IDAT")) {
            gotData = true;
            // Without preceding fcTL this is the default poster, not an
            // animation frame. Do not display it as a synthesized extra frame.
            if (!frames.empty()) {
                if (frames.size() != 1) { return Valdi::Error("Unexpected APNG IDAT frame"); }
                frames.back().compressed.insert(frames.back().compressed.end(), body, body + length);
            }
        } else if (is("fdAT")) {
            if (length < 4 || frames.empty() || !gotData || read32(body) != sequence++) {
                return Valdi::Error("Invalid APNG frame data sequence");
            }
            frames.back().compressed.insert(frames.back().compressed.end(), body + 4, body + length);
        } else if (is("IEND")) {
            if (length) { return Valdi::Error("Invalid PNG end"); }
            ended = true;
            break;
        } else if (!gotData) {
            // Preserve palette, transparency, ICC/gamma, and all pre-IDAT
            // decoding metadata in the standalone PNG given to the real codec.
            sharedChunks.insert(sharedChunks.end(), bytes + offset, bytes + offset + length + 12);
        }
        offset += static_cast<size_t>(length) + 12;
    }
    if (!gotHeader || !gotData || !ended) { return Valdi::Error("Incomplete PNG image"); }
    if (!expectedFrames) {
        auto codec = tgfx::ImageCodec::MakeFrom(skDataFromBytes(data, DataConversionModeCopyIfUnsafe));
        auto image = codec ? tgfx::Image::MakeFrom(codec) : nullptr;
        if (!image) { return Valdi::Error("Unable to decode PNG image"); }
        TgfxMediaFrames output{image->width(), image->height(), {{image, Duration(), std::move(codec)}}};
        return output;
    }
    if (frames.size() != expectedFrames || frames.back().compressed.empty()) {
        return Valdi::Error("Incomplete APNG frame timeline");
    }
    TgfxMediaFrames output{width, height, {}};
    Pixels canvas(static_cast<size_t>(width) * height * 4);
    double elapsedMilliseconds = 0;
    for (const auto& frame : frames) {
        Pixels png(bytes, bytes + 8);
        Pixels frameHeader(header.begin(), header.end());
        for (int i = 0; i < 4; ++i) {
            frameHeader[i] = static_cast<uint8_t>(frame.width >> (24 - i * 8));
            frameHeader[i + 4] = static_cast<uint8_t>(frame.height >> (24 - i * 8));
        }
        chunk(png, "IHDR", frameHeader.data(), frameHeader.size());
        png.insert(png.end(), sharedChunks.begin(), sharedChunks.end());
        // A PNG IDAT is limited by its uint32 length; reject oversized assembly.
        if (frame.compressed.size() > std::numeric_limits<uint32_t>::max()) {
            return Valdi::Error("APNG frame compressed data is too large");
        }
        chunk(png, "IDAT", frame.compressed.data(), frame.compressed.size());
        chunk(png, "IEND", nullptr, 0);
        auto codec = tgfx::ImageCodec::MakeFrom(tgfx::Data::MakeWithCopy(png.data(), png.size()));
        auto info = tgfx::ImageInfo::Make(static_cast<int>(frame.width), static_cast<int>(frame.height),
                                         tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied,
                                         0, tgfx::ColorSpace::SRGB());
        Pixels decoded(info.byteSize());
        if (!codec || !codec->readPixels(info, decoded.data())) { return Valdi::Error("Cannot decode APNG frame PNG"); }
        Pixels previous;
        if (frame.dispose == 2) { previous = canvas; }
        for (uint32_t y = 0; y < frame.height; ++y) {
            for (uint32_t x = 0; x < frame.width; ++x) {
                const auto* source = decoded.data() + (static_cast<size_t>(y) * frame.width + x) * 4;
                auto* target = canvas.data() + (static_cast<size_t>(y + frame.top) * width + x + frame.left) * 4;
                if (frame.blend == 0) {
                    std::copy(source, source + 4, target);
                } else {
                    const unsigned inverse = 255u - source[3];
                    for (int channel = 0; channel < 4; ++channel) {
                        target[channel] = static_cast<uint8_t>(std::min(255u, source[channel] +
                                                               (target[channel] * inverse + 127u) / 255u));
                    }
                }
            }
        }
        elapsedMilliseconds += 1000.0 * frame.delayNumerator /
                               (frame.delayDenominator ? frame.delayDenominator : 100);
        auto appended = appendFrame(output, budget, canvas.data(), Duration::fromMilliseconds(elapsedMilliseconds));
        if (!appended) { return appended.moveError(); }
        if (frame.dispose == 2) {
            canvas = std::move(previous);
        } else if (frame.dispose == 1) {
            for (uint32_t y = 0; y < frame.height; ++y) {
                auto* row = canvas.data() + (static_cast<size_t>(y + frame.top) * width + frame.left) * 4;
                std::fill(row, row + static_cast<size_t>(frame.width) * 4, 0);
            }
        }
    }
    return output;
}
} // namespace

Valdi::Result<TgfxMediaFrames> tgfxMediaDecodeFrames(const Valdi::BytesView& data) {
    if (!data.data() || data.empty()) { return Valdi::Error("Empty image payload"); }
    if (data.size() > kTgfxMediaMaxEncodedBytes) { return capacityError(); }
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    if (data.size() >= 6 && (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0)) {
        return decodeGif(data);
    }
    if (data.size() >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0) {
        return decodeWebP(data);
    }
    static constexpr uint8_t pngMagic[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data.size() >= 8 && std::memcmp(bytes, pngMagic, 8) == 0) { return decodePng(data); }
    auto codec = tgfx::ImageCodec::MakeFrom(skDataFromBytes(data, DataConversionModeCopyIfUnsafe));
    auto image = codec ? tgfx::Image::MakeFrom(codec) : nullptr;
    if (!image) {
        auto message = describeUndecodablePayload(data, "Unsupported image format");
        return Valdi::Error(std::string_view(message));
    }
    return TgfxMediaFrames{image->width(), image->height(), {{image, Duration(), std::move(codec)}}};
}
} // namespace snap::drawing
#endif
