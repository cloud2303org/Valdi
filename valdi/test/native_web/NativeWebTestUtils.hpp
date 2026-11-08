#pragma once

#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/runtime/JavaScript/JSValueRefHolder.hpp"
#include "valdi/runtime/NativeWeb/NativeWeb.hpp"
#include "valdi/test/integration/JSBridgeTestFixture.hpp"
#include "valdi/test/integration/JSIntegrationTestsUtils.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

namespace ValdiTest {

inline Valdi::JSValueRef loadAndInstallNativeWeb(Valdi::JavaScriptModuleFactory& factory,
                                                Valdi::IJavaScriptContext& cx,
                                                Valdi::JSExceptionTracker& ex) {
    auto exports = factory.loadModule(cx, Valdi::ReferenceInfoBuilder(), ex);
    if (ex) {
        auto installer = cx.getObjectProperty(exports.get(), "installNativeWeb", ex);
        if (ex) {
            Valdi::JSFunctionCallContext call(cx, nullptr, 0, ex);
            cx.callObjectAsFunction(installer.get(), call);
        }
    }
    return exports;
}

// JSContextWrapper has no Runtime bootstrap. Supply the native scheduling and
// disposable callback primitive used by JavaScriptRuntime::runtimeScheduleWorkItem
// and its queueMicrotask binding, so rejection tracking and stream tee use the
// same native scheduling services as a fully bootstrapped runtime.
// No Promise or Web implementation is replaced.
class NativeWebScope {
  public:
    explicit NativeWebScope(JSContextWrapper& wrapper) : _wrapper(wrapper), _timers(std::make_shared<Timers>()) {
        std::weak_ptr<Timers> weak = _timers;
        _wrapper.withContext([weak](auto& cx, auto& ex) {
            auto set = cx.newFunction(
                Valdi::makeShared<Valdi::JSFunctionWithCallable>(
                    Valdi::ReferenceInfoBuilder(),
                    [weak](Valdi::JSFunctionNativeCallContext& call) {
                        auto timers = weak.lock();
                        auto& cx = call.getContext();
                        if (timers == nullptr || timers->disposed) {
                            return cx.newNumber(0);
                        }
                        auto callback = Valdi::JSValueRefHolder::makeRetainedCallback(
                            cx, call.getParameter(0), Valdi::ReferenceInfoBuilder(), call.getExceptionTracker());
                        int32_t delay = call.getParameterSize() > 1 ? call.getParameterAsInt(1) : 0;
                        if (!call.getExceptionTracker()) {
                            return cx.newUndefined();
                        }
                        int32_t id = timers->nextId++;
                        timers->callbacks.emplace(id, std::move(callback));
                        cx.getTaskScheduler()->dispatchOnJsThread(
                            Valdi::Ref<Valdi::Context>(), Valdi::JavaScriptTaskScheduleTypeAlwaysAsync,
                            static_cast<uint32_t>(std::max(0, delay)),
                            [weak, id](Valdi::JavaScriptEntryParameters& entry) {
                                auto timers = weak.lock();
                                if (timers == nullptr || timers->disposed) {
                                    return;
                                }
                                auto found = timers->callbacks.find(id);
                                if (found == timers->callbacks.end()) {
                                    return;
                                }
                                auto callback = std::move(found->second);
                                timers->callbacks.erase(found);
                                auto function = callback->getJsValue(entry.jsContext, entry.exceptionTracker);
                                if (!entry.exceptionTracker) {
                                    return;
                                }
                                Valdi::JSFunctionCallContext call(entry.jsContext, nullptr, 0, entry.exceptionTracker);
                                entry.jsContext.callObjectAsFunction(function, call);
                            });
                        return cx.newNumber(id);
                    }),
                ex);
            auto clear = cx.newFunction(Valdi::makeShared<Valdi::JSFunctionWithCallable>(
                                            Valdi::ReferenceInfoBuilder(),
                                            [weak](Valdi::JSFunctionNativeCallContext& call) {
                                                auto timers = weak.lock();
                                                if (timers != nullptr && !timers->disposed) {
                                                    timers->callbacks.erase(call.getParameterAsInt(0));
                                                }
                                                return call.getContext().newUndefined();
                                            }),
                                        ex);
            auto global = cx.getGlobalObject(ex);
            cx.setObjectProperty(global.get(), "setTimeout", set.get(), ex);
            cx.setObjectProperty(global.get(), "clearTimeout", clear.get(), ex);
            auto queueMicrotask = cx.newFunction(
                Valdi::makeShared<Valdi::JSFunctionWithCallable>(
                    Valdi::ReferenceInfoBuilder(), [](Valdi::JSFunctionNativeCallContext& call) {
                        auto& cx = call.getContext();
                        cx.enqueueMicrotask(call.getParameter(0), call.getExceptionTracker());
                        return cx.newUndefined();
                    }),
                ex);
            cx.setObjectProperty(global.get(), "queueMicrotask", queueMicrotask.get(), ex);
        });
    }
    ~NativeWebScope() {
        _wrapper.withContext([this](auto& cx, auto&) {
            _timers->disposed = true;
            _timers->callbacks.clear();
            Valdi::NativeWeb::shutdown(cx);
            Valdi::NativeWeb::release(cx);
        });
    }

  private:
    struct Timers {
        int32_t nextId = 1;
        bool disposed = false;
        Valdi::FlatMap<int32_t, Valdi::Shared<Valdi::JSValueRefHolder>> callbacks;
    };
    JSContextWrapper& _wrapper;
    std::shared_ptr<Timers> _timers;
};

inline bool waitForWeb(JSContextWrapper& wrapper, const char* expression,
                       std::chrono::seconds timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        wrapper.taskScheduler->taskQueue->flushUpToNow();
        if (wrapper.evaluateScript(expression).toBool()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace ValdiTest
