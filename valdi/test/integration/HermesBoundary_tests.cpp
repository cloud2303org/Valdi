#include "JSBridgeTestFixture.hpp"
#include "JSIntegrationTestsUtils.hpp"
#include "valdi/runtime/JavaScript/JSClassDefinition.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"

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

INSTANTIATE_TEST_SUITE_P(HermesBoundaryTests,
                         HermesBoundaryTest,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());

} // namespace
} // namespace ValdiTest
