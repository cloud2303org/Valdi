#pragma once

#include <string>

namespace snap::drawing::performance {

// Run on one dedicated worker thread, with no concurrent runs in this process.
// fixtureDirectory is full/tests/media-data (or an unchanged packaged copy).
// Returns JSON. success=false includes any setup, render or canary failure.
// This is synchronous offscreen GPU throughput, not presented/on-screen FPS.
std::string runRenderPerformance(const std::string& fixtureDirectory);

// Host startup/persistence failures use the same machine-readable failure shape.
std::string renderPerformanceFailure(const std::string& message);

} // namespace snap::drawing::performance
