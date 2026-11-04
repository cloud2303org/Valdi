#pragma once
#include <android/native_window_jni.h>
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurface.hpp"
#include <mutex>
#include <optional>
#include <vector>

namespace snap::drawing {
struct DisplayParams {
    int mSAASampleCount = 1;
    bool disableVsync = false;
};
class ANativeWindowGraphicsContext : public GraphicsContext {
public:
    ANativeWindowGraphicsContext(const DisplayParams& displayParams, const Ref<GrGraphicsContextOptions>& options);
    ~ANativeWindowGraphicsContext() override;
    void setResourceCacheLimit(size_t bytes) override;
    void performCleanup(bool purgeScratch, std::chrono::seconds age) override;
    void commit() override;
    Ref<DrawableSurface> createSurfaceForNativeWindow(ANativeWindow* window);
    int64_t getMaxRenderTargetSize();
    static Ref<ANativeWindowGraphicsContext> create(const DisplayParams& displayParams,
                                                   const Ref<GrGraphicsContextOptions>& options);
private:
    friend class ANativeWindowDrawableSurface;
    std::mutex _mutex;
    DisplayParams _displayParams;
    Ref<GrGraphicsContextOptions> _options;
    std::optional<size_t> _cacheLimit;
    std::vector<std::weak_ptr<tgfx::Device>> _devices;
    Ref<GrGraphicsContext> _offscreen;
    std::vector<Ref<GrGraphicsContext>> liveContexts();
    void applyCacheLimit(const Ref<GrGraphicsContext>& context);
};
} // namespace snap::drawing
