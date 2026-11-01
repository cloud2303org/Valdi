#include "JSBridgeTestFixture.hpp"
#include "JSIntegrationTestsUtils.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>

using namespace Valdi;

namespace ValdiTest {
namespace {

// Host-only, full Hermes builds: both factories evaluate these exact source bytes.
// No HBC, application bootstrap, compilation, property-name interning, context
// creation, explicit GC, or logging is included in a timed sample. Natural GC is.
// Run the dedicated target with -c opt, changing only hermes_use_jsi between runs.
// The factory's define is private to :valdi_hermes, so identify the adapter in the
// command/log filename rather than guessing it from this translation unit.
constexpr int32_t kIterations = 4096;
constexpr size_t kWarmups = 2;
constexpr size_t kSamples = 7;
using Clock = std::chrono::steady_clock;

constexpr const char* kSource = R"JS((function () {
    'use strict';
    return {
        increment: function (x) { return x + 1; },
        callbacks: function (callback, count) {
            var sum = 0;
            for (var i = 0; i < count; ++i) { sum += callback(i); }
            return sum;
        }
    };
})())JS";

class HermesAdapterBenchmark : public JSBridgeTestFixture {};

// Work returns an externally observed, deterministic checksum. Check correctness
// after stopping the timer, including every warmup; never assert a time threshold.
// One op means one logical iteration of the named workload, NOT one API call.
// A five-second scheduling budget bounds normal host runs; it cannot preempt a
// single stuck VM/API call. An exhausted budget skips the run, never publishes a
// partial-sample median. Use a runner timeout as the outer hard limit.
template<typename Work>
bool measure(JSEntry& entry,
             Clock::time_point deadline,
             const char* name,
             double expectedChecksum,
             Work&& work) {
    std::array<double, kSamples> samples{};
    double checksum = 0;
    for (size_t batch = 0; batch < kWarmups + kSamples; ++batch) {
        if (Clock::now() >= deadline) {
            std::printf("HERMES_ADAPTER_BENCH incomplete workload=%s budget_seconds=5\n", name);
            return false;
        }
        const auto start = Clock::now();
        checksum = work();
        const auto end = Clock::now();
        entry.checkException();
        EXPECT_EQ(expectedChecksum, checksum) << name << " batch=" << batch;
        if (checksum != expectedChecksum) {
            return false;
        }
        if (batch >= kWarmups) {
            samples[batch - kWarmups] =
                std::chrono::duration<double, std::nano>(end - start).count() / kIterations;
        }
    }
    std::sort(samples.begin(), samples.end());
    std::printf("HERMES_ADAPTER_BENCH workload=%s ops_per_sample=%d warmups=%zu samples=%zu "
                "median_ns_per_op=%.2f min_ns_per_op=%.2f max_ns_per_op=%.2f checksum=%.0f\n",
                name, kIterations, kWarmups, kSamples, samples[kSamples / 2], samples.front(),
                samples.back(), checksum);
    return true;
}

// test_integration also globs this directory. Keep timing opt-in even there;
// the dedicated benchmark command supplies --gtest_also_run_disabled_tests.
TEST_P(HermesAdapterBenchmark, DISABLED_publicContextHostWorkloads) {
    MAIN_THREAD_INIT();
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto& context = entry.context;
    auto& tracker = entry.exceptionTracker;

    // All handles are RAII-owned and die before entry/wrapper. No global callback
    // roots, captured context references, or accumulating per-iteration handles.
    auto fixture = context.evaluate(kSource, "HermesAdapterBenchmark.js", tracker);
    entry.checkException();
    auto increment = context.getObjectProperty(fixture.get(), "increment", tracker);
    entry.checkException();
    auto callbacks = context.getObjectProperty(fixture.get(), "callbacks", tracker);
    entry.checkException();
    auto object = context.newObject(tracker);
    auto key = context.newPropertyName("value");
    auto initial = context.newNumber(37);
    context.setObjectProperty(object.get(), key.get(), initial.get(), tracker);
    entry.checkException();

    std::array<JSValueRef, 256> propertyValues;
    for (size_t i = 0; i < propertyValues.size(); ++i) {
        propertyValues[i] = context.newNumber(static_cast<int32_t>(i));
    }
    const std::array<std::string, 2> strings = {"adapter-value-0", "adapter-value-1"};
    auto native = context.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [](JSFunctionNativeCallContext& call) {
            const auto value = call.getParameterAsInt(0);
            return call.getContext().newNumber(value + 1);
        }), tracker);
    entry.checkException();
    const std::array<JSValueRef, 2> callbackArguments = {native, context.newNumber(kIterations)};

    // This is one VM entry for all batches. No microtasks/asynchronous work is
    // scheduled. Per-operation JSValueRef destruction IS part of the timed work.
    const auto deadline = Clock::now() + std::chrono::seconds(5);

    // One op: create a nonintegral number + convert to double + consume it.
    // Multiply by four so the checksum is an exactly representable integer.
    if (!measure(entry, deadline, "number_create_to_double", 33550336, [&]() {
        double sum = 0;
        for (int32_t i = 0; i < kIterations; ++i) {
            auto value = context.newNumber(static_cast<double>(i) + 0.25);
            sum += context.valueToDouble(value.get(), tracker) * 4;
        }
        return sum;
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }

    // One op: create one of two short UTF-8 strings + convert to StringBox.
    // Includes returned-string destruction and a tiny checksum; repeated keys
    // intentionally represent the intern/cache-friendly end of string traffic.
    if (!measure(entry, deadline, "short_string_create_to_utf8", 260096, [&]() {
        double sum = 0;
        for (int32_t i = 0; i < kIterations; ++i) {
            auto value = context.newStringUTF8(strings[i & 1], tracker);
            auto converted = context.valueToString(value.get(), tracker);
            auto view = converted.toStringView();
            sum += view.size() + (view.empty() ? 0 : static_cast<unsigned char>(view.back()));
        }
        return sum;
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }

    // One op: get an existing property using a pre-interned key + convert to int.
    if (!measure(entry, deadline, "property_get_to_int", 151552, [&]() {
        double sum = 0;
        for (int32_t i = 0; i < kIterations; ++i) {
            auto value = context.getObjectProperty(object.get(), key.get(), tracker);
            sum += context.valueToInt(value.get(), tracker);
        }
        return sum;
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }

    // One op: set a varying pre-created value + get it + convert to int. Report
    // as a round trip, not as two independent property operations or pure set.
    if (!measure(entry, deadline, "property_set_get_to_int", 522240, [&]() {
        double sum = 0;
        for (int32_t i = 0; i < kIterations; ++i) {
            context.setObjectProperty(object.get(), key.get(), propertyValues[i & 255].get(), tracker);
            auto value = context.getObjectProperty(object.get(), key.get(), tracker);
            sum += context.valueToInt(value.get(), tracker);
        }
        return sum;
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }

    // One op: marshal one integer + call a cached JS function + convert result.
    if (!measure(entry, deadline, "native_to_js_increment", 8390656, [&]() {
        double sum = 0;
        for (int32_t i = 0; i < kIterations; ++i) {
            auto argument = context.newNumber(i);
            JSFunctionCallContext call(context, &argument, 1, tracker);
            auto value = context.callObjectAsFunction(increment.get(), call);
            sum += context.valueToInt(value.get(), tracker);
        }
        return sum;
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }

    // Exactly kIterations JS->native callbacks per batch. Includes JS loop/sum,
    // callback parameter/result conversion, plus one amortized native->JS call
    // and final result conversion. This is not an isolated callback-only cost.
    if (!measure(entry, deadline, "js_to_native_increment_batched", 8390656, [&]() {
        JSFunctionCallContext call(context, callbackArguments.data(), callbackArguments.size(), tracker);
        auto value = context.callObjectAsFunction(callbacks.get(), call);
        return context.valueToDouble(value.get(), tracker);
    })) {
        if (HasFailure()) {
            return;
        }
        GTEST_SKIP() << "Benchmark incomplete: five-second scheduling budget exhausted";
    }
}

INSTANTIATE_TEST_SUITE_P(HermesAdapterBenchmarks,
                         HermesAdapterBenchmark,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());

} // namespace
} // namespace ValdiTest
