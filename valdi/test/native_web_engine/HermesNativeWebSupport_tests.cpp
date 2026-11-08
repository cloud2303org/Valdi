#include "valdi/runtime/Interfaces/IJavaScriptNativeWebSupport.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/test/integration/JSBridgeTestFixture.hpp"
#include "valdi/test/integration/JSIntegrationTestsUtils.hpp"

#include <atomic>
#include <array>
#include <limits>
#include <vector>

namespace ValdiTest {
using namespace Valdi;
namespace {

class HermesNativeWebSupportTests : public JSBridgeTestFixture {};

class WebConstructorCapture final : public SimpleRefCountable {
public:
    WebConstructorCapture(JSValueRef value, std::atomic<int>& destroyed)
        : value(std::move(value)), _destroyed(destroyed) {}
    ~WebConstructorCapture() override {
        value = JSValueRef();
        ++_destroyed;
    }
    JSValueRef value;
private:
    std::atomic<int>& _destroyed;
};

// Deliberately contains no JS or Rust context: GC may release this owner.
class WebBufferOwner final : public SimpleRefCountable {
public:
    explicit WebBufferOwner(std::atomic<int>& destroyed) : _destroyed(destroyed) {
        bytes[0] = 17;
        bytes[63] = 93;
    }
    ~WebBufferOwner() override {
        ++_destroyed;
    }
    std::array<uint8_t, 64> bytes{};
private:
    std::atomic<int>& _destroyed;
};

JSValueRef evaluateWeb(JSEntry& entry, const std::string& source) {
    auto result = entry.context.evaluate(source, "HermesNativeWebSupport_tests.js", entry.exceptionTracker);
    entry.checkException();
    return result;
}

void publishWeb(JSEntry& entry, const char* name, const JSValueRef& value) {
    auto global = entry.context.getGlobalObject(entry.exceptionTracker);
    entry.context.setObjectProperty(global.get(), name, value.get(), entry.exceptionTracker);
    entry.checkException();
}

void installWeakRefCollector(JSContextWrapper& wrapper, bool nestedEntry) {
    wrapper.withContext([&](auto& cx, auto& ex) {
        auto collector = cx.newFunction(makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
            [nestedEntry](JSFunctionNativeCallContext& call) {
                auto& context = call.getContext();
                if (nestedEntry) {
                    context.willEnterVM();
                    JSExceptionTracker nestedException(context);
                    context.willExitVM(nestedException);
                    if (!nestedException) {
                        call.getExceptionTracker().storeException(nestedException.getExceptionAndClear());
                        return context.newUndefined();
                    }
                }
                context.garbageCollect();
                return context.newUndefined();
            }), ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "collectWeakRefTarget", collector.get(), ex);
    });
}

void collectAfterWeakRefJob(JSContextWrapper& wrapper) {
    // Do not dereference between collections: a successful deref starts a new
    // kept-object lifetime and would hide a missing end-of-job cleanup.
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
}

TEST_P(HermesNativeWebSupportTests, WeakRefTargetCollectsAfterOutermostJob) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        const target = {};
        target.self = target;
        globalThis.jobWeakRef = new WeakRef(target);
        return true;
    })())JS").toBool());
    collectAfterWeakRefJob(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("jobWeakRef.deref() === undefined").toBool());
}

TEST_P(HermesNativeWebSupportTests, WeakRefTargetSurvivesNestedEntriesAndEntireMicrotaskCheckpoint) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    installWeakRefCollector(wrapper, true);
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        globalThis.jobWeakRef = new WeakRef({});
        globalThis.weakRefStages = '';
        collectWeakRefTarget();
        if (jobWeakRef.deref() !== undefined) weakRefStages += 'sync;';
        Promise.resolve().then(() => {
            collectWeakRefTarget();
            if (jobWeakRef.deref() !== undefined) weakRefStages += 'first;';
            return Promise.resolve().then(() => {
                collectWeakRefTarget();
                if (jobWeakRef.deref() !== undefined) weakRefStages += 'second;';
            });
        });
        return true;
    })())JS").toBool());
    EXPECT_TRUE(wrapper.evaluateScript("weakRefStages === 'sync;first;second;'").toBool());
    collectAfterWeakRefJob(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("jobWeakRef.deref() === undefined").toBool());
}

TEST_P(HermesNativeWebSupportTests, WeakRefTargetSurvivesInterruptedMicrotaskDrainUntilResumedCheckpointCompletes) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    installWeakRefCollector(wrapper, false);
    {
        auto entry = wrapper.makeJsEntry();
        evaluateWeb(entry, "globalThis.jobWeakRef = new WeakRef({}); globalThis.resumedWeakRefAlive = false;");
        auto throwingJob = evaluateWeb(entry, "() => { throw new Error('stop this drain'); }");
        auto remainingJob = evaluateWeb(entry, R"JS(() => {
            collectWeakRefTarget();
            resumedWeakRefAlive = jobWeakRef.deref() !== undefined;
        })JS");
        entry.context.enqueueMicrotask(throwingJob.get(), entry.exceptionTracker);
        entry.context.enqueueMicrotask(remainingJob.get(), entry.exceptionTracker);
        entry.checkException();
        entry.willExitVM();
        ASSERT_FALSE(entry.exceptionTracker);
        entry.exceptionTracker.getExceptionAndClear();
    }
    // The first exceptional job has been popped; a new outer entry resumes
    // the remaining queue before ending the checkpoint.
    wrapper.withContext([](auto&, auto&) {});
    EXPECT_TRUE(wrapper.evaluateScript("resumedWeakRefAlive").toBool());
    collectAfterWeakRefJob(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("jobWeakRef.deref() === undefined").toBool());
}

TEST_P(HermesNativeWebSupportTests, ExposesOptionalNativeSupport) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    ASSERT_NE(nullptr, dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context));
}

TEST_P(HermesNativeWebSupportTests, ConstructorReturnsCallbackObjectAndPassesSubclassNewTarget) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto callback = makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
        [](JSFunctionNativeCallContext& call) {
            auto& cx = call.getContext();
            auto& ex = call.getExceptionTracker();
            auto result = cx.newObject(ex);
            cx.setObjectProperty(result.get(), "target", call.getThisValue(), ex);
            cx.setObjectProperty(result.get(), "arg", call.getParameter(0), ex);
            return result;
        });
    auto ctor = support->newWebConstructor(callback, STRING_LITERAL("NativeWeb"), 1, entry.exceptionTracker);
    publishWeb(entry, "NativeWeb", ctor);
    auto result = evaluateWeb(entry, R"JS((() => {
        // The NSH binding installs its own prototype, then locks the descriptor.
        const prototype = {constructor: NativeWeb};
        Object.defineProperty(NativeWeb, 'prototype', {
            value: prototype, writable: false, enumerable: false, configurable: false
        });
        let callsRejected = false;
        try { NativeWeb(); } catch (e) { callsRejected = e instanceof TypeError; }
        class Derived extends NativeWeb {}
        const direct = new NativeWeb(12), derived = new Derived(23);
        return callsRejected && NativeWeb.prototype === prototype &&
            Object.getPrototypeOf(Derived.prototype) === prototype &&
            NativeWeb.name === 'NativeWeb' && NativeWeb.length === 1 &&
            direct.target === NativeWeb && direct.arg === 12 &&
            derived.target === Derived && derived.arg === 23;
    })())JS");
    EXPECT_TRUE(entry.context.valueToBool(result.get(), entry.exceptionTracker));
}

TEST_P(HermesNativeWebSupportTests, ClassifiesSymbolsAndBigInts) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    EXPECT_EQ(NativeWebValueKind::Symbol, support->getWebValueKind(evaluateWeb(entry, "Symbol('x')").get()));
    EXPECT_EQ(NativeWebValueKind::BigInt, support->getWebValueKind(evaluateWeb(entry, "123n").get()));
    EXPECT_EQ(NativeWebValueKind::Other, support->getWebValueKind(evaluateWeb(entry, "({})").get()));
}

TEST_P(HermesNativeWebSupportTests, ConstructorSurvivesGcAndReleasesCapturesAtShutdown) {
    MAIN_THREAD_INIT();
    std::atomic<int> destroyed{0};
    {
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        auto entry = wrapper.makeJsEntry();
        auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
        ASSERT_NE(nullptr, support);
        auto capture = makeShared<WebConstructorCapture>(
            entry.context.newStringUTF8("captured", entry.exceptionTracker), destroyed);
        auto callback = makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(),
            [capture](JSFunctionNativeCallContext& call) {
                auto& cx = call.getContext();
                auto& ex = call.getExceptionTracker();
                cx.garbageCollect();
                if (call.getParameterSize() != 0) {
                    ex.storeException(call.getParameter(0));
                    return cx.newUndefined();
                }
                auto object = cx.newObject(ex);
                cx.setObjectProperty(object.get(), "target", call.getThisValue(), ex);
                cx.setObjectProperty(object.get(), "value", capture->value.get(), ex);
                return object;
            });
        auto constructor = support->newWebConstructor(callback, STRING_LITERAL("RootedWeb"), 0,
                                                      entry.exceptionTracker);
        publishWeb(entry, "RootedWeb", constructor);
        callback = nullptr;
        capture = nullptr;
        constructor = JSValueRef();
        entry.context.garbageCollect();
        EXPECT_EQ(0, destroyed.load());
        EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry, R"JS((() => {
            const value = new RootedWeb();
            const thrown = {identity: true};
            let same = false;
            try { new RootedWeb(thrown); } catch (e) { same = e === thrown; }
            return value.target === RootedWeb && value.value === 'captured' && same;
        })())JS").get(), entry.exceptionTracker));
        entry.checkException();
    }
    EXPECT_EQ(1, destroyed.load());
}

TEST_P(HermesNativeWebSupportTests, ReadsVmBufferMetadataWithoutUserProperties) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    const char* names[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
                           "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array",
                           "BigUint64Array"};
    const size_t sizes[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
    auto buffer = evaluateWeb(entry, "globalThis.webBuffer = new ArrayBuffer(64); webBuffer");
    for (int32_t kind = -1; kind <= 10; ++kind) {
        SCOPED_TRACE(kind);
        auto source = kind < 0 ? "new DataView(webBuffer, 8, 3)" :
            std::string("new ") + names[kind] + "(webBuffer, 8, 3)";
        auto view = evaluateWeb(entry, source);
        publishWeb(entry, "webView", view);
        evaluateWeb(entry, R"JS(
            for (const key of ['buffer', 'byteOffset', 'byteLength', 'length']) {
                Object.defineProperty(webView, key, {get() { throw Error('forged metadata'); }});
            }
        )JS");
        NativeWebBufferInfo info;
        ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
        entry.checkException();
        EXPECT_EQ(kind, info.kind);
        EXPECT_EQ(8u, info.byteOffset);
        EXPECT_EQ(kind < 0 ? 1u : sizes[kind], info.elementSize);
        EXPECT_EQ(3 * info.elementSize, info.byteLength);
        EXPECT_FALSE(info.detached);
        EXPECT_TRUE(entry.context.isValueEqual(buffer.get(), info.buffer.get()));
        ASSERT_NE(nullptr, info.data);
        info.data[0] = 42;
        EXPECT_TRUE(entry.context.valueToBool(
            evaluateWeb(entry, "new Uint8Array(webBuffer)[8] === 42").get(), entry.exceptionTracker));
    }
    NativeWebBufferInfo info;
    EXPECT_FALSE(support->getWebBufferInfo(evaluateWeb(entry, "({buffer: webBuffer})").get(),
                                         info, entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesNativeWebSupportTests, CreatesDistinctEmptyBuffersAndRealViewsAndDetachesBacking) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto emptyA = support->newWebArrayBuffer(nullptr, 0, entry.exceptionTracker);
    auto emptyB = support->newWebArrayBuffer(nullptr, 0, entry.exceptionTracker);
    EXPECT_FALSE(entry.context.isValueEqual(emptyA.get(), emptyB.get()));
    NativeWebBufferInfo emptyInfo;
    ASSERT_TRUE(support->getWebBufferInfo(emptyA.get(), emptyInfo, entry.exceptionTracker));
    EXPECT_FALSE(emptyInfo.detached);
    EXPECT_EQ(0u, emptyInfo.byteLength);
    auto emptyView = support->newWebBufferView(emptyA.get(), 0, 0, 10, entry.exceptionTracker);
    entry.checkException();
    ASSERT_TRUE(support->getWebBufferInfo(emptyView.get(), emptyInfo, entry.exceptionTracker));
    EXPECT_FALSE(emptyInfo.detached);
    EXPECT_EQ(0u, emptyInfo.byteLength);
    uint8_t bytes[64] = {1, 2, 3};
    auto buffer = support->newWebArrayBuffer(bytes, sizeof(bytes), entry.exceptionTracker);
    publishWeb(entry, "createdBuffer", buffer);
    for (int32_t kind = -1; kind <= 10; ++kind) {
        SCOPED_TRACE(kind);
        auto view = support->newWebBufferView(buffer.get(), 8, 2, kind, entry.exceptionTracker);
        entry.checkException();
        NativeWebBufferInfo info;
        ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
        EXPECT_EQ(kind, info.kind);
        EXPECT_EQ(8u, info.byteOffset);
        EXPECT_EQ(2 * info.elementSize, info.byteLength);
        publishWeb(entry, "createdView", view);
    }
    support->newWebBufferView(buffer.get(), 1, 2, 3, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker); // Int16 alignment is validated by the VM.
    publishWeb(entry, "alignmentError", entry.exceptionTracker.getExceptionAndClear());
    EXPECT_TRUE(entry.context.valueToBool(
        evaluateWeb(entry, "alignmentError instanceof RangeError").get(), entry.exceptionTracker));
    support->newWebBufferView(buffer.get(), 64, 1, -1, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.getExceptionAndClear();
    support->newWebBufferView(buffer.get(), std::numeric_limits<size_t>::max(), 1, 1, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.getExceptionAndClear();
    support->detachWebArrayBuffer(buffer.get(), entry.exceptionTracker);
    entry.checkException();
    NativeWebBufferInfo info;
    ASSERT_TRUE(support->getWebBufferInfo(buffer.get(), info, entry.exceptionTracker));
    EXPECT_TRUE(info.detached);
    EXPECT_EQ(nullptr, info.data);
    EXPECT_EQ(0u, info.byteLength);
    auto view = evaluateWeb(entry, "createdView");
    ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
    EXPECT_TRUE(info.detached);
    EXPECT_EQ(nullptr, info.data);
    EXPECT_EQ(0u, info.byteLength);
    EXPECT_TRUE(entry.context.valueToBool(
        evaluateWeb(entry, "createdBuffer.byteLength === 0 && createdView.byteLength === 0").get(),
        entry.exceptionTracker));
    support->detachWebArrayBuffer(buffer.get(), entry.exceptionTracker); // Already detached is a no-op.
    entry.checkException();
    support->newWebBufferView(buffer.get(), 0, 0, 1, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.getExceptionAndClear();
}

TEST_P(HermesNativeWebSupportTests, UsesIntrinsicViewConstructorsAndRejectsInvalidBufferBrands) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto buffer = support->newWebArrayBuffer(nullptr, 32, entry.exceptionTracker);
    evaluateWeb(entry, R"JS(
        globalThis.Uint8Array = globalThis.DataView = globalThis.BigInt64Array =
            function() { throw Error('application replacement'); };
    )JS");
    for (int32_t kind : {-1, 1, 9}) {
        auto view = support->newWebBufferView(buffer.get(), 8, 2, kind, entry.exceptionTracker);
        entry.checkException();
        NativeWebBufferInfo info;
        ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
        EXPECT_EQ(kind, info.kind);
    }
    auto object = evaluateWeb(entry, "({byteLength: 32})");
    support->newWebBufferView(object.get(), 0, 1, 1, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    publishWeb(entry, "bufferBrandError", entry.exceptionTracker.getExceptionAndClear());
    EXPECT_TRUE(entry.context.valueToBool(
        evaluateWeb(entry, "bufferBrandError instanceof TypeError").get(), entry.exceptionTracker));
    support->detachWebArrayBuffer(object.get(), entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.getExceptionAndClear();
    support->newWebBufferView(buffer.get(), 0, 1, 11, entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    entry.exceptionTracker.getExceptionAndClear();
}

TEST_P(HermesNativeWebSupportTests, DataViewDetachmentRemainsVisibleThroughVmMetadata) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto buffer = support->newWebArrayBuffer(nullptr, 16, entry.exceptionTracker);
    auto view = support->newWebBufferView(buffer.get(), 3, 5, -1, entry.exceptionTracker);
    publishWeb(entry, "detachedDataView", view);
    support->detachWebArrayBuffer(buffer.get(), entry.exceptionTracker);
    NativeWebBufferInfo info;
    ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
    EXPECT_TRUE(info.detached);
    EXPECT_EQ(-1, info.kind);
    EXPECT_EQ(nullptr, info.data);
    EXPECT_EQ(0u, info.byteLength);
    EXPECT_TRUE(entry.context.isValueEqual(buffer.get(), info.buffer.get()));
    EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry, R"JS((() => {
        try { return detachedDataView.byteLength; } catch (e) { return e instanceof TypeError; }
    })())JS").get(), entry.exceptionTracker));
    entry.checkException();
}

TEST_P(HermesNativeWebSupportTests, EncodesToStringAsWtf8AndThrowsForSymbols) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto str = evaluateWeb(entry, "'a\\u0000\\ud800b\\udc00\\ud83d\\ude00'");
    EXPECT_EQ(std::string("a\0\xed\xa0\x80" "b\xed\xb0\x80\xf0\x9f\x98\x80", 13),
              support->valueToWebWTF8(str.get(), entry.exceptionTracker));
    auto object = evaluateWeb(entry, "({toString() { return 'converted'; }})");
    EXPECT_EQ("converted", support->valueToWebWTF8(object.get(), entry.exceptionTracker));
    auto symbol = evaluateWeb(entry, "Symbol('x')");
    support->valueToWebWTF8(symbol.get(), entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    publishWeb(entry, "webStringError", entry.exceptionTracker.getExceptionAndClear());
    EXPECT_TRUE(entry.context.valueToBool(
        evaluateWeb(entry, "webStringError instanceof TypeError").get(), entry.exceptionTracker));
}

TEST_P(HermesNativeWebSupportTests, TransfersExternalBackingByIdentityAcrossGcUntilLastViewIsDropped) {
    MAIN_THREAD_INIT();
    std::atomic<int> destroyed{0};
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    {
        auto entry = wrapper.makeJsEntry();
        auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
        ASSERT_NE(nullptr, support);
        {
            auto owner = makeShared<WebBufferOwner>(destroyed);
            auto* originalData = owner->bytes.data();
            auto source = entry.context.newArrayBuffer(
                BytesView(owner, originalData, owner->bytes.size()), entry.exceptionTracker);
            owner = nullptr;
            publishWeb(entry, "transferSource", source);
            auto oldView = support->newWebBufferView(source.get(), 8, 4, 1, entry.exceptionTracker);
            publishWeb(entry, "transferOldView", oldView);
            auto target = support->transferWebArrayBuffer(source.get(), entry.exceptionTracker);
            entry.checkException();
            NativeWebBufferInfo info;
            ASSERT_TRUE(support->getWebBufferInfo(target.get(), info, entry.exceptionTracker));
            EXPECT_EQ(originalData, info.data); // Payload equality alone cannot prove a move.
            EXPECT_EQ(64u, info.byteLength);
            EXPECT_FALSE(info.detached);
            EXPECT_FALSE(entry.context.isValueEqual(source.get(), target.get()));
            auto lastView = support->newWebBufferView(target.get(), 0, 64, 1, entry.exceptionTracker);
            publishWeb(entry, "transferLastView", lastView);
            ASSERT_TRUE(support->getWebBufferInfo(source.get(), info, entry.exceptionTracker));
            EXPECT_TRUE(info.detached);
            ASSERT_TRUE(support->getWebBufferInfo(oldView.get(), info, entry.exceptionTracker));
            EXPECT_TRUE(info.detached);
            EXPECT_EQ(nullptr, info.data);
            EXPECT_EQ(0u, info.byteLength);
        }
        entry.context.garbageCollect();
        entry.context.garbageCollect();
        EXPECT_EQ(0, destroyed.load()); // Only a view keeps the new backing alive.
        EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry, R"JS(
            transferSource.byteLength === 0 && transferOldView.length === 0 &&
            transferOldView[0] === undefined &&
            transferLastView[0] === 17 && transferLastView[63] === 93
        )JS").get(), entry.exceptionTracker));
        evaluateWeb(entry, "transferLastView = undefined;");
    }
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); cx.garbageCollect(); });
    // Detached source and old view remain rooted; neither retains the allocation.
    EXPECT_EQ(1, destroyed.load());
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_EQ(1, destroyed.load());
}

TEST_P(HermesNativeWebSupportTests, TransfersInternalBackingByIdentityAndDetachesEveryOldView) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto source = evaluateWeb(entry, "new ArrayBuffer(64)");
    NativeWebBufferInfo info;
    ASSERT_TRUE(support->getWebBufferInfo(source.get(), info, entry.exceptionTracker));
    auto* originalData = info.data;
    ASSERT_NE(nullptr, originalData);
    originalData[0] = 29;
    originalData[63] = 71;
    std::vector<JSValueRef> oldViews;
    for (int32_t kind = -1; kind <= 10; ++kind) {
        oldViews.emplace_back(support->newWebBufferView(source.get(), 8, 2, kind, entry.exceptionTracker));
    }
    entry.checkException();
    auto target = support->transferWebArrayBuffer(source.get(), entry.exceptionTracker);
    entry.checkException();
    EXPECT_FALSE(entry.context.isValueEqual(source.get(), target.get()));
    for (const auto& view : oldViews) {
        ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, entry.exceptionTracker));
        EXPECT_TRUE(info.detached);
        EXPECT_EQ(nullptr, info.data);
        EXPECT_EQ(0u, info.byteOffset);
        EXPECT_EQ(0u, info.byteLength);
    }
    ASSERT_TRUE(support->getWebBufferInfo(source.get(), info, entry.exceptionTracker));
    EXPECT_TRUE(info.detached);
    info = NativeWebBufferInfo();
    entry.context.garbageCollect();
    ASSERT_TRUE(support->getWebBufferInfo(target.get(), info, entry.exceptionTracker));
    EXPECT_EQ(originalData, info.data);
    EXPECT_EQ(64u, info.byteLength);
    ASSERT_NE(nullptr, info.data);
    EXPECT_EQ(29, info.data[0]);
    EXPECT_EQ(71, info.data[63]);
    publishWeb(entry, "transferredInternal", target);
    publishWeb(entry, "transferredOldDataView", oldViews[0]);
    publishWeb(entry, "transferredOldTypedArray", oldViews[2]);
    EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry, R"JS((() => {
        let dataViewThrows = false, typedArrayThrows = false;
        try { transferredOldDataView.getUint8(0); } catch (e) { dataViewThrows = e instanceof TypeError; }
        try { transferredOldTypedArray.set([1]); } catch (e) { typedArrayThrows = e instanceof TypeError; }
        return dataViewThrows && typedArrayThrows &&
            transferredOldTypedArray.length === 0 && new Uint8Array(transferredInternal)[63] === 71;
    })())JS").get(), entry.exceptionTracker));
    // Exercise balanced eject/debit/adopt/credit repeatedly, including after GC.
    for (int i = 0; i < 32; ++i) {
        auto previous = std::move(target);
        target = support->transferWebArrayBuffer(previous.get(), entry.exceptionTracker);
        entry.checkException();
        ASSERT_TRUE(support->getWebBufferInfo(previous.get(), info, entry.exceptionTracker));
        EXPECT_TRUE(info.detached);
        ASSERT_TRUE(support->getWebBufferInfo(target.get(), info, entry.exceptionTracker));
        EXPECT_EQ(originalData, info.data);
        info = NativeWebBufferInfo();
        entry.context.garbageCollect();
    }
}

TEST_P(HermesNativeWebSupportTests, TransferUsesFreshIntrinsicBufferWithoutUserPropertyReads) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto source = evaluateWeb(entry, R"JS((() => {
        globalThis.transferIntrinsicPrototype = ArrayBuffer.prototype;
        globalThis.transferPropertyError = {identity: true};
        class DerivedBuffer extends ArrayBuffer {}
        const source = new DerivedBuffer(16);
        for (const key of ['constructor', 'byteLength']) {
            Object.defineProperty(source, key, {get() { throw transferPropertyError; }});
        }
        Object.defineProperty(ArrayBuffer, Symbol.species, {get() { throw transferPropertyError; }});
        globalThis.ArrayBuffer = function() { throw transferPropertyError; };
        globalThis.Uint8Array = globalThis.DataView = function() { throw transferPropertyError; };
        return source;
    })())JS");
    auto target = support->transferWebArrayBuffer(source.get(), entry.exceptionTracker);
    entry.checkException();
    publishWeb(entry, "intrinsicTransferTarget", target);
    EXPECT_FALSE(entry.context.isValueEqual(source.get(), target.get()));
    EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry,
        "Object.getPrototypeOf(intrinsicTransferTarget) === transferIntrinsicPrototype && "
        "intrinsicTransferTarget.byteLength === 16").get(), entry.exceptionTracker));
    NativeWebBufferInfo info;
    ASSERT_TRUE(support->getWebBufferInfo(source.get(), info, entry.exceptionTracker));
    EXPECT_TRUE(info.detached);
}

TEST_P(HermesNativeWebSupportTests, TransferCreatesDistinctAttachedEmptyBuffersAndRejectsDetachedEmpty) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    auto emptyA = support->newWebArrayBuffer(nullptr, 0, entry.exceptionTracker);
    auto emptyB = support->newWebArrayBuffer(nullptr, 0, entry.exceptionTracker);
    auto targetA = support->transferWebArrayBuffer(emptyA.get(), entry.exceptionTracker);
    auto targetB = support->transferWebArrayBuffer(emptyB.get(), entry.exceptionTracker);
    entry.checkException();
    EXPECT_FALSE(entry.context.isValueEqual(targetA.get(), targetB.get()));
    EXPECT_FALSE(entry.context.isValueEqual(emptyA.get(), targetA.get()));
    for (const auto* value : {&emptyA, &emptyB, &targetA, &targetB}) {
        NativeWebBufferInfo info;
        ASSERT_TRUE(support->getWebBufferInfo(value->get(), info, entry.exceptionTracker));
        EXPECT_EQ(value == &emptyA || value == &emptyB, info.detached);
        EXPECT_EQ(0u, info.byteLength);
    }
    auto view = support->newWebBufferView(targetA.get(), 0, 0, 1, entry.exceptionTracker);
    entry.checkException();
    support->transferWebArrayBuffer(emptyA.get(), entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    publishWeb(entry, "emptyTransferError", entry.exceptionTracker.getExceptionAndClear());
    EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry,
        "emptyTransferError instanceof TypeError").get(), entry.exceptionTracker));
}

TEST_P(HermesNativeWebSupportTests, TransferRejectsViewsProxiesAndOtherBrandsWithoutExecutingJs) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    auto entry = wrapper.makeJsEntry();
    auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
    ASSERT_NE(nullptr, support);
    evaluateWeb(entry, "globalThis.transferBrandSentinel = {identity: true};");
    for (const char* source : {
        "undefined", "null", "42", "'buffer'", "Symbol('buffer')", "1n",
        "({get byteLength() { throw transferBrandSentinel; }})",
        "new Uint8Array(4)", "new DataView(new ArrayBuffer(4))",
        "Object.create(ArrayBuffer.prototype)",
        "new Proxy(new ArrayBuffer(4), {get() { throw transferBrandSentinel; }})"}) {
        SCOPED_TRACE(source);
        auto value = evaluateWeb(entry, source);
        support->transferWebArrayBuffer(value.get(), entry.exceptionTracker);
        ASSERT_FALSE(entry.exceptionTracker);
        publishWeb(entry, "transferBrandError", entry.exceptionTracker.getExceptionAndClear());
        EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry,
            "transferBrandError instanceof TypeError && transferBrandError !== transferBrandSentinel").get(),
            entry.exceptionTracker));
    }
    auto source = support->newWebArrayBuffer(nullptr, 4, entry.exceptionTracker);
    support->detachWebArrayBuffer(source.get(), entry.exceptionTracker);
    support->transferWebArrayBuffer(source.get(), entry.exceptionTracker);
    ASSERT_FALSE(entry.exceptionTracker);
    publishWeb(entry, "detachedTransferError", entry.exceptionTracker.getExceptionAndClear());
    EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry,
        "detachedTransferError instanceof TypeError").get(), entry.exceptionTracker));
}

TEST_P(HermesNativeWebSupportTests, TransferredExternalOwnerReleasesExactlyOnceAtShutdown) {
    MAIN_THREAD_INIT();
    std::atomic<int> destroyed{0};
    {
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        auto entry = wrapper.makeJsEntry();
        auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
        ASSERT_NE(nullptr, support);
        auto owner = makeShared<WebBufferOwner>(destroyed);
        auto* originalData = owner->bytes.data();
        auto target = entry.context.newArrayBuffer(
            BytesView(owner, originalData, owner->bytes.size()), entry.exceptionTracker);
        owner = nullptr;
        for (int i = 0; i < 8; ++i) {
            auto previous = std::move(target);
            target = support->transferWebArrayBuffer(previous.get(), entry.exceptionTracker);
            entry.checkException();
            NativeWebBufferInfo info;
            ASSERT_TRUE(support->getWebBufferInfo(target.get(), info, entry.exceptionTracker));
            EXPECT_EQ(originalData, info.data);
            entry.context.garbageCollect();
            EXPECT_EQ(0, destroyed.load());
        }
        publishWeb(entry, "shutdownTransferRoot", target);
    }
    EXPECT_EQ(1, destroyed.load());
}

TEST_P(HermesNativeWebSupportTests, TransfersExternalBuffersWithDivergentOwnPropertiesAcrossGc) {
    MAIN_THREAD_INIT();
    // Issue #2201: transitioning a's hidden class before creating b can leave
    // b's external-finalizer descriptor lookup needing a property-map rebuild.
    for (bool addPropertiesToB : {false, true}) {
        SCOPED_TRACE(addPropertiesToB);
        std::atomic<int> destroyed{0};
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        {
            auto entry = wrapper.makeJsEntry();
            auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&entry.context);
            ASSERT_NE(nullptr, support);
            auto ownerA = makeShared<WebBufferOwner>(destroyed);
            auto* dataA = ownerA->bytes.data();
            auto a = entry.context.newArrayBuffer(
                BytesView(ownerA, dataA, ownerA->bytes.size()), entry.exceptionTracker);
            ownerA = nullptr;
            publishWeb(entry, "externalTransferA", a);
            evaluateWeb(entry, R"JS(
                externalTransferA.extra = 'a';
                for (let i = 0; i < 32; ++i) externalTransferA['a' + i] = i;
            )JS");
            auto ownerB = makeShared<WebBufferOwner>(destroyed);
            auto* dataB = ownerB->bytes.data();
            auto b = entry.context.newArrayBuffer(
                BytesView(ownerB, dataB, ownerB->bytes.size()), entry.exceptionTracker);
            ownerB = nullptr;
            publishWeb(entry, "externalTransferB", b);
            if (addPropertiesToB) {
                evaluateWeb(entry, R"JS(
                    externalTransferB.different = 'b';
                    Object.defineProperty(externalTransferB, 'hidden', {value: 42});
                )JS");
            }
            entry.context.garbageCollect();
            auto targetB = support->transferWebArrayBuffer(b.get(), entry.exceptionTracker);
            auto targetA = support->transferWebArrayBuffer(a.get(), entry.exceptionTracker);
            entry.checkException();
            entry.context.garbageCollect();
            for (const auto& pair : {std::make_pair(&targetA, dataA), std::make_pair(&targetB, dataB)}) {
                NativeWebBufferInfo info;
                ASSERT_TRUE(support->getWebBufferInfo(pair.first->get(), info, entry.exceptionTracker));
                EXPECT_EQ(pair.second, info.data);
                EXPECT_FALSE(info.detached);
                ASSERT_NE(nullptr, info.data);
                EXPECT_EQ(17, info.data[0]);
                EXPECT_EQ(93, info.data[63]);
            }
            EXPECT_EQ(0, destroyed.load());
            publishWeb(entry, "externalTargetA", targetA);
            publishWeb(entry, "externalTargetB", targetB);
            EXPECT_TRUE(entry.context.valueToBool(evaluateWeb(entry, R"JS(
                externalTransferA.extra === 'a' && externalTransferA.a31 === 31 &&
                externalTransferA.byteLength === 0 && externalTransferB.byteLength === 0 &&
                !Object.prototype.hasOwnProperty.call(externalTargetA, 'extra') &&
                !Object.prototype.hasOwnProperty.call(externalTargetB, 'different')
            )JS").get(), entry.exceptionTracker));
            evaluateWeb(entry, "externalTargetA = externalTargetB = undefined;");
        }
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); cx.garbageCollect(); });
        EXPECT_EQ(2, destroyed.load());
    }
}

INSTANTIATE_TEST_SUITE_P(Hermes, HermesNativeWebSupportTests,
    ::testing::Values(JavaScriptEngineTestCase::Hermes), PrintJavaScriptEngineType());
} // namespace
} // namespace ValdiTest
