#pragma once

#include <string>
#include <vector>

struct GPUErrorCheckResult {
    std::string name;
    bool passed = false;
    std::string details;
};

struct GPUErrorCheckReport {
    int passed = 0;
    int failed = 0;
    bool success = false;
    std::string details;
    std::vector<GPUErrorCheckResult> checks;
};

// Source fixture for the real complete backend, not a mock renderer. The parent
// runner owns startup/wiring/execution. Missing GPU support is FAIL, never SKIP.
// Exercises production DrawableSurfaceCanvas completion, including a native
// capture failure first raised by lazy PictureImage materialization at readback.
// Negative bitmap cases use production flushWithResult(), verify unchanged caller
// bytes and recoverable reuse, and exercise cleanup of a failed unfinished frame.
// Includes actual queued/executing vertex and atlas workers (including hardware
// atlas storage), cancellation cleanup joining, and deferred group alpha/transform
// replay. Serialized worker fixtures drain/configure/restore the real Task pool.
// Native internal headers require the TGFX src and concurrentqueue include trees.
GPUErrorCheckReport runGPUErrorChecks();
