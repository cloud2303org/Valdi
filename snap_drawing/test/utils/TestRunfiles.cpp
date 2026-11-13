#include "TestRunfiles.hpp"

#include "rules_cc/cc/runfiles/runfiles.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>

namespace snap::drawing {

namespace {

using rules_cc::cc::runfiles::Runfiles;

const char* environment(const char* name) {
    auto* value = std::getenv(name);
    return value == nullptr ? "" : value;
}

bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

} // namespace

std::string resolveTestDataFile(const std::string& filename, std::string& error) {
    error.clear();
    const std::filesystem::path relativePath(filename);
    if (filename.empty() || relativePath.is_absolute()) {
        error = "Invalid SnapDrawing test resource name: '" + filename + "'";
        return {};
    }
    for (const auto& component : relativePath) {
        if (component == ".." || component == ".") {
            error = "Invalid SnapDrawing test resource name: '" + filename + "'";
            return {};
        }
    }

    // "valdi" is this module's apparent repository name, not a canonical
    // directory such as valdi+ or +local_repos+valdi. Let Bazel map it relative
    // to the repository that compiled this helper.
    const auto logicalPath = "valdi/snap_drawing/testdata/" + filename;
    std::string runfilesError;
    std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &runfilesError));
    if (runfiles == nullptr) {
        // Direct invocations can supply RUNFILES_DIR rather than TEST_SRCDIR.
        runfiles.reset(Runfiles::Create("", BAZEL_CURRENT_REPOSITORY, &runfilesError));
    }

    std::string runfilesPath;
    if (runfiles != nullptr) {
        runfilesPath = runfiles->Rlocation(logicalPath);
        if (!runfilesPath.empty() && isFile(runfilesPath)) {
            return runfilesPath;
        }
    }

    // Preserve standalone execution from the checkout root without probing
    // parent directories or assuming an external repository's directory name.
    std::error_code cwdError;
    auto cwd = std::filesystem::current_path(cwdError);
    auto checkoutPath = cwd / "snap_drawing/testdata" / relativePath;
    if (!cwdError && isFile(checkoutPath)) {
        return checkoutPath.string();
    }

    std::ostringstream diagnostic;
    diagnostic << "Unable to resolve SnapDrawing test resource '" << filename << "'. "
               << "Runfiles key: '" << logicalPath << "' (source repository: '" << BAZEL_CURRENT_REPOSITORY << "'). "
               << "Runfiles path: '" << runfilesPath << "'. "
               << "Checkout path: '" << (cwdError ? cwdError.message() : checkoutPath.string()) << "'. "
               << "TEST_SRCDIR='" << environment("TEST_SRCDIR") << "', "
               << "RUNFILES_DIR='" << environment("RUNFILES_DIR") << "', "
               << "RUNFILES_MANIFEST_FILE='" << environment("RUNFILES_MANIFEST_FILE") << "'.";
    if (runfiles == nullptr) {
        diagnostic << " Runfiles initialization: " << runfilesError;
    }
    error = diagnostic.str();
    return {};
}

} // namespace snap::drawing
