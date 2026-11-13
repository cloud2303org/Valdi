#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/PathUtils.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

namespace snap::drawing {

// Required test resources: lookup/read failures print a diagnostic and exit
// with EXIT_FAILURE, including in optimized builds where SC_ASSERT is disabled.
Valdi::Path resolveTestPath(const std::string& path);
Valdi::Result<Valdi::BytesView> getTestData(const std::string& filename);

} // namespace snap::drawing
