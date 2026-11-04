#pragma once
#include "snap_drawing/cpp/Drawing/GraphicsContext/GrGraphicsContext.hpp"
#include "snap_drawing/cpp/Drawing/Surface/DrawableSurface.hpp"
#include "valdi_core/cpp/Interfaces/IBitmap.hpp"
#include "tgfx/gpu/Backend.h"
#include "tgfx/gpu/Texture.h"

namespace snap::drawing {
class GLGraphicsContext;
class GLDrawableSurface : public DrawableSurface {
public:
    GLDrawableSurface(const Ref<GLGraphicsContext>& context, const tgfx::BackendTexture& texture,
                      int sampleCount, Valdi::ColorType colorType, bool isOwned,
                      std::shared_ptr<tgfx::Texture> owner = nullptr);
    ~GLDrawableSurface() override;
    GraphicsContext* getGraphicsContext() override;
    Valdi::Result<DrawableSurfaceCanvas> prepareCanvas() override;
    void flush() override;
    Valdi::Result<Valdi::Void> flushWithResult() override;
    unsigned getTextureId() const;
    int getTextureWidth() const;
    int getTextureHeight() const;
    // The caller must hold this surface's device lock.
    static void flushSurface(tgfx::Surface* surface);
private:
    Ref<GLGraphicsContext> _context;
    tgfx::BackendTexture _backendTexture;
    std::shared_ptr<tgfx::Texture> _texture;
    std::shared_ptr<tgfx::Surface> _surface;
    std::unique_ptr<TGFXContextLock> _frame;
    int _sampleCount;
    bool _owned;
    Valdi::Result<Valdi::Void> _completionResult = Valdi::Void();
};
class GLGraphicsContext : public GrGraphicsContext {
public:
    // Supplied devices must be dedicated to TGFX; factory creation owns an EGL
    // context sharing the current host context's textures and restores host state.
    GLGraphicsContext(std::shared_ptr<tgfx::Device> device, const Ref<GrGraphicsContextOptions>& options);
    ~GLGraphicsContext() override;
    Ref<GLDrawableSurface> createSurface(int width, int height, Valdi::ColorType colorType, int sampleCount);
    // Borrowed native texture: the caller must keep it alive through surface destruction
    // and complete external writes before rendering. All GLuint/GLenum values remain unsigned.
    Ref<GLDrawableSurface> createSurfaceWrappingTexture(unsigned textureId, int width, int height,
        Valdi::ColorType colorType, int sampleCount, unsigned format, unsigned target);
    // Requires a current host GL context. Returned surfaces publish completed GPU
    // writes at flush so the host may subsequently sample them from its context.
    static Valdi::Result<Ref<GLGraphicsContext>> create(const Ref<GrGraphicsContextOptions>& options);
};
} // namespace snap::drawing
