#include "tests/effects_checks.hpp"
#include "tests/gpu_error_checks.hpp"
#include "tests/media_checks.hpp"
#include "tests/text_checks.hpp"

#include <exception>
#include <iostream>

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "Full backend checks require --define=snap_drawing_backend=tgfx"
#endif

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: backend_checks PATH_TO_MEDIA_DATA PATH_TO_TEXT_DATA\n";
        return 2;
    }
    try {
        const auto effects = runEffectsChecks();
        for (const auto& check : effects.checks) {
            std::cout << (check.passed ? "PASS " : "FAIL ") << check.name << ": " << check.details << '\n';
        }
        std::cout << effects.details << '\n';
        const auto media = runMediaChecks(argv[1]);
        for (const auto& check : media.checks) {
            std::cout << (check.passed ? "PASS " : "FAIL ") << check.name << ": " << check.details << '\n';
        }
        std::cout << media.details << '\n';
        const auto text = runTextChecks(argv[2]);
        for (const auto& check : text.checks) {
            std::cout << (check.passed ? "PASS " : "FAIL ") << check.name << ": " << check.details << '\n';
        }
        std::cout << text.details << '\n';
        const auto gpu = runGPUErrorChecks();
        for (const auto& check : gpu.checks) {
            std::cout << (check.passed ? "PASS " : "FAIL ") << check.name << ": " << check.details << '\n';
        }
        std::cout << gpu.details << '\n';
        const bool passed = effects.success && media.success && text.success && gpu.success &&
                            effects.failed == 0 && media.failed == 0 && text.failed == 0 &&
                            gpu.failed == 0 && effects.passed > 0 && media.passed > 0 &&
                            text.passed > 0 && gpu.passed > 0;
        std::cout << "Full backend checks: " << effects.passed + media.passed + text.passed + gpu.passed
                  << " passed, " << effects.failed + media.failed + text.failed + gpu.failed << " failed\n";
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native runner exception: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "FAIL unknown native runner exception\n";
    }
    return 1;
}
