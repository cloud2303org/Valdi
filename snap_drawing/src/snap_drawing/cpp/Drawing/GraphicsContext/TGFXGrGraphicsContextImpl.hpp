#pragma once

#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "tgfx/gpu/GPU.h"
#include "valdi_core/cpp/Utils/Shared.hpp"
#include "valdi_core/cpp/Utils/FlatMap.hpp"
#include <cstdlib>
#if !defined(__APPLE__)
#include "tgfx/gpu/opengl/GLDevice.h"
#endif

namespace snap::drawing {

class TGFXDeviceState {
public:
    explicit TGFXDeviceState(std::shared_ptr<tgfx::Device> device) : device(std::move(device)) {}
    std::shared_ptr<tgfx::Device> device;
    std::recursive_mutex mutex;
    tgfx::Context* context = nullptr;
    size_t depth = 0;
    std::thread::id owner;
};

// The active state also tracks native EGL context switches when devices nest.
static thread_local TGFXDeviceState* activeTGFXState = nullptr;

// MetalDevice::MakeFrom and GLDevice::Current can return a previously registered
// device. Share our recursive lock state too, rather than deadlocking by locking
// that same non-recursive TGFX device twice on the rendering thread.
static std::shared_ptr<TGFXDeviceState> stateForDevice(std::shared_ptr<tgfx::Device> device) {
    static std::mutex mutex;
    static Valdi::FlatMap<tgfx::Device*, std::weak_ptr<TGFXDeviceState>> states;
    std::lock_guard<std::mutex> guard(mutex);
    for (auto it = states.begin(); it != states.end();) {
        if (it->second.expired()) { states.erase(it++); } else { ++it; }
    }
    auto& entry = states[device.get()];
    auto state = entry.lock();
    if (!state) {
        state = std::make_shared<TGFXDeviceState>(std::move(device));
        entry = state;
    }
    return state;
}

TGFXContextLock::TGFXContextLock(std::shared_ptr<TGFXDeviceState> state)
    : _state(std::move(state)), _lock(_state->mutex), _thread(std::this_thread::get_id()) {
    if (_state->depth != 0) {
        if (activeTGFXState != _state.get()) { return; }
        _context = _state->context;
        ++_state->depth;
    } else if (_state->device) {
        _context = _state->device->lockContext();
        if (_context) {
            _state->context = _context;
            _state->depth = 1;
            _state->owner = _thread;
        }
    }
    if (_context) {
        _previousState = activeTGFXState;
        activeTGFXState = _state.get();
    }
}

TGFXContextLock::~TGFXContextLock() {
    if (_thread != std::this_thread::get_id()) { std::abort(); }
    if (_context) {
        if (activeTGFXState != _state.get()) { std::abort(); }
        if (--_state->depth == 0) {
            _state->context = nullptr;
            _state->owner = {};
            _state->device->unlock();
        }
        activeTGFXState = _previousState;
    }
}

tgfx::Context* TGFXContextLock::get() const {
    if (_thread != std::this_thread::get_id() || (_context && activeTGFXState != _state.get())) { std::abort(); }
    return _context;
}

GrGraphicsContextOptions::GrGraphicsContextOptions() = default;
GrGraphicsContextOptions::GrGraphicsContextOptions(const Ref<IShaderCache>& cache) : _cache(cache) {}
GrGraphicsContextOptions::~GrGraphicsContextOptions() = default;
GrGraphicsContext::GrGraphicsContext(std::shared_ptr<tgfx::Device> device,
                                     const Ref<GrGraphicsContextOptions>& options)
    : _state(stateForDevice(std::move(device))), _options(options) {}
GrGraphicsContext::~GrGraphicsContext() = default;

std::unique_ptr<TGFXContextLock> GrGraphicsContext::lockContext() const {
    return std::make_unique<TGFXContextLock>(_state);
}

tgfx::Context& GrGraphicsContext::getGrContext() const {
    std::lock_guard<std::recursive_mutex> guard(_state->mutex);
    if (!_state->context || !_state->depth || _state->owner != std::this_thread::get_id() ||
        activeTGFXState != _state.get()) {
        std::abort();
    }
    return *_state->context;
}

const std::shared_ptr<tgfx::Device>& GrGraphicsContext::getDevice() const { return _state->device; }

void GrGraphicsContext::setResourceCacheLimit(size_t bytes) {
    auto lock = lockContext();
    if (*lock) { lock->get()->setCacheLimit(bytes); }
}
void GrGraphicsContext::performCleanup(bool purgeScratch, std::chrono::seconds age) {
    auto lock = lockContext();
    if (!*lock) { return; }
    // TGFX exposes purgeable resources, but no scratch-only classification.
    if (purgeScratch) { lock->get()->purgeResourcesUntilMemoryTo(0); }
    lock->get()->purgeResourcesNotUsedSince(std::chrono::steady_clock::now() - age);
}
void GrGraphicsContext::commit() {
    // Each drawable completes its checked batch in flush(). There is no pending
    // host command buffer to commit. Do not submit through a context-only path:
    // it cannot observe the destination canvas's sticky native rendering error.
}
int64_t GrGraphicsContext::getMaxRenderTargetSize() const {
    auto lock = lockContext();
    return *lock ? lock->get()->gpu()->limits()->maxTextureDimension2D : -1;
}
Ref<GrGraphicsContext> GrGraphicsContext::makeOffscreen(const Ref<GrGraphicsContextOptions>& options) {
#if defined(__APPLE__)
    auto device = makeTGFXOffscreenMetalDevice();
#else
    auto device = tgfx::GLDevice::Make();
#endif
    if (!device) { return nullptr; }
    return Valdi::makeShared<GrGraphicsContext>(std::move(device), options);
}

} // namespace snap::drawing
