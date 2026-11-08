// Compile the actual production adapter into this test TU under distinct
// exported class names. Its anonymous WebState/registry then remain test-local.
// This exercises the real Rust ABI and Hermes handle table without a production
// accessor, injected Web implementation, or replacement engine vtable.
#include "valdi/runtime/NativeWeb/NativeWeb.hpp"

namespace Valdi {
class ReceiveFixtureNativeWeb {
  public:
    static void shutdown(IJavaScriptContext&);
    static void release(IJavaScriptContext&);
};
class ReceiveFixtureModuleFactory final : public JavaScriptModuleFactory {
  public:
    StringBox getModulePath() const final;
    JSValueRef loadModule(IJavaScriptContext&, const ReferenceInfoBuilder&, JSExceptionTracker&) final;
};
} // namespace Valdi

#define NativeWeb ReceiveFixtureNativeWeb
#define NativeWebModuleFactory ReceiveFixtureModuleFactory
#include "valdi/src/valdi/runtime/NativeWeb/NativeWeb.cpp"
#undef NativeWebModuleFactory
#undef NativeWeb

#include "valdi/test/native_web/NativeWebTestUtils.hpp"

namespace ValdiTest {
using namespace Valdi;
namespace {
class BufferAllocation final : public SimpleRefCountable {
  public:
    explicit BufferAllocation(std::atomic<int>& releases) : releases(releases) {}
    ~BufferAllocation() override { ++releases; }
    uint8_t data[5] = {0, 65, 66, 67, 0};
    std::atomic<int>& releases;
};
struct DescriptorAllocation {
    std::atomic<int>& releases;
    uint8_t bytes[3] = {65, 66, 67};
    static void release(void* p) {
        auto* allocation = static_cast<DescriptorAllocation*>(p);
        ++allocation->releases;
        delete allocation;
    }
};

class ReceiveFixture {
  public:
    explicit ReceiveFixture(IJavaScriptBridge* bridge) : wrapper(bridge, nullptr), scope(wrapper) {
        wrapper.withContext([&](auto& cx, auto& ex) {
            ReceiveFixtureModuleFactory factory;
            loadAndInstallNativeWeb(factory, cx, ex);
            ASSERT_TRUE(ex);
            auto& store = Valdi::registry();
            std::lock_guard<std::mutex> lock(store.mutex);
            state = store.states.find(&cx)->second;
        });
    }
    ~ReceiveFixture() {
        wrapper.withContext([](auto& cx, auto&) {
            ReceiveFixtureNativeWeb::shutdown(cx);
            ReceiveFixtureNativeWeb::release(cx);
        });
        state.reset();
    }
    uint32_t startHttp(const char* script = R"JS(
        globalThis.received = undefined;
        fetch('http://127.0.0.1:1/fixture').then(r => {
            globalThis.response = r;
            globalThis.reader = r.body.getReader();
            reader.read().then(r => { received = r.value; });
        });
    )JS") {
        uint32_t id = 0;
        wrapper.withContext([&](auto& cx, auto& ex) {
            ASSERT_TRUE(state->enter(ex));
            cx.evaluate(script, "NativeWebReceiveBuffer_tests.js", ex);
            nsh_web_drain_http(
                state->rust,
                [](void* user, const NshWebHttpOp* op) {
                    if (op->op == 1) {
                        *static_cast<uint32_t*>(user) = op->id;
                    }
                },
                &id);
            state->leave(ex);
            ASSERT_TRUE(ex);
        });
        EXPECT_NE(id, 0u);
        wrapper.withContext([&](auto&, auto& ex) {
            ASSERT_TRUE(state->enter(ex));
            NshWebHttpEvent head{};
            head.id = id;
            head.event = 1;
            head.status = 200;
            EXPECT_EQ(nsh_web_http_event(state->rust, &head), 0);
            state->leave(ex);
            ASSERT_TRUE(ex);
        });
        return id;
    }
    uint32_t startSocket(const char* binaryType = "arraybuffer") {
        uint32_t id = 0;
        wrapper.withContext([&](auto& cx, auto& ex) {
            ASSERT_TRUE(state->enter(ex));
            cx.evaluate(std::string("globalThis.received = undefined; globalThis.ws = new WebSocket("
                                   "'ws://127.0.0.1:1/fixture'); ws.binaryType = '") +
                            binaryType + "'; ws.onmessage = e => { received = e.data; };",
                        "NativeWebReceiveBuffer_tests.js", ex);
            nsh_web_drain_ws(
                state->rust,
                [](void* user, const NshWebSocketOp* op) {
                    if (op->op == 1) {
                        *static_cast<uint32_t*>(user) = op->id;
                    }
                },
                &id);
            EXPECT_EQ(nsh_web_ws_event(state->rust, id, 1, {nullptr, 0}, 0, 0), 0);
            state->leave(ex);
            ASSERT_TRUE(ex);
        });
        EXPECT_NE(id, 0u);
        return id;
    }
    // Retain an ABI handle while calling Rust, with its actual JSValueRef still
    // rooted locally. Rust can retain only the stream/event algorithm's values.
    void receive(uint32_t id, std::atomic<int>& releases, const uint8_t*& original, bool socket) {
        wrapper.withContext([&](auto& cx, auto& ex) {
            ASSERT_TRUE(state->enter(ex));
            auto owner = makeShared<BufferAllocation>(releases);
            original = owner->data;
            auto buffer = cx.newArrayBuffer(BytesView(owner, owner->data, sizeof(owner->data)), ex);
            auto value = socket ? buffer : state->support->newWebBufferView(buffer.get(), 1, 3, 1, ex);
            auto raw = state->toRaw(value, ex);
            ASSERT_TRUE(ex);
            EXPECT_EQ(socket ? nsh_web_ws_binary_buffer(state->rust, id, raw)
                             : nsh_web_http_data_buffer(state->rust, id, raw), 0);
            state->release(raw);
            state->leave(ex);
            ASSERT_TRUE(ex);
        });
    }
    void expectPointer(const uint8_t* expected, size_t length) {
        wrapper.withContext([&](auto& cx, auto& ex) {
            auto value = cx.getObjectProperty(cx.getGlobalObject(ex).get(), "received", ex);
            NativeWebBufferInfo info;
            ASSERT_TRUE(state->support->getWebBufferInfo(value.get(), info, ex));
            EXPECT_EQ(info.data, expected);
            EXPECT_EQ(info.byteLength, length);
        });
    }
    void collect() {
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
        wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    }
    JSContextWrapper wrapper;
    NativeWebScope scope;
    std::shared_ptr<Valdi::WebState> state;
};
} // namespace

class NativeWebReceiveBufferTests : public JSBridgeTestFixture {};

TEST_P(NativeWebReceiveBufferTests, HttpValueEntryKeepsKnownExternalPointerThroughDefaultReadAndGc) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startHttp();
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, false);
    ASSERT_TRUE(fixture.wrapper.evaluateScript("received !== undefined").toBool());
    fixture.expectPointer(original + 1, 3);
    EXPECT_EQ(releases.load(), 0);
    EXPECT_TRUE(fixture.wrapper.evaluateScript("(received[1] = 90, received[0] === 65 && received[1] === 90)").toBool());
    fixture.collect();
    EXPECT_EQ(releases.load(), 0);
    fixture.wrapper.evaluateScript("(received = undefined, true)");
    fixture.collect();
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebReceiveBufferTests, SocketValueEntryKeepsKnownExternalPointerThroughMessageAndShutdown) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startSocket();
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, true);
    ASSERT_TRUE(fixture.wrapper.evaluateScript("received instanceof ArrayBuffer").toBool());
    fixture.expectPointer(original, 5);
    EXPECT_EQ(releases.load(), 0);
    fixture.wrapper.withContext([&](auto& cx, auto&) { ReceiveFixtureNativeWeb::shutdown(cx); });
    fixture.collect();
    EXPECT_EQ(releases.load(), 0);
    EXPECT_TRUE(fixture.wrapper.evaluateScript("new Uint8Array(received)[2] === 66").toBool());
    fixture.wrapper.evaluateScript("(received = undefined, true)");
    fixture.collect();
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebReceiveBufferTests, HttpValueEntryUsesNormalByobCopyAndDetachesDestination) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    fixture.wrapper.evaluateScript("(globalThis.byobBuffer = new ArrayBuffer(8), true)");
    const uint8_t* destination = nullptr;
    fixture.wrapper.withContext([&](auto& cx, auto& ex) {
        auto buffer = cx.getObjectProperty(cx.getGlobalObject(ex).get(), "byobBuffer", ex);
        NativeWebBufferInfo info;
        ASSERT_TRUE(fixture.state->support->getWebBufferInfo(buffer.get(), info, ex));
        destination = info.data;
    });
    auto id = fixture.startHttp(R"JS(
        globalThis.received = undefined;
        fetch('http://127.0.0.1:1/fixture').then(r => {
            globalThis.response = r;
            globalThis.reader = r.body.getReader({mode: 'byob'});
            reader.read(new Uint8Array(byobBuffer, 2, 4)).then(r => { received = r.value; });
        });
    )JS");
    EXPECT_TRUE(fixture.wrapper.evaluateScript("byobBuffer.byteLength === 0").toBool());
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, false);
    fixture.expectPointer(destination + 2, 3);
    EXPECT_NE(original + 1, destination + 2);
    EXPECT_TRUE(fixture.wrapper.evaluateScript("Array.from(received).join(',') === '65,66,67'").toBool());
    fixture.collect();
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebReceiveBufferTests, HttpValueEntryRetainsResponseCloneIndependentBytes) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startHttp(R"JS(
        globalThis.received = undefined;
        globalThis.cloned = undefined;
        globalThis.cloneError = '';
        globalThis.readError = '';
        globalThis.setupError = '';
        fetch('http://127.0.0.1:1/fixture').then(r => {
            globalThis.response = r;
            r.clone().arrayBuffer().then(b => { cloned = new Uint8Array(b); },
                e => { cloneError = String(e); });
            r.body.getReader().read().then(r => { received = r.value; },
                e => { readError = String(e); });
        }).catch(e => { setupError = String(e); });
    )JS");
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, false);
    fixture.wrapper.withContext([&](auto&, auto& ex) {
        ASSERT_TRUE(fixture.state->enter(ex));
        NshWebHttpEvent end{};
        end.id = id;
        end.event = 3;
        EXPECT_EQ(nsh_web_http_event(fixture.state->rust, &end), 0);
        fixture.state->leave(ex);
    });
    auto diagnostics = fixture.wrapper.evaluateScript(R"JS(JSON.stringify({
        queueMicrotask: typeof queueMicrotask,
        received: typeof received, cloned: typeof cloned,
        cloneError, readError, setupError,
        responseLocked: response.body.locked, responseUsed: response.bodyUsed
    }))JS").toStringBox().slowToString();
    ASSERT_TRUE(fixture.wrapper.evaluateScript("received !== undefined && cloned !== undefined").toBool()) << diagnostics;
    EXPECT_TRUE(fixture.wrapper.evaluateScript(R"JS((() => {
        if (Array.from(received).join(',') !== '65,66,67' ||
            Array.from(cloned).join(',') !== '65,66,67') return false;
        received[0] = 90;
        return cloned[0] === 65;
    })())JS").toBool());
}

TEST_P(NativeWebReceiveBufferTests, MissingIdsAndAbortedFetchDropExternalOwnershipWithoutDelivery) {
    for (bool socket : {false, true}) {
        SCOPED_TRACE(socket);
        std::atomic<int> releases{0};
        ReceiveFixture fixture(getJsBridge());
        fixture.wrapper.evaluateScript("(globalThis.received = undefined, true)");
        const uint8_t* original = nullptr;
        fixture.receive(UINT32_MAX, releases, original, socket);
        fixture.collect();
        EXPECT_TRUE(fixture.wrapper.evaluateScript("received === undefined").toBool());
        EXPECT_EQ(releases.load(), 1);
    }
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startHttp(R"JS(
        globalThis.received = undefined;
        globalThis.abort = new AbortController();
        globalThis.reason = new Error('aborted fixture');
        globalThis.rejected = false;
        fetch('http://127.0.0.1:1/fixture', {signal: abort.signal}).then(r => {
            globalThis.response = r;
            r.body.getReader().read().then(r => { received = r.value; },
                e => { rejected = e === reason; });
        });
    )JS");
    fixture.wrapper.evaluateScript("(abort.abort(reason), true)");
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, false);
    fixture.collect();
    EXPECT_TRUE(fixture.wrapper.evaluateScript("received === undefined && rejected").toBool());
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebReceiveBufferTests, SocketValueEntryPreservesBlobBinaryTypeSnapshot) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startSocket("blob");
    const uint8_t* original = nullptr;
    fixture.receive(id, releases, original, true);
    EXPECT_TRUE(fixture.wrapper.evaluateScript("received instanceof Blob && received.size === 5").toBool());
    fixture.wrapper.evaluateScript("received.arrayBuffer().then(b => { globalThis.blobBytes = Array.from(new Uint8Array(b)).join(','); })");
    EXPECT_EQ(fixture.wrapper.evaluateScript("blobBytes").toStringBox().slowToString(), "0,65,66,67,0");
    fixture.collect();
    EXPECT_EQ(releases.load(), 1);
}

TEST_P(NativeWebReceiveBufferTests, OwnedEventPumpAdoptsPointerAndShutdownReleasesBoundedBacklog) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startHttp();
    const uint8_t* first = nullptr;
    fixture.wrapper.withContext([&](auto&, auto& ex) {
        for (size_t i = 0; i < 33; ++i) {
            auto* allocation = new DescriptorAllocation{releases};
            if (i == 0) {
                first = allocation->bytes;
            }
            Valdi::Event event;
            event.id = id;
            event.kind = NSH_TRANSPORT_HTTP_DATA;
            event.ownedBytes = makeShared<Valdi::OwnedReceiveBytes>(NshTransportOwnedBytes{
                allocation->bytes, sizeof(allocation->bytes), allocation, &DescriptorAllocation::release});
            fixture.state->events.push_back(std::move(event));
        }
        fixture.state->pump(ex);
        EXPECT_EQ(fixture.state->events.size(), 1u);
    });
    fixture.expectPointer(first, 3);
    fixture.collect();
    EXPECT_EQ(releases.load(), 0);
    fixture.wrapper.withContext([](auto& cx, auto&) { ReceiveFixtureNativeWeb::shutdown(cx); });
    fixture.collect();
    // Last queued native event and 31 unread Rust stream chunks are gone.
    // The delivered first JS view remains valid after all Web state is gone.
    EXPECT_EQ(releases.load(), 32);
    EXPECT_TRUE(fixture.wrapper.evaluateScript("received[1] === 66").toBool());
    fixture.wrapper.evaluateScript("(received = undefined, true)");
    fixture.collect();
    EXPECT_EQ(releases.load(), 33);
}

TEST_P(NativeWebReceiveBufferTests, OwnedEmptySocketMessagesUseDistinctBuffersAndReleaseEveryDescriptor) {
    std::atomic<int> releases{0};
    ReceiveFixture fixture(getJsBridge());
    auto id = fixture.startSocket();
    fixture.wrapper.evaluateScript("(globalThis.emptyMessages = [], ws.onmessage = e => emptyMessages.push(e.data), true)");
    fixture.wrapper.withContext([&](auto&, auto& ex) {
        for (size_t i = 0; i < 2; ++i) {
            auto* allocation = new DescriptorAllocation{releases};
            Valdi::Event event;
            event.socket = true;
            event.id = id;
            event.kind = NSH_TRANSPORT_WS_MESSAGE_BINARY;
            event.ownedBytes = makeShared<Valdi::OwnedReceiveBytes>(NshTransportOwnedBytes{
                allocation->bytes, 0, allocation, &DescriptorAllocation::release});
            fixture.state->events.push_back(std::move(event));
        }
        fixture.state->pump(ex);
    });
    EXPECT_TRUE(fixture.wrapper.evaluateScript(R"JS(emptyMessages.length === 2 &&
        emptyMessages[0] instanceof ArrayBuffer && emptyMessages[1] instanceof ArrayBuffer &&
        emptyMessages[0] !== emptyMessages[1] &&
        emptyMessages[0].byteLength === 0 && emptyMessages[1].byteLength === 0)JS").toBool());
    EXPECT_EQ(releases.load(), 2);
    fixture.collect();
    EXPECT_EQ(releases.load(), 2);
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebReceiveBufferTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
