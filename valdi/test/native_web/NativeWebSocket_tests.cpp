#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
using WebSocketPPServer = websocketpp::server<websocketpp::config::asio>;
#include "valdi/test/native_web/NativeWebTestUtils.hpp"

#include <atomic>
#include <thread>

namespace ValdiTest {
using namespace Valdi;

class WebSocketEchoServer {
  public:
    WebSocketEchoServer() {
        server.clear_access_channels(websocketpp::log::alevel::all);
        server.clear_error_channels(websocketpp::log::elevel::all);
        server.set_validate_handler([this](websocketpp::connection_hdl handle) {
            websocketpp::lib::error_code ec;
            auto connection = server.get_con_from_hdl(handle, ec);
            if (ec) {
                return false;
            }
            connection->select_subprotocol("nsh-test", ec);
            return !ec;
        });
        server.set_message_handler([this](websocketpp::connection_hdl handle, WebSocketPPServer::message_ptr message) {
            websocketpp::lib::error_code ec;
            server.send(handle, message->get_payload(), message->get_opcode(), ec);
            if (!ec) {
                ++echoes;
            }
        });
        server.set_close_handler([this](websocketpp::connection_hdl handle) {
            websocketpp::lib::error_code ec;
            auto connection = server.get_con_from_hdl(handle, ec);
            if (!ec) {
                closeCode = connection->get_remote_close_code();
            }
        });
        websocketpp::lib::error_code ec;
        server.init_asio(ec);
        if (ec) {
            return;
        }
        server.listen(websocketpp::lib::asio::ip::tcp::endpoint(websocketpp::lib::asio::ip::address_v4::loopback(), 0),
                      ec);
        if (ec) {
            return;
        }
        port = server.get_local_endpoint(ec).port();
        if (ec) {
            port = 0;
            return;
        }
        server.start_accept(ec);
        if (ec) {
            port = 0;
            return;
        }
        thread = std::thread([this] { server.run(); });
    }
    ~WebSocketEchoServer() {
        websocketpp::lib::error_code ec;
        server.stop_listening(ec);
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    uint16_t port = 0;
    std::atomic<int> echoes{0};
    std::atomic<uint16_t> closeCode{0};

  private:
    WebSocketPPServer server;
    std::thread thread;
};

class NativeWebSocketTests : public JSBridgeTestFixture {};

TEST_P(NativeWebSocketTests, RealTextBinarySubviewProtocolAndCleanClose) {
    WebSocketEchoServer server;
    ASSERT_NE(server.port, 0);
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    NativeWebModuleFactory factory;
    wrapper.withContext([&](auto& cx, auto& ex) { loadAndInstallNativeWeb(factory, cx, ex); });
    wrapper.evaluateScript("(() => { globalThis.done = false; globalThis.messages = []; "
                           "globalThis.socketError = ''; let constructed = false; "
                           "globalThis.ws = new WebSocket('ws://127.0.0.1:" +
                           std::to_string(server.port) + R"JS(/echo', ['nsh-test']);
        ws.binaryType = 'arraybuffer';
        ws.onopen = () => {
            globalThis.openState = constructed && ws.readyState === WebSocket.OPEN && ws.protocol === 'nsh-test';
            ws.send('client text');
            const binary = new Uint8Array([0, 3, 4, 0]);
            ws.send(binary.subarray(1, 3));
            binary[1] = 99; // send owns a snapshot before caller mutation.
        };
        ws.onmessage = event => {
            messages.push(typeof event.data === 'string' ? event.data :
                Array.from(new Uint8Array(event.data)).join(','));
            if (messages.length === 2) ws.close(1000, 'done');
        };
        ws.onerror = event => { socketError += event.message || 'error'; };
        ws.onclose = event => { globalThis.closeResult = event.code + ':' + event.reason + ':' + event.wasClean; done = true; };
        constructed = true;
        return ws.readyState === WebSocket.CONNECTING;
    })())JS");
    ASSERT_TRUE(waitForWeb(wrapper, "done"));
    EXPECT_TRUE(wrapper.evaluateScript("openState").toBool());
    EXPECT_EQ(wrapper.evaluateScript("messages.join('|')").toStringBox().slowToString(), "client text|3,4");
    EXPECT_EQ(wrapper.evaluateScript("closeResult").toStringBox().slowToString(), "1000:done:true");
    EXPECT_EQ(wrapper.evaluateScript("socketError").toStringBox().slowToString(), "");
    EXPECT_TRUE(wrapper.evaluateScript("ws.readyState === WebSocket.CLOSED").toBool());
    EXPECT_EQ(server.echoes.load(), 2);
    EXPECT_EQ(server.closeCode.load(), 1000);
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebSocketTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
