#include "JSBridgeTestFixture.hpp"
#include "JSIntegrationTestsUtils.hpp"
#include "HermesJsiSmoke.inc"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/runtime/JavaScript/JSNativeClassBinder.hpp"
#include "valdi/runtime/JavaScript/JSPromise.hpp"
#include "valdi/runtime/Utils/RefCountableAutoreleasePool.hpp"
#include "valdi_core/cpp/Utils/ByteBuffer.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"
#include "valdi_core/cpp/Utils/ValueMarshaller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <future>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace Valdi;

namespace ValdiTest {
namespace {

// The bridge's factory selects JSI when built with hermes_use_jsi=true. Deliberately
// use only IJavaScriptContext here, including in the compiler-free lean target.
class HermesJsiTest : public JSBridgeTestFixture {};

JSValueRef loadSmoke(JSEntry& entry) {
    // Static storage outlives the context and the prepared bytecode provider.
    auto result = entry.context.evaluatePreCompiled(
        BytesView(nullptr, kHermesJsiSmoke, sizeof(kHermesJsiSmoke)),
        "HermesJsiSmoke.js", entry.exceptionTracker);
    entry.checkException();
    return result;
}

JSValueRef property(JSEntry& entry, const JSValueRef& object, const char* name) {
    auto result = entry.context.getObjectProperty(object.get(), name, entry.exceptionTracker);
    entry.checkException();
    return result;
}

JSValueRef invoke(JSEntry& entry,
                  const JSValueRef& object,
                  const char* method,
                  std::initializer_list<JSValueRef> arguments = {}) {
    JSFunctionCallContext call(entry.context, arguments.size() ? arguments.begin() : nullptr,
                               arguments.size(), entry.exceptionTracker);
    auto result = entry.context.callObjectProperty(object.get(), method, call);
    entry.checkException();
    return result;
}

TEST_P(HermesJsiTest, preparedHbcRunsThroughValdiFactoryWithoutSourceCompilation) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto smoke = loadSmoke(entry);
    ASSERT_TRUE(entry.context.isValueObject(smoke.get()));
    EXPECT_EQ(42, entry.context.valueToInt(invoke(entry, smoke, "answer42").get(), entry.exceptionTracker));
    EXPECT_EQ(42, entry.context.valueToInt(
        invoke(entry, smoke, "sum", {entry.context.newNumber(19), entry.context.newNumber(23)}).get(),
        entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesJsiTest, retainedPrimitivesKeepTheirValuesAcrossGcAndNativeCalls) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    const std::vector<double> numbers = {
        0.0, -0.0, 1.5, -7.75, 4294967297.0, -4294967297.0, 1e300,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    std::vector<JSValueRef> retained;
    for (double number : numbers) {
        auto value = context.newNumber(number);
        if (number == 0 && std::signbit(number)) {
            // The shared newNumber API canonicalizes both zeros. Obtain -0 from
            // JS itself to check that inbound/returned values preserve its sign.
            auto global = context.getGlobalObject(entry.exceptionTracker);
            auto math = context.getObjectProperty(global.get(), "Math", entry.exceptionTracker);
            auto argument = context.newNumber(-0.5);
            JSFunctionCallContext call(context, &argument, 1, entry.exceptionTracker);
            value = context.callObjectProperty(math.get(), "trunc", call);
        }
        retained.emplace_back(JSValueRef::makeRetained(context, value.get()));
    }
    auto yes = context.newBool(true);
    auto no = context.newBool(false);
    auto null = context.newNull();
    auto undefined = context.newUndefined();
    context.garbageCollect();
    auto identity = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [](JSFunctionNativeCallContext& call) {
            return JSValueRef::makeRetained(call.getContext(), call.getParameter(0));
        }), entry.exceptionTracker);
    for (size_t i = 0; i < numbers.size(); ++i) {
        JSFunctionCallContext call(context, &retained[i], 1, entry.exceptionTracker);
        auto returned = context.callObjectAsFunction(identity.get(), call);
        const auto number = context.valueToDouble(returned.get(), entry.exceptionTracker);
        if (std::isnan(numbers[i])) {
            EXPECT_TRUE(std::isnan(number));
        } else {
            EXPECT_EQ(numbers[i], number);
            EXPECT_EQ(std::signbit(numbers[i]), std::signbit(number));
        }
    }
    EXPECT_TRUE(context.valueToBool(yes.get(), entry.exceptionTracker));
    EXPECT_FALSE(context.valueToBool(no.get(), entry.exceptionTracker));
    EXPECT_TRUE(context.isValueNull(null.get()));
    EXPECT_TRUE(context.isValueUndefined(undefined.get()));
    EXPECT_EQ(1, context.valueToInt(retained[4].get(), entry.exceptionTracker));
    EXPECT_EQ(-1, context.valueToInt(retained[5].get(), entry.exceptionTracker));
    EXPECT_EQ(0, context.valueToInt(retained[7].get(), entry.exceptionTracker));
    entry.checkException();
}

class JsiPromiseListener final : public IJavaScriptContextListener {
public:
    std::vector<JSValueRef> reasons;
    JSValueRef symbolicateError(const JSValueRef& error) override { return error; }
    Ref<JSStackTraceProvider> captureCurrentStackTrace() override { return nullptr; }
    MainThreadManager* getMainThreadManager() const override { return nullptr; }
    void onInterrupt(IJavaScriptContext&) override {}
    void onUnhandledRejectedPromise(IJavaScriptContext& context, const JSValue& reason) override {
        reasons.emplace_back(JSValueRef::makeRetained(context, reason));
    }
};

TEST_P(HermesJsiTest, nativeOwnedRejectionsAreSuppressedWithoutCompletionOrJsHooksInLeanHbc) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    JsiPromiseListener listener;
    std::deque<JSValueRef> timers;
    Defer detach([&] { context.setListener(nullptr); });
    context.setListener(&listener);
    auto setTimeout = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext& call) {
            timers.emplace_back(JSValueRef::makeRetained(context, call.getParameter(0)));
            EXPECT_GT(call.getParameterAsInt(1), 0);
            return context.newNumber(static_cast<double>(timers.size()));
        }), entry.exceptionTracker);
    auto global = context.getGlobalObject(entry.exceptionTracker);
    context.setObjectProperty(global.get(), "setTimeout", setTimeout.get(), true, entry.exceptionTracker);
    entry.checkException();
    auto fixtures = invoke(entry, smoke, "makeRejectionFixtures");
    auto owned = property(entry, fixtures, "owned");
    auto pending = property(entry, fixtures, "pending");
    auto reason = property(entry, fixtures, "reason");
    auto unrelated = property(entry, fixtures, "unrelated");
    // Exercise the real constructor integration. No onComplete is registered,
    // including for the pending promise that rejects during microtask draining.
    auto nativeOwned = makeShared<JSPromise>(context, owned.get(), ReferenceInfoBuilder().build(),
                                            nullptr, entry.exceptionTracker);
    auto nativePending = makeShared<JSPromise>(context, pending.get(), ReferenceInfoBuilder().build(),
                                              nullptr, entry.exceptionTracker);
    // Duplicate marks and non-promise values must be harmless. Marking the
    // shared reason must not suppress the unrelated promise (arg1 vs arg2).
    context.markPromiseRejectionHandled(owned.get());
    context.markPromiseRejectionHandled(reason.get());
    context.markPromiseRejectionHandled(context.newNumber(1).get());
    context.markPromiseRejectionHandled(context.newNull().get());
    entry.willExitVM();
    entry.checkException();
    EXPECT_TRUE(listener.reasons.empty());
    // The already rejected owned promise retires at its timer. The pending
    // owned promise skips tracker registration when it rejects during draining.
    ASSERT_EQ(2u, timers.size());
    auto timerEntry = wrapper.makeJsEntry();
    while (!timers.empty()) {
        auto callback = std::move(timers.front());
        timers.pop_front();
        JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
        context.callObjectAsFunction(callback.get(), call);
        timerEntry.checkException();
    }
    ASSERT_EQ(1u, listener.reasons.size());
    EXPECT_TRUE(context.isValueEqual(reason.get(), listener.reasons.front().get()));
    EXPECT_FALSE(context.isValueEqual(unrelated.get(), listener.reasons.front().get()));
    auto hooks = property(timerEntry, fixtures, "hooks");
    for (auto name : {"then", "constructor", "species"}) {
        EXPECT_EQ(0, context.valueToInt(property(timerEntry, hooks, name).get(), timerEntry.exceptionTracker));
    }
    timerEntry.checkException();
}

TEST_P(HermesJsiTest, adoptedNativeRejectionsRetireTimersAndReleasePromiseAndReasonInLeanHbc) {
    MAIN_THREAD_INIT();
    for (bool later : {false, true}) {
        SCOPED_TRACE(later ? "adopt after marking" : "mark existing adoption chain");
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        auto entry = wrapper.makeJsEntry();
        auto& context = entry.context;
        auto smoke = loadSmoke(entry);
        EXPECT_TRUE(context.valueToBool(invoke(entry, smoke, "hasNativePromiseOwnershipBridge").get(),
                                        entry.exceptionTracker));
        JsiPromiseListener listener;
        std::deque<JSValueRef> timers;
        Defer detach([&] { context.setListener(nullptr); });
        context.setListener(&listener);
        auto setTimeout = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
            [&](JSFunctionNativeCallContext& call) {
                timers.emplace_back(JSValueRef::makeRetained(context, call.getParameter(0)));
                return context.newNumber(static_cast<double>(timers.size()));
            }), entry.exceptionTracker);
        auto global = context.getGlobalObject(entry.exceptionTracker);
        context.setObjectProperty(global.get(), "setTimeout", setTimeout.get(), true, entry.exceptionTracker);
        JSValueRef weakOuter, weakMiddle, weakInner, weakReason;
        {
            RefCountableAutoreleasePool pool;
            auto fixtures = invoke(entry, smoke, "makeAdoptionFixtures", {context.newBool(later)});
            auto outer = property(entry, fixtures, "outer");
            auto middle = property(entry, fixtures, "middle");
            auto inner = property(entry, fixtures, "inner");
            auto reason = property(entry, fixtures, "reason");
            weakOuter = context.newWeakRef(outer.get(), entry.exceptionTracker);
            weakMiddle = context.newWeakRef(middle.get(), entry.exceptionTracker);
            weakInner = context.newWeakRef(inner.get(), entry.exceptionTracker);
            weakReason = context.newWeakRef(reason.get(), entry.exceptionTracker);
            auto native = makeShared<JSPromise>(context, outer.get(), ReferenceInfoBuilder().build(),
                                               nullptr, entry.exceptionTracker);
            if (later) {
                invoke(entry, fixtures, "adopt");
            }
            entry.checkException();
            // Drop native ownership and every strong fixture root before GC.
            // Ownership must already have transferred across both weak edges.
        }
        entry.willExitVM();
        entry.checkException();
        // JSPromise disposal asynchronously releases its stashed JS root.
        wrapper.taskScheduler->taskQueue->flushUpToNow();
        auto timerEntry = wrapper.makeJsEntry();
        auto alive = [&](const JSValueRef& weak) {
            auto value = context.derefWeakRef(weak.get(), timerEntry.exceptionTracker);
            timerEntry.checkException();
            return !context.isValueUndefined(value.get());
        };
        ASSERT_EQ(1u, timers.size());
        context.garbageCollect();
        EXPECT_FALSE(alive(weakOuter));
        EXPECT_FALSE(alive(weakMiddle));
        // These are retained exclusively by the builtin's pending timer record.
        EXPECT_TRUE(alive(weakInner));
        EXPECT_TRUE(alive(weakReason));
        {
            auto callback = std::move(timers.front());
            timers.pop_front();
            JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
            context.callObjectAsFunction(callback.get(), call);
            timerEntry.checkException();
        }
        EXPECT_TRUE(listener.reasons.empty());
        context.garbageCollect();
        EXPECT_FALSE(alive(weakInner));
        EXPECT_FALSE(alive(weakReason));
    }
}

class JsiSmokeCounter final : public SimpleRefCountable {
public:
    explicit JsiSmokeCounter(int32_t value) : _value(value) {}
    JsiSmokeCounter(int32_t value, JSFunctionNativeCallContext&) : JsiSmokeCounter(value) {}
    int32_t add(int32_t amount, JSFunctionNativeCallContext&) { return _value += amount; }
    int32_t value(JSFunctionNativeCallContext&) const { return _value; }
private:
    int32_t _value;
};

TEST_P(HermesJsiTest, precompiledNativeConstructorEnforcesIdentityAndDescriptorsInLeanBuilds) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto definition = JSNativeClassBinder<JsiSmokeCounter>("JsiSmokeCounter")
        .bindConstructor<int32_t>()
        .bindMethod<&JsiSmokeCounter::add>("add", false, true, false)
        .bindGetter<&JsiSmokeCounter::value>("value")
        .extractClassDefinition();
    auto constructor = context.newNativeClass(nullptr, definition, entry.exceptionTracker);
    auto native = makeShared<JsiSmokeCounter>(10);
    auto nativeCreated = context.newObjectFromNativeClass(native, constructor.get(), entry.exceptionTracker);
    entry.checkException();
    EXPECT_TRUE(context.valueToBool(
        invoke(entry, smoke, "exerciseNativeClass", {constructor, nativeCreated}).get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesJsiTest, offsetViewsRetainBackingIdentityAndShareWritesAcrossNativeCalls) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto storage = makeShared<ByteBuffer>();
    storage->resize(16);
    auto buffer = context.newArrayBuffer(storage->toBytesView(), entry.exceptionTracker);
    entry.checkException();
    auto views = invoke(entry, smoke, "makeOffsetTypedArray", {buffer});
    auto offset = property(entry, views, "offset");
    auto array = context.valueToTypedArray(offset.get(), entry.exceptionTracker);
    entry.checkException();
    ASSERT_EQ(Uint16Array, array.type);
    ASSERT_EQ(6u, array.length); // byte length, not element count
    ASSERT_EQ(storage->data() + 4, array.data);
    EXPECT_TRUE(context.isValueEqual(buffer.get(), array.arrayBuffer.get()));
    EXPECT_EQ(4, storage->data()[4]); // JS initialized native-owned memory

    bool called = false;
    auto native = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext& call) {
            called = true;
            auto incoming = call.getParameterAsTypedArray(0);
            EXPECT_EQ(Uint16Array, incoming.type);
            EXPECT_EQ(6u, incoming.length);
            EXPECT_EQ(storage->data() + 4, incoming.data);
            EXPECT_TRUE(context.isValueEqual(buffer.get(), incoming.arrayBuffer.get()));
            if (incoming.data != nullptr && incoming.length == 6) {
                static_cast<Byte*>(incoming.data)[0] = 99;
            }
            return JSValueRef::makeRetained(context, call.getParameter(0));
        }), entry.exceptionTracker);
    entry.checkException();
    auto returned = invoke(entry, smoke, "invokeNative", {native, offset});
    EXPECT_TRUE(called);
    EXPECT_TRUE(context.isValueEqual(offset.get(), returned.get()));
    EXPECT_EQ(99, storage->data()[4]);
    auto bytes = property(entry, views, "bytes");
    auto changed = context.getObjectPropertyForIndex(bytes.get(), 4, entry.exceptionTracker);
    EXPECT_EQ(99, context.valueToInt(changed.get(), entry.exceptionTracker));
    storage->data()[5] = 81;
    auto nativeChange = context.getObjectPropertyForIndex(bytes.get(), 5, entry.exceptionTracker);
    EXPECT_EQ(81, context.valueToInt(nativeChange.get(), entry.exceptionTracker));

    auto empty = property(entry, views, "empty");
    auto emptyArray = context.valueToTypedArray(empty.get(), entry.exceptionTracker);
    EXPECT_EQ(Uint8Array, emptyArray.type);
    EXPECT_EQ(0u, emptyArray.length);
    EXPECT_TRUE(context.isValueEqual(buffer.get(), emptyArray.arrayBuffer.get()));
    entry.checkException();
}

TEST_P(HermesJsiTest, typedArrayKindIgnoresMutableConstructorNamesAndToStringTags) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto fixtures = invoke(entry, smoke, "buildOtherFixtures");
    for (const auto& test : {std::make_pair("renamed", Uint16Array), std::make_pair("tagged", Int8Array)}) {
        auto input = property(entry, fixtures, test.first);
        EXPECT_EQ(ValueType::TypedArray, context.getValueType(input.get()));
        auto array = context.valueToTypedArray(input.get(), entry.exceptionTracker);
        entry.checkException();
        EXPECT_EQ(test.second, array.type);
        EXPECT_EQ(test.second == Uint16Array ? 4u : 2u, array.length);
    }
    auto unsupported = property(entry, fixtures, "unsupported");
    EXPECT_EQ(ValueType::TypedArray, context.getValueType(unsupported.get()));
    auto rejectedKind = context.valueToTypedArray(unsupported.get(), entry.exceptionTracker);
    EXPECT_EQ(nullptr, rejectedKind.data);
    EXPECT_EQ(0u, rejectedKind.length);
    EXPECT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.clearError();
    auto fake = property(entry, fixtures, "fake");
    EXPECT_EQ(ValueType::Map, context.getValueType(fake.get()));
    auto rejected = context.valueToTypedArray(fake.get(), entry.exceptionTracker);
    EXPECT_EQ(nullptr, rejected.data);
    EXPECT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.clearError();
    // DataView is intentionally outside Valdi's TypedArrayType contract.
    auto views = invoke(entry, smoke, "makeOffsetTypedArray");
    auto dataView = property(entry, views, "dataView");
    auto rejectedView = context.valueToTypedArray(dataView.get(), entry.exceptionTracker);
    EXPECT_EQ(nullptr, rejectedView.data);
    EXPECT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.clearError();
    EXPECT_EQ(42, context.valueToInt(invoke(entry, smoke, "answer42").get(), entry.exceptionTracker));
    entry.checkException();
}

struct NativeDestruction {
    std::mutex mutex;
    std::condition_variable condition;
    size_t destroyed = 0;
    std::thread::id thread;

    void record() {
        std::lock_guard<std::mutex> lock(mutex);
        ++destroyed;
        thread = std::this_thread::get_id();
        condition.notify_all();
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return destroyed;
    }
    std::thread::id destroyedOn() {
        std::lock_guard<std::mutex> lock(mutex);
        return thread;
    }
};

class TrackedNative final : public SimpleRefCountable {
public:
    explicit TrackedNative(std::shared_ptr<NativeDestruction> state, JSValueRef retained = {},
                           JSPropertyNameRef property = {})
        : _state(std::move(state)), _retained(std::move(retained)), _property(std::move(property)) {}
    ~TrackedNative() override {
        // Exercise releaseValue before publishing destruction to the test.
        _retained = JSValueRef();
        _property = JSPropertyNameRef();
        _state->record();
    }
private:
    std::shared_ptr<NativeDestruction> _state;
    JSValueRef _retained;
    JSPropertyNameRef _property;
};

bool collectUntilDestroyed(IJavaScriptContext& context, NativeDestruction& state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state.count() == 0 && std::chrono::steady_clock::now() < deadline) {
        {
            RefCountableAutoreleasePool pool;
            context.garbageCollect();
        }
        // GC only queues Hermes background finalizers. Give them time to
        // publish, then reenter GC to drain native owners on this JS thread.
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait_until(lock,
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)),
            [&] { return state.destroyed != 0; });
    }
    return state.count() != 0;
}

TEST_P(HermesJsiTest, ownerQueueReleasesCollectedCapturesWithoutAnotherVmEntry) {
    MAIN_THREAD_INIT();
    auto queue = DispatchQueue::createThreaded(STRING_LITERAL("hermes-finalizer-idle"), ThreadQoSClassNormal);
    JSContextWrapper wrapper(getJsBridge(), queue.get());
    for (size_t iteration = 0; iteration < 8; ++iteration) {
        auto destroyed = std::make_shared<NativeDestruction>();
        std::thread::id owner;
        wrapper.withContext([&](IJavaScriptContext& context, JSExceptionTracker& tracker) {
            owner = std::this_thread::get_id();
            {
                auto value = context.newObject(tracker);
                auto capture = makeShared<TrackedNative>(destroyed, value);
                auto function = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
                    [capture](JSFunctionNativeCallContext& call) {
                        (void)capture;
                        return call.getContext().newUndefined();
                    }), tracker);
            }
            context.garbageCollect();
        });
        // No new VM entry or GC while waiting: delayed Hermes finalizers must
        // wake the owner queue themselves, without executing cleanup here.
        std::unique_lock<std::mutex> lock(destroyed->mutex);
        ASSERT_TRUE(destroyed->condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&] { return destroyed->destroyed != 0; }));
        EXPECT_EQ(1u, destroyed->destroyed);
        EXPECT_EQ(owner, destroyed->thread);
    }
}

TEST_P(HermesJsiTest, ownerQueueFinalizerWakeupsAreSafeDuringContextTeardown) {
    MAIN_THREAD_INIT();
    for (size_t iteration = 0; iteration < 8; ++iteration) {
        auto queue = DispatchQueue::createThreaded(STRING_LITERAL("hermes-finalizer-close"), ThreadQoSClassNormal);
        auto destroyed = std::make_shared<NativeDestruction>();
        std::thread::id owner;
        {
            JSContextWrapper wrapper(getJsBridge(), queue.get());
            wrapper.withContext([&](IJavaScriptContext& context, JSExceptionTracker& tracker) {
                owner = std::this_thread::get_id();
                {
                    auto native = makeShared<TrackedNative>(destroyed, context.newObject(tracker));
                    auto object = context.newWrappedObject(native, tracker);
                }
                context.garbageCollect();
            });
            // Teardown can race a background publication or an already queued
            // wakeup; it must still reclaim every owner before returning.
        }
        EXPECT_EQ(1u, destroyed->count());
        EXPECT_EQ(owner, destroyed->destroyedOn());
    }
}

TEST_P(HermesJsiTest, nativePromiseMarksDoNotRootOwnersAndInvalidateWeakHandlesBeforeTeardown) {
    MAIN_THREAD_INIT();
    const auto ownerThread = std::this_thread::get_id();
    for (bool atTeardown : {false, true}) {
        SCOPED_TRACE(atTeardown ? "runtime teardown" : "GC before teardown");
        auto destroyed = std::make_shared<NativeDestruction>();
        {
            JSContextWrapper wrapper(getJsBridge(), nullptr);
            auto entry = wrapper.makeJsEntry();
            auto& context = entry.context;
            auto smoke = loadSmoke(entry);
            JSValueRef weak;
            {
                RefCountableAutoreleasePool pool;
                auto native = makeShared<TrackedNative>(destroyed);
                auto owner = context.newWrappedObject(native, entry.exceptionTracker);
                // Leave live adoption-edge handles for the teardown branch.
                auto promise = invoke(entry, smoke, "makePendingPromise", {owner, context.newBool(atTeardown)});
                context.markPromiseRejectionHandled(promise.get());
                context.markPromiseRejectionHandled(promise.get());
                weak = context.newWeakRef(promise.get(), entry.exceptionTracker);
                if (atTeardown) {
                    // A runtime-owned host function keeps the marked promise
                    // alive; its deferred owner releases a JS root after VM death.
                    auto capture = makeShared<TrackedNative>(std::make_shared<NativeDestruction>(), promise);
                    auto function = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
                        [capture](JSFunctionNativeCallContext& call) {
                            (void)capture;
                            return call.getContext().newUndefined();
                        }), entry.exceptionTracker);
                    auto global = context.getGlobalObject(entry.exceptionTracker);
                    context.setObjectProperty(global.get(), "markedPromiseOwner", function.get(), true,
                                              entry.exceptionTracker);
                    context.setObjectProperty(global.get(), "markedPromiseWeak", weak.get(), true,
                                              entry.exceptionTracker);
                }
                entry.checkException();
            }
            if (atTeardown) {
                context.garbageCollect();
                EXPECT_EQ(0u, destroyed->count());
                auto live = context.derefWeakRef(weak.get(), entry.exceptionTracker);
                EXPECT_FALSE(context.isValueUndefined(live.get()));
            } else {
                EXPECT_TRUE(collectUntilDestroyed(context, *destroyed));
                auto dead = context.derefWeakRef(weak.get(), entry.exceptionTracker);
                EXPECT_TRUE(context.isValueUndefined(dead.get()));
                // Sweep a dead entry again, then leave a live weak handle for
                // teardown even in this branch.
                auto replacement = invoke(entry, smoke, "makePendingPromise");
                context.markPromiseRejectionHandled(replacement.get());
            }
            entry.checkException();
        }
        EXPECT_EQ(1u, destroyed->count());
        EXPECT_EQ(ownerThread, destroyed->destroyedOn());
    }
}

TEST_P(HermesJsiTest, nativeStateCannotBeCopiedAndReleasesItsOwnerAfterCollection) {
    MAIN_THREAD_INIT();
    auto destroyed = std::make_shared<NativeDestruction>();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    {
        RefCountableAutoreleasePool pool;
        auto native = makeShared<TrackedNative>(destroyed);
        auto object = context.newWrappedObject(native, entry.exceptionTracker);
        entry.checkException();
        auto fixtures = invoke(entry, smoke, "buildOtherFixtures", {object});
        auto copied = property(entry, fixtures, "copiedNative");
        EXPECT_EQ(nullptr, context.valueToWrappedObject(copied.get(), entry.exceptionTracker).get());
        EXPECT_EQ(ValueType::Map, context.getValueType(copied.get()));
        auto inherited = property(entry, fixtures, "inheritedNative");
        EXPECT_EQ(nullptr, context.valueToWrappedObject(inherited.get(), entry.exceptionTracker).get());
        EXPECT_EQ(ValueType::Map, context.getValueType(inherited.get()));
        // Unlike a generic unwrapping check, keep only JS ownership across a GC.
        auto* identity = native.get();
        native = nullptr;
        context.garbageCollect();
        EXPECT_EQ(0u, destroyed->count());
        EXPECT_EQ(identity, context.valueToWrappedObject(object.get(), entry.exceptionTracker).get());
        entry.checkException();
    }
    // Releasing both the wrapper and its prototype child must release NativeState
    // before runtime teardown, rather than merely hiding an owner leak at teardown.
    EXPECT_TRUE(collectUntilDestroyed(context, *destroyed));
    EXPECT_EQ(1u, destroyed->count());
    EXPECT_EQ(std::this_thread::get_id(), destroyed->destroyedOn());
}

TEST_P(HermesJsiTest, nativeFunctionCapturedJsValueIsReleasedOnOwnerThreadAfterGcAndTeardown) {
    MAIN_THREAD_INIT();
    const auto ownerThread = std::this_thread::get_id();
    for (bool atTeardown : {false, true}) {
        SCOPED_TRACE(atTeardown ? "runtime teardown" : "GC before teardown");
        auto destroyed = std::make_shared<NativeDestruction>();
        {
            JSContextWrapper wrapper(getJsBridge(), nullptr);
            auto entry = wrapper.makeJsEntry();
            auto& context = entry.context;
            {
                auto retained = context.newObject(entry.exceptionTracker);
                auto capture = makeShared<TrackedNative>(destroyed,
                    JSValueRef::makeRetained(context, retained.get()), context.newPropertyName("capturedProperty"));
                auto function = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
                    [capture](JSFunctionNativeCallContext& call) {
                        (void)capture; // Its lifetime is the behavior under test.
                        return call.getContext().newUndefined();
                    }), entry.exceptionTracker);
                entry.checkException();
                if (atTeardown) {
                    // Force the runtime teardown path, independent of GC timing.
                    auto global = context.getGlobalObject(entry.exceptionTracker);
                    context.setObjectProperty(global.get(), "retainedNative", function.get(), true,
                                              entry.exceptionTracker);
                    // Keep a WeakState alive until runtime destruction too.
                    auto weak = context.newWeakRef(retained.get(), entry.exceptionTracker);
                    context.setObjectProperty(global.get(), "retainedWeak", weak.get(), true,
                                              entry.exceptionTracker);
                    entry.checkException();
                }
            }
            if (atTeardown) {
                context.garbageCollect();
                EXPECT_EQ(0u, destroyed->count());
            } else {
                EXPECT_TRUE(collectUntilDestroyed(context, *destroyed));
                EXPECT_EQ(1u, destroyed->count());
                EXPECT_EQ(ownerThread, destroyed->destroyedOn());
            }
        }
        // Destruction must be complete when the runtime destructor returns.
        EXPECT_EQ(1u, destroyed->count());
        EXPECT_EQ(ownerThread, destroyed->destroyedOn());
    }
}

TEST_P(HermesJsiTest, externalBufferSourceIsReleasedOnOwnerThreadAfterGcAndTeardown) {
    MAIN_THREAD_INIT();
    const auto ownerThread = std::this_thread::get_id();
    for (bool atTeardown : {false, true}) {
        SCOPED_TRACE(atTeardown ? "runtime teardown" : "GC before teardown");
        auto destroyed = std::make_shared<NativeDestruction>();
        Byte bytes[4] = {};
        {
            JSContextWrapper wrapper(getJsBridge(), nullptr);
            auto entry = wrapper.makeJsEntry();
            auto& context = entry.context;
            {
                auto retained = context.newObject(entry.exceptionTracker);
                auto source = makeShared<TrackedNative>(destroyed,
                    JSValueRef::makeRetained(context, retained.get()));
                auto buffer = context.newArrayBuffer(BytesView(source, bytes, sizeof(bytes)), entry.exceptionTracker);
                if (atTeardown) {
                    auto global = context.getGlobalObject(entry.exceptionTracker);
                    context.setObjectProperty(global.get(), "retainedBuffer", buffer.get(), true,
                                              entry.exceptionTracker);
                }
                entry.checkException();
            }
            if (atTeardown) {
                context.garbageCollect();
                EXPECT_EQ(0u, destroyed->count());
            } else {
                EXPECT_TRUE(collectUntilDestroyed(context, *destroyed));
                EXPECT_EQ(1u, destroyed->count());
                EXPECT_EQ(ownerThread, destroyed->destroyedOn());
            }
        }
        EXPECT_EQ(1u, destroyed->count());
        EXPECT_EQ(ownerThread, destroyed->destroyedOn());
    }
}

class NamesVisitor final : public IJavaScriptPropertyNamesVisitor {
public:
    std::vector<StringBox> names;
    bool visitPropertyName(IJavaScriptContext& context, JSValue,
                           const JSPropertyName& name, JSExceptionTracker&) override {
        names.emplace_back(context.propertyNameToString(name));
        return true;
    }
};

TEST_P(HermesJsiTest, descriptorsAndEnumerationDoNotInvokeAccessorsOrIncludeSymbols) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto object = context.newObject(entry.exceptionTracker);
    context.setObjectProperty(object.get(), "hidden", context.newNumber(7).get(), false, entry.exceptionTracker);
    context.setObjectProperty(object.get(), "visible", context.newNumber(9).get(), true, entry.exceptionTracker);
    entry.checkException();
    EXPECT_TRUE(context.valueToBool(invoke(entry, smoke, "inspectDescriptors", {object}).get(),
                                    entry.exceptionTracker));
    auto fixtures = invoke(entry, smoke, "buildOtherFixtures");
    auto properties = property(entry, fixtures, "properties");
    NamesVisitor visitor;
    context.visitObjectPropertyNames(properties.get(), entry.exceptionTracker, visitor);
    entry.checkException();
    // The public visitor follows for-in semantics, including enumerable inherited keys.
    EXPECT_EQ((std::vector<StringBox>{STRING_LITERAL("throwing"), STRING_LITERAL("visible"),
                                    STRING_LITERAL("inherited")}), visitor.names);
    // Reading the same getter must forward the original thrown object.
    context.getObjectProperty(properties.get(), "throwing", entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    auto thrown = entry.exceptionTracker.getExceptionAndClear();
    auto token = property(entry, fixtures, "token");
    EXPECT_TRUE(context.isValueEqual(token.get(), thrown.get()));
}

TEST_P(HermesJsiTest, hostFunctionForwardsThrownObjectIdentityAndRemainsCallable) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto token = context.newObject(entry.exceptionTracker);
    int calls = 0;
    auto native = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext& call) {
            ++calls;
            call.getExceptionTracker().storeException(call.getParameter(0));
            return context.newUndefined();
        }), entry.exceptionTracker);
    entry.checkException();
    EXPECT_TRUE(context.valueToBool(invoke(entry, smoke, "catchNative", {native, token}).get(),
                                    entry.exceptionTracker));
    EXPECT_EQ(1, calls);
    JSValueRef arguments[] = {token};
    JSFunctionCallContext call(context, arguments, 1, entry.exceptionTracker);
    context.callObjectAsFunction(native.get(), call);
    ASSERT_FALSE(entry.exceptionTracker);
    auto thrown = entry.exceptionTracker.getExceptionAndClear();
    EXPECT_TRUE(context.isValueEqual(token.get(), thrown.get()));
    EXPECT_EQ(2, calls);
    EXPECT_EQ(42, context.valueToInt(invoke(entry, smoke, "answer42").get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesJsiTest, rawHbcPureJsLoopCanBeTerminatedFromAnotherThreadInLeanBuilds) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto smoke = loadSmoke(entry);
    auto loop = property(entry, smoke, "loop");
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto ready = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext&) {
            entered.set_value();
            return context.newUndefined();
        }), entry.exceptionTracker);
    entry.checkException();
    bool observedEntry = false;
    std::thread terminator([&]() {
        observedEntry = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        // Ensure the readiness host call returns: the interrupt must be handled
        // inside the allocation-free JS loop, not at a native call boundary.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        context.requestExecutionTermination();
    });
    Defer join([&]() { if (terminator.joinable()) terminator.join(); });
    JSValueRef arguments[] = {ready};
    JSFunctionCallContext call(context, arguments, 1, entry.exceptionTracker);
    context.callObjectAsFunction(loop.get(), call);
    terminator.join();
    EXPECT_TRUE(observedEntry);
    EXPECT_TRUE(context.executionTerminationRequested());
    EXPECT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.clearError();
}

INSTANTIATE_TEST_SUITE_P(HermesJsiTests,
                         HermesJsiTest,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());

} // namespace
} // namespace ValdiTest
