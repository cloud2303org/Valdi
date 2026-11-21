//
//  ViewManagerContext.cpp
//  valdi-desktop-apple
//
//  Created by Simon Corsin on 10/19/20.
//

#include "valdi/runtime/Context/ViewManagerContext.hpp"
#include "valdi/runtime/Attributes/AttributesManager.hpp"
#include "valdi/runtime/Attributes/DefaultAttributeProcessors.hpp"
#include "valdi/runtime/Interfaces/IViewManager.hpp"
#include "valdi/runtime/Utils/MainThreadManager.hpp"
#include "valdi/runtime/Views/GlobalViewFactories.hpp"
#include "valdi/runtime/Views/View.hpp"
#include "valdi/runtime/Views/ViewPreloader.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"
#include "valdi_core/cpp/Utils/Trace.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace Valdi {

namespace {

void registerDefaultGenericViewTagAliases(GlobalViewFactories& factories, PlatformType platformType) {
    factories.registerViewClassReplacement(STRING_LITERAL("layout"),
                                           AttributesManager::getLayoutPlaceholderClassName());

    if (platformType == PlatformTypeAndroid) {
        factories.registerViewClassReplacement(STRING_LITERAL("view"), STRING_LITERAL("com.snap.valdi.views.ValdiView"));
        factories.registerViewClassReplacement(STRING_LITERAL("label"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiTextView"));
        factories.registerViewClassReplacement(STRING_LITERAL("scroll"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiScrollView"));
        factories.registerViewClassReplacement(STRING_LITERAL("image"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiImageView"));
        factories.registerViewClassReplacement(STRING_LITERAL("video"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiVideoView"));
        factories.registerViewClassReplacement(STRING_LITERAL("webview"),
                                               STRING_LITERAL("com.snap.valdi.modules.webview.ValdiWebView"));
        factories.registerViewClassReplacement(STRING_LITERAL("button"), STRING_LITERAL("android.widget.Button"));
        factories.registerViewClassReplacement(STRING_LITERAL("spinner"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiSpinnerView"));
        factories.registerViewClassReplacement(STRING_LITERAL("blur"), STRING_LITERAL("com.snap.valdi.views.ValdiView"));
        factories.registerViewClassReplacement(STRING_LITERAL("glass"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiView"));
        factories.registerViewClassReplacement(STRING_LITERAL("textfield"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiEditText"));
        factories.registerViewClassReplacement(STRING_LITERAL("textview"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiEditTextMultiline"));
        factories.registerViewClassReplacement(STRING_LITERAL("textanimationgroup"),
                                               STRING_LITERAL("com.snap.valdi.views.ValdiTextAnimationGroup"));
        factories.registerViewClassReplacement(STRING_LITERAL("shape"),
                                               STRING_LITERAL("com.snap.valdi.views.ShapeView"));
        factories.registerViewClassReplacement(STRING_LITERAL("animatedimage"),
                                               STRING_LITERAL("com.snap.valdi.views.AnimatedImageView"));
    } else {
        factories.registerViewClassReplacement(STRING_LITERAL("view"), STRING_LITERAL("SCValdiView"));
        factories.registerViewClassReplacement(STRING_LITERAL("label"), STRING_LITERAL("SCValdiLabel"));
        factories.registerViewClassReplacement(STRING_LITERAL("scroll"), STRING_LITERAL("SCValdiScrollView"));
        factories.registerViewClassReplacement(STRING_LITERAL("image"), STRING_LITERAL("SCValdiImageView"));
        factories.registerViewClassReplacement(STRING_LITERAL("video"), STRING_LITERAL("SCValdiVideoView"));
        factories.registerViewClassReplacement(STRING_LITERAL("webview"), STRING_LITERAL("SCValdiWebView"));
        factories.registerViewClassReplacement(STRING_LITERAL("button"), STRING_LITERAL("UIButton"));
        factories.registerViewClassReplacement(STRING_LITERAL("spinner"), STRING_LITERAL("SCValdiSpinnerView"));
        factories.registerViewClassReplacement(STRING_LITERAL("blur"), STRING_LITERAL("SCValdiBlurView"));
        factories.registerViewClassReplacement(STRING_LITERAL("glass"), STRING_LITERAL("SCValdiGlassView"));
        factories.registerViewClassReplacement(STRING_LITERAL("textfield"), STRING_LITERAL("SCValdiTextField"));
        factories.registerViewClassReplacement(STRING_LITERAL("textview"), STRING_LITERAL("SCValdiTextView"));
        factories.registerViewClassReplacement(STRING_LITERAL("textanimationgroup"),
                                               STRING_LITERAL("SCValdiTextAnimationGroup"));
        factories.registerViewClassReplacement(STRING_LITERAL("shape"), STRING_LITERAL("SCValdiShapeView"));
        factories.registerViewClassReplacement(STRING_LITERAL("animatedimage"),
                                               STRING_LITERAL("SCValdiAnimatedContentView"));
    }
}

} // namespace

ViewManagerContext::ViewManagerContext(IViewManager& viewManager,
                                       AttributeIds& attributeIds,
                                       const Ref<ColorPaletteManager>& colorPaletteManager,
                                       const Shared<YGConfig>& yogaConfig,
                                       bool enablePreloading,
                                       const Ref<MainThreadManager>& mainThreadManager,
                                       ILogger& logger)
    : _viewManager(viewManager),
      _attributesManager(viewManager, attributeIds, colorPaletteManager, logger, yogaConfig),
      _mainThreadManager(mainThreadManager) {
    Valdi::registerDefaultProcessors(_attributesManager);

    _globalViewFactories = Valdi::makeShared<GlobalViewFactories>(_attributesManager);
    registerDefaultGenericViewTagAliases(*_globalViewFactories, _viewManager.getPlatformType());

    if (enablePreloading) {
        _viewPreloader = Valdi::makeShared<ViewPreloader>(_globalViewFactories, *mainThreadManager, logger);
    }
}

ViewManagerContext::~ViewManagerContext() {
    if (_viewPreloader != nullptr) {
        _viewPreloader->stopPreload();
    }
}

void ViewManagerContext::registerViewClassReplacement(const StringBox& className,
                                                      const StringBox& replacementClassName) {
    _globalViewFactories->registerViewClassReplacement(className, replacementClassName);
}

namespace {

// Small enough that one batch stays well under a frame on low-end Android devices.
constexpr size_t kPooledViewReleaseBatchSize = 16;

void releaseViewsInBatches(const Ref<MainThreadManager>& mainThreadManager,
                           std::shared_ptr<std::vector<Ref<View>>> views) {
    mainThreadManager->dispatch(nullptr, [mainThreadManager, views = std::move(views)]() mutable {
        VALDI_TRACE("Valdi.releasePooledViews");
        auto batchSize = std::min(views->size(), kPooledViewReleaseBatchSize);
        views->erase(views->end() - static_cast<std::ptrdiff_t>(batchSize), views->end());
        if (!views->empty()) {
            // Each re-dispatch gets its own flush, so every batch runs in a separate main-thread message.
            releaseViewsInBatches(mainThreadManager, std::move(views));
        }
    });
}

} // namespace

void ViewManagerContext::clearViewPools() {
    if (_viewPreloader != nullptr) {
        _viewPreloader->stopPreload();
    }
    // The pools are emptied right away, but the views themselves are destroyed in small batches:
    // on Android each one releases a Java reference through JNI, and destroying a full pool inline
    // during applicationWillPause stalls the main thread long enough to trip the ANR watchdog.
    auto views = std::make_shared<std::vector<Ref<View>>>();
    _globalViewFactories->takePooledViews(*views);
    if (!views->empty()) {
        releaseViewsInBatches(_mainThreadManager, std::move(views));
    }
}

void ViewManagerContext::preloadViews(const StringBox& className, size_t count) {
    if (_viewPreloader != nullptr) {
        _viewPreloader->startPreload(className, count);
    }
}

void ViewManagerContext::setPreloadingPaused(bool preloadingPaused) {
    if (_viewPreloader != nullptr) {
        if (preloadingPaused) {
            _viewPreloader->pausePreload();
        } else {
            _viewPreloader->resumePreload();
        }
    }
}

void ViewManagerContext::setAccessibilityEnabled(const bool accessibilityEnabled) {
    _accessibilityEnabled = accessibilityEnabled;
}

bool ViewManagerContext::getAccessibilityEnabled() const {
    return _accessibilityEnabled;
}

void ViewManagerContext::setApplyManagedChildFramePadding(const bool applyManagedChildFramePadding) {
    _applyManagedChildFramePadding = applyManagedChildFramePadding;
}

bool ViewManagerContext::getApplyManagedChildFramePadding() const {
    return _applyManagedChildFramePadding;
}

void ViewManagerContext::setKeepsViewPoolingOnLayoutCallbacks(const bool keepsViewPoolingOnLayoutCallbacks) {
    _keepsViewPoolingOnLayoutCallbacks.store(keepsViewPoolingOnLayoutCallbacks, std::memory_order_relaxed);
}

bool ViewManagerContext::getKeepsViewPoolingOnLayoutCallbacks() const {
    return _keepsViewPoolingOnLayoutCallbacks.load(std::memory_order_relaxed);
}

void ViewManagerContext::setPreloadingWorkQueue(const Ref<DispatchQueue>& preloadingWorkQueue) {
    if (_viewPreloader != nullptr) {
        _viewPreloader->setWorkQueue(preloadingWorkQueue);
    }
}

ViewPoolsStats ViewManagerContext::getViewPoolsStats() const {
    ViewPoolsStats stats;

    auto viewFactories = _globalViewFactories->copyViewFactories();
    for (const auto& viewFactory : viewFactories) {
        stats[viewFactory->getViewClassName()] = viewFactory->getPoolSize();
    }

    return stats;
}

} // namespace Valdi
