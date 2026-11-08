#include "valdi/test/native_web/NativeWebTestUtils.hpp"

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace ValdiTest {
using namespace Valdi;

// Real loopback HTTP, with head/body separation controlled by the test. No JS
// transport fake and no implementation assertions about Rust queues.
class HeadBeforeBodyServer {
  public:
    explicit HeadBeforeBodyServer(std::string prefix = {}, std::string suffix = "hello",
                                  std::string contentType = "text/plain") {
        _listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (_listener < 0 || bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(_listener, 1) != 0) {
            if (_listener >= 0) {
                close(_listener);
            }
            _listener = -1;
            return;
        }
        socklen_t length = sizeof(address);
        getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        _thread = std::thread([this, prefix = std::move(prefix), suffix = std::move(suffix),
                               contentType = std::move(contentType)] {
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
            // Wait for a complete request before replying (bounded socket read).
            timeval readTimeout{2, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &readTimeout, sizeof(readTimeout));
            char request[4096];
            if (recv(client, request, sizeof(request), 0) > 0) {
                sendBytes(client, "HTTP/1.1 200 OK\r\nContent-Length: " +
                                      std::to_string(prefix.size() + suffix.size()) + "\r\nContent-Type: " +
                                      contentType + "\r\nConnection: close\r\n\r\n");
                sendBytes(client, prefix);
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!releaseBody.load() && !_stop.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (!_stop.load())
                    sendBytes(client, suffix);
            }
            close(client);
        });
    }
    ~HeadBeforeBodyServer() {
        _stop = true;
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
    uint16_t port = 0;
    std::atomic<bool> releaseBody{false};

  private:
    int _listener = -1;
    std::atomic<bool> _stop{false};
    std::thread _thread;
    static void sendBytes(int socket, std::string_view bytes) {
        while (!bytes.empty()) {
#if defined(__linux__)
            auto written = send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
            auto written = send(socket, bytes.data(), bytes.size(), 0);
#endif
            if (written <= 0) {
                return;
            }
            bytes.remove_prefix(static_cast<size_t>(written));
        }
    }
};

class NativeWebHttpTests : public JSBridgeTestFixture {};

TEST_P(NativeWebHttpTests, FetchResolvesAtHeadersAndBodyStreamsOnJsScheduler) {
    HeadBeforeBodyServer server;
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.head = false; globalThis.body = ''; "
                           "fetch('http://127.0.0.1:" +
                           std::to_string(server.port) + R"JS(/').then(response => {
            head = response.status === 200;
            return response.text();
        }).then(text => { body = text; }, error => { body = 'ERROR:' + error; });
        return true;
    })())JS");
    auto waitFor = [&](const char* expression) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            wrapper.taskScheduler->taskQueue->flushUpToNow();
            if (wrapper.evaluateScript(expression).toBool()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    ASSERT_TRUE(waitFor("head"));
    EXPECT_EQ(wrapper.evaluateScript("body").toStringBox().slowToString(), "");
    server.releaseBody = true;
    ASSERT_TRUE(waitFor("body.length > 0"));
    EXPECT_EQ(wrapper.evaluateScript("body").toStringBox().slowToString(), "hello");
}

TEST_P(NativeWebHttpTests, AbortAfterHeadersRejectsRealPendingBodyWithOriginalReason) {
    HeadBeforeBodyServer server;
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.head = false; globalThis.abortResult = ''; "
                           "globalThis.abort = new AbortController(); globalThis.reason = new Error('stop body'); "
                           "fetch('http://127.0.0.1:" +
                           std::to_string(server.port) + R"JS(/', {signal: abort.signal}).then(response => {
            head = true;
            return response.text();
        }).then(() => { abortResult = 'FULFILLED'; }, error => { abortResult = error === reason ? 'same reason' : String(error); });
        return true;
    })())JS");
    ASSERT_TRUE(waitForWeb(wrapper, "head"));
    wrapper.evaluateScript("(abort.abort(reason), true)");
    ASSERT_TRUE(waitForWeb(wrapper, "abortResult.length > 0"));
    EXPECT_EQ(wrapper.evaluateScript("abortResult").toStringBox().slowToString(), "same reason");
    server.releaseBody = true;
}

TEST_P(NativeWebHttpTests, ShutdownDiscardsQueuedAndLateBodyDeliveryBeforeVmDestruction) {
    HeadBeforeBodyServer server;
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.head = false; globalThis.bodyCompleted = false; "
                           "fetch('http://127.0.0.1:" +
                           std::to_string(server.port) + R"JS(/').then(response => {
            head = true;
            return response.text();
        }).then(() => { bodyCompleted = true; }, () => { bodyCompleted = true; });
        return true;
    })())JS");
    ASSERT_TRUE(waitForWeb(wrapper, "head"));
    wrapper.withContext([](auto& cx, auto&) { NativeWeb::shutdown(cx); });
    server.releaseBody = true;
    wrapper.taskScheduler->taskQueue->flushUpToNow();
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.taskScheduler->taskQueue->flushUpToNow();
    EXPECT_FALSE(wrapper.evaluateScript("bodyCompleted").toBool());
}

TEST_P(NativeWebHttpTests, RealSseSplitUtf8SurvivesCollectionBetweenTransportChunks) {
    // Release the final UTF-8 bytes only after JS has decoded the first chunk.
    // This forces a genuine transport boundary inside one four-byte scalar.
    HeadBeforeBodyServer server("data: \xF0\x9F", "\x98\x80\n\ndata: end\n\n", "text/event-stream");
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.sse = ''; globalThis.sseDone = false; globalThis.sseError = ''; "
                           "fetch('http://127.0.0.1:" + std::to_string(server.port) + R"JS(/').then(async response => {
        const reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
        for (;;) {
            const chunk = await reader.read();
            if (chunk.done) break;
            sse += chunk.value;
        }
        sseDone = true;
    }).catch(error => { sseError = String(error); sseDone = true; });
    return true;
    })())JS");
    ASSERT_TRUE(waitForWeb(wrapper, "sse === 'data: ' || sseDone"));
    EXPECT_FALSE(wrapper.evaluateScript("sseDone").toBool());
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    wrapper.withContext([](auto& cx, auto&) { cx.garbageCollect(); });
    server.releaseBody = true;
    ASSERT_TRUE(waitForWeb(wrapper, "sseDone"));
    EXPECT_EQ(wrapper.evaluateScript("sseError").toStringBox().slowToString(), "");
    EXPECT_TRUE(wrapper.evaluateScript("sse === 'data: \\u{1f600}\\n\\ndata: end\\n\\n'").toBool());
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebHttpTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
#endif
