#include "valdi/test/native_web/NativeWebTestUtils.hpp"
#include "valdi/runtime/Interfaces/IJavaScriptNativeWebSupport.hpp"

#include <atomic>

namespace ValdiTest {
using namespace Valdi;

class NativeWebTests : public JSBridgeTestFixture {};

namespace {
// The owner contains no JS or Rust context. Its destructor can run on the GC
// worker, including after NativeWeb shutdown.
class ReceiveBufferOwner final : public SimpleRefCountable {
  public:
    explicit ReceiveBufferOwner(std::atomic<int>& releases) : releases(releases) {}
    ~ReceiveBufferOwner() override { ++releases; }
    uint8_t data[5] = {0, 41, 42, 43, 0};
    std::atomic<int>& releases;
};
} // namespace

TEST_P(NativeWebTests, ByteStreamEnqueuePreservesExternalAllocationAndReleasesAfterLastView) {
    std::atomic<int> releases{0};
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    const uint8_t* original = nullptr;
    wrapper.withContext([&](auto& cx, auto& ex) {
        loadAndInstallNativeWeb(factory, cx, ex);
        auto owner = makeShared<ReceiveBufferOwner>(releases);
        original = owner->data;
        auto buffer = cx.newArrayBuffer(BytesView(owner, owner->data, sizeof(owner->data)), ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "receiveBuffer", buffer.get(), ex);
    });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.received = undefined;
        globalThis.oldView = new Uint8Array(receiveBuffer);
        const stream = new ReadableStream({type: 'bytes', start(c) {
            c.enqueue(new Uint8Array(receiveBuffer, 1, 3));
            c.close();
        }});
        stream.getReader().read().then(r => { received = r.value; });
        return true;
    })())JS");
    ASSERT_TRUE(wrapper.evaluateScript("received !== undefined").toBool());
    EXPECT_TRUE(wrapper.evaluateScript("receiveBuffer.byteLength === 0 && oldView.byteLength === 0").toBool());
    wrapper.withContext([&](auto& cx, auto& ex) {
        auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&cx);
        ASSERT_NE(support, nullptr);
        auto view = cx.getObjectProperty(cx.getGlobalObject(ex).get(), "received", ex);
        NativeWebBufferInfo info;
        ASSERT_TRUE(support->getWebBufferInfo(view.get(), info, ex));
        EXPECT_EQ(info.data, original + 1);
        EXPECT_EQ(info.byteOffset, 1u);
        EXPECT_EQ(info.byteLength, 3u);
    });
    EXPECT_EQ(releases.load(), 0);
    // Mutation belongs to JS after transfer; Rust retains no byte-slice alias.
    EXPECT_TRUE(wrapper.evaluateScript("(received[0] = 99, received[0] === 99 && received[2] === 43)").toBool());
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_EQ(releases.load(), 0);
    wrapper.evaluateScript("(globalThis.lastView = received.subarray(1), received = undefined, true)");
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_EQ(releases.load(), 0);
    wrapper.evaluateScript("(lastView = undefined, receiveBuffer = undefined, oldView = undefined, true)");
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebTests, QueuedExternalByteChunkSurvivesWebShutdownUntilJsDropsIt) {
    std::atomic<int> releases{0};
    {
        JSContextWrapper wrapper(getJsBridge(), nullptr);
        NativeWebScope scope(wrapper);
        NativeWebModuleFactory factory;
        const uint8_t* original = nullptr;
        wrapper.withContext([&](auto& cx, auto& ex) {
            loadAndInstallNativeWeb(factory, cx, ex);
            auto owner = makeShared<ReceiveBufferOwner>(releases);
            original = owner->data;
            auto buffer = cx.newArrayBuffer(BytesView(owner, owner->data, sizeof(owner->data)), ex);
            cx.setObjectProperty(cx.getGlobalObject(ex).get(), "receiveBuffer", buffer.get(), ex);
        });
        wrapper.evaluateScript(R"JS((() => {
            globalThis.queuedStream = new ReadableStream({type: 'bytes', start(c) {
                c.enqueue(new Uint8Array(receiveBuffer));
            }});
            return true;
        })())JS");
        EXPECT_EQ(releases.load(), 0);
        wrapper.evaluateScript("queuedStream.getReader().read().then(r => { globalThis.retainedChunk = r.value; })");
        wrapper.withContext([&](auto& cx, auto& ex) {
            auto* support = dynamic_cast<IJavaScriptNativeWebSupport*>(&cx);
            ASSERT_NE(support, nullptr);
            auto chunk = cx.getObjectProperty(cx.getGlobalObject(ex).get(), "retainedChunk", ex);
            NativeWebBufferInfo info;
            ASSERT_TRUE(support->getWebBufferInfo(chunk.get(), info, ex));
            EXPECT_EQ(info.data, original);
            NativeWeb::shutdown(cx);
        });
        EXPECT_EQ(releases.load(), 0);
        EXPECT_TRUE(wrapper.evaluateScript("retainedChunk[2] === 42").toBool());
        wrapper.evaluateScript("(retainedChunk = undefined, queuedStream = undefined, receiveBuffer = undefined, true)");
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
        EXPECT_EQ(releases.load(), 1);
    }
    EXPECT_EQ(releases.load(), 1);
}

// These execute in the actual Hermes VM; the Rust ABI library must be linked.
TEST_P(NativeWebTests, InstallationIsOptInAndExportsNativeClasses) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("typeof fetch === 'undefined' && typeof Response === 'undefined'").toBool());
    wrapper.withContext([&](auto& cx, auto& ex) {
        NativeWebModuleFactory factory;
        auto exports = loadAndInstallNativeWeb(factory, cx, ex);
        ASSERT_TRUE(ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "web", exports.get(), ex);
    });
    EXPECT_TRUE(wrapper
                    .evaluateScript(R"JS((() => {
        if (typeof web.Response !== 'function') return false;
        const r = new web.Response('hello', {status: 201});
        return r instanceof web.Response && r.status === 201 && !r.bodyUsed &&
            web.fetch === fetch && web.ReadableStream === ReadableStream;
    })())JS")
                    .toBool());
}

TEST_P(NativeWebTests, ResponseBodyUsesRealPromiseAndRejectsSecondRead) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.result = '';
        const response = new Response('hello');
        const first = response.text();
        if (!(first instanceof Promise) || !response.bodyUsed) throw Error('not a native promise/body');
        first.then(s => { result += s; });
        response.text().then(() => { result += 'BAD'; }, e => { result += ':' + e.name; });
        return true;
    })())JS");
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "hello:TypeError");
}

TEST_P(NativeWebTests, ReadableReadCancelAndThrownPullUsePromiseQueue) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.result = '';
        const stream = new ReadableStream({
            start(c) { c.enqueue('chunk'); },
            cancel(reason) { result += reason; }
        });
        const reader = stream.getReader();
        reader.read().then(r => { result += r.value + ':' + r.done + ':'; return reader.cancel('stop'); });
        new ReadableStream({pull() { throw new RangeError('pull failed'); }})
            .getReader().read().catch(e => { globalThis.pullError = e.name + ':' + e.message; });
        return true;
    })())JS");
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "chunk:false:stop");
    EXPECT_EQ(wrapper.evaluateScript("pullError").toStringBox().slowToString(), "RangeError:pull failed");
}

TEST_P(NativeWebTests, EventTargetInheritedMethodsAndSymbolIterator) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper
                    .evaluateScript(R"JS((() => {
        const ac = new AbortController();
        let called = false;
        ac.signal.addEventListener('abort', () => { called = true; });
        ac.abort('reason');
        const headers = new Headers([['A', 'b']]);
        return called && ac.signal.reason === 'reason' &&
            ac.signal instanceof EventTarget && headers[Symbol.iterator]().next().value[0] === 'a';
    })())JS")
                    .toBool());
}

TEST_P(NativeWebTests, DefaultAbortCreatesDOMExceptionAndPropagatesTheSameReason) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        const controller = new AbortController();
        let events = 0;
        controller.signal.addEventListener('abort', () => { events++; });
        controller.abort();
        const reason = controller.signal.reason;
        if (!(reason instanceof DOMException) || !(reason instanceof Error) ||
            reason.name !== 'AbortError' || reason.code !== 20 || !reason.message ||
            !controller.signal.aborted || events !== 1) return false;
        try { controller.signal.throwIfAborted(); return false; }
        catch (error) { if (error !== reason) return false; }
        controller.abort();
        if (controller.signal.reason !== reason || events !== 1) return false;
        const signal = AbortSignal.abort();
        const explicitUndefined = new AbortController();
        explicitUndefined.abort(undefined);
        const explicit = new AbortController();
        explicit.abort(null);
        globalThis.defaultAbortFetch = false;
        fetch('data:text/plain,unused', {signal: controller.signal}).catch(error => {
            defaultAbortFetch = error === reason;
        });
        return signal.reason instanceof DOMException && signal.reason.code === 20 &&
            explicitUndefined.signal.reason instanceof DOMException && explicit.signal.reason === null;
    })())JS").toBool());
    EXPECT_TRUE(wrapper.evaluateScript("defaultAbortFetch").toBool());
}

TEST_P(NativeWebTests, DOMExceptionIsOptInNativeErrorWithReadonlyBrandedFields) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("typeof DOMException === 'undefined'").toBool());
    // Replacing the application's global Error must not replace the VM's
    // intrinsic Error used by native DOMException, even before module loading.
    wrapper.evaluateScript(R"JS((() => {
        globalThis.IntrinsicError = Error;
        globalThis.errorCalls = 0;
        globalThis.Error = function () { errorCalls++; throw new IntrinsicError('app Error'); };
        return true;
    })())JS");
    wrapper.withContext([&](auto& cx, auto& ex) {
        NativeWebModuleFactory factory;
        auto exports = loadAndInstallNativeWeb(factory, cx, ex);
        ASSERT_TRUE(ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "web", exports.get(), ex);
    });
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        if (web.DOMException !== DOMException || DOMException.name !== 'DOMException' ||
            DOMException.length !== 0 || Object.getPrototypeOf(DOMException.prototype) !== IntrinsicError.prototype)
            return false;
        const empty = new DOMException();
        const error = new DOMException('stopped', 'AbortError');
        if (empty.message !== '' || empty.name !== 'Error' || empty.code !== 0 ||
            !(error instanceof IntrinsicError) || !(error instanceof DOMException) ||
            error.toString() !== 'AbortError: stopped' || errorCalls !== 0 ||
            Object.prototype.toString.call(error) !== '[object DOMException]') return false;
        // Removing the tag exposes [[ErrorData]], which a prototype-only fake lacks.
        delete DOMException.prototype[Symbol.toStringTag];
        if (Object.prototype.toString.call(error) !== '[object Error]') return false;
        for (const field of ['message', 'name', 'code']) {
            const descriptor = Object.getOwnPropertyDescriptor(DOMException.prototype, field);
            if (typeof descriptor.get !== 'function' || descriptor.set !== undefined ||
                !descriptor.enumerable || !descriptor.configurable || Object.hasOwnProperty.call(error, field))
                return false;
            try { descriptor.get.call(Object.create(DOMException.prototype)); return false; }
            catch (failure) { if (!(failure instanceof TypeError)) return false; }
            try { (() => { 'use strict'; error[field] = 'changed'; })(); return false; }
            catch (failure) { if (!(failure instanceof TypeError)) return false; }
        }
        return error.message === 'stopped' && error.name === 'AbortError' && error.code === 20;
    })())JS").toBool());
}

TEST_P(NativeWebTests, DOMExceptionConvertsStringsAndHonorsNewTarget) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        class Derived extends DOMException {}
        const derived = new Derived('late', 'TimeoutError');
        if (!(derived instanceof Derived) || !(derived instanceof Error) || derived.code !== 23) return false;
        const custom = {};
        function Target() {}
        Target.prototype = custom;
        const reflected = Reflect.construct(DOMException, ['x', 'NetworkError'], Target);
        const getName = Object.getOwnPropertyDescriptor(DOMException.prototype, 'name').get;
        if (Object.getPrototypeOf(reflected) !== custom || getName.call(reflected) !== 'NetworkError')
            return false;
        Target.prototype = 1;
        if (Object.getPrototypeOf(Reflect.construct(DOMException, [], Target)) !== DOMException.prototype)
            return false;
        let order = '';
        const converted = new DOMException(
            {toString() { order += 'm'; return '\ud800'; }},
            {toString() { order += 'n'; return 'custom'; }});
        if (converted.message !== '\ud800' || converted.name !== 'custom' ||
            converted.code !== 0 || order !== 'mn') return false;
        if (new DOMException(undefined, undefined).name !== 'Error' ||
            new DOMException(null, null).message !== 'null') return false;
        for (const invoke of [
            () => DOMException(), () => DOMException.call({}),
            () => new DOMException(Symbol()), () => new DOMException('', Symbol())]) {
            try { invoke(); return false; }
            catch (failure) { if (!(failure instanceof TypeError)) return false; }
        }
        const sentinel = {};
        try { new DOMException({toString() { throw sentinel; }}); return false; }
        catch (failure) { if (failure !== sentinel) return false; }
        return true;
    })())JS").toBool());
}

TEST_P(NativeWebTests, DOMExceptionMapsAllLegacyCodesAndConstants) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        const names = ['IndexSizeError', '', 'HierarchyRequestError', 'WrongDocumentError',
            'InvalidCharacterError', '', 'NoModificationAllowedError', 'NotFoundError',
            'NotSupportedError', 'InUseAttributeError', 'InvalidStateError', 'SyntaxError',
            'InvalidModificationError', 'NamespaceError', 'InvalidAccessError', '',
            'TypeMismatchError', 'SecurityError', 'NetworkError', 'AbortError', 'URLMismatchError',
            'QuotaExceededError', 'TimeoutError', 'InvalidNodeTypeError', 'DataCloneError'];
        const constants = ['INDEX_SIZE_ERR', 'DOMSTRING_SIZE_ERR', 'HIERARCHY_REQUEST_ERR',
            'WRONG_DOCUMENT_ERR', 'INVALID_CHARACTER_ERR', 'NO_DATA_ALLOWED_ERR',
            'NO_MODIFICATION_ALLOWED_ERR', 'NOT_FOUND_ERR', 'NOT_SUPPORTED_ERR', 'INUSE_ATTRIBUTE_ERR',
            'INVALID_STATE_ERR', 'SYNTAX_ERR', 'INVALID_MODIFICATION_ERR', 'NAMESPACE_ERR',
            'INVALID_ACCESS_ERR', 'VALIDATION_ERR', 'TYPE_MISMATCH_ERR', 'SECURITY_ERR', 'NETWORK_ERR',
            'ABORT_ERR', 'URL_MISMATCH_ERR', 'QUOTA_EXCEEDED_ERR', 'TIMEOUT_ERR', 'INVALID_NODE_TYPE_ERR',
            'DATA_CLONE_ERR'];
        for (let i = 0; i < constants.length; i++) {
            const exception = new DOMException('', names[i]);
            if (exception.code !== (names[i] ? i + 1 : 0) || exception[constants[i]] !== i + 1)
                return false;
            for (const target of [DOMException, DOMException.prototype]) {
                const descriptor = Object.getOwnPropertyDescriptor(target, constants[i]);
                if (descriptor.value !== i + 1 || descriptor.writable || !descriptor.enumerable ||
                    descriptor.configurable) return false;
            }
        }
        return new DOMException('', 'EncodingError').code === 0 &&
            new DOMException('', 'aborterror').code === 0;
    })())JS").toBool());
}

TEST_P(NativeWebTests, TimeoutAbortCreatesNativeDOMException) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((globalThis.timeoutSignal = AbortSignal.timeout(0), true))JS");
    ASSERT_TRUE(waitForWeb(wrapper, "timeoutSignal.aborted"));
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((
        timeoutSignal.reason instanceof DOMException && timeoutSignal.reason instanceof Error &&
        timeoutSignal.reason.name === 'TimeoutError' && timeoutSignal.reason.code === 23
    ))JS").toBool());
}

TEST_P(NativeWebTests, InternalDOMExceptionSurvivesGlobalConstructorReplacementAndGc) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    EXPECT_TRUE(wrapper.evaluateScript("typeof performance === 'undefined'").toBool());
    wrapper.withContext([&](auto& cx, auto& ex) {
        // Rust event timestamps probe performance.now(), recovering failures
        // with 0. A missing primitive would exercise generic Error conversion,
        // obscuring this constructor test. Match JavaScriptRuntime's native
        // monotonic clock; no Web algorithm or error implementation is mocked.
        const auto origin = std::chrono::steady_clock::now();
        auto now = cx.newFunction(makeShared<JSFunctionWithCallable>(
            ReferenceInfoBuilder(), [origin](JSFunctionNativeCallContext& call) {
                auto elapsed = std::chrono::steady_clock::now() - origin;
                return call.getContext().newNumber(std::chrono::duration<double, std::milli>(elapsed).count());
            }), ex);
        auto performance = cx.newObject(ex);
        cx.setObjectProperty(performance.get(), "now", now.get(), ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "performance", performance.get(), ex);
        loadAndInstallNativeWeb(factory, cx, ex);
    });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.NativeDOMException = DOMException;
        globalThis.NativeError = Error;
        globalThis.domExceptionReads = 0;
        globalThis.errorCalls = 0;
        Object.defineProperty(globalThis, 'DOMException', {
            configurable: true, get() { domExceptionReads++; throw new NativeError('app DOMException'); }
        });
        globalThis.Error = function () {
            errorCalls++;
            throw new NativeError('app Error');
        };
        globalThis.savedException = new NativeDOMException('retained', 'DataCloneError');
        return true;
    })())JS");
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_EQ(wrapper.evaluateScript(R"JS((() => {
        const controller = new AbortController();
        controller.abort();
        const staticSignal = AbortSignal.abort();
        globalThis.replacedTimeout = AbortSignal.timeout(0);
        return JSON.stringify([controller.signal.reason instanceof NativeDOMException,
            controller.signal.reason instanceof NativeError, controller.signal.reason.code,
            staticSignal.reason.code, savedException.message, savedException.code, domExceptionReads, errorCalls]);
    })())JS").toStringBox().slowToString(), "[true,true,20,20,\"retained\",25,0,0]");
    ASSERT_TRUE(waitForWeb(wrapper, "replacedTimeout.aborted"));
    EXPECT_EQ(wrapper.evaluateScript(R"JS(JSON.stringify([
        replacedTimeout.reason instanceof NativeDOMException, replacedTimeout.reason instanceof NativeError,
        replacedTimeout.reason.name, replacedTimeout.reason.code, domExceptionReads, errorCalls
    ]))JS").toStringBox().slowToString(), "[true,true,\"TimeoutError\",23,0,0]");
}

TEST_P(NativeWebTests, CyclicControllerAndCallbackCanBeCollected) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        const stream = new ReadableStream({start(controller) {
            controller.callback = () => stream;
        }});
        globalThis.abandoned = new WeakRef(stream);
        return true;
    })())JS");
    // Separate VM entries end the WeakRef kept-alive job and release test
    // temporaries.
    wrapper.withContext([&](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.withContext([&](auto& cx, auto&) { cx.garbageCollect(); });
    EXPECT_TRUE(wrapper.evaluateScript("abandoned.deref() === undefined").toBool());
}

TEST_P(NativeWebTests, FetchDataUrlAndArrayBufferSubviewUseRustAlgorithms) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.result = '';
        fetch('data:text/plain;base64,aGVsbG8=').then(r => r.text()).then(s => { result = s; });
        const bytes = new Uint8Array([0, 65, 66, 0]);
        new Response(bytes.subarray(1, 3)).text().then(s => { globalThis.subview = s; });
        return true;
    })())JS");
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "hello");
    EXPECT_EQ(wrapper.evaluateScript("subview").toStringBox().slowToString(), "AB");
}

TEST_P(NativeWebTests, InvalidConstructorAndReceiverThrowActualTypeErrors) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper
                    .evaluateScript(R"JS((() => {
        let count = 0;
        globalThis.receiverRejected = false;
        try { Response('body'); } catch (e) { if (e instanceof TypeError) count++; }
        const invalid = Response.prototype.text.call({});
        invalid.then(() => { receiverRejected = false; }, error => { receiverRejected = error instanceof TypeError; });
        try { new Response('', {status: 99}); } catch (e) { if (e instanceof RangeError) count++; }
        return count === 2 && invalid instanceof Promise;
    })())JS")
                    .toBool());
    EXPECT_TRUE(wrapper.evaluateScript("receiverRejected").toBool());
}

TEST_P(NativeWebTests, NativeConstructorsHonorSubclassNewTarget) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_TRUE(wrapper
                    .evaluateScript(R"JS((() => {
        class Derived extends Response {}
        const response = new Derived('body');
        class Target extends EventTarget {}
        const target = new Target();
        let called = false;
        target.addEventListener('x', () => { called = true; });
        target.dispatchEvent(new Event('x'));
        return response instanceof Derived && response instanceof Response &&
            Object.getPrototypeOf(response) === Derived.prototype && called;
    })())JS")
                    .toBool());
}

TEST_P(NativeWebTests, ByobReadTransfersAndDetachesTheProvidedBuffer) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.result = '';
        const stream = new ReadableStream({type: 'bytes', pull(c) {
            const request = c.byobRequest;
            request.view[0] = 42;
            request.respond(1);
            c.close();
        }});
        const original = new ArrayBuffer(8);
        const read = stream.getReader({mode: 'byob'}).read(new Uint8Array(original, 2, 3));
        globalThis.detached = original.byteLength === 0;
        read.then(r => { result = r.value[0] + ':' + r.value.byteLength; });
        return true;
    })())JS");
    EXPECT_TRUE(wrapper.evaluateScript("detached").toBool());
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "42:1");
}

TEST_P(NativeWebTests, PromiseIntrinsicsIgnoreApplicationReplacementAndPreserveThrownIdentity) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        const PromiseIntrinsic = Promise;
        const thrown = {reason: 'identity'};
        const then = Promise.prototype.then;
        globalThis.Promise = function() { throw Error('replaced Promise'); };
        PromiseIntrinsic.prototype.then = function() { throw Error('replaced then'); };
        const promise = new Response('native').text();
        then.call(promise, text => { globalThis.result = text; });
        const stream = new ReadableStream({pull() { throw thrown; }});
        then.call(stream.getReader().read(), undefined, error => { globalThis.sameError = error === thrown; });
        PromiseIntrinsic.prototype.then = then;
        globalThis.Promise = PromiseIntrinsic;
        return true;
    })())JS");
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "native");
    EXPECT_TRUE(wrapper.evaluateScript("sameError").toBool());
}

TEST_P(NativeWebTests, ExplicitShutdownInvalidatesCallbacksBeforeVmDies) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) {
        loadAndInstallNativeWeb(factory, cx, ex);
        NativeWeb::shutdown(cx);
        NativeWeb::shutdown(cx);
    });
    EXPECT_TRUE(wrapper
                    .evaluateScript(R"JS((() => {
        try { new Response('late'); return false; }
        catch (e) { return /closed|disposed/.test(e.message); }
    })())JS")
                    .toBool());
}

TEST_P(NativeWebTests, ReleasedWriterReplacesFulfilledClosedPromiseWithTypeError) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.writerResult = '';
        const writer = new WritableStream().getWriter();
        writer.close().then(() => {
            const original = writer.closed;
            writer.releaseLock();
            globalThis.closedWasReplaced = writer.closed !== original;
            writer.closed.then(() => { writerResult = 'FULFILLED'; }, error => { writerResult = error.name; });
        });
        return true;
    })())JS");
    EXPECT_TRUE(wrapper.evaluateScript("closedWasReplaced").toBool());
    EXPECT_EQ(wrapper.evaluateScript("writerResult").toStringBox().slowToString(), "TypeError");
}

TEST_P(NativeWebTests, UrlSearchParamsReadsOriginalSurrogateRecordKeyBeforeConvertingName) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    EXPECT_EQ(
        wrapper.evaluateScript(R"JS(new URLSearchParams({['\uD800']: 'x'}).toString())JS").toStringBox().slowToString(),
        "%EF%BF%BD=x");
}

TEST_P(NativeWebTests, OldGenerationNativeCyclesFinalizeWhileLiveObjectsRemainUsable) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript(R"JS((() => {
        globalThis.keepOld = [];
        for (let i = 0; i < 128; i++) {
            const stream = new ReadableStream({start(controller) { controller.cycle = () => stream; }});
            keepOld.push(stream);
        }
        globalThis.oldWeak = new WeakRef(keepOld[0]);
        globalThis.liveResponse = new Response('still alive');
        return true;
    })())JS");
    // Collect with native objects reachable before abandoning them, so their
    // NativeState cells are promoted rather than only nursery-finalized.
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.evaluateScript("(keepOld = null, true)");
    for (int pass = 0; pass < 8; ++pass) {
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
        wrapper.taskScheduler->taskQueue->flushUpToNow();
        EXPECT_TRUE(
            wrapper.evaluateScript("new Response('turn').status === 200 && liveResponse.status === 200").toBool());
    }
    EXPECT_TRUE(wrapper.evaluateScript("oldWeak.deref() === undefined").toBool());
}

TEST_P(NativeWebTests, TerminalShutdownRejectsModuleReloadInTheSameContext) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) {
        loadAndInstallNativeWeb(factory, cx, ex);
        ASSERT_TRUE(ex);
        NativeWeb::shutdown(cx);
        auto reloaded = loadAndInstallNativeWeb(factory, cx, ex);
        EXPECT_FALSE(static_cast<bool>(ex));
        if (!ex) {
            ex.getExceptionAndClear();
        }
    });
}

TEST_P(NativeWebTests, TerminalShutdownRejectsFirstLateInstallWithoutLeakingContextAddress) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) {
        NativeWeb::shutdown(cx);
        loadAndInstallNativeWeb(factory, cx, ex);
        EXPECT_FALSE(static_cast<bool>(ex));
        if (!ex) {
            ex.getExceptionAndClear();
        }
        // Final release retires the closed entry. No runtime JS follows this
        // point; fixture scopes also release contexts that never installed.
        NativeWeb::release(cx);
    });
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
