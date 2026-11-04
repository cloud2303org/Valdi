#include "vector_checks.hpp"
#include <iostream>

int main() {
    @autoreleasepool {
        auto report = runVectorChecks();
        std::cout << report.details;
        return report.success ? 0 : 1;
    }
}
