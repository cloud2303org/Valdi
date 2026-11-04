#pragma once

#include <string>
#include <vector>

struct MediaCheckResult {
    std::string name;
    bool passed = false;
    std::string details;
};

struct MediaTestReport {
    int passed = 0;
    int failed = 0;
    bool success = false;
    std::string details;
    std::vector<MediaCheckResult> checks;
};

// fixtureDirectory is the directory containing media-data/PROVENANCE.md,
// pattern.png, disposal.gif, etc., not the repository root. Every registered
// case produces PASS or FAIL. Missing fixtures/GPU/font support are failures.
// Requires the real full SnapDrawing sources and EXPERIMENT + COMPLETE defines.
// No main(): the parent's native runner owns startup, linkage and execution.
MediaTestReport runMediaChecks(const std::string& fixtureDirectory);
