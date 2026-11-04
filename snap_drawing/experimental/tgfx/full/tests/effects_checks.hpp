#pragma once

#include <string>
#include <vector>

struct EffectsCheckResult {
    std::string name;
    bool passed = false;
    std::string details;
};

struct EffectsCheckReport {
    int passed = 0;
    int failed = 0;
    bool success = false;
    std::string details;
    std::vector<EffectsCheckResult> checks;
};

// Requires the real full backend, SNAP_DRAWING_TGFX_EXPERIMENT and
// SNAP_DRAWING_TGFX_COMPLETE. Creates and locks an offscreen GrGraphicsContext
// on the calling thread. No global device, application, or existing fixture is
// needed. GPU use and surface destruction hold the device lock. The serialized
// thread-transfer regression keeps destinations alive across unlock/relock,
// releasing the harness lock first because Metal wrappers may share a device.
//
// A missing device/lock, surface/readback failure, unsupported contract, or
// failed pixel assertion is a FAIL. There is no skip/xfail-to-PASS conversion.
// The caller should print details and return nonzero unless success is true.
EffectsCheckReport runEffectsChecks();
