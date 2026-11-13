#include "TestDataUtils.hpp"
#include "TestRunfiles.hpp"
#include "valdi_core/cpp/Utils/DiskUtils.hpp"
#include <cstdio>
#include <cstdlib>

using namespace Valdi;

namespace snap::drawing {

Path resolveTestPath(const std::string& path) {
    std::string error;
    auto resolvedPath = resolveTestDataFile(path, error);
    if (resolvedPath.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        std::exit(EXIT_FAILURE);
    }
    return Path(resolvedPath);
}

Valdi::Result<Valdi::BytesView> getTestData(const std::string& filename) {
    auto path = resolveTestPath(filename);
    auto data = DiskUtils::load(path);
    if (!data) {
        // Do not rely on SC_ASSERT, which can be disabled. Callers must not
        // dereference an unsuccessful Result and crash without a diagnostic.
        std::fprintf(stderr,
                     "Unable to load SnapDrawing test resource '%s' at '%s': %s\n",
                     filename.c_str(),
                     path.toString().c_str(),
                     data.description());
        std::exit(EXIT_FAILURE);
    }
    return data;
}

} // namespace snap::drawing
