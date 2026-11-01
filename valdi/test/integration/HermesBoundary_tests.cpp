#include "JSBridgeTestFixture.hpp"
#include "JSIntegrationTestsUtils.hpp"
#include "valdi/runtime/JavaScript/JSClassDefinition.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/runtime/JavaScript/JSPromise.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"
#include "valdi_core/cpp/Utils/ValueMarshaller.hpp"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

using namespace Valdi;

namespace ValdiTest {
namespace {

class HermesBoundaryTest : public JSBridgeTestFixture {};

JSValueRef evaluate(JSEntry& entry, const std::string& source) {
    auto result = entry.context.evaluate(source, "HermesBoundary_tests.js", entry.exceptionTracker);
    entry.checkException();
    return result;
}

void publish(JSEntry& entry, const char* name, const JSValueRef& value) {
    auto global = entry.context.getGlobalObject(entry.exceptionTracker);
    entry.context.setObjectProperty(global.get(), name, value.get(), entry.exceptionTracker);
    entry.checkException();
}

// Check the entire argument list, including the pointer contract for empty calls.
JSValueRef checkArguments(JSFunctionNativeCallContext& call) {
    auto count = call.getParameterSize();
    EXPECT_EQ(count == 0, call.getParameters() == nullptr);
    for (size_t i = 0; i < count; i++) {
        EXPECT_EQ(static_cast<int32_t>(i), call.getParameterAsInt(i));
    }
    return call.getContext().newNumber(static_cast<double>(count));
}

class BoundaryObject final : public SimpleRefCountable {
public:
    size_t constructorArguments = 0;
};

Ref<RefCountable> constructBoundary(RefCountable*, JSFunctionNativeCallContext& call) noexcept {
    checkArguments(call);
    auto result = makeShared<BoundaryObject>();
    result->constructorArguments = call.getParameterSize();
    return result;
}

JSValueRef boundaryMember(RefCountable* opaque, JSFunctionNativeCallContext& call) noexcept {
    EXPECT_NE(nullptr, dynamic_cast<BoundaryObject*>(opaque));
    return checkArguments(call);
}

JSClassDefinition boundaryDefinition() {
    JSClassDefinition definition(STRING_LITERAL("BoundaryObject"), &constructBoundary);
    definition.appendMethod(STRING_LITERAL("count"), &boundaryMember);
    definition.appendClassMethod(STRING_LITERAL("count"), &boundaryMember);
    return definition;
}

TEST_P(HermesBoundaryTest, rejectsBigIntTypedArraysWithoutAborting) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    for (const char* source : {"new BigInt64Array(2)", "new BigUint64Array(2)"}) {
        auto value = evaluate(entry, source);
        EXPECT_EQ(ValueType::TypedArray, context.getValueType(value.get()));
        auto rejected = context.valueToTypedArray(value.get(), entry.exceptionTracker);
        EXPECT_EQ(nullptr, rejected.data);
        EXPECT_EQ(0u, rejected.length);
        ASSERT_FALSE(entry.exceptionTracker);
        auto error = entry.exceptionTracker.getExceptionAndClear();
        publish(entry, "boundaryError", error);
        EXPECT_TRUE(context.valueToBool(
            evaluate(entry, "String(boundaryError).includes('Unsupported TypedArray type')").get(),
            entry.exceptionTracker));
    }
    // A tracked rejection must leave the engine usable for supported arrays.
    auto supported = evaluate(entry, "new Uint8Array([1, 2, 3])");
    auto array = context.valueToTypedArray(supported.get(), entry.exceptionTracker);
    EXPECT_EQ(3u, array.length);
    entry.checkException();
}

TEST_P(HermesBoundaryTest, usesECMAScriptBooleanAndNumericCoercion) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    for (const char* source : {"NaN", "0", "-0", "0n", "''", "null", "undefined"}) {
        EXPECT_FALSE(context.valueToBool(evaluate(entry, source).get(), entry.exceptionTracker)) << source;
    }
    for (const char* source : {"Infinity", "-Infinity", "1n", "({})", "'0'"}) {
        EXPECT_TRUE(context.valueToBool(evaluate(entry, source).get(), entry.exceptionTracker)) << source;
    }
    for (const char* source : {"undefined", "'not a number'", "NaN"}) {
        EXPECT_TRUE(std::isnan(context.valueToDouble(evaluate(entry, source).get(), entry.exceptionTracker)));
    }
    EXPECT_EQ(0.0, context.valueToDouble(evaluate(entry, "null").get(), entry.exceptionTracker));
    EXPECT_EQ(0.0, context.valueToDouble(evaluate(entry, "'  '").get(), entry.exceptionTracker));
    EXPECT_EQ(16.0, context.valueToDouble(evaluate(entry, "'0x10'").get(), entry.exceptionTracker));
    EXPECT_EQ(42.0, context.valueToDouble(evaluate(entry, "({valueOf() { return 42; }})").get(),
                                         entry.exceptionTracker));
    EXPECT_TRUE(std::isinf(context.valueToDouble(evaluate(entry, "Infinity").get(), entry.exceptionTracker)));
    for (const char* source : {"NaN", "Infinity", "-Infinity", "undefined"}) {
        EXPECT_EQ(0, context.valueToInt(evaluate(entry, source).get(), entry.exceptionTracker)) << source;
    }
    EXPECT_EQ(1, context.valueToInt(evaluate(entry, "4294967297").get(), entry.exceptionTracker));
    EXPECT_EQ(-1, context.valueToInt(evaluate(entry, "4294967295").get(), entry.exceptionTracker));
    EXPECT_EQ(-1, context.valueToInt(evaluate(entry, "-1.9").get(), entry.exceptionTracker));
    EXPECT_EQ(7, context.valueToInt(evaluate(entry, "({valueOf() { return 4294967303; }})").get(),
                                    entry.exceptionTracker));
    auto collect = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [](JSFunctionNativeCallContext& call) {
            call.getContext().garbageCollect();
            return call.getContext().newUndefined();
        }), entry.exceptionTracker);
    publish(entry, "boundaryCollect", collect);
    // Conversion can re-enter native code and collect while Hermes is coercing the rooted object.
    auto collectingValue = evaluate(entry, "({valueOf() { boundaryCollect(); return 4294967297; }})");
    EXPECT_EQ(4294967297.0, context.valueToDouble(collectingValue.get(), entry.exceptionTracker));
    EXPECT_EQ(1, context.valueToInt(collectingValue.get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesBoundaryTest, tracksThrowingNumericCoercionAndRetainsThrownIdentity) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto value = evaluate(entry, "globalThis.coercionError = {}; "
                                 "({valueOf() { throw coercionError; }})");
    // ToBoolean must not invoke valueOf.
    EXPECT_TRUE(context.valueToBool(value.get(), entry.exceptionTracker));
    entry.checkException();
    for (bool integer : {false, true}) {
        if (integer) {
            context.valueToInt(value.get(), entry.exceptionTracker);
        } else {
            context.valueToDouble(value.get(), entry.exceptionTracker);
        }
        ASSERT_FALSE(entry.exceptionTracker);
        auto error = entry.exceptionTracker.getExceptionAndClear();
        publish(entry, "caughtCoercionError", error);
        EXPECT_TRUE(context.valueToBool(evaluate(entry, "caughtCoercionError === coercionError").get(),
                                         entry.exceptionTracker));
    }
    // ToNumber/ToInt32 also reject Symbol and BigInt rather than silently returning zero.
    for (const char* source : {"Symbol('boundary')", "1n"}) {
        auto input = evaluate(entry, source);
        context.valueToDouble(input.get(), entry.exceptionTracker);
        ASSERT_FALSE(entry.exceptionTracker);
        entry.exceptionTracker.clearError();
        context.valueToInt(input.get(), entry.exceptionTracker);
        ASSERT_FALSE(entry.exceptionTracker);
        entry.exceptionTracker.clearError();
    }
    entry.checkException();
}

TEST_P(HermesBoundaryTest, inheritedObjectsDoNotAcquireNativeIdentity) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto opaque = makeShared<BoundaryObject>();
    auto wrapped = context.newWrappedObject(opaque, entry.exceptionTracker);
    auto cls = context.newNativeClass(opaque, boundaryDefinition(), entry.exceptionTracker);
    auto instance = context.newObjectFromNativeClass(opaque, cls.get(), entry.exceptionTracker);
    publish(entry, "wrapped", wrapped);
    publish(entry, "BoundaryObject", cls);
    publish(entry, "instance", instance);
    EXPECT_EQ(opaque.get(), context.valueToWrappedObject(wrapped.get(), entry.exceptionTracker).get());
    // Native-class instance data must still unwrap to the original opaque object.
    EXPECT_EQ(opaque.get(), context.valueToWrappedObject(instance.get(), entry.exceptionTracker).get());
    for (const char* source : {"wrapped", "instance"}) {
        EXPECT_EQ(ValueType::ValdiObject, context.getValueType(evaluate(entry, source).get()));
        auto inherited = evaluate(entry, std::string("Object.create(") + source + ")");
        EXPECT_EQ(nullptr, context.valueToWrappedObject(inherited.get(), entry.exceptionTracker).get());
        EXPECT_EQ(ValueType::Map, context.getValueType(inherited.get()));
    }
    auto otherClass = context.newNativeClass(opaque, boundaryDefinition(), entry.exceptionTracker);
    auto otherInstance = context.newObjectFromNativeClass(opaque, otherClass.get(), entry.exceptionTracker);
    publish(entry, "otherInstance", otherInstance);
    EXPECT_TRUE(context.valueToBool(evaluate(entry, "(() => { try { "
        "instance.count.call(otherInstance); } catch(e) { return e instanceof TypeError; } "
        "return false; })()").get(), entry.exceptionTracker));
    // The receiver and new.target guards must continue requiring their own matching native state.
    EXPECT_TRUE(context.valueToBool(evaluate(entry, "(() => { try { "
        "instance.count.call(Object.create(instance)); } catch(e) { return e instanceof TypeError; } "
        "return false; })()").get(), entry.exceptionTracker));
    EXPECT_TRUE(context.valueToBool(evaluate(entry, "(() => { try { "
        "class Derived extends BoundaryObject {} new Derived(); } "
        "catch(e) { return e instanceof TypeError; } return false; })()").get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesBoundaryTest, allTrampolinesSupportZeroAndLargeArgumentLists) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto opaque = makeShared<BoundaryObject>();
    auto cls = context.newNativeClass(opaque, boundaryDefinition(), entry.exceptionTracker);
    auto function = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [](JSFunctionNativeCallContext& call) { return checkArguments(call); }), entry.exceptionTracker);
    publish(entry, "BoundaryObject", cls);
    publish(entry, "boundaryFunction", function);
    evaluate(entry, "globalThis.instance = new BoundaryObject();");
    for (size_t count : {size_t(0), size_t(4096)}) {
        evaluate(entry, "globalThis.boundaryArgs = Array.from({length: " + std::to_string(count) +
                        "}, (_, i) => i)");
        for (const char* source : {"boundaryFunction(...boundaryArgs)",
                                  "instance.count(...boundaryArgs)",
                                  "BoundaryObject.count(...boundaryArgs)"}) {
            EXPECT_EQ(static_cast<int32_t>(count), context.valueToInt(evaluate(entry, source).get(),
                                                                      entry.exceptionTracker));
        }
        auto constructed = evaluate(entry, "new BoundaryObject(...boundaryArgs)");
        auto native = castOrNull<BoundaryObject>(context.valueToWrappedObject(constructed.get(), entry.exceptionTracker));
        ASSERT_NE(nullptr, native);
        EXPECT_EQ(count, native->constructorArguments);
    }
    entry.checkException();
}

class BoundaryPromiseListener final : public IJavaScriptContextListener {
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

TEST_P(HermesBoundaryTest, rejectionTimerForwardsReasonIdentityToListener) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    BoundaryPromiseListener listener;
    std::deque<JSValueRef> timers;
    // Detach before listener destruction, including fatal assertion exits.
    Defer detach([&]() { context.setListener(nullptr); });
    context.setListener(&listener);
    auto setTimeout = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext& call) {
            timers.emplace_back(JSValueRef::makeRetained(context, call.getParameter(0)));
            EXPECT_GT(call.getParameterAsInt(1), 0);
            return context.newNumber(static_cast<double>(timers.size()));
        }), entry.exceptionTracker);
    publish(entry, "setTimeout", setTimeout);
    // Exercise both an immediate rejection and a rejection raised while draining jobs.
    evaluate(entry, "globalThis.rejectionReason = {marker: 73}; "
                    "globalThis.rejectedPromise = Promise.reject(rejectionReason); "
                    "Promise.resolve().then(() => { throw rejectionReason; });");
    entry.willExitVM();
    entry.checkException();
    EXPECT_TRUE(listener.reasons.empty());
    ASSERT_EQ(2u, timers.size());
    {
        auto timerEntry = wrapper.makeJsEntry();
        while (!timers.empty()) {
            auto callback = std::move(timers.front());
            timers.pop_front();
            JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
            context.callObjectAsFunction(callback.get(), call);
            timerEntry.checkException();
        }
        ASSERT_EQ(2u, listener.reasons.size());
        for (const auto& reason : listener.reasons) {
            publish(timerEntry, "reportedReason", reason);
            EXPECT_TRUE(context.valueToBool(evaluate(timerEntry,
                "reportedReason === rejectionReason && reportedReason !== rejectedPromise").get(),
                timerEntry.exceptionTracker));
        }
        context.setListener(nullptr);
        // No listener is also supported: drive another real rejection timer.
        evaluate(timerEntry, "Promise.reject(rejectionReason)");
        ASSERT_EQ(1u, timers.size());
        JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
        context.callObjectAsFunction(timers.front().get(), call);
        timers.clear();
        timerEntry.checkException();
        EXPECT_EQ(2u, listener.reasons.size());
    }
}

TEST_P(HermesBoundaryTest, markingNativePromiseDoesNotExecuteJavaScript) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto promise = evaluate(entry, R"JS(
        globalThis.markReads = 0;
        globalThis.markJobs = 0;
        var promise = new Promise(() => {});
        Object.defineProperty(promise, 'then', {get() { ++markReads; throw Error('then'); }});
        Object.defineProperty(promise, 'constructor', {get() { ++markReads; throw Error('constructor'); }});
        Promise.resolve().then(() => { ++markJobs; });
        Object.defineProperty(Promise, Symbol.species, {get() { ++markReads; throw Error('species'); }});
        promise;
    )JS");
    // Constructing the native wrapper marks ownership even when onComplete is never called.
    auto native = makeShared<JSPromise>(context, promise.get(), ReferenceInfoBuilder().build(),
                                        nullptr, entry.exceptionTracker);
    context.markPromiseRejectionHandled(promise.get()); // Idempotent.
    entry.checkException();
    EXPECT_EQ(0, context.valueToInt(evaluate(entry, "markReads + markJobs").get(), entry.exceptionTracker));
    // Even a Proxy's property traps must not run during the mark operation itself.
    auto proxy = evaluate(entry, "new Proxy({}, {get() { ++markReads; throw Error('get'); }, "
                                 "has() { ++markReads; throw Error('has'); }})");
    context.markPromiseRejectionHandled(proxy.get());
    context.markPromiseRejectionHandled(context.newUndefined().get());
    EXPECT_EQ(0, context.valueToInt(evaluate(entry, "markReads + markJobs").get(), entry.exceptionTracker));
    entry.willExitVM();
    entry.checkException();
    auto nextEntry = wrapper.makeJsEntry();
    EXPECT_EQ(1, context.valueToInt(evaluate(nextEntry, "markJobs").get(), nextEntry.exceptionTracker));
    EXPECT_EQ(0, context.valueToInt(evaluate(nextEntry, "markReads").get(), nextEntry.exceptionTracker));
}

TEST_P(HermesBoundaryTest, ownershipGraphHandlesCyclesDuplicatesAndProxyTargetsWithoutJavaScript) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto parent = evaluate(entry, R"JS(
        globalThis.savedOwnershipHook = __valdiNativePromiseOwnership;
        globalThis.graphReads = 0;
        var traps = {
          get() { ++graphReads; throw Error('get'); },
          has() { ++graphReads; throw Error('has'); },
          getOwnPropertyDescriptor() { ++graphReads; throw Error('descriptor'); },
          getPrototypeOf() { ++graphReads; throw Error('prototype'); }
        };
        globalThis.graphA = new Proxy({}, traps);
        globalThis.graphB = new Proxy({}, traps);
        globalThis.graphC = new Proxy({}, traps);
        savedOwnershipHook(0, graphA, graphB);
        savedOwnershipHook(0, graphB, graphC);
        savedOwnershipHook(0, graphC, graphA);
        savedOwnershipHook(0, graphA, graphB);
        graphA;
    )JS");
    context.markPromiseRejectionHandled(parent.get());
    context.markPromiseRejectionHandled(parent.get());
    EXPECT_TRUE(context.valueToBool(evaluate(entry,
        "savedOwnershipHook(1, graphA) && savedOwnershipHook(1, graphB) && "
        "savedOwnershipHook(1, graphC) && graphReads === 0").get(), entry.exceptionTracker));
    // The builtin must use its captured hook even if the public rendezvous name is replaced.
    auto adopted = evaluate(entry,
        "globalThis.__valdiNativePromiseOwnership = () => { throw Error('replaced hook'); }; "
        "globalThis.capturedInner = new Promise(() => {}); "
        "new Promise(resolve => resolve(capturedInner))");
    context.markPromiseRejectionHandled(adopted.get());
    EXPECT_TRUE(context.valueToBool(evaluate(entry,
        "savedOwnershipHook(1, capturedInner)").get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesBoundaryTest, rejectionTimersSuppressOnlyNativeOwnedPromiseIdentity) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    BoundaryPromiseListener listener;
    std::deque<JSValueRef> timers;
    Defer detach([&]() { context.setListener(nullptr); });
    context.setListener(&listener);
    auto setTimeout = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [&](JSFunctionNativeCallContext& call) {
            timers.emplace_back(JSValueRef::makeRetained(context, call.getParameter(0)));
            EXPECT_GT(call.getParameterAsInt(1), 0);
            return context.newNumber(static_cast<double>(timers.size()));
        }), entry.exceptionTracker);
    publish(entry, "setTimeout", setTimeout);
    auto immediate = evaluate(entry, "globalThis.ownedReason = {}; Promise.reject(ownedReason)");
    auto nativeImmediate = makeShared<JSPromise>(context, immediate.get(), ReferenceInfoBuilder().build(),
                                                 nullptr, entry.exceptionTracker);
    auto pending = evaluate(entry, "globalThis.rejectOwned = undefined; "
                                   "new Promise((_, reject) => { rejectOwned = reject; })");
    auto nativePending = makeShared<JSPromise>(context, pending.get(), ReferenceInfoBuilder().build(),
                                               nullptr, entry.exceptionTracker);
    // The unrelated rejection has the SAME reason; filtering must compare promises, not reasons.
    evaluate(entry, "Promise.reject(ownedReason); Promise.resolve().then(() => rejectOwned(ownedReason));");
    entry.willExitVM();
    entry.checkException();
    ASSERT_EQ(2u, timers.size());
    EXPECT_TRUE(listener.reasons.empty());
    auto timerEntry = wrapper.makeJsEntry();
    context.garbageCollect(); // Weak wrapper roots and promise identities must survive a collection.
    while (!timers.empty()) {
        auto callback = std::move(timers.front());
        timers.pop_front();
        JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
        context.callObjectAsFunction(callback.get(), call);
        timerEntry.checkException();
    }
    ASSERT_EQ(1u, listener.reasons.size());
    publish(timerEntry, "reportedReason", listener.reasons.front());
    EXPECT_TRUE(context.valueToBool(evaluate(timerEntry, "reportedReason === ownedReason").get(),
                                    timerEntry.exceptionTracker));
}

TEST_P(HermesBoundaryTest, nativeOwnershipIsWeakAndWrappersAreReleasedBeforeRuntimeTeardown) {
    MAIN_THREAD_INIT();
    // Recreate contexts to exercise teardown with both cleared and live weak targets.
    for (size_t iteration = 0; iteration < 3; ++iteration) {
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        JSValueRef weak;
        {
            auto entry = wrapper.makeJsEntry();
            // A pending promise avoids the rejection tracker's own strong reference to a rejection.
            auto promise = evaluate(entry, "new Promise(() => {})");
            weak = entry.context.newWeakRef(promise.get(), entry.exceptionTracker);
            auto native = makeShared<JSPromise>(entry.context, promise.get(), ReferenceInfoBuilder().build(),
                                                nullptr, entry.exceptionTracker);
            entry.checkException();
        }
        // JSPromise destruction asynchronously releases its stashed JS root.
        wrapper.taskScheduler->taskQueue->flushUpToNow();
        {
            auto entry = wrapper.makeJsEntry();
            entry.context.garbageCollect();
            auto target = entry.context.derefWeakRef(weak.get(), entry.exceptionTracker);
            entry.checkException();
            EXPECT_TRUE(entry.context.isValueUndefined(target.get()));
            // Marking another promise prunes the dead wrapper and leaves a live wrapper for teardown.
            auto live = evaluate(entry, "globalThis.liveOwned = new Promise(() => {})");
            entry.context.markPromiseRejectionHandled(live.get());
            entry.context.garbageCollect();
            auto liveWeak = entry.context.newWeakRef(live.get(), entry.exceptionTracker);
            EXPECT_TRUE(entry.context.isValueEqual(live.get(),
                entry.context.derefWeakRef(liveWeak.get(), entry.exceptionTracker).get()));
            entry.checkException();
        }
        weak = JSValueRef();
    }
}

// Exercise the patched builtin as well as the adapter: a pending-only weak test misses tracker roots.
TEST_P(HermesBoundaryTest, droppedNativePromisesRetireRejectedAndAdoptedTrackerRecords) {
    MAIN_THREAD_INIT();
    for (const char* scenario : {"rejected", "adopted", "lateAdopted", "adoptedPending"}) {
        SCOPED_TRACE(scenario);
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        BoundaryPromiseListener listener;
        std::deque<JSValueRef> timers;
        auto entry = wrapper.makeJsEntry();
        auto& context = entry.context;
        Defer detach([&]() { context.setListener(nullptr); });
        context.setListener(&listener);
        auto setTimeout = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
            [&](JSFunctionNativeCallContext& call) {
                timers.emplace_back(JSValueRef::makeRetained(context, call.getParameter(0)));
                return context.newNumber(static_cast<double>(timers.size()));
            }), entry.exceptionTracker);
        publish(entry, "setTimeout", setTimeout);
        evaluate(entry, "globalThis.collectionReason = {}; globalThis.innerPromise = undefined; "
                        "globalThis.outerPromise = undefined; globalThis.resolveOuter = undefined; "
                        "globalThis.rejectInner = undefined;");
        const bool late = std::string(scenario) == "lateAdopted";
        const bool adoptedPending = std::string(scenario) == "adoptedPending";
        {
            const char* source;
            if (late) {
                source = "outerPromise = new Promise(resolve => { resolveOuter = resolve; }); outerPromise";
            } else if (adoptedPending) {
                source = "innerPromise = new Promise((_, reject) => { rejectInner = reject; }); "
                         "outerPromise = new Promise(resolve => resolve(innerPromise)); outerPromise";
            } else if (std::string(scenario) == "adopted") {
                source = "innerPromise = Promise.reject(collectionReason); "
                         "outerPromise = new Promise(resolve => resolve(innerPromise)); outerPromise";
            } else {
                source = "innerPromise = Promise.reject(collectionReason); outerPromise = innerPromise; outerPromise";
            }
            auto promise = evaluate(entry, source);
            auto native = makeShared<JSPromise>(context, promise.get(), ReferenceInfoBuilder().build(),
                                                nullptr, entry.exceptionTracker);
            entry.checkException();
            // Never call onComplete; drop the native wrapper BEFORE settling a pending outer.
        }
        entry.willExitVM();
        entry.checkException();
        wrapper.taskScheduler->taskQueue->flushUpToNow();
        auto timerEntry = wrapper.makeJsEntry();
        if (late) {
            evaluate(timerEntry, "innerPromise = Promise.reject(collectionReason); "
                                 "resolveOuter(innerPromise); resolveOuter = undefined;");
        } else if (adoptedPending) {
            evaluate(timerEntry, "rejectInner(collectionReason); rejectInner = undefined;");
        }
        auto outer = evaluate(timerEntry, "outerPromise");
        auto inner = evaluate(timerEntry, "innerPromise");
        auto reason = evaluate(timerEntry, "collectionReason");
        auto weakOuter = context.newWeakRef(outer.get(), timerEntry.exceptionTracker);
        auto weakInner = context.newWeakRef(inner.get(), timerEntry.exceptionTracker);
        auto weakReason = context.newWeakRef(reason.get(), timerEntry.exceptionTracker);
        outer = JSValueRef();
        inner = JSValueRef();
        reason = JSValueRef();
        evaluate(timerEntry, "outerPromise = innerPromise = collectionReason = resolveOuter = undefined;");
        // The outer adoption wrapper can disappear while the tracker still retains the inner.
        context.garbageCollect();
        EXPECT_EQ(adoptedPending ? 0u : 1u, timers.size());
        while (!timers.empty()) {
            auto callback = std::move(timers.front());
            timers.pop_front();
            JSFunctionCallContext call(context, nullptr, 0, timerEntry.exceptionTracker);
            context.callObjectAsFunction(callback.get(), call);
            timerEntry.checkException();
            context.callObjectAsFunction(callback.get(), call);
            timerEntry.checkException();
        }
        EXPECT_TRUE(listener.reasons.empty());
        listener.reasons.clear(); // A failed report assertion must not itself retain the reason below.
        context.garbageCollect();
        EXPECT_TRUE(context.isValueUndefined(context.derefWeakRef(weakOuter.get(), timerEntry.exceptionTracker).get()));
        EXPECT_TRUE(context.isValueUndefined(context.derefWeakRef(weakInner.get(), timerEntry.exceptionTracker).get()));
        EXPECT_TRUE(context.isValueUndefined(context.derefWeakRef(weakReason.get(), timerEntry.exceptionTracker).get()));
        timerEntry.checkException();
    }
}

INSTANTIATE_TEST_SUITE_P(HermesBoundaryTests,
                         HermesBoundaryTest,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());

} // namespace
} // namespace ValdiTest
