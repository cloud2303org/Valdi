//
//  HermesDebuggerRegistry.cpp
//  valdi-hermes
//
//  Created by Edward Lee on 4/8/24.
//

#include "valdi/hermes/HermesDebuggerRegistry.hpp"

#include "valdi/runtime/JavaScript/JavaScriptFunctionCallContext.hpp"
#include "valdi/runtime/JavaScript/JavaScriptTaskScheduler.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"
#include "valdi_core/cpp/Utils/LoggerUtils.hpp"
#include "valdi_core/cpp/Utils/ValueUtils.hpp"

#include <hermes/hermes.h>
#if defined(HERMES_API)
#include <hermes/cdp/CDPAgent.h>
#include <hermes/cdp/CDPDebugAPI.h>
#endif

namespace Valdi::Hermes {

#if defined(HERMES_API)
/// Hermes V1 runs debugger work through tasks that need the runtime while JS isn't running; run them on the JS thread.
static facebook::hermes::debugger::EnqueueRuntimeTaskFunc makeEnqueueRuntimeTask(
    const std::shared_ptr<facebook::hermes::HermesRuntime>& jsiRuntime, JavaScriptTaskScheduler* taskScheduler) {
    std::weak_ptr<facebook::hermes::HermesRuntime> weakRuntime = jsiRuntime;
    Ref<JavaScriptTaskScheduler> scheduler(taskScheduler);
    return [weakRuntime, scheduler](facebook::hermes::debugger::RuntimeTask task) {
        constexpr auto reason = JsThreadDispatchReason::DebuggerTickle;
        scheduler->dispatchOnJsThreadAsync(reason, [weakRuntime, task = std::move(task)](JavaScriptEntryParameters&) {
            if (auto runtime = weakRuntime.lock()) {
                task(*runtime);
            }
        });
    };
}
#endif

HermesDebuggerRegistry::HermesDebuggerRegistry() = default;
HermesDebuggerRegistry::~HermesDebuggerRegistry() = default;

HermesRuntimeId HermesDebuggerRegistry::append(const std::shared_ptr<facebook::hermes::HermesRuntime>& jsiRuntime,
                                               const hermes::vm::RuntimeConfig& runtimeConfig,
                                               JavaScriptTaskScheduler* taskScheduler,
                                               HermesRuntimeId parentRuntimeId) {
#if defined(HERMES_API)
    jsiRuntime->registerForProfiling();
    std::lock_guard<Mutex> lock(_mutex);
    auto sequence = ++_runtimeIdSequence;
    HermesRuntimeId runtimeId = StringCache::getGlobal().makeString(std::to_string(sequence));
    std::shared_ptr<facebook::hermes::cdp::CDPDebugAPI> debugAPI =
        facebook::hermes::cdp::CDPDebugAPI::create(*jsiRuntime);
    std::shared_ptr<facebook::hermes::cdp::CDPAgent> hermesAgent = facebook::hermes::cdp::CDPAgent::create(
        static_cast<int32_t>(sequence),
        *debugAPI,
        makeEnqueueRuntimeTask(jsiRuntime, taskScheduler),
        [this, runtimeId](const std::string& message) { this->onHermesMessage(message, runtimeId); });
    auto& entry = _entries.emplace_back();
    entry.runtimeId = runtimeId;
    entry.commandGate = std::make_shared<CommandGate>();
    entry.commandGate->jsiRuntime = jsiRuntime;
    entry.commandGate->debugAPI = std::move(debugAPI);
    entry.commandGate->hermesAgent = std::move(hermesAgent);
    entry.parentRuntimeId = parentRuntimeId;

    if (parentRuntimeId != kRuntimeIdUndefined) {
        for (auto* listener : _listeners) {
            if (listener->getConnectedRuntimeId() == parentRuntimeId) {
                listener->onWorkerAttached(runtimeId);
            }
        }
    }
    return entry.runtimeId;
#else
    return kRuntimeIdUndefined;
#endif
}

void HermesDebuggerRegistry::remove(const HermesRuntimeId& runtimeId) {
    std::shared_ptr<CommandGate> commandGate;
    {
        std::lock_guard<Mutex> lock(_mutex);
        auto it = _entries.begin();
        while (it != _entries.end()) {
            if (it->runtimeId == runtimeId) {
                commandGate = std::move(it->commandGate);
                _entries.erase(it);
                break;
            } else {
                it++;
            }
        }
    }
    if (commandGate != nullptr) {
        // Erase first, then wait outside _mutex: a command may be enqueueing work,
        // and Hermes callbacks may need _mutex. No new lookup can find this gate.
        std::lock_guard<RecursiveMutex> lock(commandGate->mutex);
        // Default scheduler dispatch can run inline on JS. A recursive gate
        // permits nested commands, but context teardown from an active command
        // cannot safely destroy Hermes or defer its destruction past teardown.
        SC_ABORT_UNLESS(commandGate->activeCommands == 0,
                        "Cannot remove a Hermes runtime from an active debugger command");
        commandGate->closed = true;
        // CDPAgent's destructor enqueues disposal through the still-live API.
        // CDPDebugAPI then drains pending interrupts on this owner JS thread.
        // Escaped gates must retain none of these resources after remove returns.
        commandGate->hermesAgent.reset();
        commandGate->debugAPI.reset();
        commandGate->jsiRuntime.reset();
    }
}

void HermesDebuggerRegistry::registerListener(HermesRuntimeMessageListener* listener) {
    std::lock_guard<Mutex> lock(_mutex);
    _listeners.emplace_back(listener);
}

void HermesDebuggerRegistry::unregisterListener(HermesRuntimeMessageListener* listener) {
    std::lock_guard<Mutex> lock(_mutex);
    auto it = _listeners.begin();
    while (it != _listeners.end()) {
        if (*it == listener) {
            it = _listeners.erase(it);
        } else {
            it++;
        }
    }
}

void HermesDebuggerRegistry::onHermesMessage(const std::string& messageStr, HermesRuntimeId runtimeId) {
    auto messageJson = jsonToValue(messageStr);
    if (!messageJson.success()) {
        return;
    }
    auto message = messageJson.value();

    std::lock_guard<Mutex> lock(_mutex);
    bool isWorker = false;
    HermesRuntimeId workerId = kRuntimeIdUndefined;
    for (auto& entry : _entries) {
        if (entry.runtimeId == runtimeId) {
            if (entry.isWorker()) {
                runtimeId = entry.parentRuntimeId;
                workerId = entry.runtimeId;
                isWorker = true;
            }
            break;
        }
    }

    for (auto* listener : _listeners) {
        if (listener->getConnectedRuntimeId() == runtimeId) {
            if (isWorker) {
                listener->handleHermesWorkerMessage(message, workerId);
            } else {
                listener->handleHermesMessage(message);
            }
        }
    }
}

std::vector<HermesRuntimeId> HermesDebuggerRegistry::getRuntimeIds() {
    std::lock_guard<Mutex> lock(_mutex);
    std::vector<HermesRuntimeId> runtimeIds;
    for (const auto& entry : _entries) {
        if (!entry.isWorker()) {
            runtimeIds.push_back(entry.runtimeId);
        }
    }
    return runtimeIds;
}

std::vector<HermesRuntimeId> HermesDebuggerRegistry::getWorkers(HermesRuntimeId runtimeId) {
    std::lock_guard<Mutex> lock(_mutex);
    std::vector<HermesRuntimeId> runtimeIds;
    for (const auto& entry : _entries) {
        if (entry.parentRuntimeId == runtimeId) {
            runtimeIds.push_back(entry.runtimeId);
        }
    }
    return runtimeIds;
}

void HermesDebuggerRegistry::submitMessageToHermes(const Value& message, HermesRuntimeId runtimeId) {
#if defined(HERMES_API)
    std::shared_ptr<CommandGate> commandGate;
    {
        std::lock_guard<Mutex> lock(_mutex);
        for (const auto& entry : _entries) {
            if (entry.runtimeId == runtimeId) {
                commandGate = entry.commandGate;
                break;
            }
        }
    }
    if (commandGate != nullptr) {
        auto json = valueToJsonString(message);
        std::lock_guard<RecursiveMutex> lock(commandGate->mutex);
        if (commandGate->closed) {
            return;
        }
        ++commandGate->activeCommands;
        Defer finishCommand([&] { --commandGate->activeCommands; });
        commandGate->hermesAgent->handleCommand(std::move(json));
    }
#endif
    return;
}

} // namespace Valdi::Hermes
