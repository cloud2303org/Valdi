#include "valdi/test/native_web/NativeWebTestUtils.hpp"

namespace ValdiTest {
using namespace Valdi;

class NativeWebInstallTests : public JSBridgeTestFixture {};

namespace {
void importWeb(JSContextWrapper& wrapper) {
    wrapper.withContext([](auto& cx, auto& ex) {
        NativeWebModuleFactory factory;
        auto web = factory.loadModule(cx, ReferenceInfoBuilder(), ex);
        ASSERT_TRUE(ex);
        cx.setObjectProperty(cx.getGlobalObject(ex).get(), "web", web.get(), ex);
    });
}
} // namespace

TEST_P(NativeWebInstallTests, ImportPreservesGlobalsAndNamedExportsWorkBeforeInstallation) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    wrapper.evaluateScript(R"JS((() => {
        globalThis.originalFetch = function originalFetch() {};
        globalThis.fetch = originalFetch;
        globalThis.originalEncoder = function originalEncoder() {};
        globalThis.TextEncoder = originalEncoder;
        return true;
    })())JS");
    importWeb(wrapper);
    EXPECT_TRUE(wrapper.evaluateScript("fetch === originalFetch").toBool());
    EXPECT_TRUE(wrapper.evaluateScript("TextEncoder === originalEncoder").toBool());
    EXPECT_EQ(wrapper.evaluateScript("typeof Response").toStringBox().slowToString(), "undefined");
    EXPECT_EQ(wrapper.evaluateScript("typeof WebSocket").toStringBox().slowToString(), "undefined");
    EXPECT_EQ(wrapper.evaluateScript("typeof DOMException").toStringBox().slowToString(), "undefined");
    wrapper.evaluateScript(R"JS((() => {
        globalThis.result = '';
        globalThis.writableDone = false;
        const controller = new web.AbortController();
        const request = new web.Request('http://localhost/', {signal: controller.signal});
        const clone = request.clone();
        controller.abort();
        if (!request.signal.aborted || !clone.signal.aborted ||
            request.signal.reason.name !== 'AbortError') throw Error('named abort');
        const writable = new web.WritableStream();
        writable.abort().then(() => { writableDone = true; });
        new web.Response('named').text().then(text => { result += text; });
        return true;
    })())JS");
    EXPECT_EQ(wrapper.evaluateScript("result").toStringBox().slowToString(), "named");
    EXPECT_TRUE(wrapper.evaluateScript("writableDone").toBool());
}

TEST_P(NativeWebInstallTests, ExplicitInstallationUsesOriginalIdentitiesAndIsRepeatSafe) {
    JSContextWrapper wrapper(getJsBridge(), nullptr);
    NativeWebScope scope(wrapper);
    importWeb(wrapper);
    ASSERT_TRUE(wrapper.evaluateScript("typeof web.installNativeWeb === 'function'").toBool());
    EXPECT_TRUE(wrapper.evaluateScript(R"JS((() => {
        const nativeResponse = web.Response;
        const nativeFetch = web.fetch;
        const nativeSocket = web.WebSocket;
        web.Response = function replacementExport() {};
        web.installNativeWeb();
        if (Response !== nativeResponse || fetch !== nativeFetch || WebSocket !== nativeSocket ||
            DOMException !== web.DOMException || ReadableStream !== web.ReadableStream) return false;
        web.installNativeWeb();
        return Response === nativeResponse && fetch === nativeFetch && WebSocket === nativeSocket &&
            typeof globalThis.installNativeWeb === 'undefined';
    })())JS").toBool());
}

TEST_P(NativeWebInstallTests, InstallerCannotReviveClosedContextAndContextsAreIndependent) {
    JSContextWrapper first(getJsBridge(), nullptr);
    NativeWebScope firstScope(first);
    importWeb(first);
    ASSERT_TRUE(first.evaluateScript("typeof web.installNativeWeb === 'function'").toBool());
    first.withContext([](auto& cx, auto&) { NativeWeb::shutdown(cx); });
    EXPECT_TRUE(first.evaluateScript(R"JS((() => {
        try { web.installNativeWeb(); return false; }
        catch (error) { return typeof Response === 'undefined'; }
    })())JS").toBool());
    JSContextWrapper second(getJsBridge(), nullptr);
    NativeWebScope secondScope(second);
    importWeb(second);
    EXPECT_TRUE(second.evaluateScript(R"JS((() => {
        if (typeof Response !== 'undefined') return false;
        web.installNativeWeb();
        return new Response('fresh').status === 200;
    })())JS").toBool());
}

INSTANTIATE_TEST_SUITE_P(Hermes, NativeWebInstallTests, ::testing::Values(JavaScriptEngineTestCase::Hermes),
                         PrintJavaScriptEngineType());
} // namespace ValdiTest
