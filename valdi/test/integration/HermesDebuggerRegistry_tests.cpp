#include "JSBridgeTestFixture.hpp"
#include "JSIntegrationTestsUtils.hpp"
#include "valdi/hermes/HermesDebuggerRegistry.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"

#include <hermes/hermes.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace Valdi;

namespace ValdiTest {
namespace {

class HermesDebuggerRegistryTest : public JSBridgeTestFixture {};

class BlockingDebuggerScheduler final : public TaskSchedulerImpl {
public:
    explicit BlockingDebuggerScheduler(const Ref<Context>& context)
        : TaskSchedulerImpl(nullptr, context), _released(_release.get_future().share()) {}

    void dispatchOnJsThread(JsThreadDispatchReason reason,
                            JavaScriptTaskScheduleType scheduleType,
                            uint32_t delayMs,
                            JavaScriptThreadTask&& function) override {
        if (blockNext.exchange(false)) {
            entered.set_value();
            _released.wait();
        }
        TaskSchedulerImpl::dispatchOnJsThread(reason, scheduleType, delayMs, std::move(function));
    }

    void unblock() {
        if (!_unblocked.exchange(true)) {
            _release.set_value();
        }
    }

    std::atomic<bool> blockNext{false};
    std::promise<void> entered;

private:
    std::promise<void> _release;
    std::shared_future<void> _released;
    std::atomic<bool> _unblocked{false};
};

TEST_P(HermesDebuggerRegistryTest, removalWaitsForCommandToFinishEnqueuingBeforeDestroyingDependencies) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto scheduler = makeShared<BlockingDebuggerScheduler>(wrapper.valdiContext);
    scheduler->jsContext = wrapper.taskScheduler->jsContext;
    auto registry = makeShared<Hermes::HermesDebuggerRegistry>();
    std::shared_ptr<facebook::hermes::HermesRuntime> runtime = facebook::hermes::makeHermesRuntime();
    auto id = registry->append(runtime, hermes::vm::RuntimeConfig::Builder().build(), scheduler.get());
    ASSERT_NE(Hermes::kRuntimeIdUndefined, id);
    scheduler->taskQueue->flushUpToNow();

    auto entered = scheduler->entered.get_future();
    scheduler->blockNext = true;
    std::thread command([&] {
        auto message = Value().setMapValue("id", Value(1)).setMapValue("method", Value("Runtime.enable"));
        registry->submitMessageToHermes(message, id);
    });
    Defer joinCommand([&] {
        scheduler->unblock();
        command.join();
    });
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(5)));

    std::promise<void> startingRemoval;
    auto started = startingRemoval.get_future();
    std::promise<void> finishedRemoval;
    auto finished = finishedRemoval.get_future();
    bool waitedForCommand = false;
    bool observedRemoval = false;
    std::thread unblock([&] {
        started.wait();
        // Observe removal's registry mutation before checking that it waits.
        // Signaling before the call alone would only test scheduling latency.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (registry->getRuntimeIds().empty()) {
                observedRemoval = true;
                break;
            }
            finished.wait_for(std::chrono::milliseconds(1));
        }
        waitedForCommand = observedRemoval &&
            finished.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
        scheduler->unblock();
    });
    startingRemoval.set_value();
    // remove must quiesce the command while its scheduler enqueue is blocked.
    // This happens on the same thread that created and owns the runtime.
    registry->remove(id);
    finishedRemoval.set_value();
    unblock.join();
    EXPECT_TRUE(observedRemoval);
    EXPECT_TRUE(waitedForCommand);
    EXPECT_TRUE(registry->getRuntimeIds().empty());
    scheduler->taskQueue->flushUpToNow();
    // A late message must be discarded without using the removed dependencies.
    registry->submitMessageToHermes(Value(), id);
    runtime.reset();
}

INSTANTIATE_TEST_SUITE_P(HermesDebuggerRegistryTests,
                         HermesDebuggerRegistryTest,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());

} // namespace
} // namespace ValdiTest
