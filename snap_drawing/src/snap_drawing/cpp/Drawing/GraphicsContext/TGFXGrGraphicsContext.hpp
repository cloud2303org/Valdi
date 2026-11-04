#pragma once

#include "snap_drawing/cpp/Drawing/GraphicsContext/GraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/GraphicsContext/IShaderCache.hpp"
#include "tgfx/gpu/Context.h"
#include "tgfx/gpu/Device.h"
#include <memory>
#include <mutex>
#include <thread>

namespace snap::drawing {

// TGFX has no public persistent shader/program cache callbacks at the pinned revision.
// Keep the existing host cache alive and expose the unsupported capability explicitly.
class GrGraphicsContextOptions : public Valdi::SimpleRefCountable {
public:
    GrGraphicsContextOptions();
    explicit GrGraphicsContextOptions(const Ref<IShaderCache>& cache);
    ~GrGraphicsContextOptions() override;
    // Host storage only: the pinned TGFX backend never invokes its load/store methods.
    const Ref<IShaderCache>& getShaderCache() const { return _cache; }
    static constexpr bool supportsPersistentShaderCache() { return false; }
private:
    Ref<IShaderCache> _cache;
};

class TGFXDeviceState;

// A real device lock. Recursive acquisitions on this thread share the same TGFX lock.
// This is useful when composition creates an offscreen surface inside a display frame.
// Nested devices must be used and released in stack order. Destruction, like
// TGFX Device::unlock(), must occur on the acquiring thread.
class TGFXContextLock {
public:
    explicit TGFXContextLock(std::shared_ptr<TGFXDeviceState> state);
    ~TGFXContextLock();
    TGFXContextLock(const TGFXContextLock&) = delete;
    TGFXContextLock& operator=(const TGFXContextLock&) = delete;
    tgfx::Context* get() const;
    explicit operator bool() const { return get() != nullptr; }
private:
    std::shared_ptr<TGFXDeviceState> _state;
    std::unique_lock<std::recursive_mutex> _lock;
    tgfx::Context* _context = nullptr;
    TGFXDeviceState* _previousState = nullptr;
    std::thread::id _thread;
};

class GrGraphicsContext : public GraphicsContext {
public:
    GrGraphicsContext(std::shared_ptr<tgfx::Device> device, const Ref<GrGraphicsContextOptions>& options);
    ~GrGraphicsContext() override;
    void setResourceCacheLimit(size_t resourceCacheLimitBytes) override;
    void performCleanup(bool shouldPurgeScratchResources, std::chrono::seconds secondsNotUsed) override;
    void commit() override;

    std::unique_ptr<TGFXContextLock> lockContext() const;
    // Legacy method name, real TGFX return type. Only valid inside lockContext() or
    // between this context's drawable.prepareCanvas() and drawable.flush().
    tgfx::Context& getGrContext() const;
    const std::shared_ptr<tgfx::Device>& getDevice() const;
    int64_t getMaxRenderTargetSize() const;
    static Ref<GrGraphicsContext> makeOffscreen(const Ref<GrGraphicsContextOptions>& options = nullptr);
private:
    std::shared_ptr<TGFXDeviceState> _state;
    Ref<GrGraphicsContextOptions> _options;
};

#if defined(__APPLE__)
// Implemented in the existing MetalGraphicsContext.mm translation unit.
std::shared_ptr<tgfx::Device> makeTGFXOffscreenMetalDevice();
#endif

} // namespace snap::drawing
