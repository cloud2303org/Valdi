//
//  HermesJavaScriptContextFactory.cpp
//  valdi-hermes
//
//  Created by Simon Corsin on 9/27/23.
//

#include "valdi/hermes/HermesJavaScriptContextFactory.hpp"
#if defined(VALDI_HERMES_USE_JSI)
#include "valdi/hermes/HermesJsiJavaScriptContext.hpp"
#else
#include "valdi/hermes/HermesJavaScriptContext.hpp"
#endif
#include "valdi/runtime/JavaScript/JavaScriptHeapDumpBuilder.hpp"

#ifdef HERMES_ENABLE_DEBUGGER
#include "valdi/hermes/HermesDebuggerServer.hpp"
#endif

namespace Valdi::Hermes {

HermesJavaScriptContextFactory::HermesJavaScriptContextFactory() = default;

const char* HermesJavaScriptContextFactory::getName() {
    return "Hermes";
}

bool HermesJavaScriptContextFactory::requiresDedicatedThread() {
    return true;
}

Ref<IJavaScriptContext> HermesJavaScriptContextFactory::createJsContext(JavaScriptTaskScheduler* taskScheduler,
                                                                        ILogger& logger) {
#if defined(VALDI_HERMES_USE_JSI)
    return makeShared<HermesJsiJavaScriptContext>(taskScheduler, logger);
#else
    return makeShared<HermesJavaScriptContext>(taskScheduler, logger);
#endif
}

void HermesJavaScriptContextFactory::startJsDebuggerServer([[maybe_unused]] ILogger& logger) {
#ifdef HERMES_ENABLE_DEBUGGER
    if (_debuggerServer == nullptr) {
        _debuggerServer = HermesDebuggerServer::getInstance(logger);
    }
    if (_debuggerServer != nullptr) {
        _debuggerServer->start();
    }
#else
    return;
#endif
}

void HermesJavaScriptContextFactory::stopJsDebuggerServer() {
#ifdef HERMES_ENABLE_DEBUGGER
    if (_debuggerServer != nullptr) {
        _debuggerServer->stop();
    }
#else
    return;
#endif
}

BytesView HermesJavaScriptContextFactory::dumpHeap(std::span<IJavaScriptContext*> jsContexts,
                                                   JSExceptionTracker& exceptionTracker) {
    if constexpr (shouldEnableJsHeapDump()) {
        Valdi::JavaScriptHeapDumpBuilder heapDumpBuilder;

        for (auto* jsContext : jsContexts) {
#if defined(VALDI_HERMES_USE_JSI)
            auto heapDump = dynamic_cast<HermesJsiJavaScriptContext*>(jsContext)->dumpHeap(exceptionTracker);
#else
            auto heapDump = dynamic_cast<HermesJavaScriptContext*>(jsContext)->dumpHeap(exceptionTracker);
#endif
            if (!exceptionTracker) {
                return BytesView();
            }

            heapDumpBuilder.appendDump(heapDump.data(), heapDump.size(), exceptionTracker);
            if (!exceptionTracker) {
                return BytesView();
            }
        }

        return heapDumpBuilder.build();
    } else {
        exceptionTracker.onError("Heap dump support not enabled");
        return BytesView();
    }
}

} // namespace Valdi::Hermes
