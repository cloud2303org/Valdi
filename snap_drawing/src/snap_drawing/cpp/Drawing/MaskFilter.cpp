#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/MaskFilter.h"
#include "tgfx/core/PictureRecorder.h"
#include "tgfx/core/Surface.h"

#include <cmath>
#include <limits>
#endif

namespace snap::drawing {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
MaskFilter::MaskFilter() : MaskFilter(BlurStyleNormal, 0, true, nullptr) {}

MaskFilter::MaskFilter(BlurStyle style, Scalar sigma, bool respectCTM, std::shared_ptr<tgfx::ImageFilter> filter)
    : WrappedTgfxValue<std::shared_ptr<tgfx::ImageFilter>>(std::move(filter)),
      _style(style),
      _sigma(sigma),
      _respectCTM(respectCTM) {}
#else
MaskFilter::MaskFilter() : MaskFilter(nullptr) {}

MaskFilter::MaskFilter(sk_sp<SkMaskFilter> skValue) : WrappedSkValue<sk_sp<SkMaskFilter>>(std::move(skValue)) {}
#endif
MaskFilter::~MaskFilter() = default;

MaskFilter MaskFilter::makeBlur(BlurStyle blurStyle, Scalar sigma, bool respectCTM) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    if (!(sigma > 0) || !std::isfinite(sigma) || blurStyle < BlurStyleNormal || blurStyle > BlurStyleInner) {
        return MaskFilter();
    }
    return MaskFilter(blurStyle, sigma, respectCTM, tgfx::ImageFilter::Blur(sigma, sigma, tgfx::TileMode::Decal));
#else
    return MaskFilter(SkMaskFilter::MakeBlur(static_cast<SkBlurStyle>(blurStyle), sigma, respectCTM));
#endif
}

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
void MaskFilter::drawPath(tgfx::Canvas* canvas, const tgfx::Path& path, const tgfx::Paint& paint) const {
    if (!getSkValue()) {
        canvas->drawPath(path, paint);
        return;
    }
    auto bounds = path.getBounds();
    if (auto* stroke = paint.getStroke()) {
        const auto outset = stroke->width * std::max(1.0f, stroke->miterLimit) * 0.5f;
        bounds.outset(outset, outset);
    }
    // Keep immutable real TGFX path/paint/filter values in the picture, not a
    // pre-rasterized coverage mask. The native record invokes this at playback
    // with the final geometry matrix and the destination's actual clip.
    // Device-sized effects can have arbitrarily large local extents under
    // later minification, so conservatively mark this record clip-dependent.
    canvas->drawDeferred(bounds, std::ceil(3 * _sigma) + 2,
                         [filter = *this, path, paint](tgfx::Canvas* target) {
                             filter.drawResolvedPath(target, path, paint);
                         }, true);
}

void MaskFilter::drawResolvedPath(tgfx::Canvas* canvas, const tgfx::Path& path, const tgfx::Paint& paint) const {
    // Skia's default blur respects the CTM, using the geometric mean of the
    // lengths of the mapped radius vectors. Blur the coverage in device pixels,
    // rather than blurring the paint's RGB or its shader.
    const auto& matrix = canvas->getMatrix();
    const auto scales = matrix.getAxisScales();
    const auto sigma =
        _respectCTM ? static_cast<float>(_sigma * std::sqrt(static_cast<double>(scales.x) * scales.y)) : _sigma;
    if (!(sigma > 0) || !std::isfinite(sigma)) {
        canvas->drawPath(path, paint);
        return;
    }
    auto filter = sigma == _sigma ? getSkValue() : tgfx::ImageFilter::Blur(sigma, sigma, tgfx::TileMode::Decal);

    tgfx::PictureRecorder recorder;
    auto* coverageCanvas = recorder.beginRecording();
    // Inverse fills need finite recording bounds. Include a blur halo beyond
    // the destination clip so clipping does not introduce a false blurred edge.
    if (path.isInverseFillType() && paint.getStyle() == tgfx::PaintStyle::Fill) {
        auto clip = canvas->getTotalClipBounds();
        if (!clip && canvas->getSurface()) {
            clip = tgfx::Rect::MakeWH(canvas->getSurface()->width(), canvas->getSurface()->height());
        }
        if (!clip) {
            // An unbounded inverse mask has no finite image representation.
            // The caller must establish a clip on a recording-only canvas.
            return;
        }
        clip->outset(std::ceil(3 * sigma) + 1, std::ceil(3 * sigma) + 1);
        coverageCanvas->clipRect(*clip, false);
    }
    coverageCanvas->setMatrix(matrix);
    tgfx::Paint coveragePaint;
    coveragePaint.setColor(tgfx::Color::White());
    coveragePaint.setAntiAlias(paint.isAntiAlias());
    coveragePaint.setStyle(paint.getStyle());
    if (auto* stroke = paint.getStroke()) {
        coveragePaint.setStroke(*stroke);
    }
    coverageCanvas->drawPath(path, coveragePaint);
    auto picture = recorder.finishRecordingAsPicture();
    if (!picture) {
        return;
    }
    auto bounds = picture->getBounds();
    // TGFX advertises a 2-sigma filter outset. Pad the input by another sigma
    // to retain the 3-sigma coverage halo rather than clipping its outer tail.
    // Include the antialias fringe before quantizing the image bounds as well.
    bounds.outset(std::ceil(sigma) + 1, std::ceil(sigma) + 1);
    bounds.roundOut();
    if (bounds.isEmpty() || !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom) ||
        static_cast<double>(bounds.width()) > std::numeric_limits<int>::max() ||
        static_cast<double>(bounds.height()) > std::numeric_limits<int>::max()) {
        return;
    }
    auto originMatrix = tgfx::Matrix::MakeTrans(-bounds.left, -bounds.top);
    auto image = tgfx::Image::MakeFrom(
        std::move(picture), static_cast<int>(bounds.width()), static_cast<int>(bounds.height()), &originMatrix);
    if (!image) {
        return;
    }
    tgfx::Point offset = {};
    auto blurred = image->makeWithFilter(std::move(filter), &offset);
    if (!blurred) {
        return;
    }
    const tgfx::SamplingOptions sampling(tgfx::FilterMode::Nearest, tgfx::MipmapMode::None);
    auto originalShader =
        tgfx::Shader::MakeImageShader(image, tgfx::TileMode::Decal, tgfx::TileMode::Decal, sampling)
            ->makeWithMatrix(tgfx::Matrix::MakeTrans(bounds.left, bounds.top));
    auto blurredShader =
        tgfx::Shader::MakeImageShader(blurred, tgfx::TileMode::Decal, tgfx::TileMode::Decal, sampling)
            ->makeWithMatrix(tgfx::Matrix::MakeTrans(bounds.left + offset.x, bounds.top + offset.y));
    auto maskShader = blurredShader;
    switch (_style) {
        case BlurStyleNormal:
            break;
        case BlurStyleSolid:
            maskShader = tgfx::Shader::MakeBlend(tgfx::BlendMode::SrcOver, blurredShader, originalShader);
            break;
        case BlurStyleOuter:
            maskShader = tgfx::Shader::MakeBlend(tgfx::BlendMode::DstOut, blurredShader, originalShader);
            break;
        case BlurStyleInner:
            maskShader = tgfx::Shader::MakeBlend(tgfx::BlendMode::DstIn, blurredShader, originalShader);
            break;
    }

    auto outputBounds = tgfx::Rect::MakeXYWH(
        bounds.left + offset.x,
        bounds.top + offset.y,
        static_cast<float>(blurred->width()),
        static_cast<float>(blurred->height()));
    tgfx::Paint outputPaint = paint;
    outputPaint.setStyle(tgfx::PaintStyle::Fill);
    outputPaint.setAntiAlias(false);
    // This one draw changes the geometry to device coordinates and resets the
    // canvas matrix. Move the original local shader to that space exactly once;
    // ordinary path/rect draws leave their brush in local coordinates.
    if (auto shader = outputPaint.getShader()) {
        outputPaint.setShader(shader->makeWithMatrix(matrix));
    }
    outputPaint.setMaskFilter(tgfx::MaskFilter::MakeShader(std::move(maskShader)));
    const auto saveCount = canvas->save();
    canvas->resetMatrix();
    canvas->drawRect(outputBounds, outputPaint);
    canvas->restoreToCount(saveCount);
}
#endif

} // namespace snap::drawing
