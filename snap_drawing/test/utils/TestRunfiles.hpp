#pragma once

#include <string>

namespace snap::drawing {

// Resolve a single declared resource, including manifest-only runfiles. Returns
// an empty string and a diagnostic on failure. Also supports a checkout-root cwd.
std::string resolveTestDataFile(const std::string& filename, std::string& error);

} // namespace snap::drawing
