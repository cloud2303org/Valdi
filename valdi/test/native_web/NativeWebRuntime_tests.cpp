#include "valdi/test/integration/JSBridgeTestFixture.hpp"
#include "valdi/test/integration/RuntimeTestsUtils.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/runtime/JavaScript/JavaScriptFunctionCallContext.hpp"
#include "valdi_core/cpp/Resources/StaticResourceRegistry.hpp"
#include "third-party/nsh_web/runtime_test_assets.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ValdiTest {
using namespace Valdi;

namespace {
bool registerRuntimeAssets() {
    struct Asset {
        std::string basename;
        std::vector<uint8_t> bytes;
    };
    // The registry borrows both pointers for the process lifetime.
    static std::vector<Asset> assets;
    const char* sourceDirectory = std::getenv("TEST_SRCDIR");
    const char* workspace = std::getenv("TEST_WORKSPACE");
    if (sourceDirectory == nullptr || workspace == nullptr) {
        ADD_FAILURE() << "Bazel runfile environment is missing";
        return false;
    }
    for (const char* shortPath : NativeWebTestAssets::paths) {
        std::string relative(shortPath);
        std::string path = std::string(sourceDirectory) + "/" +
                           (relative.rfind("../", 0) == 0 ? relative.substr(3) : std::string(workspace) + "/" + relative);
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            ADD_FAILURE() << "Could not load actual Node-compiled runtime module: " << path;
            return false;
        }
        Asset asset;
        asset.basename = relative.substr(relative.find_last_of('/') + 1);
        asset.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (asset.bytes.empty()) {
            ADD_FAILURE() << "Empty runtime module archive: " << path;
            return false;
        }
        assets.push_back(std::move(asset));
    }
    for (const auto& asset : assets) {
        StaticResourceRegistry::sharedInstance().registerResource(
            asset.basename.c_str(), asset.bytes.data(), asset.bytes.size(), false);
    }
    return true;
}

struct PartialTeardownProgress {
    std::mutex mutex;
    std::condition_variable changed;
    bool enteredPull = false;
    bool resumePull = false;
    bool gateTimedOut = false;
    bool partialReturned = false;
    bool completed = false;
    bool jsThreadExited = false;
    std::string result;
};

// Hermes owns a dedicated native JS thread. Its thread-local destructor gives
// the test a completion barrier after the teardown task and its strong runtime
// capture have both been destroyed. A queue barrier would be discarded when
// partial teardown disposes the queue.
struct RuntimeThreadExit {
    std::weak_ptr<PartialTeardownProgress> progress;
    ~RuntimeThreadExit() {
        if (auto state = progress.lock()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->jsThreadExited = true;
            state->changed.notify_all();
        }
    }
};
thread_local RuntimeThreadExit runtimeThreadExit;

#if defined(__APPLE__) || defined(__linux__)
// Like the native_web HTTP fixtures, use a real loopback socket. Hold the
// entire response until the initiating JS entry has returned to force a
// background transport notification through the real runtime scheduler.
class BackgroundFetchServer {
public:
    BackgroundFetchServer() {
        _listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (_listener < 0 || bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(_listener, 1) != 0) {
            return;
        }
        socklen_t length = sizeof(address);
        if (getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return;
        }
        port = ntohs(address.sin_port);
        _thread = std::thread([this] {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(_listener, &readSet);
            timeval timeout{5, 0};
            if (select(_listener + 1, &readSet, nullptr, nullptr, &timeout) <= 0) {
                return;
            }
            int client = accept(_listener, nullptr, nullptr);
            if (client < 0) {
                return;
            }
#if defined(__APPLE__)
            int yes = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            timeval ioTimeout{2, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &ioTimeout, sizeof(ioTimeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &ioTimeout, sizeof(ioTimeout));
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
                auto count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    break;
                }
                request.append(buffer, static_cast<size_t>(count));
            }
            {
                std::unique_lock<std::mutex> lock(_mutex);
                if (_changed.wait_for(lock, std::chrono::seconds(5), [&] { return _reply || _stop; }) &&
                    !_stop && request.find("\r\n\r\n") != std::string::npos) {
                    std::string_view response =
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 14\r\nConnection: close\r\n\r\nownerless wake";
                    while (!response.empty()) {
#if defined(__linux__)
                        auto count = send(client, response.data(), response.size(), MSG_NOSIGNAL);
#else
                        auto count = send(client, response.data(), response.size(), 0);
#endif
                        if (count <= 0) {
                            break;
                        }
                        response.remove_prefix(static_cast<size_t>(count));
                    }
                }
            }
            close(client);
        });
    }

    ~BackgroundFetchServer() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stop = true;
            _changed.notify_all();
        }
        if (_listener >= 0) {
            shutdown(_listener, SHUT_RDWR);
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        if (_listener >= 0) {
            close(_listener);
        }
    }

    void reply() {
        std::lock_guard<std::mutex> lock(_mutex);
        _reply = true;
        _changed.notify_all();
    }

    uint16_t port = 0;

private:
    int _listener = -1;
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _reply = false;
    bool _stop = false;
    std::thread _thread;
};
#endif
} // namespace

class NativeWebRuntimeTests : public JSBridgeTestFixture {
protected:
    void SetUp() override {
        JSBridgeTestFixture::SetUp();
        if (!IsSkipped()) {
            static const bool assetsReady = registerRuntimeAssets();
            ASSERT_TRUE(assetsReady);
            wrapper = RuntimeWrapper(getJsBridge(), TSNMode::Disabled);
            runtime = wrapper.runtime->getJavaScriptRuntime();
        }
    }

    void TearDown() override {
        wrapper.teardown();
    }

    RuntimeWrapper wrapper;
    JavaScriptRuntime* runtime = nullptr;
};

#if defined(__APPLE__) || defined(__linux__)
TEST_P(NativeWebRuntimeTests, BackgroundFetchWakesRealRuntimeAndResolvesNativePromise) {
    BackgroundFetchServer server;
    ASSERT_NE(server.port, 0);
    struct Progress {
        std::mutex mutex;
        std::condition_variable changed;
        bool completed = false;
        std::string result;
    };
    auto state = std::make_shared<Progress>();
    runtime->dispatchSynchronouslyOnJsThread(STRING_LITERAL("test.installFetchCompletion"), [state](auto& entry) {
        auto& cx = entry.jsContext;
        auto& ex = entry.exceptionTracker;
        auto complete = cx.newFunction(
            makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(), [state](auto& call) {
                auto result = call.getParameterAsString(0).slowToString();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(result);
                state->completed = true;
                state->changed.notify_all();
                return call.getContext().newUndefined();
            }),
            ex);
        auto global = cx.getGlobalObject(ex);
        cx.setObjectProperty(global.get(), "__nativeWebFetchComplete", complete.get(), ex);
        ASSERT_TRUE(ex);
    });
    auto script = "const web = global.require('native_web/src/NativeWeb'); "
                  "if (typeof fetch !== 'undefined' || typeof Response !== 'undefined') "
                  "throw Error('Import installed Web globals'); "
                  "web.fetch('http://127.0.0.1:" + std::to_string(server.port) + R"JS(/background').then(async response => {
        const body = await response.text();
        __nativeWebFetchComplete(response.status + ':' + response.headers.get('content-type') + ':' + body);
    }).catch(error => __nativeWebFetchComplete('ERROR:' + error));
    return 'started';
    )JS";
    auto started = runtime->evaluateScript(makeShared<ByteBuffer>(script)->toBytesView(),
                                          STRING_LITERAL("native-web-background-fetch.js"));
    ASSERT_TRUE(started) << started.description();
    ASSERT_EQ("started", started.value().toStringBox().slowToString());
    server.reply();
    std::unique_lock<std::mutex> lock(state->mutex);
    ASSERT_TRUE(state->changed.wait_for(lock, std::chrono::seconds(10), [&] { return state->completed; }))
        << "Background fetch never completed on the real JS runtime";
    EXPECT_EQ("200:text/plain:ownerless wake", state->result);
}
#endif

TEST_P(NativeWebRuntimeTests, PartialTeardownReturnsWhileRustReadIsInFlightAndCleansUpOnJsThread) {
    auto loaded = runtime->evaluateScript(
        makeShared<ByteBuffer>("global.require('native_web/src/NativeWeb').installNativeWeb(); return typeof ReadableStream;")->toBytesView(),
        STRING_LITERAL("native-web-runtime.js"));
    ASSERT_TRUE(loaded) << loaded.description();
    ASSERT_EQ("function", loaded.value().toStringBox().slowToString());

    auto state = std::make_shared<PartialTeardownProgress>();
    runtime->dispatchSynchronouslyOnJsThread(STRING_LITERAL("test.installGate"), [state](auto& entry) {
        auto& cx = entry.jsContext;
        auto& ex = entry.exceptionTracker;
        auto gate = cx.newFunction(
            makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(), [state](auto& call) {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->enteredPull = true;
                state->changed.notify_all();
                state->gateTimedOut = !state->changed.wait_for(
                    lock, std::chrono::seconds(10), [&] { return state->resumePull; });
                return call.getContext().newUndefined();
            }),
            ex);
        auto complete = cx.newFunction(
            makeShared<JSFunctionWithCallable>(ReferenceInfoBuilder(), [state](auto& call) {
                auto result = call.getParameterAsString(0).slowToString();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(result);
                state->completed = true;
                state->changed.notify_all();
                return call.getContext().newUndefined();
            }),
            ex);
        auto global = cx.getGlobalObject(ex);
        cx.setObjectProperty(global.get(), "__nativeWebGate", gate.get(), ex);
        cx.setObjectProperty(global.get(), "__nativeWebComplete", complete.get(), ex);
        ASSERT_TRUE(ex);
    });

    runtime->dispatchOnJsThread(
        STRING_LITERAL("test.inFlightRustRead"), JavaScriptTaskScheduleTypeAlwaysAsync, 0, [state](auto& entry) {
            runtimeThreadExit.progress = state;
            entry.jsContext.evaluate(R"JS(
                const stream = new ReadableStream({
                    pull(controller) {
                        __nativeWebGate();
                        controller.enqueue('chunk');
                        controller.close();
                    }
                });
                stream.getReader().read().then(
                    result => __nativeWebComplete(result.value + ':' + result.done),
                    error => __nativeWebComplete('ERROR:' + error));
            )JS", "native-web-partial-teardown.js", entry.exceptionTracker);
            EXPECT_TRUE(entry.exceptionTracker);
        });

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        ASSERT_TRUE(state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return state->enteredPull; }));
    }
    std::thread disposer([this, state] {
        runtime->partialTeardown();
        std::lock_guard<std::mutex> lock(state->mutex);
        state->partialReturned = true;
        state->changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        EXPECT_TRUE(state->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return state->partialReturned; }))
            << "Off-JS partial teardown waited for an active Rust-to-JS callback";
        // Resume the actual Rust read, even on RED, so failure cannot leave the
        // test's native callback blocked forever.
        state->resumePull = true;
        state->changed.notify_all();
    }
    disposer.join();
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        EXPECT_TRUE(state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return state->jsThreadExited; }));
        EXPECT_FALSE(state->gateTimedOut);
        EXPECT_TRUE(state->completed);
        EXPECT_EQ("chunk:false", state->result);
    }

    bool enteredAfterTeardown = false;
    runtime->dispatchSynchronouslyOnJsThread(STRING_LITERAL("test.afterTeardown"), [&](auto&) {
        enteredAfterTeardown = true;
    });
    EXPECT_FALSE(enteredAfterTeardown);

    // Match Runtime_tests.cpp::makeRuntimeForTermination: application termination
    // intentionally keeps the VM/runtime alive through process exit. A separate
    // pre-fix probe without a Web import confirms this baseline retention. Keep
    // the terminated wrapper reachable; this case does not prove old-VM
    // destruction or context-address reuse.
    static auto* terminatedRuntimes = new std::vector<std::unique_ptr<RuntimeWrapper>>();
    terminatedRuntimes->push_back(std::make_unique<RuntimeWrapper>(wrapper));
    wrapper = RuntimeWrapper();
    runtime = nullptr;

    // A separate normally owned runtime can still load and use NativeWeb.
    wrapper = RuntimeWrapper(getJsBridge(), TSNMode::Disabled);
    runtime = wrapper.runtime->getJavaScriptRuntime();
    auto fresh = runtime->evaluateScript(
        makeShared<ByteBuffer>(
            "const web = global.require('native_web/src/NativeWeb'); return new web.Response('fresh').status;")
            ->toBytesView(),
        STRING_LITERAL("native-web-fresh-runtime.js"));
    ASSERT_TRUE(fresh) << fresh.description();
    EXPECT_EQ(200, fresh.value().toInt());
}

INSTANTIATE_TEST_SUITE_P(NativeWebRuntime,
                         NativeWebRuntimeTests,
                         ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
