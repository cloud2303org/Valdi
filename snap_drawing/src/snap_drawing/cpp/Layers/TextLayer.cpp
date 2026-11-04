//
//  TextLayer.cpp
//  valdi-skia
//
//  Created by Simon Corsin on 6/28/20.
//

#include "snap_drawing/cpp/Layers/TextLayer.hpp"

#include "valdi_core/cpp/Attributes/TextAttributeValue.hpp"
#include "valdi_core/cpp/Utils/Trace.hpp"
#include "snap_drawing/cpp/Drawing/MaskFilter.hpp"

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/core/Image.h"
#include "tgfx/core/MaskFilter.h"
#include "tgfx/core/PictureRecorder.h"
#else
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/effects/SkGradientShader.h"
#endif
#include "snap_drawing/cpp/Text/FontManager.hpp"
#include "snap_drawing/cpp/Text/TextLayoutBuilder.hpp"
#include "snap_drawing/cpp/Touches/AttributedTextOnTapGestureRecognizer.hpp"
#include "snap_drawing/cpp/Utils/GradientWrapper.hpp"
#include "snap_drawing/cpp/Utils/Path.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace snap::drawing {

constexpr double kMaxAdjustsFontSizeToFitWidthAttempt = 8;

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
static void drawResolvedTextShadowCoverage(NativeCanvas* canvas,
                                           const NativeTextBlob& blob,
                                           const Paint& paint,
                                           const MaskFilter& blur,
                                           Scalar sigma,
                                           Scalar x,
                                           Scalar y) {
    // Resolve the actual rendered glyph alpha, including bitmap/COLR emoji.
    // A color glyph's bounding rectangle or outline is not its coverage.
    // This is the text equivalent of MaskFilter's geometry coverage resolver:
    // blur in device coordinates, then color the resulting alpha mask.
    const auto matrix = canvas->getMatrix();
    tgfx::PictureRecorder recorder;
    auto* coverage = recorder.beginRecording();
    if (auto clip = canvas->getTotalClipBounds()) {
        // Do not clip source coverage at the destination edge before blur:
        // glyphs just outside that edge still contribute to its interior.
        const auto halo = blur.getSkValue() ? std::ceil(3 * sigma) + 2 : 1;
        clip->outset(halo, halo);
        coverage->clipRect(*clip, false);
    }
    coverage->setMatrix(matrix);
    tgfx::Paint coveragePaint;
    coveragePaint.setColor(tgfx::Color::White());
    coveragePaint.setAntiAlias(paint.getSkValue().isAntiAlias());
    coveragePaint.setStyle(paint.getSkValue().getStyle());
    if (auto* stroke = paint.getSkValue().getStroke()) coveragePaint.setStroke(*stroke);
    coverage->drawTextBlob(blob, x, y, coveragePaint);
    auto picture = recorder.finishRecordingAsPicture();
    if (!picture) return;
    auto bounds = picture->getBounds();
    // Match the geometry resolver's extra halo beyond TGFX's 2-sigma outset.
    auto padding = blur.getSkValue() ? std::ceil(sigma) + 1 : 1;
    bounds.outset(padding, padding);
    bounds.roundOut();
    if (bounds.isEmpty() || !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom) ||
        static_cast<double>(bounds.width()) > std::numeric_limits<int>::max() ||
        static_cast<double>(bounds.height()) > std::numeric_limits<int>::max()) return;
    auto origin = tgfx::Matrix::MakeTrans(-bounds.left, -bounds.top);
    auto image = tgfx::Image::MakeFrom(
        std::move(picture), static_cast<int>(bounds.width()), static_cast<int>(bounds.height()), &origin);
    if (!image) return;
    tgfx::Point offset = {};
    if (blur.getSkValue()) image = image->makeWithFilter(blur.getSkValue(), &offset);
    if (!image) return;
    const tgfx::SamplingOptions sampling(tgfx::FilterMode::Nearest, tgfx::MipmapMode::None);
    auto mask = tgfx::Shader::MakeImageShader(image, tgfx::TileMode::Decal, tgfx::TileMode::Decal, sampling);
    if (!mask) return;
    mask = mask->makeWithMatrix(tgfx::Matrix::MakeTrans(bounds.left + offset.x, bounds.top + offset.y));
    auto output = paint.getSkValue();
    if (auto shader = output.getShader()) output.setShader(shader->makeWithMatrix(matrix));
    output.setImageFilter(nullptr);
    output.setMaskFilter(tgfx::MaskFilter::MakeShader(std::move(mask)));
    output.setStyle(tgfx::PaintStyle::Fill);
    output.setAntiAlias(false);
    auto saveCount = canvas->save();
    canvas->resetMatrix();
    canvas->drawRect(tgfx::Rect::MakeXYWH(bounds.left + offset.x, bounds.top + offset.y,
        static_cast<float>(image->width()), static_cast<float>(image->height())), output);
    canvas->restoreToCount(saveCount);
}

static void drawTextShadowCoverage(NativeCanvas* canvas,
                                   const NativeTextBlob& blob,
                                   const Paint& paint,
                                   const MaskFilter& blur,
                                   Scalar sigma,
                                   Scalar x,
                                   Scalar y) {
    if (!blob) return;
    auto bounds = blob->getBounds();
    bounds.offset(x, y);
    if (auto* stroke = paint.getSkValue().getStroke()) {
        const auto outset = stroke->width * std::max(1.0f, stroke->miterLimit) * 0.5f;
        bounds.outset(outset, outset);
    }
    const auto halo = blur.getSkValue() ? std::ceil(3 * sigma) + 2 : 1;
    // A cached TextLayer picture can be replayed at any scale/display scale.
    // Retain the real blob/font/paint/filter, not a raster mask resolved using
    // the recorder's matrix. The final target supplies its actual matrix and
    // clip. Clip-dependent bounds preserve fixed device halos on minification.
    canvas->drawDeferred(bounds, halo,
        [blob, paint, blur, sigma, x, y](NativeCanvas* target) {
            drawResolvedTextShadowCoverage(target, blob, paint, blur, sigma, x, y);
        }, true);
}
#endif

static Path makeTextDecorationPath(const Rect& bounds) {
    Path path;
    auto centerY = bounds.y() + bounds.height() / 2.0f;
    path.moveTo(bounds.x(), centerY);
    path.lineTo(bounds.right, centerY);
    return path;
}

static void drawTextDecoration(DrawingContext& drawingContext,
                               Paint& paint,
                               TextLayoutDecorationStyle style,
                               const std::optional<TextCustomUnderlineStyle>& customUnderlineStyle,
                               const Rect& bounds) {
    if (customUnderlineStyle) {
        paint.setStroke(true);
        paint.setStrokeWidth(customUnderlineStyle->height);
        paint.setStrokeCap(PaintStrokeCapButt);
        if (customUnderlineStyle->isPatterned()) {
            paint.setStrokeDashPattern(customUnderlineStyle->onWidth, customUnderlineStyle->offWidth);
        }

        auto path = makeTextDecorationPath(bounds);
        drawingContext.drawPaint(paint, path);
    } else if (style == TextLayoutDecorationStyleDashed) {
        auto strokeWidth = std::max<Scalar>(bounds.height(), 1.0f);
        paint.setStroke(true);
        paint.setStrokeWidth(strokeWidth);
        paint.setStrokeCap(PaintStrokeCapButt);
        paint.setStrokeDashPattern(strokeWidth * 3.0f, strokeWidth * 2.0f);

        auto path = makeTextDecorationPath(bounds);
        drawingContext.drawPaint(paint, path);
    } else if (style == TextLayoutDecorationStyleDotted) {
        auto strokeWidth = std::max<Scalar>(bounds.height(), 1.0f);
        paint.setStroke(false);
        paint.setStrokeDotPattern(strokeWidth / 2.0f, strokeWidth * 2.0f);

        auto path = makeTextDecorationPath(bounds);
        drawingContext.drawPaint(paint, path);
    } else {
        drawingContext.drawPaint(paint, bounds);
    }
}

TextLayer::TextLayer(const Ref<Resources>& resources) : Layer(resources) {
    _textPaint.setAntiAlias(true);
    _gradientWrapper = GradientWrapper();
}

TextLayer::~TextLayer() = default;

Size TextLayer::sizeThatFits(Size maxSize) {
    auto displayScale = getResources()->getDisplayScale();
    auto& layout = getTextLayout(maxSize, *getResources());

    return Size::make(layout.getBounds().width() / displayScale, layout.getBounds().height() / displayScale);
}

void TextLayer::onDraw(DrawingContext& drawingContext) {
    Layer::onDraw(drawingContext);

    auto displayScale = getResources()->getDisplayScale();
    auto layerSize = Size::make(getFrame().width(), getFrame().height());
    auto& layout = getTextLayout(layerSize, *getResources());
    auto offsetY = getTextVerticalOffset(layerSize, layout, displayScale);
    drawingContext.concat(Matrix::makeScaleTranslate(1.0f / displayScale, 1.0f / displayScale, 0.0f, offsetY));

    if (hasTextShadow()) {
        // Draw all of the shadows first
        drawTextVisualEntriesShadows(drawingContext, layout.getVisualEntries());
        for (const auto& entry : layout.getEntries()) {
            if (entry.textBlob != nullptr) {
                drawTextShadows(drawingContext, entry.textBlob);
            }
        }
    }

    // Then draw all of the text things
    drawTextVisualEntries(drawingContext, layout.getVisualEntries(), /* predraw */ true);

    for (const auto& entry : layout.getEntries()) {
        if (entry.textBlob != nullptr) {
            drawText(drawingContext, entry.textBlob, entry.color);
        }
    }

    drawTextVisualEntries(drawingContext, layout.getVisualEntries(), /* predraw */ false);
}

void TextLayer::onBoundsChanged() {
    Layer::onBoundsChanged();
    setNeedsLayout();
}

void TextLayer::onChildInserted(Layer* childLayer, size_t index) {
    Layer::onChildInserted(childLayer, index);
    setNeedsLayout();
}

void TextLayer::onChildRemoved(Layer* childLayer) {
    Layer::onChildRemoved(childLayer);
    setNeedsLayout();
}

void TextLayer::onLayout() {
    Layer::onLayout();
    layoutInlineChildrenInLayer(*this);
}

void TextLayer::layoutInlineChildrenInLayer(Layer& childrenLayer) {
    auto displayScale = getResources()->getDisplayScale();
    auto layerSize = Size::make(getFrame().width(), getFrame().height());
    auto& layout = getTextLayout(layerSize, *getResources());
    auto offsetY = getTextVerticalOffset(layerSize, layout, displayScale);
    auto childrenSize = childrenLayer.getChildrenSize();
    struct InlineChildLayout {
        Ref<Valdi::TextInlineAttachment> attachment;
        Rect bounds;
    };
    std::vector<InlineChildLayout> childIndexToInlineChildLayouts(childrenSize);

    for (const auto& attachment : layout.getAttachments()) {
        auto inlineViewAttachment = Valdi::castOrNull<Valdi::TextInlineAttachment>(attachment.attachment);
        if (inlineViewAttachment == nullptr) {
            continue;
        }

        auto childIndex = inlineViewAttachment->getChildIndex();
        if (childIndex >= childrenSize) {
            continue;
        }

        childIndexToInlineChildLayouts[childIndex] = {inlineViewAttachment, attachment.bounds};
    }

    for (size_t childIndex = 0; childIndex < childrenSize; childIndex++) {
        const auto& inlineChildLayout = childIndexToInlineChildLayouts[childIndex];
        if (inlineChildLayout.attachment == nullptr) {
            childrenLayer.getChild(childIndex)->setFrame(Rect::makeXYWH(0, 0, 0, 0));
            continue;
        }

        auto bounds = inlineChildLayout.bounds;
        childrenLayer.getChild(childIndex)
            ->setFrame(Rect::makeXYWH(bounds.x() / displayScale,
                                      (bounds.y() / displayScale) + offsetY,
                                      bounds.width() / displayScale,
                                      bounds.height() / displayScale));
    }
}

void TextLayer::drawTextVisualEntriesShadows(DrawingContext& drawingContext,
                                             const std::vector<TextLayoutVisualEntry>& visualEntries) {
    for (const auto& visualEntry : visualEntries) {
        if (visualEntry.kind == TextLayoutVisualEntryKindBackground) {
            continue;
        }

        auto resolvedPaint = _textPaint;
        resolvedPaint.setMaskFilter(MaskFilter::makeBlur(BlurStyleNormal, _textShadow.radius, false));

        resolvedPaint.setColor(_textShadow.color);
        resolvedPaint.setAlpha(_textShadow.opacity);

        auto bounds = visualEntry.bounds;
        bounds.offsetX(_textShadow.offsetX);
        bounds.offsetY(_textShadow.offsetY);

        drawTextDecoration(drawingContext, resolvedPaint, visualEntry.style, visualEntry.customUnderlineStyle, bounds);
    }
}

void TextLayer::drawTextVisualEntries(DrawingContext& drawingContext,
                                      const std::vector<TextLayoutVisualEntry>& visualEntries,
                                      bool predraw) {
    for (const auto& visualEntry : visualEntries) {
        if (visualEntry.predraw != predraw) {
            continue;
        }

        auto resolvedPaint = _textPaint;

        if (visualEntry.kind == TextLayoutVisualEntryKindBackground) {
            if (!visualEntry.color) {
                continue;
            }
            resolvedPaint.setColor(visualEntry.color.value());
            if (!visualEntry.borderRadius.isEmpty()) {
                auto path = visualEntry.borderRadius.getPath(visualEntry.bounds);
                drawingContext.drawPaint(resolvedPaint, path);
                continue;
            }
        } else if (_gradientWrapper.hasGradient()) {
            applyGradientToTextPaint(resolvedPaint);
        } else if (visualEntry.color) {
            resolvedPaint.setColor(visualEntry.color.value());
        }

        drawTextDecoration(
            drawingContext, resolvedPaint, visualEntry.style, visualEntry.customUnderlineStyle, visualEntry.bounds);
    }
}

void TextLayer::drawText(DrawingContext& drawingContext,
                         const NativeTextBlob& textBlob,
                         std::optional<Color> textColor) {
    auto resolvedPaint = _textPaint;
    if (_gradientWrapper.hasGradient()) {
        applyGradientToTextPaint(resolvedPaint);
    } else {
        if (textColor) {
            resolvedPaint.setColor(textColor.value());
        }
    }

    drawingContext.canvas()->drawTextBlob(textBlob, 0, 0, resolvedPaint.getSkValue());
}

void TextLayer::applyGradientToTextPaint(Paint& paint) {
    if (_textLayout != nullptr) {
        _gradientWrapper.update(_textLayout->getBounds());
        _gradientWrapper.applyToPaint(paint);
    }
}

bool TextLayer::hasTextShadow() const {
    auto isVisible = _textShadow.opacity > 0;
    auto hiddenBehindText = _textShadow.radius == 0 && _textShadow.offsetX == 0 && _textShadow.offsetY == 0;
    return isVisible && !hiddenBehindText;
}

void TextLayer::drawTextShadows(DrawingContext& drawingContext, const NativeTextBlob& textBlob) {
    auto resolvedPaint = _textPaint;

    auto blur = MaskFilter::makeBlur(BlurStyleNormal, _textShadow.radius, false);
    resolvedPaint.setMaskFilter(blur);
    resolvedPaint.setColor(_textShadow.color);
    resolvedPaint.setAlpha(_textShadow.opacity);

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    drawTextShadowCoverage(drawingContext.canvas(), textBlob, resolvedPaint, blur, _textShadow.radius,
                           _textShadow.offsetX, _textShadow.offsetY);
#else
    drawingContext.canvas()->drawTextBlob(
        textBlob.get(), _textShadow.offsetX, _textShadow.offsetY, resolvedPaint.getSkValue());
#endif
}

void TextLayer::setText(const Valdi::StringBox& text) {
    if (_text != text || _attributedText != nullptr) {
        _text = text;
        _attributedText = nullptr;

        setNeedsTextLayout();
    }
}

const Valdi::StringBox& TextLayer::getText() const {
    return _text;
}

void TextLayer::setAttributedText(const Valdi::Ref<AttributedText>& attributedText) {
    if (_attributedText != attributedText) {
        _attributedText = attributedText;
        _text = Valdi::StringBox();

        setNeedsTextLayout();
    }
}

const Valdi::Ref<AttributedText>& TextLayer::getAttributedText() const {
    return _attributedText;
}

void TextLayer::setTextColor(Color textColor) {
    if (_textPaint.getColor() != textColor) {
        _textPaint.setColor(textColor);

        setNeedsDisplay();
    }
}

Color TextLayer::getTextColor() const {
    return _textPaint.getColor();
}

void TextLayer::setTextLinearGradient(std::vector<Scalar>&& locations,
                                      std::vector<Color>&& colors,
                                      LinearGradientOrientation orientation) {
    if (_gradientWrapper.clearIfNeeded(GradientWrapper::GradientType::RADIAL)) {
        setNeedsDisplay();
    }

    if (colors.empty()) {
        if (_gradientWrapper.clearIfNeeded(GradientWrapper::GradientType::LINEAR)) {
            setNeedsDisplay();
        }
        return;
    }
    _gradientWrapper.setAsLinear(std::move(locations), std::move(colors), orientation);

    if (_gradientWrapper.isDirty()) {
        setNeedsDisplay();
    }
}

void TextLayer::setTextRadialGradient(std::vector<Scalar>&& locations, std::vector<Color>&& colors) {
    if (_gradientWrapper.clearIfNeeded(GradientWrapper::GradientType::LINEAR)) {
        setNeedsDisplay();
    }

    if (colors.empty()) {
        if (_gradientWrapper.clearIfNeeded(GradientWrapper::GradientType::RADIAL)) {
            setNeedsDisplay();
        }
        return;
    }

    _gradientWrapper.setAsRadial(std::move(locations), std::move(colors));

    if (_gradientWrapper.isDirty()) {
        setNeedsDisplay();
    }
}

void TextLayer::resetTextGradient() {
    _gradientWrapper.clear();
    setNeedsDisplay();
}

void TextLayer::setTextAlign(TextAlign textAlign) {
    if (_textAlign != textAlign) {
        _textAlign = textAlign;

        setNeedsTextLayout();
    }
}

TextAlign TextLayer::getTextAlign() const {
    return _textAlign;
}

void TextLayer::setTextVerticalAlignment(TextVerticalAlignment textVerticalAlignment) {
    if (_textVerticalAlignment != textVerticalAlignment) {
        _textVerticalAlignment = textVerticalAlignment;
        setNeedsDisplay();
    }
}

TextVerticalAlignment TextLayer::getTextVerticalAlignment() const {
    return _textVerticalAlignment;
}

Scalar TextLayer::getTextVerticalOffset(const Size& layerSize, const TextLayout& layout, Scalar displayScale) const {
    if (_textVerticalAlignment == TextVerticalAlignmentCenter) {
        auto layoutHeight = layout.getBounds().height() / displayScale;
        return std::max<Scalar>((layerSize.height - layoutHeight) / 2.0f, 0.0f);
    }

    return 0.0f;
}

void TextLayer::setTextDecoration(TextDecoration textDecoration) {
    if (_textDecoration != textDecoration) {
        _textDecoration = textDecoration;
        setNeedsTextLayout();
    }
}

TextDecoration TextLayer::getTextDecoration() const {
    return _textDecoration;
}

void TextLayer::setCustomUnderlineStyle(std::optional<TextCustomUnderlineStyle> customUnderlineStyle) {
    if (_customUnderlineStyle != customUnderlineStyle) {
        _customUnderlineStyle = customUnderlineStyle;
        setNeedsTextLayout();
    }
}

const std::optional<TextCustomUnderlineStyle>& TextLayer::getCustomUnderlineStyle() const {
    return _customUnderlineStyle;
}

void TextLayer::setTextOverflow(TextOverflow textOveflow) {
    if (_textOverflow != textOveflow) {
        _textOverflow = textOveflow;
        setNeedsTextLayout();
    }
}

TextOverflow TextLayer::getTextOverflow() const {
    return _textOverflow;
}

void TextLayer::setTextShadow(Color color, Scalar radius, float opacity, Scalar offsetX, Scalar offsetY) {
    auto textShadow = TextShadow(color, radius, opacity, offsetX, offsetY);
    if (_textShadow != textShadow) {
        _textShadow = textShadow;
        setNeedsDisplay();
    }
}

void TextLayer::resetTextShadow() {
    setTextShadow(Color(), 0, 0, 0, 0);
}

TextShadow TextLayer::getTextShadow() const {
    return _textShadow;
}

void TextLayer::setTextFont(const Valdi::Ref<Font>& font) {
    if (_textFont != font) {
        _textFont = font;

        setNeedsTextLayout();
    }
}

const Valdi::Ref<Font>& TextLayer::getTextFont() const {
    return _textFont;
}

void TextLayer::setNumberOfLines(int numberOfLines) {
    if (_numberOfLines != numberOfLines) {
        _numberOfLines = numberOfLines;

        setNeedsTextLayout();
    }
}

int TextLayer::getNumberOfLines() const {
    return _numberOfLines;
}

void TextLayer::setAdjustsFontSizeToFitWidth(bool adjustsFontSizeToFitWidth) {
    if (_adjustsFontSizeToFitWidth != adjustsFontSizeToFitWidth) {
        _adjustsFontSizeToFitWidth = adjustsFontSizeToFitWidth;
        setNeedsTextLayout();
    }
}

bool TextLayer::getAdjustsFontSizeToFitWidth() const {
    return _adjustsFontSizeToFitWidth;
}

void TextLayer::setMinimumScaleFactor(double minimumScaleFactor) {
    minimumScaleFactor = std::clamp(minimumScaleFactor, 0.0, 1.0);

    if (_minimumScaleFactor != minimumScaleFactor) {
        _minimumScaleFactor = minimumScaleFactor;
        if (_adjustsFontSizeToFitWidth) {
            setNeedsTextLayout();
        }
    }
}

double TextLayer::getMinimumScaleFactor() const {
    return _minimumScaleFactor;
}

void TextLayer::setLineHeight(Scalar lineHeight) {
    if (_lineHeight != lineHeight) {
        _lineHeight = lineHeight;
        setNeedsTextLayout();
    }
}

Scalar TextLayer::getLineHeight() const {
    return _lineHeight;
}

void TextLayer::setLineHeightAbsolute(Scalar lineHeightAbsolute) {
    if (_usesLineHeightAbsolute != true || _lineHeightAbsolute != lineHeightAbsolute) {
        _usesLineHeightAbsolute = true;
        _lineHeightAbsolute = lineHeightAbsolute;
        setNeedsTextLayout();
    }
}

void TextLayer::resetLineHeightAbsolute() {
    if (_usesLineHeightAbsolute) {
        _usesLineHeightAbsolute = false;
        _lineHeightAbsolute = 0.0f;
        setNeedsTextLayout();
    }
}

void TextLayer::setLetterSpacing(Scalar letterSpacing) {
    if (_letterSpacing != letterSpacing) {
        _letterSpacing = letterSpacing;
        setNeedsTextLayout();
    }
}

Scalar TextLayer::getLetterSpacing() const {
    return _letterSpacing;
}

void TextLayer::setNeedsTextLayout() {
    if (_textLayout != nullptr) {
        _textLayout = nullptr;
        if (_attributedTextOnTapGestureRecognizer != nullptr) {
            _attributedTextOnTapGestureRecognizer->setTextLayout(nullptr);
        }
        setNeedsLayout();
        setNeedsDisplay();
    }
}

static bool hasOnTapAttributeInTextLayout(const TextLayout& textLayout) {
    for (const auto& attachment : textLayout.getAttachments()) {
        if (Valdi::castOrNull<AttributedTextOnTapAttribute>(attachment.attachment) != nullptr) {
            return true;
        }
    }

    return false;
}

static bool textLayoutHasStaleInlineAttachmentSizes(const TextLayout& textLayout, Scalar displayScale) {
    for (const auto& attachment : textLayout.getAttachments()) {
        auto inlineViewAttachment = Valdi::castOrNull<Valdi::TextInlineAttachment>(attachment.attachment);
        if (inlineViewAttachment == nullptr) {
            continue;
        }

        auto currentSize = inlineViewAttachment->getSize();
        auto currentReplacementSize = Size::make(currentSize.width * displayScale, currentSize.height * displayScale);
        if (attachment.replacementSize != currentReplacementSize) {
            return true;
        }
    }

    return false;
}

TextLayout& TextLayer::getTextLayout(Size size, const Resources& resources) {
    return getTextLayout(
        size, resources.getRespectDynamicType(), resources.getDisplayScale(), resources.getDynamicTypeScale());
}

TextLayout& TextLayer::getTextLayout(Size size, bool respectDynamicType, Scalar displayScale, Scalar dynamicTypeScale) {
    // Align maxSize to pixel grid
    auto maxSize = Size::make(ceilf(size.width * displayScale), ceilf(size.height * displayScale));

    if (_textLayout != nullptr &&
        (_textLayout->getMaxSize() != maxSize || textLayoutHasStaleInlineAttachmentSizes(*_textLayout, displayScale))) {
        _textLayout = nullptr;
    }

    if (_textLayout == nullptr) {
        VALDI_TRACE("SnapDrawing.makeTextLayout");
        auto resolvedLineHeight = resolveLineHeight(displayScale);
        _textLayout = TextLayer::makeTextLayout(maxSize,
                                                _text,
                                                _attributedText,
                                                _textFont,
                                                _textAlign,
                                                _textDecoration,
                                                _textOverflow,
                                                _numberOfLines,
                                                resolvedLineHeight,
                                                _letterSpacing,
                                                isRightToLeft(),
                                                _adjustsFontSizeToFitWidth,
                                                _minimumScaleFactor,
                                                respectDynamicType,
                                                /* includeTextBlob*/ true,
                                                displayScale,
                                                dynamicTypeScale,
                                                getResources()->getFontManager(),
                                                _customUnderlineStyle);

        if (hasOnTapAttributeInTextLayout(*_textLayout)) {
            addOnTapGestureRecognizer();
        } else {
            removeOnTapGestureRecognizer();
        }
    }

    return *_textLayout;
}

TextLayoutLineHeight TextLayer::resolveLineHeight(Scalar displayScale) const {
    if (!_usesLineHeightAbsolute) {
        return TextLayoutLineHeight::multiple(_lineHeight);
    }

    return TextLayoutLineHeight::absolute(_lineHeightAbsolute * displayScale);
}

void TextLayer::removeOnTapGestureRecognizer() {
    if (_attributedTextOnTapGestureRecognizer != nullptr) {
        removeGestureRecognizer(_attributedTextOnTapGestureRecognizer);
        _attributedTextOnTapGestureRecognizer = nullptr;
    }
}

void TextLayer::addOnTapGestureRecognizer() {
    if (_attributedTextOnTapGestureRecognizer == nullptr) {
        _attributedTextOnTapGestureRecognizer =
            Valdi::makeShared<AttributedTextOnTapGestureRecognizer>(getResources()->getGesturesConfiguration());
        addGestureRecognizer(_attributedTextOnTapGestureRecognizer);
    }

    _attributedTextOnTapGestureRecognizer->setTextLayout(_textLayout);
}

void TextLayer::onRightToLeftChanged() {
    Layer::onRightToLeftChanged();

    setNeedsTextLayout();
}

Size TextLayer::measureText(Size maxSize,
                            const String& text,
                            const Ref<AttributedText>& attributedText,
                            const Ref<Font>& font,
                            TextAlign textAlign,
                            TextDecoration textDecoration,
                            TextOverflow textOverflow,
                            int numberOfLines,
                            TextLayoutLineHeight lineHeight,
                            Scalar letterSpacing,
                            bool isRightToLeft,
                            bool adjustsFontSizeToFitWidth,
                            double minimumScaleFactor,
                            bool respectDynamicType,
                            Scalar displayScale,
                            Scalar dynamicTypeScale,
                            const Ref<FontManager>& fontManager,
                            std::optional<TextCustomUnderlineStyle> customUnderlineStyle) {
    auto textLayout = makeTextLayout(maxSize,
                                     text,
                                     attributedText,
                                     font,
                                     textAlign,
                                     textDecoration,
                                     textOverflow,
                                     numberOfLines,
                                     lineHeight,
                                     letterSpacing,
                                     isRightToLeft,
                                     adjustsFontSizeToFitWidth,
                                     minimumScaleFactor,
                                     respectDynamicType,
                                     /* includeTextBlob*/ false,
                                     displayScale,
                                     dynamicTypeScale,
                                     fontManager,
                                     customUnderlineStyle);

    return textLayout->getBounds().size();
}

Ref<TextLayout> TextLayer::makeTextLayout(Size maxSize,
                                          const String& text,
                                          const Ref<AttributedText>& attributedText,
                                          const Ref<Font>& font,
                                          TextAlign textAlign,
                                          TextDecoration textDecoration,
                                          TextOverflow textOverflow,
                                          int numberOfLines,
                                          TextLayoutLineHeight lineHeight,
                                          Scalar letterSpacing,
                                          bool isRightToLeft,
                                          bool adjustsFontSizeToFitWidth,
                                          double minimumScaleFactor,
                                          bool respectDynamicType,
                                          bool includeTextBlob,
                                          Scalar displayScale,
                                          Scalar dynamicTypeScale,
                                          const Ref<FontManager>& fontManager,
                                          std::optional<TextCustomUnderlineStyle> customUnderlineStyle) {
    if (!adjustsFontSizeToFitWidth || numberOfLines != 1) {
        return makeTextLayoutUnscaled(maxSize,
                                      text,
                                      attributedText,
                                      font,
                                      textAlign,
                                      textDecoration,
                                      textOverflow,
                                      numberOfLines,
                                      lineHeight,
                                      letterSpacing,
                                      isRightToLeft,
                                      1.0,
                                      respectDynamicType,
                                      includeTextBlob,
                                      displayScale,
                                      dynamicTypeScale,
                                      fontManager,
                                      customUnderlineStyle);
    }

    auto currentScale = 1.0;
    auto scaleIncrement = (1.0 - minimumScaleFactor) / kMaxAdjustsFontSizeToFitWidthAttempt;

    for (;;) {
        auto layout = makeTextLayoutUnscaled(maxSize,
                                             text,
                                             attributedText,
                                             font,
                                             textAlign,
                                             textDecoration,
                                             textOverflow,
                                             numberOfLines,
                                             lineHeight,
                                             letterSpacing,
                                             isRightToLeft,
                                             currentScale,
                                             respectDynamicType,
                                             includeTextBlob,
                                             displayScale,
                                             dynamicTypeScale,
                                             fontManager,
                                             customUnderlineStyle);

        if (layout->fitsInMaxSize() || currentScale <= minimumScaleFactor) {
            return layout;
        } else {
            currentScale = std::max(minimumScaleFactor, currentScale - scaleIncrement);
        }
    }
}

static double resolveFontScale(
    const Ref<Font>& font, double fontScale, bool respectDynamicType, Scalar displayScale, Scalar dynamicTypeScale) {
    auto fontScaleRespectingDisplayScale = fontScale * displayScale;
    if (!respectDynamicType || !font->respectDynamicType()) {
        return fontScaleRespectingDisplayScale;
    }

    return fontScaleRespectingDisplayScale * dynamicTypeScale;
}

Ref<TextLayout> TextLayer::makeTextLayoutUnscaled(Size maxSize,
                                                  const String& text,
                                                  const Ref<AttributedText>& attributedText,
                                                  const Ref<Font>& font,
                                                  TextAlign textAlign,
                                                  TextDecoration textDecoration,
                                                  TextOverflow textOverflow,
                                                  int numberOfLines,
                                                  TextLayoutLineHeight lineHeight,
                                                  Scalar letterSpacing,
                                                  bool isRightToLeft,
                                                  double fontScale,
                                                  bool respectDynamicType,
                                                  bool includeTextBlob,
                                                  Scalar displayScale,
                                                  Scalar dynamicTypeScale,
                                                  const Ref<FontManager>& fontManager,
                                                  std::optional<TextCustomUnderlineStyle> customUnderlineStyle) {
    TextLayoutBuilder builder(
        textAlign, textOverflow, maxSize, numberOfLines, fontManager, isRightToLeft, displayScale, false);
    builder.setIncludeTextBlob(includeTextBlob);

    auto textFont = font;
    if (textFont == nullptr && fontManager != nullptr) {
        auto defaultFont = fontManager->getDefaultFont();
        if (defaultFont) {
            textFont = defaultFont.moveValue();
        }
    }

    if (attributedText != nullptr) {
        for (size_t i = 0; i < attributedText->getPartsSize(); i++) {
            const auto& style = attributedText->getStyleAtIndex(i);
            const auto& content = attributedText->getContentAtIndex(i);

            Ref<Font> resolvedFont = style.font != nullptr ? style.font : textFont;
            if (resolvedFont == nullptr) {
                continue;
            }
            auto resolvedFontScale =
                resolveFontScale(resolvedFont, fontScale, respectDynamicType, displayScale, dynamicTypeScale);
            auto resolvedTextDecoration = style.textDecoration ? style.textDecoration.value() : textDecoration;
            std::optional<TextBackgroundStyle> backgroundStyle;
            if (style.backgroundColor) {
                backgroundStyle = TextBackgroundStyle{style.backgroundColor,
                                                      style.backgroundPadding.value_or(TextBackgroundPadding()),
                                                      style.backgroundBorderRadius.value_or(BorderRadius())};
            }

            Ref<Valdi::RefCountable> attachment = style.onTap;
            std::optional<Size> replacementSize;
            auto replacementVerticalAlignment = Valdi::InlineViewVerticalAlignment::Center;
            if (style.inlineViewAttachment != nullptr) {
                attachment = style.inlineViewAttachment;
                auto inlineViewSize = style.inlineViewAttachment->getSize();
                replacementSize = Size::make(inlineViewSize.width * displayScale, inlineViewSize.height * displayScale);
                replacementVerticalAlignment = style.inlineViewAttachment->getVerticalAlignment();
            }

            builder.append(content.toStringView(),
                           resolvedFont->withScale(resolvedFontScale),
                           lineHeight,
                           letterSpacing,
                           resolvedTextDecoration,
                           attachment,
                           style.color,
                           backgroundStyle,
                           customUnderlineStyle,
                           replacementSize,
                           replacementVerticalAlignment);
        }
    } else if (!text.isEmpty() && textFont != nullptr) {
        auto resolvedFontScale =
            resolveFontScale(textFont, fontScale, respectDynamicType, displayScale, dynamicTypeScale);
        builder.append(text.toStringView(),
                       textFont->withScale(resolvedFontScale),
                       lineHeight,
                       letterSpacing,
                       textDecoration,
                       nullptr,
                       std::nullopt,
                       std::nullopt,
                       customUnderlineStyle);
    }

    return builder.build();
}

} // namespace snap::drawing
