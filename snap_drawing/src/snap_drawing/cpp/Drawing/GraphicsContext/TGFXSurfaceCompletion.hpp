#pragma once

#include "snap_drawing/cpp/Drawing/Surface/DrawableSurfaceCanvas.hpp"
#include "tgfx/core/Paint.h"
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include <string>

namespace snap::drawing {

// Compatibility-only void callers cannot consume a result. Keep the failure
// visible in diagnostics; production draw/raster callers use flushWithResult().
inline void logTGFXCompletionError(const Valdi::Result<Valdi::Void>& status, const char* operation) {
    if (!status) {
        auto& logger = Valdi::ConsoleLogger::getErrorLogger();
        if (logger.isEnabledForType(Valdi::LogTypeError)) {
            logger.log(Valdi::LogTypeError, std::string(operation) + ": " + status.error().toString());
        }
    }
}

inline void discardTGFXSurface(NativeSurface* surface) {
    if (surface) {
        DrawableSurfaceCanvas(surface, surface->width(), surface->height()).discard();
    }
}

// Complete effect/deferred work on an offscreen source before touching the native
// window. Both surfaces must belong to the same locked device. The final window
// batch contains only a validated texture copy, never the user's deferred effects.
inline Valdi::Result<Valdi::Void> presentTGFXSurface(NativeSurface* source, NativeSurface* destination) {
    if (!source || !destination || source == destination || source->getContext() != destination->getContext() ||
        source->width() != destination->width() || source->height() != destination->height()) {
        return Valdi::Error("TGFX presentation requires separate source and destination on the same device");
    }
    DrawableSurfaceCanvas sourceCanvas(source, source->width(), source->height());
    DrawableSurfaceCanvas destinationCanvas(destination, destination->width(), destination->height());
    auto failed = [&](Valdi::Error error) -> Valdi::Result<Valdi::Void> {
        // The current batch may contain source snapshot or destination copy work.
        // Cancel only that new batch, preserving unrelated flushed recordings.
        sourceCanvas.discard();
        return error;
    };
    auto status = sourceCanvas.validate();
    if (!status) { return failed(status.moveError()); }
    status = destinationCanvas.validate();
    if (!status) { return failed(status.moveError()); }
    status = sourceCanvas.flush();
    if (!status) { return failed(status.moveError()); }
    auto image = sourceCanvas.makeImageSnapshot();
    if (!image) { return failed(image.moveError()); }
    // Snapshot copying may itself queue/close GPU work. Validate that boundary
    // before registering any drawing/presentation on the native window.
    status = sourceCanvas.flush();
    if (!status) { return failed(status.moveError()); }
    status = destinationCanvas.validate();
    if (!status) { return failed(status.moveError()); }
    tgfx::Paint paint;
    paint.setBlendMode(tgfx::BlendMode::Src);
    destination->getCanvas()->drawImage(image.moveValue(), &paint);
    status = sourceCanvas.validate();
    if (!status) { return failed(status.moveError()); }
    status = destinationCanvas.flush();
    if (!status) { return failed(status.moveError()); }
    return sourceCanvas.validate();
}

} // namespace snap::drawing
