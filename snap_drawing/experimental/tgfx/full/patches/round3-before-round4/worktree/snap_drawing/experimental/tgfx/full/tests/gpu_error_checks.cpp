#include "gpu_error_checks.hpp"

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "gpu_error_checks requires the actual complete TGFX production backend"
#endif

#include "snap_drawing/cpp/Drawing/GraphicsContext/BitmapGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/TGFXSurfaceCompletion.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "tgfx/core/Data.h"
#include "tgfx/core/ImageFilter.h"
#include "tgfx/core/PictureRecorder.h"
#include "tgfx/gpu/CommandEncoder.h"
#include "tgfx/core/Task.h"
#include "tgfx/core/ImageCodec.h"
#include "tgfx/core/RenderFlags.h"
#include "tgfx/platform/HardwareBuffer.h"
// These are the actual production worker/upload owners. The fixture target
// needs the TGFX src include directory, in addition to its public include tree.
#include "gpu/DrawingManager.h"
#include "gpu/ProxyProvider.h"
#include "gpu/VertexProvider.h"
#include "gpu/tasks/GPUBufferUploadTask.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace snap::drawing;
constexpr int kSize = 16;

struct Check {
    bool passed = true;
    std::string details;
    bool expect(bool condition, const std::string& message) {
        if (!condition) {
            passed = false;
            if (!details.empty()) { details += "; "; }
            details += message;
        }
        return condition;
    }
};

std::shared_ptr<tgfx::Surface> surface(Check& check, tgfx::Context* context) {
    auto result = tgfx::Surface::Make(context, kSize, kSize, tgfx::ColorType::RGBA_8888);
    if (check.expect(result != nullptr, "Real TGFX offscreen surface allocation failed")) {
        result->getCanvas()->clear(tgfx::Color::Blue());
    }
    return result;
}

// Public native failure path, not a test hook/error flag: a nonfinite destination
// subset cannot be captured. Balance the failed save according to Canvas's contract.
bool rejectCapture(tgfx::Canvas* canvas) {
    const auto depth = canvas->getSaveCount();
    const auto count = canvas->saveBehind(tgfx::Rect::MakeXYWH(
        std::numeric_limits<float>::quiet_NaN(), 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)));
    canvas->restoreToCount(depth);
    return count < 0 && canvas->hasError();
}

struct DeferredCapture {
    int runs = 0;
    bool rejected = false;
};

std::shared_ptr<DeferredCapture> deferCaptureFailure(Check& check, tgfx::Canvas* destination) {
    auto capture = std::make_shared<DeferredCapture>();
    tgfx::PictureRecorder recorder;
    recorder.beginRecording()->drawDeferred(tgfx::Rect::MakeWH(kSize, kSize), 0,
        [capture](tgfx::Canvas* canvas) {
            ++capture->runs;
            capture->rejected = rejectCapture(canvas);
        });
    auto picture = recorder.finishRecordingAsPicture();
    if (!check.expect(picture != nullptr, "Failed to record native deferred capture")) { return nullptr; }
    auto image = tgfx::Image::MakeFrom(std::move(picture), kSize, kSize);
    if (!check.expect(image != nullptr, "Failed to create real lazy PictureImage")) { return nullptr; }
    // This pending image is materialized when TGFX closes the compositor. The
    // preconditions below ensure the case actually tests a post-operation failure.
    destination->drawImage(std::move(image));
    if (!check.expect(capture->runs == 0 && !destination->hasError(),
                      "Deferred failure ran before the production completion boundary")) { return nullptr; }
    return capture;
}

void bluePixels(Check& check, const NativeData& data) {
    if (!check.expect(data != nullptr && data->size() == static_cast<size_t>(kSize * kSize * 4),
                      "Missing complete owned RGBA readback")) { return; }
    const auto* pixels = static_cast<const uint8_t*>(data->data());
    for (int i = 0; i < kSize * kSize; ++i) {
        const auto offset = static_cast<size_t>(i) * 4;
        if (!check.expect(pixels[offset] == 0 && pixels[offset + 1] == 0 &&
                          pixels[offset + 2] == 255 && pixels[offset + 3] == 255,
                          "Unmodified destination pixels are not opaque blue")) { return; }
    }
}

void healthySnapshot(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target) { return; }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    auto snapshot = canvas.snapshot();
    if (!check.expect(snapshot.success(), "Supported production snapshot returned an error")) { return; }
    check.expect(snapshot.value() != nullptr && snapshot.value()->width() == kSize &&
                 snapshot.value()->height() == kSize, "Snapshot wrapper/dimensions are invalid");
    auto pixels = canvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888));
    if (check.expect(pixels.success(), "Supported production readback returned an error")) {
        bluePixels(check, pixels.value());
    }
}

void failedSnapshot(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target || !check.expect(rejectCapture(target->getCanvas()), "Native capture failure was not observable")) {
        return;
    }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    auto snapshot = canvas.snapshot();
    check.expect(snapshot.failure(), "Production snapshot published an image from an already-failed destination");
    auto nativeSnapshot = canvas.makeImageSnapshot();
    check.expect(nativeSnapshot.failure(), "GPU snapshot published an already-failed destination");
}

void deferredSnapshot(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target) { return; }
    auto capture = deferCaptureFailure(check, target->getCanvas());
    if (!capture) { return; }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    auto snapshot = canvas.snapshot();
    check.expect(capture->runs > 0 && capture->rejected,
                 "Readback did not materialize the real deferred capture failure");
    check.expect(target->getCanvas()->hasError(), "Deferred child failure did not reach its destination");
    check.expect(snapshot.failure(), "Production snapshot accepted a failure first raised during readback");
}

void deferredFlush(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target) { return; }
    auto capture = deferCaptureFailure(check, target->getCanvas());
    if (!capture) { return; }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    auto completed = canvas.flush(true);
    check.expect(capture->runs > 0 && capture->rejected, "Flush did not materialize the deferred capture failure");
    check.expect(completed.failure(), "Checked production GPU flush reported successful native completion");
}

void failedReadback(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target) { return; }
    auto capture = deferCaptureFailure(check, target->getCanvas());
    if (!capture) { return; }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    auto pixels = canvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888));
    check.expect(capture->runs > 0 && capture->rejected, "Readback did not reach deferred materialization");
    check.expect(pixels.failure(), "Checked production readback exposed owned pixels after a native failure");
}

void failedPresentation(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    // Establish destination contents in a separate completed batch. Cancellation
    // is atomic per flush, so independent content must precede the rejected batch.
    auto destination = surface(check, context);
    if (!destination) { return; }
    DrawableSurfaceCanvas destinationCanvas(destination.get(), kSize, kSize);
    if (!check.expect(destinationCanvas.flush(true).success(), "Failed to establish destination pixels")) { return; }
    auto source = surface(check, context);
    if (!source) { return; }
    auto capture = deferCaptureFailure(check, source->getCanvas());
    if (!capture) { return; }
    auto completed = presentTGFXSurface(source.get(), destination.get());
    check.expect(capture->runs > 0 && capture->rejected, "Presentation did not check materialized source work");
    if (!check.expect(completed.failure(), "Presentation accepted a known-failed source")) { return; }
    auto pixels = destinationCanvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888));
    if (check.expect(pixels.success(), "Refused source damaged the independent destination's completion state")) {
        bluePixels(check, pixels.value());
    }
}

void healthyPresentation(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto source = surface(check, context);
    auto destination = surface(check, context);
    if (!source || !destination) { return; }
    destination->getCanvas()->clear(tgfx::Color::Red());
    auto completed = presentTGFXSurface(source.get(), destination.get());
    if (!check.expect(completed.success(), "Supported source-to-destination completion failed")) { return; }
    DrawableSurfaceCanvas canvas(destination.get(), kSize, kSize);
    auto pixels = canvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888));
    if (check.expect(pixels.success(), "Supported presentation result could not be read back")) {
        bluePixels(check, pixels.value());
    }
}

void bitmapCompletion(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context*) {
    constexpr size_t rowBytes = kSize * 4 + 8;
    std::vector<uint8_t> pixels(rowBytes * kSize, 0x5a);
    Valdi::BitmapInfo info(kSize, kSize, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, rowBytes);
    BitmapGraphicsContext bitmapContext(graphics);
    auto drawable = bitmapContext.createBitmapSurface(info, pixels.data());
    if (!check.expect(drawable != nullptr, "Failed to create production bitmap drawable")) { return; }
    auto canvas = drawable->prepareCanvas();
    if (!check.expect(canvas.success(), "Failed to prepare production bitmap canvas")) { return; }
    canvas.value().getNativeCanvas()->clear(tgfx::Color::Green());
    auto completed = drawable->flushWithResult();
    if (!check.expect(completed.success(), "Production bitmap completion returned an error")) { return; }
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const auto offset = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4;
            if (!check.expect(pixels[offset] == 0 && pixels[offset + 1] == 255 &&
                              pixels[offset + 2] == 0 && pixels[offset + 3] == 255,
                              "Checked bitmap completion did not publish opaque green pixels")) { return; }
        }
        for (size_t x = kSize * 4; x < rowBytes; ++x) {
            if (!check.expect(pixels[static_cast<size_t>(y) * rowBytes + x] == 0x5a,
                              "Checked bitmap completion overwrote caller row padding")) { return; }
        }
    }
}

void failedBitmap(Check& check, const Ref<GrGraphicsContext>& graphics, bool deferred) {
    constexpr size_t rowBytes = kSize * 4 + 8;
    std::vector<uint8_t> pixels(rowBytes * kSize, 0x5a);
    const auto original = pixels;
    Valdi::BitmapInfo info(kSize, kSize, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, rowBytes);
    BitmapGraphicsContext bitmapContext(graphics);
    auto drawable = bitmapContext.createBitmapSurface(info, pixels.data());
    if (!check.expect(drawable != nullptr, "Could not create real bitmap surface for failure case")) { return; }
    auto canvas = drawable->prepareCanvas();
    if (!check.expect(canvas.success(), "Could not prepare real bitmap surface for failure case")) { return; }
    std::shared_ptr<DeferredCapture> capture;
    if (deferred) {
        capture = deferCaptureFailure(check, canvas.value().getNativeCanvas());
        if (!capture) { return; }
    } else if (!check.expect(rejectCapture(canvas.value().getNativeCanvas()),
                             "Expected public native capture rejection did not occur")) {
        return;
    }
    auto completed = drawable->flushWithResult();
    check.expect(completed.failure(), "Failed production bitmap flush returned success");
    check.expect(pixels == original, "Failed bitmap completion changed caller bytes or row padding");
    if (capture) {
        check.expect(capture->runs > 0 && capture->rejected,
                     "Bitmap completion did not materialize the deferred native failure");
    }
    auto repeated = drawable->flushWithResult();
    check.expect(repeated.failure(), "Repeated failed completion forgot its error before a new frame");
    // Failure is local to the completed destination/frame, not a global poison
    // flag. A new prepared frame on the same production drawable can succeed.
    auto retry = drawable->prepareCanvas();
    if (!check.expect(retry.success(), "Failed bitmap frame could not be prepared again")) { return; }
    retry.value().getNativeCanvas()->clear(tgfx::Color::Green());
    auto recovered = drawable->flushWithResult();
    if (!check.expect(recovered.success(), "Supported bitmap frame did not recover after native failure")) { return; }
    check.expect(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255,
                 "Recovered bitmap frame did not publish its supported drawing");
}

void existingBitmapFailure(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context*) {
    failedBitmap(check, graphics, false);
}

void deferredBitmapFailure(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context*) {
    failedBitmap(check, graphics, true);
}

void failedBitmapCleanup(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context*) {
    constexpr size_t rowBytes = kSize * 4;
    std::vector<uint8_t> pixels(rowBytes * kSize, 0x5a);
    const auto original = pixels;
    Valdi::BitmapInfo info(kSize, kSize, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, rowBytes);
    {
        BitmapGraphicsContext bitmapContext(graphics);
        auto drawable = bitmapContext.createBitmapSurface(info, pixels.data());
        if (!check.expect(drawable != nullptr, "Failed to create cleanup-case bitmap drawable")) { return; }
        auto canvas = drawable->prepareCanvas();
        if (!check.expect(canvas.success(), "Failed to prepare cleanup-case bitmap drawable")) { return; }
        if (!check.expect(rejectCapture(canvas.value().getNativeCanvas()),
                          "Cleanup case did not enter a real native failure")) { return; }
        // Do not complete the invalid operation. Destruction must release state
        // without readback, publication, or an abort for this unsupported frame.
    }
    check.expect(pixels == original, "Destructor cleanup published an unfinished failed bitmap frame");
}

struct SubmissionState {
    int runs = 0;
    int destroyed = 0;
};

// A real RuntimeDrawTask owns this effect until encoding or cancellation. The
// public callback copies a texture with the native encoder and records execution;
// its destructor witnesses task lifetime without a production test-only getter.
class SubmissionProbe final : public tgfx::RuntimeEffect {
public:
    explicit SubmissionProbe(std::shared_ptr<SubmissionState> state) : _state(std::move(state)) {}
    ~SubmissionProbe() override { ++_state->destroyed; }
protected:
    bool onDraw(tgfx::CommandEncoder* encoder,
                const std::vector<std::shared_ptr<tgfx::Texture>>& inputs,
                std::shared_ptr<tgfx::Texture> output, const tgfx::Point&) const override {
        ++_state->runs;
        if (inputs.empty() || !inputs.front() || !output) { return false; }
        encoder->copyTextureToTexture(inputs.front(), tgfx::Rect::MakeWH(kSize, kSize),
                                      std::move(output), tgfx::Point::Zero());
        return true;
    }
private:
    std::shared_ptr<SubmissionState> _state;
};

std::weak_ptr<SubmissionProbe> queueProbe(Check& check, tgfx::Canvas* canvas,
                                         const std::shared_ptr<SubmissionState>& state) {
    std::vector<uint8_t> blue(static_cast<size_t>(kSize * kSize * 4), 0);
    for (size_t i = 0; i < blue.size(); i += 4) { blue[i + 2] = 255; blue[i + 3] = 255; }
    auto data = tgfx::Data::MakeWithCopy(blue.data(), blue.size());
    auto image = tgfx::Image::MakeFrom(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888),
                                      std::move(data));
    auto probe = std::make_shared<SubmissionProbe>(state);
    std::weak_ptr<SubmissionProbe> weak = probe;
    auto filter = tgfx::ImageFilter::Runtime(std::move(probe));
    if (!check.expect(image != nullptr && filter != nullptr, "Failed to create actual submission probe")) {
        return weak;
    }
    image = image->makeWithFilter(std::move(filter));
    if (!check.expect(image != nullptr, "Failed to apply actual runtime probe filter")) { return weak; }
    canvas->drawImage(std::move(image));
    return weak;
}

void canceledProbe(Check& check, const std::shared_ptr<SubmissionState>& state,
                   const std::weak_ptr<SubmissionProbe>& weak) {
    check.expect(state->runs == 0, "Rejected batch executed a real native RuntimeDrawTask");
    check.expect(weak.expired() && state->destroyed == 1,
                 "Rejected batch retained task-owned payload in the queue or reuse pool");
}

void nativeCancellation(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto before = surface(check, context);
    if (!before) { return; }
    auto beforeState = std::make_shared<SubmissionState>();
    auto beforeWeak = queueProbe(check, before->getCanvas(), beforeState);
    auto beforeToken = context->flush();
    auto rejected = surface(check, context);
    if (!rejected) { return; }
    auto rejectedState = std::make_shared<SubmissionState>();
    auto rejectedWeak = queueProbe(check, rejected->getCanvas(), rejectedState);
    auto rejectedToken = context->flush();
    auto after = surface(check, context);
    if (!after) { return; }
    auto afterState = std::make_shared<SubmissionState>();
    auto afterWeak = queueProbe(check, after->getCanvas(), afterState);
    auto afterToken = context->flush();
    if (!check.expect(beforeToken && rejectedToken && afterToken, "Native probe batches were not recorded")) {
        return;
    }
    check.expect(context->discard(std::move(rejectedToken)), "Exact pending batch was not canceled");
    canceledProbe(check, rejectedState, rejectedWeak);
    rejected.reset();
    check.expect(!beforeWeak.expired() && !afterWeak.expired() &&
                 beforeState->runs == 0 && afterState->runs == 0,
                 "Canceling the middle batch changed an unrelated queued neighbor");
    // Queue more work while the canceled buffer is eligible for reuse.
    auto later = surface(check, context);
    if (!later) { return; }
    auto laterState = std::make_shared<SubmissionState>();
    auto laterWeak = queueProbe(check, later->getCanvas(), laterState);
    auto laterToken = context->flush();
    if (!check.expect(laterToken != nullptr, "Failed to record reused-buffer batch")) { return; }
    context->submit(std::move(afterToken), true);
    check.expect(beforeState->runs == 1 && afterState->runs == 1 && laterState->runs == 0 &&
                 !laterWeak.expired(), "Submitting a neighbor lost queue order or submitted a later batch");
    canceledProbe(check, rejectedState, rejectedWeak);
    // Keep an implicitly submitted token alive across additional recording.
    auto newest = surface(check, context);
    if (!newest) { return; }
    auto newestState = std::make_shared<SubmissionState>();
    auto newestWeak = queueProbe(check, newest->getCanvas(), newestState);
    auto newestToken = context->flush();
    if (!check.expect(newestToken != nullptr, "Failed to record stale-generation control")) { return; }
    check.expect(!context->discard(std::move(beforeToken)), "Stale submitted token canceled a recycled generation");
    check.expect(!newestWeak.expired() && newestState->runs == 0,
                 "Stale-token cancellation released live replacement work");
    context->submit(std::move(newestToken), true);
    check.expect(laterState->runs == 1 && newestState->runs == 1,
                 "Unrelated batches did not survive cancellation and generation reuse");
    canceledProbe(check, rejectedState, rejectedWeak);
    check.expect(!context->discard(std::move(laterToken)), "Already submitted batch remained cancellable");
}

enum class RejectionBoundary { Flush, Snapshot, Readback, Bitmap };

void repeatedRejection(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context* context,
                       RejectionBoundary boundary) {
    // An independent earlier batch must remain unsubmitted through every failure,
    // then execute once when a subsequent healthy production completion submits.
    auto before = surface(check, context);
    if (!before) { return; }
    auto beforeState = std::make_shared<SubmissionState>();
    auto beforeWeak = queueProbe(check, before->getCanvas(), beforeState);
    auto beforeToken = context->flush();
    if (!check.expect(beforeToken != nullptr, "Failed to queue independent production neighbor")) { return; }
    constexpr size_t rowBytes = kSize * 4 + 8;
    std::vector<uint8_t> callerPixels(rowBytes * kSize, 0x5a);
    const auto original = callerPixels;
    Valdi::BitmapInfo bitmapInfo(kSize, kSize, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, rowBytes);
    BitmapGraphicsContext bitmapContext(graphics);
    auto drawable = bitmapContext.createBitmapSurface(bitmapInfo, callerPixels.data());
    if (!check.expect(drawable != nullptr, "Failed to allocate production retry drawable")) { return; }
    context->purgeResourcesUntilMemoryTo(0);
    const auto baseline = context->memoryUsage();
    std::vector<std::shared_ptr<SubmissionState>> states;
    for (int iteration = 0; iteration < 32; ++iteration) {
        std::shared_ptr<tgfx::Surface> target;
        auto state = std::make_shared<SubmissionState>();
        states.push_back(state);
        std::weak_ptr<SubmissionProbe> weak;
        std::shared_ptr<DeferredCapture> capture;
        if (boundary == RejectionBoundary::Bitmap) {
            auto canvas = drawable->prepareCanvas();
            if (!check.expect(canvas.success(), "Failed to prepare repeated bitmap rejection")) { return; }
            weak = queueProbe(check, canvas.value().getNativeCanvas(), state);
            capture = deferCaptureFailure(check, canvas.value().getNativeCanvas());
            if (!capture) { return; }
            check.expect(drawable->flushWithResult().failure(), "Repeated failed bitmap flush published success");
            check.expect(callerPixels == original, "Repeated rejected bitmap changed caller bytes/padding");
        } else {
            target = surface(check, context);
            if (!target) { return; }
            weak = queueProbe(check, target->getCanvas(), state);
            capture = deferCaptureFailure(check, target->getCanvas());
            if (!capture) { return; }
            DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
            if (boundary == RejectionBoundary::Flush) {
                check.expect(canvas.flush(true).failure(), "Repeated deferred GPU flush returned success");
            } else if (boundary == RejectionBoundary::Snapshot) {
                check.expect(canvas.snapshot().failure(), "Repeated deferred snapshot published an image");
            } else {
                check.expect(canvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888)).failure(),
                             "Repeated deferred readback published pixels");
            }
        }
        check.expect(capture->runs > 0 && capture->rejected, "Repeated case missed deferred native failure");
        canceledProbe(check, state, weak);
        target.reset();
        check.expect(beforeState->runs == 0 && !beforeWeak.expired(),
                     "Failed production completion submitted or discarded unrelated earlier work");
        context->purgeResourcesUntilMemoryTo(0);
        check.expect(context->memoryUsage() <= baseline, "Repeated rejected frames accumulated live GPU resources");
    }
    auto retry = drawable->prepareCanvas();
    if (!check.expect(retry.success(), "Failed to prepare healthy frame after repeated rejection")) { return; }
    retry.value().getNativeCanvas()->clear(tgfx::Color::Green());
    check.expect(drawable->flushWithResult().success(), "Healthy production submit failed after repeated cancellation");
    check.expect(beforeState->runs == 1, "Later successful submit lost the unrelated earlier recording");
    for (const auto& state : states) {
        check.expect(state->runs == 0 && state->destroyed == 1,
                     "Later successful submit encoded a previously rejected batch");
    }
    check.expect(callerPixels[0] == 0 && callerPixels[1] == 255 &&
                 callerPixels[2] == 0 && callerPixels[3] == 255,
                 "Healthy retry did not publish supported green pixels");
}

void repeatedFlushCancellation(Check& c, const Ref<GrGraphicsContext>& g, tgfx::Context* native) {
    repeatedRejection(c, g, native, RejectionBoundary::Flush);
}
void repeatedSnapshotCancellation(Check& c, const Ref<GrGraphicsContext>& g, tgfx::Context* native) {
    repeatedRejection(c, g, native, RejectionBoundary::Snapshot);
}
void repeatedReadbackCancellation(Check& c, const Ref<GrGraphicsContext>& g, tgfx::Context* native) {
    repeatedRejection(c, g, native, RejectionBoundary::Readback);
}
void repeatedBitmapCancellation(Check& c, const Ref<GrGraphicsContext>& g, tgfx::Context* native) {
    repeatedRejection(c, g, native, RejectionBoundary::Bitmap);
}

void existingErrorCancellation(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto target = surface(check, context);
    if (!target) { return; }
    auto state = std::make_shared<SubmissionState>();
    auto weak = queueProbe(check, target->getCanvas(), state);
    if (!check.expect(rejectCapture(target->getCanvas()), "Existing-error cancellation trigger failed")) { return; }
    DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
    check.expect(canvas.flush(true).failure(), "Production flush accepted an already failed native canvas");
    canceledProbe(check, state, weak);
    auto healthy = surface(check, context);
    if (!healthy) { return; }
    check.expect(DrawableSurfaceCanvas(healthy.get(), kSize, kSize).flush(true).success(),
                 "Later healthy submit failed after preexisting-error cancellation");
    canceledProbe(check, state, weak);
}

void destructorCancellation(Check& check, const Ref<GrGraphicsContext>& graphics, tgfx::Context* context) {
    std::vector<uint8_t> callerPixels(static_cast<size_t>(kSize * kSize * 4), 0x5a);
    const auto original = callerPixels;
    auto state = std::make_shared<SubmissionState>();
    std::weak_ptr<SubmissionProbe> weak;
    std::shared_ptr<DeferredCapture> capture;
    {
        BitmapGraphicsContext bitmapContext(graphics);
        Valdi::BitmapInfo info(kSize, kSize, Valdi::ColorTypeRGBA8888, Valdi::AlphaTypePremul, kSize * 4);
        auto drawable = bitmapContext.createBitmapSurface(info, callerPixels.data());
        if (!check.expect(drawable != nullptr, "Failed to allocate abandoned-frame drawable")) { return; }
        auto canvas = drawable->prepareCanvas();
        if (!check.expect(canvas.success(), "Failed to prepare abandoned-frame drawable")) { return; }
        weak = queueProbe(check, canvas.value().getNativeCanvas(), state);
        capture = deferCaptureFailure(check, canvas.value().getNativeCanvas());
        if (!capture) { return; }
        // Destruction must close and cancel even a not-yet-materialized failure.
    }
    check.expect(callerPixels == original, "Abandoned frame destructor published bitmap pixels");
    check.expect(capture->runs > 0 && capture->rejected, "Abandoned deferred failure was not materialized");
    canceledProbe(check, state, weak);
    auto healthy = surface(check, context);
    if (!healthy) { return; }
    check.expect(DrawableSurfaceCanvas(healthy.get(), kSize, kSize).flush(true).success(),
                 "Healthy submit failed after abandoned-frame cleanup");
    canceledProbe(check, state, weak);
}

void immutableFailureMarker(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto healthy = surface(check, context);
    if (!healthy) { return; }
    DrawableSurfaceCanvas healthyCanvas(healthy.get(), kSize, kSize);
    if (!check.expect(healthyCanvas.flush(true).success(), "Failed to establish isolated marker control")) { return; }
    std::shared_ptr<tgfx::Picture> failedPicture;
    std::shared_ptr<tgfx::Picture> nestedPicture;
    std::shared_ptr<tgfx::Picture> healthyPicture;
    {
        tgfx::PictureRecorder recorder;
        auto* canvas = recorder.beginRecording();
        canvas->markRenderFailure();
        check.expect(canvas->hasError() && canvas->getSaveCount() == 0,
                     "Public native failure marker was not immediate or changed saves");
        canvas->clear(tgfx::Color::Green());
        check.expect(canvas->hasError(), "Opaque fill reset the recording failure");
        failedPicture = recorder.finishRecordingAsPicture();
        if (!check.expect(failedPicture != nullptr, "Failed recording lost its immutable failure marker")) { return; }
        canvas = recorder.beginRecording();
        check.expect(!canvas->hasError(), "New recording inherited an earlier recording's failure");
        canvas->clear(tgfx::Color::Blue());
        healthyPicture = recorder.finishRecordingAsPicture();

        tgfx::PictureRecorder outer;
        auto* outerCanvas = outer.beginRecording();
        outerCanvas->drawPicture(failedPicture);
        check.expect(outerCanvas->hasError(), "Referenced failed picture did not fail its receiving recorder");
        outerCanvas->clear(tgfx::Color::Green());
        nestedPicture = outer.finishRecordingAsPicture();
        if (!check.expect(nestedPicture != nullptr, "Opaque discard lost a referenced child's failure")) { return; }
    }
    // Both recorders are gone. Measurement must neither execute destination work
    // nor retain/modify the status of this independent live GPU destination.
    (void)failedPicture->getBounds();
    (void)nestedPicture->getBounds();
    check.expect(!healthy->getCanvas()->hasError(), "Failed-picture measurement contaminated another destination");
    for (const auto& picture : {failedPicture, nestedPicture}) {
        auto target = surface(check, context);
        if (!target) { return; }
        picture->playback(target->getCanvas());
        check.expect(target->getCanvas()->hasError() && target->getCanvas()->getSaveCount() == 0,
                     "Immutable failure did not survive recorder destruction/replay");
        DrawableSurfaceCanvas canvas(target.get(), kSize, kSize);
        check.expect(canvas.snapshot().failure(), "Production snapshot accepted a replayed explicit failure marker");
        check.expect(canvas.flush(true).failure(), "Production completion accepted a replayed explicit failure marker");
    }
    if (!check.expect(healthyPicture != nullptr, "Healthy reused recorder produced no picture")) { return; }
    healthy->getCanvas()->drawPicture(std::move(healthyPicture));
    auto pixels = healthyCanvas.readPixels(tgfx::ImageInfo::Make(kSize, kSize, tgfx::ColorType::RGBA_8888));
    if (check.expect(pixels.success(), "Failed marker poisoned the independent healthy destination")) {
        bluePixels(check, pixels.value());
    }
}

void activeLayerFailureMarker(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    tgfx::PictureRecorder recorder;
    auto* recording = recorder.beginRecording();
    const auto recordingDepth = recording->getSaveCount();
    recording->saveLayerAlpha(0);
    recording->markRenderFailure();
    check.expect(recording->hasError(), "Explicit failure did not mark its active recording layer");
    recording->restoreToCount(recordingDepth);
    check.expect(recording->hasError() && recording->getSaveCount() == recordingDepth,
                 "Invisible layer restore lost its recording ancestor's failure or saves");
    auto picture = recorder.finishRecordingAsPicture();
    if (!check.expect(picture != nullptr, "Failure-only transparent layer was dropped as empty")) { return; }

    auto target = surface(check, context);
    if (!target) { return; }
    auto* canvas = target->getCanvas();
    const auto depth = canvas->getSaveCount();
    canvas->saveLayerAlpha(0);
    canvas->markRenderFailure();
    check.expect(canvas->hasError(), "Explicit failure did not mark its active GPU layer");
    canvas->restoreToCount(depth);
    canvas->clear(tgfx::Color::Blue());
    check.expect(canvas->hasError() && canvas->getSaveCount() == depth,
                 "GPU layer ancestor lost failure after restore/opaque clear");
    check.expect(DrawableSurfaceCanvas(target.get(), kSize, kSize).flush(true).failure(),
                 "Production completion published an explicitly failed invisible layer");

    auto replay = surface(check, context);
    if (!replay) { return; }
    replay->getCanvas()->drawPicture(std::move(picture));
    check.expect(replay->getCanvas()->hasError(), "Failure-only layer picture replay was optimized away");
    check.expect(DrawableSurfaceCanvas(replay.get(), kSize, kSize).flush(true).failure(),
                 "Production completion accepted an optimized failure-only layer");
}

struct WriterState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<bool> finished{false};
    std::atomic<bool> ownerReturned{false};
    std::atomic<bool> inlineExecution{false};
    std::atomic<bool> timedOut{false};
    std::atomic<bool> suppressWrite{false};
    std::atomic<int> runs{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> queuedDestroyed{0};
    std::thread::id caller = std::this_thread::get_id();

    template <typename Predicate>
    bool await(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }
    void open() {
        { std::lock_guard<std::mutex> lock(mutex); release = true; }
        changed.notify_all();
    }
    bool beginWrite() {
        ++runs;
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        inlineExecution = std::this_thread::get_id() == caller;
        changed.notify_all();
        // Unsupported inline execution is a failed prerequisite, never a deadlock/skip.
        if (inlineExecution) { return false; }
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) {
            timedOut = true;
            return false;
        }
        return !suppressWrite;
    }
    void finishWrite() {
        { std::lock_guard<std::mutex> lock(mutex); finished = true; }
        changed.notify_all();
    }
    void destroyedPayload(bool queued) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++destroyed;
            if (queued) { ++queuedDestroyed; }
        }
        changed.notify_all();
    }
};

struct OpenWriterOnExit {
    std::shared_ptr<WriterState> state;
    ~OpenWriterOnExit() { state->open(); }
};

// Public native pool lifecycle, used only in these serialized fixture bodies.
// Workers never acquire the GPU context or call a native graphics API.
struct SingleWorkerPool {
    size_t previous = tgfx::Task::MaxThreadCount();
    SingleWorkerPool() { tgfx::Task::ReleaseThreads(); tgfx::Task::SetMaxThreadCount(1); }
    ~SingleWorkerPool() { tgfx::Task::ReleaseThreads(); tgfx::Task::SetMaxThreadCount(previous); }
};

class GatedVertexProvider final : public tgfx::VertexProvider {
public:
    GatedVertexProvider(std::shared_ptr<tgfx::BlockAllocator> allocator,
                        std::shared_ptr<WriterState> state, bool queued)
        : VertexProvider(std::move(allocator)), _state(std::move(state)), _queued(queued) {}
    ~GatedVertexProvider() override { _state->destroyedPayload(_queued); }
    size_t vertexCount() const override { return 4; }
    void getVertices(float* vertices) const override {
        if (_state->beginWrite()) {
            for (size_t i = 0; i < vertexCount(); ++i) { vertices[i] = 42.0f; }
        }
        _state->finishWrite();
    }
private:
    std::shared_ptr<WriterState> _state;
    bool _queued;
};

class GatedCellCodec final : public tgfx::ImageCodec {
public:
    GatedCellCodec(std::shared_ptr<WriterState> state, bool queued)
        : ImageCodec(1, 1), _state(std::move(state)), _queued(queued) {}
    ~GatedCellCodec() override { _state->destroyedPayload(_queued); }
    bool readPixels(const tgfx::ImageInfo& info, void* pixels) const override {
        const bool allowed = _state->beginWrite();
        if (allowed) {
            for (int y = 0; y < info.height(); ++y) {
                std::memset(static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * info.rowBytes(),
                            0x5b, info.minRowBytes());
            }
        }
        _state->finishWrite();
        return allowed;
    }
protected:
    bool onReadPixels(tgfx::ColorType type, tgfx::AlphaType alpha, size_t rowBytes,
                      std::shared_ptr<tgfx::ColorSpace> colorSpace, void* pixels) const override {
        return readPixels(tgfx::ImageInfo::Make(1, 1, type, alpha, rowBytes, std::move(colorSpace)), pixels);
    }
private:
    std::shared_ptr<WriterState> _state;
    bool _queued;
};

enum class WriterKind { Vertex, Atlas, HardwareAtlas };

void asyncWriterCancellation(Check& check, tgfx::Context* context, WriterKind kind, bool executing) {
    SingleWorkerPool pool;
    auto blocker = std::make_shared<WriterState>();
    OpenWriterOnExit openBlocker{blocker};
    std::shared_ptr<tgfx::Task> blockerTask;
    if (!executing) {
        blockerTask = tgfx::Task::Run([blocker] { blocker->beginWrite(); blocker->finishWrite(); });
        if (!check.expect(blocker->await([&] { return blocker->entered; }) && !blocker->inlineExecution,
                          "Actual asynchronous worker prerequisite failed")) { return; }
    }
    auto state = std::make_shared<WriterState>();
    OpenWriterOnExit openWriter{state};
    std::weak_ptr<tgfx::ResourceProxy> resource;
    std::shared_ptr<tgfx::BlockAllocator> storageLease;
    tgfx::HardwareBufferRef hardware = nullptr;
    struct ReleaseHardware {
        tgfx::HardwareBufferRef& buffer;
        ~ReleaseHardware() { if (buffer) { tgfx::HardwareBufferRelease(buffer); } }
    } releaseHardware{hardware};
    if (kind == WriterKind::Vertex) {
        auto* drawing = context->drawingAllocator();
        auto* vertices = context->drawingManager()->vertexAllocator();
        // Pin real allocator storage until the observer drains all real workers.
        // A bounded missing-join mutation can fail resource lifetime checks without
        // allowing its still-running provider to write into freed storage.
        if (executing) { storageLease = vertices->addReference(); }
        auto* pixels = static_cast<float*>(vertices->allocate(sizeof(float) * 4));
        if (!check.expect(pixels != nullptr, "Failed to allocate actual vertex destination")) { return; }
        std::vector<std::shared_ptr<tgfx::Task>> tasks;
        auto first = drawing->make<GatedVertexProvider>(drawing->addReference(), state, !executing);
        auto firstTask = std::make_shared<tgfx::VertexProviderTask>(std::move(first), pixels);
        tgfx::Task::Run(firstTask);
        tasks.push_back(std::move(firstTask));
        if (executing && !check.expect(state->await([&] { return state->entered; }) && !state->inlineExecution,
                                       "Real VertexProviderTask did not enter on a worker")) {
            state->open(); tgfx::Task::ReleaseThreads(); storageLease.reset(); return;
        }
        auto second = drawing->make<GatedVertexProvider>(drawing->addReference(), state, true);
        auto secondTask = std::make_shared<tgfx::VertexProviderTask>(std::move(second), pixels);
        tgfx::Task::Run(secondTask);
        tasks.push_back(std::move(secondTask));
        auto data = tgfx::Data::MakeWithoutCopy(pixels, sizeof(float) * 4);
        auto source = std::make_unique<tgfx::AsyncVertexSource>(std::move(data), std::move(tasks));
        // The native generic upload factory exposes its real proxy and owns the
        // same GPUBufferUploadTask/AsyncVertexSource lifecycle as vertex uploads.
        // Suppress only its additional DataTask wrapper; our VertexProviderTasks
        // already run in the real pool. This batch is canceled, never GPU-encoded.
        auto proxy = context->proxyProvider()->createIndexBufferProxy(
            std::move(source), tgfx::RenderFlags::DisableAsyncTask);
        if (!check.expect(proxy != nullptr, "Failed to create actual GPU upload owner")) {
            state->open(); tgfx::Task::ReleaseThreads(); storageLease.reset(); return;
        }
        resource = proxy;
    } else {
        if (executing) { storageLease = context->drawingAllocator()->addReference(); }
        std::shared_ptr<tgfx::TextureProxy> proxy;
        if (kind == WriterKind::HardwareAtlas) {
            hardware = tgfx::HardwareBufferAllocate(64, 64, false);
            if (!check.expect(hardware != nullptr, "Real hardware atlas allocation prerequisite failed")) {
                storageLease.reset(); return;
            }
            proxy = context->proxyProvider()->createTextureProxy(hardware);
        } else {
            proxy = context->proxyProvider()->createTextureProxy({}, 64, 64, tgfx::PixelFormat::RGBA_8888);
        }
        if (!check.expect(proxy != nullptr, "Failed to create real atlas texture proxy")) {
            storageLease.reset(); return;
        }
        resource = proxy;
        context->drawingManager()->addAtlasCellTask(proxy, {4, 4},
            std::make_shared<GatedCellCodec>(state, !executing));
        if (executing && !check.expect(state->await([&] { return state->entered; }) && !state->inlineExecution,
                                       "Real AsyncCellUploadTask did not enter codec on a worker")) {
            state->open(); tgfx::Task::ReleaseThreads(); storageLease.reset();
            context->discard(context->flush()); return;
        }
        context->drawingManager()->addAtlasCellTask(std::move(proxy), {24, 24},
            std::make_shared<GatedCellCodec>(state, true));
    }
    auto recording = context->flush();
    if (!check.expect(recording != nullptr, "Actual upload batch was not recorded")) {
        state->open(); tgfx::Task::ReleaseThreads(); storageLease.reset(); return;
    }
    std::atomic<bool> cancelPhaseObserved{false};
    std::atomic<bool> retainedWhileRunning{false};
    std::thread observer;
    if (executing) {
        observer = std::thread([&, lease = std::move(storageLease)]() mutable {
            // The queued codec/provider destructor is a real cancellation-phase
            // witness. Keep the first writer gated while inspecting owner lifetime.
            cancelPhaseObserved = state->await([&] { return state->queuedDestroyed.load() > 0; });
            state->await([&] { return state->ownerReturned.load(); }, std::chrono::milliseconds(150));
            retainedWhileRunning = !resource.expired() && !state->ownerReturned && !state->finished;
            if (!retainedWhileRunning) { state->suppressWrite = true; }
            state->open();
            // Drain queued/executing native work before dropping the safety lease.
            // No worker calls Context, so the caller's device lock is not needed.
            tgfx::Task::ReleaseThreads();
            lease.reset();
        });
    }
    const bool discarded = context->discard(std::move(recording));
    { std::lock_guard<std::mutex> lock(state->mutex); state->ownerReturned = true; }
    state->changed.notify_all();
    if (observer.joinable()) { observer.join(); }
    check.expect(discarded, "Context did not cancel the actual upload batch");
    if (executing) {
        check.expect(cancelPhaseObserved && retainedWhileRunning,
                     "Discard released upload owner/storage/hardware lock before the running writer joined");
        check.expect(state->runs == 1 && state->finished && state->queuedDestroyed == 1,
                     "Executing upload did not finish once while queued upload was canceled");
    } else {
        check.expect(state->runs == 0 && state->queuedDestroyed == 2,
                     "Queued upload writer ran instead of being canceled");
        blocker->open();
        tgfx::Task::ReleaseThreads();
    }
    check.expect(state->destroyed == 2 && resource.expired(),
                 "Canceled actual upload retained codec/provider or resource ownership");
    check.expect(!state->timedOut && !state->inlineExecution, "Async writer synchronization timed out or ran inline");
    if (hardware && executing) {
        auto* pixels = static_cast<const uint8_t*>(tgfx::HardwareBufferLock(hardware));
        if (check.expect(pixels != nullptr, "Could not relock real hardware atlas after joined cleanup")) {
            auto info = tgfx::HardwareBufferGetInfo(hardware);
            const auto offset = size_t{4} * info.rowBytes + size_t{4} * 4;
            check.expect(pixels[offset] == 0x5b, "Joined atlas writer did not complete its real hardware write");
            tgfx::HardwareBufferUnlock(hardware);
        }
    }
    auto healthy = surface(check, context);
    if (healthy) {
        check.expect(DrawableSurfaceCanvas(healthy.get(), kSize, kSize).flush(true).success(),
                     "Supported submit did not recover after asynchronous upload cancellation");
    }
}

void queuedVertexCancellation(Check& c, const Ref<GrGraphicsContext>&, tgfx::Context* native) {
    asyncWriterCancellation(c, native, WriterKind::Vertex, false);
}
void runningVertexCancellation(Check& c, const Ref<GrGraphicsContext>&, tgfx::Context* native) {
    asyncWriterCancellation(c, native, WriterKind::Vertex, true);
}
void queuedAtlasCancellation(Check& c, const Ref<GrGraphicsContext>&, tgfx::Context* native) {
    asyncWriterCancellation(c, native, WriterKind::Atlas, false);
}
void runningAtlasCancellation(Check& c, const Ref<GrGraphicsContext>&, tgfx::Context* native) {
    asyncWriterCancellation(c, native, WriterKind::Atlas, true);
}
void runningHardwareAtlasCancellation(Check& c, const Ref<GrGraphicsContext>&, tgfx::Context* native) {
    asyncWriterCancellation(c, native, WriterKind::HardwareAtlas, true);
}

class GatedCancellationTask final : public tgfx::Task {
public:
    explicit GatedCancellationTask(std::shared_ptr<WriterState> state) : _state(std::move(state)) {}
protected:
    void onExecute() override { ++_state->runs; }
    void onCancel() override { _state->beginWrite(); _state->finishWrite(); }
private:
    std::shared_ptr<WriterState> _state;
};

void taskCancellationJoinsCleanup(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context*) {
    SingleWorkerPool pool;
    auto blocker = std::make_shared<WriterState>();
    OpenWriterOnExit openBlocker{blocker};
    auto blockingTask = tgfx::Task::Run([blocker] { blocker->beginWrite(); blocker->finishWrite(); });
    if (!check.expect(blocker->await([&] { return blocker->entered; }) && !blocker->inlineExecution,
                      "Actual task queue prerequisite failed")) { return; }
    auto state = std::make_shared<WriterState>();
    OpenWriterOnExit openCancel{state};
    auto task = std::make_shared<GatedCancellationTask>(state);
    tgfx::Task::Run(task);
    std::thread cancel([&] { task->cancel(); });
    const bool entered = state->await([&] { return state->entered; });
    std::atomic<bool> waiterReturned{false};
    std::thread waiter([&] { task->wait(); waiterReturned = true; state->changed.notify_all(); });
    const bool returnedEarly = state->await([&] { return waiterReturned.load(); }, std::chrono::milliseconds(150));
    check.expect(entered && !returnedEarly, "Task::wait returned before queued onCancel cleanup finished");
    state->open();
    cancel.join();
    waiter.join();
    check.expect(waiterReturned && task->status() == tgfx::TaskStatus::Canceled && state->finished,
                 "Canceled task did not publish joined cleanup completion");
    blocker->open();
    tgfx::Task::ReleaseThreads();
    check.expect(!state->timedOut, "Queued cancellation cleanup synchronization timed out");
}

struct DeferredReplay {
    int runs = 0;
    tgfx::Matrix matrix;
    std::optional<tgfx::Rect> clip;
};

std::shared_ptr<tgfx::Picture> overlappingDeferredPicture(const std::shared_ptr<DeferredReplay>& replay,
                                                         bool large = false) {
    tgfx::PictureRecorder recorder;
    auto* canvas = recorder.beginRecording();
    const auto bounds = large ? tgfx::Rect::MakeLTRB(8, 8, 64, 48) : tgfx::Rect::MakeLTRB(4, 4, 20, 16);
    canvas->drawDeferred(bounds, 0, [replay, large](tgfx::Canvas* target) {
        ++replay->runs;
        replay->matrix = target->getMatrix();
        replay->clip = target->getTotalClipBounds();
        tgfx::Paint red;
        red.setColor(tgfx::Color::Red());
        red.setAntiAlias(false);
        target->drawRect(large ? tgfx::Rect::MakeLTRB(8, 8, 48, 48) :
                                 tgfx::Rect::MakeLTRB(4, 4, 16, 16), red);
        target->drawRect(large ? tgfx::Rect::MakeLTRB(24, 8, 64, 48) :
                                 tgfx::Rect::MakeLTRB(8, 4, 20, 16), red);
    });
    return recorder.finishRecordingAsPicture();
}

void alphaPixel(Check& check, const NativeData& pixels, int width, int x, int y, int expected) {
    const auto offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    if (!check.expect(pixels != nullptr && pixels->size() >= offset + 4, "Missing deferred group readback")) {
        return;
    }
    const auto* bytes = pixels->bytes() + offset;
    check.expect(std::abs(static_cast<int>(bytes[3]) - expected) <= 1 &&
                 std::abs(static_cast<int>(bytes[0]) - expected) <= 1 &&
                 bytes[1] == 0 && bytes[2] == 0,
                 "Deferred group opacity/clip produced incorrect premultiplied red coverage");
}

void deferredGroupOpacity(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto replay = std::make_shared<DeferredReplay>();
    auto picture = overlappingDeferredPicture(replay);
    if (!check.expect(picture != nullptr && replay->runs == 0, "Deferred group ran during recording")) { return; }
    auto target = tgfx::Surface::Make(context, 64, 64, tgfx::ColorType::RGBA_8888);
    if (!check.expect(target != nullptr, "Failed to allocate group opacity target")) { return; }
    target->getCanvas()->clear(tgfx::Color::Transparent());
    tgfx::Paint half;
    half.setAlpha(0.5f);
    target->getCanvas()->drawPicture(std::move(picture), nullptr, &half);
    auto pixels = DrawableSurfaceCanvas(target.get(), 64, 64).readPixels(
        tgfx::ImageInfo::Make(64, 64, tgfx::ColorType::RGBA_8888));
    if (!check.expect(pixels.success() && replay->runs == 1, "Deferred opacity group failed or replayed twice")) {
        return;
    }
    alphaPixel(check, pixels.value(), 64, 5, 8, 128);
    alphaPixel(check, pixels.value(), 64, 10, 8, 128); // Per-primitive .5 would incorrectly yield .75.
    alphaPixel(check, pixels.value(), 64, 18, 8, 128);
    alphaPixel(check, pixels.value(), 64, 2, 8, 0);
}

void deferredRerecordFinalTransform(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto replay = std::make_shared<DeferredReplay>();
    auto original = overlappingDeferredPicture(replay);
    if (!check.expect(original != nullptr, "Failed to record deferred rerecord source")) { return; }
    std::shared_ptr<tgfx::Picture> picture;
    {
        tgfx::PictureRecorder recorder;
        auto* canvas = recorder.beginRecording();
        canvas->translate(4, 6);
        canvas->clipRect(tgfx::Rect::MakeLTRB(0, 0, 12, 20), false);
        tgfx::Paint half;
        half.setAlpha(0.5f);
        canvas->drawPicture(std::move(original), nullptr, &half);
        picture = recorder.finishRecordingAsPicture();
        if (!check.expect(picture != nullptr && replay->runs == 0,
                          "Picture opacity unroll executed deferred work during intermediate recording")) { return; }
        (void)picture->getBounds();
        tgfx::PictureRecorder outer;
        outer.beginRecording()->drawPicture(std::move(picture));
        picture = outer.finishRecordingAsPicture();
        if (!check.expect(picture != nullptr && replay->runs == 0,
                          "Nested rerecord/measurement executed deferred work before final playback")) { return; }
    }
    const tgfx::Matrix matrices[] = {
        tgfx::Matrix::MakeScale(2, 1), tgfx::Matrix::MakeScale(1, 2), tgfx::Matrix::MakeScale(2, 1),
    };
    for (const auto& matrix : matrices) {
        auto target = tgfx::Surface::Make(context, 96, 64, tgfx::ColorType::RGBA_8888);
        if (!check.expect(target != nullptr, "Failed to allocate final-transform target")) { return; }
        target->getCanvas()->clear(tgfx::Color::Transparent());
        target->getCanvas()->setMatrix(matrix);
        const auto runs = replay->runs;
        target->getCanvas()->drawPicture(picture);
        auto pixels = DrawableSurfaceCanvas(target.get(), 96, 64).readPixels(
            tgfx::ImageInfo::Make(96, 64, tgfx::ColorType::RGBA_8888));
        if (!check.expect(pixels.success() && replay->runs == runs + 1,
                          "Immutable deferred picture failed final playback/reuse")) { return; }
        check.expect(replay->matrix.getScaleX() == matrix.getScaleX() &&
                     replay->matrix.getScaleY() == matrix.getScaleY() &&
                     replay->matrix.getSkewX() == 0 && replay->matrix.getSkewY() == 0 && replay->clip.has_value(),
                     "Deferred replay received scalar/intermediate matrix instead of final nonuniform device matrix");
        const bool wide = matrix.getScaleX() == 2;
        alphaPixel(check, pixels.value(), 96, wide ? 20 : 10, wide ? 12 : 24, 128);
        alphaPixel(check, pixels.value(), 96, wide ? 30 : 15, wide ? 12 : 24, 128);
        alphaPixel(check, pixels.value(), 96, wide ? 34 : 17, wide ? 12 : 24, 0);
    }
}

void deferredFilteredFinalTransform(Check& check, const Ref<GrGraphicsContext>&, tgfx::Context* context) {
    auto replay = std::make_shared<DeferredReplay>();
    auto picture = overlappingDeferredPicture(replay, true);
    auto target = tgfx::Surface::Make(context, 160, 64, tgfx::ColorType::RGBA_8888);
    if (!check.expect(picture != nullptr && target != nullptr, "Failed to allocate filtered group fixture")) { return; }
    target->getCanvas()->clear(tgfx::Color::Transparent());
    target->getCanvas()->scale(2, 1);
    tgfx::Paint half;
    half.setAlpha(0.5f);
    half.setImageFilter(tgfx::ImageFilter::Blur(1, 1));
    target->getCanvas()->drawPicture(std::move(picture), nullptr, &half);
    auto pixels = DrawableSurfaceCanvas(target.get(), 160, 64).readPixels(
        tgfx::ImageInfo::Make(160, 64, tgfx::ColorType::RGBA_8888));
    if (!check.expect(pixels.success() && replay->runs == 1, "Filtered deferred group failed or replayed twice")) {
        return;
    }
    check.expect(replay->matrix.getScaleX() == 2 && replay->matrix.getScaleY() == 1,
                 "Outer image filter replayed device effects using scalar PictureImage sampling");
    alphaPixel(check, pixels.value(), 160, 60, 28, 128);
}

using Body = void (*)(Check&, const Ref<GrGraphicsContext>&, tgfx::Context*);
struct Case { const char* name; Body body; };
} // namespace

GPUErrorCheckReport runGPUErrorChecks() {
    const Case cases[] = {
        {"production-snapshot-supported-positive-control", healthySnapshot},
        {"production-snapshot-rejects-existing-native-error", failedSnapshot},
        {"production-snapshot-rejects-deferred-readback-error", deferredSnapshot},
        {"production-flush-rejects-deferred-native-error", deferredFlush},
        {"production-readback-does-not-publish-deferred-error-pixels", failedReadback},
        {"production-presentation-supported-positive-control", healthyPresentation},
        {"production-presentation-refuses-failed-source-keeps-destination", failedPresentation},
        {"production-bitmap-completion-publishes-pixels-preserves-padding", bitmapCompletion},
        {"production-bitmap-error-returns-without-publishing-and-recovers", existingBitmapFailure},
        {"production-bitmap-deferred-error-returns-without-publishing-and-recovers", deferredBitmapFailure},
        {"production-bitmap-failed-destructor-does-not-publish-or-abort", failedBitmapCleanup},
        {"native-discard-preserves-neighbors-and-rejects-stale-generation", nativeCancellation},
        {"production-repeated-deferred-flush-cancels-before-later-submit", repeatedFlushCancellation},
        {"production-repeated-deferred-snapshot-cancels-before-later-submit", repeatedSnapshotCancellation},
        {"production-repeated-deferred-readback-cancels-before-later-submit", repeatedReadbackCancellation},
        {"production-repeated-deferred-bitmap-cancels-before-later-submit", repeatedBitmapCancellation},
        {"production-existing-error-cancels-unflushed-work", existingErrorCancellation},
        {"production-abandoned-destructor-cancels-before-later-submit", destructorCancellation},
        {"native-explicit-failure-marker-immutable-recording-replay-isolation", immutableFailureMarker},
        {"native-explicit-failure-marker-survives-invisible-active-layers", activeLayerFailureMarker},
        {"native-discard-cancels-queued-vertex-writers", queuedVertexCancellation},
        {"native-discard-joins-executing-vertex-writer-retains-storage", runningVertexCancellation},
        {"native-discard-cancels-queued-atlas-writers", queuedAtlasCancellation},
        {"native-discard-joins-executing-atlas-writer-retains-storage", runningAtlasCancellation},
        {"native-discard-joins-hardware-atlas-writer-before-unlock", runningHardwareAtlasCancellation},
        {"native-task-wait-joins-queued-cancellation-cleanup", taskCancellationJoinsCleanup},
        {"native-deferred-overlap-opacity-applies-once-per-group", deferredGroupOpacity},
        {"native-deferred-rerecord-preserves-final-matrix-clip-and-reuse", deferredRerecordFinalTransform},
        {"native-deferred-filtered-group-preserves-final-device-matrix", deferredFilteredFinalTransform},
    };
    GPUErrorCheckReport report;
    auto graphics = GrGraphicsContext::makeOffscreen();
    auto lock = graphics != nullptr ? graphics->lockContext() : nullptr;
    const bool ready = lock != nullptr && static_cast<bool>(*lock);
    for (const auto& test : cases) {
        Check check;
        if (!ready) { check.expect(false, "Real offscreen GPU context prerequisite failed"); }
        else { test.body(check, graphics, lock->get()); }
        report.checks.push_back({test.name, check.passed, check.details});
        if (check.passed) { ++report.passed; } else { ++report.failed; }
        report.details += std::string(check.passed ? "PASS " : "FAIL ") + test.name;
        if (!check.details.empty()) { report.details += ": " + check.details; }
        report.details += "\n";
    }
    report.success = ready && report.failed == 0;
    report.details += "TOTAL " + std::to_string(report.passed + report.failed) + ", PASS " +
                      std::to_string(report.passed) + ", FAIL " + std::to_string(report.failed) + "\n";
    return report;
}
