#include "valdi/test/native_web/NativeWebTestUtils.hpp"

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

namespace ValdiTest {
using namespace Valdi;

class UploadServer {
  public:
    UploadServer() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 1) != 0) {
            return;
        }
        socklen_t size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
        thread = std::thread([this] {
            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(listener, &reads);
            timeval deadline{5, 0};
            if (select(listener + 1, &reads, nullptr, nullptr, &deadline) <= 0) {
                return;
            }
            int client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            timeval timeout{3, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#if defined(__APPLE__)
            int yes = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            std::string wire;
            char buffer[4096];
            bool complete = false;
            while (!stopped.load() && wire.size() < 65536 && !complete) {
                auto count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0)
                    break;
                wire.append(buffer, static_cast<size_t>(count));
                auto body = wire.find("\r\n\r\n");
                if (body == std::string::npos)
                    continue;
                body += 4;
                std::string decoded;
                size_t offset = body;
                while (offset < wire.size()) {
                    auto line = wire.find("\r\n", offset);
                    if (line == std::string::npos)
                        break;
                    auto chunk = wire.substr(offset, line - offset);
                    char* end = nullptr;
                    auto bytes = std::strtoul(chunk.c_str(), &end, 16);
                    if (end == chunk.c_str() || *end != 0 || bytes > 65536)
                        break;
                    offset = line + 2;
                    if (bytes == 0) {
                        complete = true;
                        break;
                    }
                    if (bytes + 2 > wire.size() - offset)
                        break;
                    decoded.append(wire, offset, bytes);
                    offset += bytes + 2;
                }
                if (complete)
                    uploaded = std::move(decoded);
            }
            if (complete) {
                received.store(true);
                std::string response =
                    "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + std::to_string(uploaded.size()) +
                    "\r\n\r\n" + uploaded;
                std::string_view remaining(response);
                while (!remaining.empty()) {
#if defined(__linux__)
                    auto count = send(client, remaining.data(), remaining.size(), MSG_NOSIGNAL);
#else
                    auto count = send(client, remaining.data(), remaining.size(), 0);
#endif
                    if (count <= 0)
                        break;
                    remaining.remove_prefix(static_cast<size_t>(count));
                }
            }
            close(client);
        });
    }
    ~UploadServer() {
        stopped = true;
        if (listener >= 0) {
            shutdown(listener, SHUT_RDWR);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (listener >= 0) {
            close(listener);
        }
    }
    uint16_t port = 0;
    std::atomic<bool> received{false};
    std::string uploaded;

  private:
    int listener = -1;
    std::atomic<bool> stopped{false};
    std::thread thread;
};

class NativeWebUploadTests : public JSBridgeTestFixture {};

TEST_P(NativeWebUploadTests, ReadableUploadDemandSendsAllChunksAndFinishToRealHttpServer) {
    UploadServer server;
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.uploadResult = ''; globalThis.pulls = 0; "
                           "const stream = new ReadableStream({pull(controller) { "
                           "if (pulls === 0) controller.enqueue(new Uint8Array([117,112])); "
                           "else if (pulls === 1) controller.enqueue(new Uint8Array([108,111,97,100])); "
                           "else controller.close(); pulls++; }}); "
                           "fetch('http://127.0.0.1:" +
                           std::to_string(server.port) + R"JS(/upload', {
            method: 'POST', body: stream, duplex: 'half'
        }).then(response => response.text()).then(text => { uploadResult = text; },
            error => { uploadResult = 'ERROR:' + error; });
        return true;
    })())JS");
    ASSERT_TRUE(waitForWeb(wrapper, "uploadResult.length > 0"));
    EXPECT_EQ(wrapper.evaluateScript("uploadResult").toStringBox().slowToString(), "upload");
    ASSERT_TRUE(server.received.load());
    EXPECT_EQ(server.uploaded, "upload");
    EXPECT_EQ(wrapper.evaluateScript("pulls").toInt(), 3);
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebUploadTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
#endif
