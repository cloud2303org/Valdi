#pragma once
#include <string>

struct VectorTestReport {
    int passed = 0;
    bool success = false;
    std::string details;
};

VectorTestReport runVectorChecks();
