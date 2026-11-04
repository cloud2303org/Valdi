#pragma once

#include <string>
#include <vector>

struct TextCheckResult {
    std::string name;
    bool passed = false;
    std::string details;
};

struct TextCheckReport {
    int passed = 0;
    int failed = 0;
    bool success = false;
    std::string details;
    std::vector<TextCheckResult> checks;
};

// Pass the directory containing text-data/RobotoFlex.ttf (the text-data
// directory itself). Requires the real complete SnapDrawing TGFX backend and
// EXPERIMENT + COMPLETE defines. No main, mocks, platform launcher or skips.
// Apple uses actual CoreText variation instances; FreeType platforms require
// the recorded TGFX variable-font extension in the linked native library.
// Every case reports PASS or FAIL. Missing fixture, native font coverage,
// variation support, device/lock or GPU readback is a failure of that case.
// GPU resources are released before unlocking their context on this thread.
// The parent owns linkage, fixture deployment, execution and exit status:
// print details and fail the runner unless success is true.
// Registers exactly 26 cases: the original 20 text/font cases plus six real
// TextLayer shadow/device-radius/clip/color-emoji regressions.
TextCheckReport runTextChecks(const std::string& fixtureDirectory);
