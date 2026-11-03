#include "valdi/hermes/HermesJavaScriptContext.hpp"
#include "valdi/runtime/JavaScript/JavaScriptUtils.hpp"
#include "valdi/runtime/JavaScript/JavaScriptValueDelegate.hpp"
#include "valdi_core/cpp/Utils/ConsoleLogger.hpp"
#include "valdi_core/cpp/Utils/ValueArray.hpp"
#include "valdi_core/cpp/Utils/ByteBuffer.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace Valdi;

static void check(JSExceptionTracker& tracker) {
    if (!tracker) {
        std::cerr << tracker.getError().toString() << '\n';
        std::exit(2);
    }
}

// Exercise the production delegate map entry point recursively, including maps
// inside arrays; leaves use the production untyped conversion.
static JSValueRef viaDelegate(JavaScriptValueDelegate& delegate, IJavaScriptContext& context,
                              const Value& value, JSExceptionTracker& tracker) {
    if (value.getType() == ValueType::Map) {
        auto result = delegate.newMap(value.getMap()->size(), tracker);
        check(tracker);
        for (const auto& [key, child] : *value.getMap()) {
            auto jsKey = context.newStringUTF8(key.toStringView(), tracker);
            auto jsChild = viaDelegate(delegate, context, child, tracker);
            check(tracker);
            delegate.setMapEntry(result, jsKey, jsChild, tracker);
            check(tracker);
        }
        return result;
    }
    if (value.getType() == ValueType::Array) {
        const auto& array = *value.getArray();
        auto result = context.newArray(array.size(), tracker);
        for (size_t i = 0; i < array.size(); ++i) {
            auto child = viaDelegate(delegate, context, array[i], tracker);
            context.setObjectPropertyIndex(result.get(), i, child.get(), tracker);
            check(tracker);
        }
        return result;
    }
    return valueToJSValue(context, value, ReferenceInfoBuilder(), tracker);
}

int main(int argc, char** argv) {
    if (argc != 2) return 4;
    Hermes::HermesJavaScriptContext engine(nullptr, ConsoleLogger::getLogger());
    IJavaScriptContext& context = engine;
    JSExceptionTracker tracker(context);
    IJavaScriptContextConfig config;
    config.disableValueMarshaller = true;
    context.initialize(config, tracker);
    check(tracker);
    JavaScriptValueDelegate delegate(context, true, tracker);
    check(tracker);
    size_t programIndex = 0;
    // The runner compiles each literal below with the matching official hermesc.
    // Both full and lean consumers execute those actual HBC files.
    auto evaluate = [&](const std::string&) {
        const auto filename = std::string(argv[1]) + "/" + std::to_string(programIndex++) + ".hbc";
        std::ifstream input(filename, std::ios::binary);
        if (!input) std::exit(5);
        std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto buffer = makeShared<ByteBuffer>(data);
        auto result = context.evaluatePreCompiled(buffer->toBytesView(), "LosslessMapConsumer.js", tracker);
        check(tracker);
        return result;
    };
    // JSON.parse, rather than a JS object literal, creates a real __proto__ key.
    auto input = evaluate(R"JS(
        globalThis.original = JSON.parse('{"__proto__":{"polluted":true},"constructor":{"prototype":{"marker":false}},"prototype":true,"nested":{"__proto__":null,"constructor":0,"prototype":1,"items":[true,false,null,0,1,{"__proto__":false},{"__proto__":0},{"__proto__":[true,null,{"__proto__":"leaf"}]}]},"empty":{},"unicode":"空\\u0000键"}');
        original;
    )JS");
    auto native = jsValueToValue(context, input.get(), ReferenceInfoBuilder(), tracker);
    check(tracker);
    auto global = context.getGlobalObject(tracker);
    // The production initialization must capture the intrinsic before business
    // code replaces either Object or Object.defineProperty.
    evaluate(R"JS(
        globalThis.savedObject = Object;
        Object.defineProperty = function() { throw new Error('late defineProperty'); };
        Object.create = function() { throw new Error('late create'); };
        Object.prototype.get = function() { throw new Error('inherited descriptor getter'); };
        Object.prototype.set = function() { throw new Error('inherited descriptor setter'); };
        globalThis.Object = function() { throw new Error('late Object'); };
    )JS");
    auto untyped = valueToJSValue(context, native, ReferenceInfoBuilder(), tracker);
    check(tracker);
    auto typed = viaDelegate(delegate, context, native, tracker);
    context.setObjectProperty(global.get(), "untyped", untyped.get(), tracker);
    context.setObjectProperty(global.get(), "typed", typed.get(), tracker);
    check(tracker);
    auto verified = evaluate(R"JS(
        (function() {
          const O = savedObject;
          function fail(message) { throw new Error(message); }
          function compare(expected, actual, path) {
            if (expected === null || typeof expected !== 'object') {
              if (expected !== actual || typeof expected !== typeof actual) fail(path + ': leaf');
              return;
            }
            if (Array.isArray(expected) !== Array.isArray(actual)) fail(path + ': array');
            if (O.getPrototypeOf(actual) !== (Array.isArray(actual) ? Array.prototype : O.prototype)) fail(path + ': prototype');
            const keys = O.keys(expected).sort(), received = O.keys(actual).sort();
            if (JSON.stringify(keys) !== JSON.stringify(received)) fail(path + ': keys');
            for (const key of keys) {
              const d = O.getOwnPropertyDescriptor(actual, key);
              if (!d || !('value' in d) || !d.enumerable || !d.writable || !d.configurable) fail(path + ': descriptor ' + key);
              compare(expected[key], d.value, path + '.' + key);
            }
          }
          compare(original, untyped, 'untyped');
          compare(original, typed, 'delegate');
          if (O.prototype.polluted !== undefined || ({}).polluted !== undefined) fail('global prototype pollution');
          // A preserved property must support ordinary data mutation and deletion.
          for (const result of [untyped, typed]) {
            result.__proto__ = 17;
            if (result.__proto__ !== 17 || O.getPrototypeOf(result) !== O.prototype) fail('write');
            if (!delete result.__proto__ || O.prototype.hasOwnProperty.call(result, '__proto__')) fail('delete');
          }
          delete O.prototype.get;
          delete O.prototype.set;
          return true;
        })();
    )JS");
    if (!context.valueToBool(verified.get(), tracker)) return 3;
    check(tracker);
    delegate.teardown();
#ifdef HERMESVM_LEAN
    std::cout << "PASS real LEAN Hermes initialize, untyped and delegate map consumer\n";
#else
    std::cout << "PASS real full Hermes initialize, untyped and delegate map consumer\n";
#endif
}
