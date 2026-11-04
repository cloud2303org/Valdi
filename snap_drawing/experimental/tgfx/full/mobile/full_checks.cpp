#include "full_checks.hpp"

#include "../tests/effects_checks.hpp"
#include "../tests/gpu_error_checks.hpp"
#include "../tests/media_checks.hpp"
#include "../tests/text_checks.hpp"

#include <exception>

#if !defined(SNAP_DRAWING_TGFX_EXPERIMENT) || !defined(SNAP_DRAWING_TGFX_COMPLETE)
#error "Mobile full checks require the real complete TGFX production backend"
#endif

namespace tgfx_full_mobile {

void recordRunnerFailure(FullChecksReport& output, const std::string& message) {
    output.success = false;
    ++output.runnerFailures;
    output.status = "FAIL " + std::to_string(output.passed) + "/" + std::to_string(output.registered);
    output.details += "FAIL runner: " + message + "\n";
}

namespace {

void finish(FullChecksReport& report) {
    report.success = report.runnerFailures == 0 && report.failed == 0 &&
                     report.registered > 0 && report.passed == report.registered;
    report.status = std::string(report.success ? "PASS " : "FAIL ") +
                    std::to_string(report.passed) + "/" + std::to_string(report.registered);
    report.details = report.status + "\nActual native cases: passed=" + std::to_string(report.passed) +
                     ", failed=" + std::to_string(report.failed) +
                     ", registered=" + std::to_string(report.registered) +
                     ", runner failures=" + std::to_string(report.runnerFailures) + "\n" +
                     report.details;
}

} // namespace

FullChecksReport startupFailure(const std::string& details) {
    FullChecksReport report;
    recordRunnerFailure(report, details);
    finish(report);
    return report;
}

FullChecksReport runFullChecks(const std::string& fixturesDirectory, const std::string& textDirectory,
                              AdditionalChecks additionalChecks) {
    FullChecksReport report;
    report.details = "Real full SnapDrawing/TGFX backend checks\nmedia-data=" + fixturesDirectory +
                     "\ntext-data=" + textDirectory + "\n";
#if defined(TGFX_FULL_MOBILE_FIXTURE_HASH)
    report.details += "Parent-attested fixture hash: " TGFX_FULL_MOBILE_FIXTURE_HASH "\n";
#else
    report.details += "Fixture hash not supplied; host/mobile fixture parity is unverified\n";
#endif
    appendFullChecksSuite(report, "effects", [] { return ::runEffectsChecks(); });
    appendFullChecksSuite(report, "media", [&] { return ::runMediaChecks(fixturesDirectory); });
    appendFullChecksSuite(report, "text", [&] { return ::runTextChecks(textDirectory); });
    appendFullChecksSuite(report, "gpu-errors", [] { return ::runGPUErrorChecks(); });
    if (additionalChecks != nullptr) {
        try {
            additionalChecks(report, fixturesDirectory, textDirectory);
        } catch (const std::exception& error) {
            recordRunnerFailure(report, std::string("Additional suite adapter threw: ") + error.what());
        } catch (...) {
            recordRunnerFailure(report, "Additional suite adapter threw an unknown exception");
        }
    }
    finish(report);
    return report;
}

} // namespace tgfx_full_mobile
