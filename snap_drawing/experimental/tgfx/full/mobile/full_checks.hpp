#pragma once

#include <cstddef>
#include <exception>
#include <string>

namespace tgfx_full_mobile {

// Counts are actual native case results. Runner failures (missing wiring,
// exceptions or inconsistent reports) are separate and also prevent success.
struct FullChecksReport {
    bool success = false;
    int passed = 0;
    int failed = 0;
    int registered = 0;
    int runnerFailures = 0;
    std::string status;
    std::string details;
};

void recordRunnerFailure(FullChecksReport& report, const std::string& details);

// Reusable for a fourth real suite once its owner publishes the signature.
// The callable must return the actual report, with the same report/check field
// contract as effects/media/text; this helper supplies no algorithms or cases.
template<typename Run>
void appendFullChecksSuite(FullChecksReport& output, const char* name, Run&& run) {
    output.details += std::string("\n=== ") + name + " ===\n";
    try {
        const auto report = run();
        output.passed += report.passed;
        output.failed += report.failed;
        output.registered += static_cast<int>(report.checks.size());
        output.details += "Native report: success=" + std::string(report.success ? "true" : "false") +
                          ", passed=" + std::to_string(report.passed) +
                          ", failed=" + std::to_string(report.failed) +
                          ", registered=" + std::to_string(report.checks.size()) + "\n";
        bool everyCheckPassed = true;
        for (const auto& check : report.checks) {
            everyCheckPassed = everyCheckPassed && check.passed;
            output.details += std::string(check.passed ? "PASS " : "FAIL ") + name + "/" + check.name +
                              ": " + check.details + "\n";
        }
        output.details += report.details;
        output.details += "\n";
        if (!report.success || report.failed != 0 || report.passed <= 0 || report.checks.empty() ||
            static_cast<std::size_t>(report.passed) != report.checks.size() || !everyCheckPassed) {
            recordRunnerFailure(output, std::string(name) +
                                           ": success/count/individual-result gate failed; "
                                           "a nonempty suite must pass every registered case");
        }
    } catch (const std::exception& error) {
        recordRunnerFailure(output, std::string(name) + " threw: " + error.what());
    } catch (...) {
        recordRunnerFailure(output, std::string(name) + " threw an unknown exception");
    }
}

// Optional extension adapter for a real additional suite. The adapter uses
// appendFullChecksSuite() above. Existing three suites always run first.
using AdditionalChecks = void (*)(FullChecksReport&, const std::string& mediaDirectory,
                                 const std::string& textDirectory);

// Link this wrapper and the real full/tests checks through the parent's Bazel
// target. The app shell must not compile a second backend or substitute checks.
FullChecksReport runFullChecks(const std::string& fixturesDirectory, const std::string& textDirectory,
                              AdditionalChecks additionalChecks = nullptr);
FullChecksReport startupFailure(const std::string& details);

} // namespace tgfx_full_mobile
