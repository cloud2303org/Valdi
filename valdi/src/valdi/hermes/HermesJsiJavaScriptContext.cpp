#include "valdi/hermes/HermesJsiJavaScriptContext.hpp"
#include "valdi/runtime/JavaScript/JSFunctionWithCallable.hpp"
#include "valdi/runtime/JavaScript/JavaScriptFunctionCallContext.hpp"
#include "valdi/runtime/JavaScript/JavaScriptUtils.hpp"
#include "valdi/runtime/Utils/RefCountableAutoreleasePool.hpp"
#include "valdi_core/cpp/Utils/Defer.hpp"
#include "valdi_core/cpp/Utils/StaticString.hpp"
#include "valdi_core/cpp/Utils/SmallVector.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"
#include "valdi_core/cpp/Utils/ValueUtils.hpp"

#include <jsi/instrumentation.h>
#ifndef HERMESVM_LEAN
#include <hermes/CompileJS.h>
#endif
#ifdef HERMES_ENABLE_DEBUGGER
#include "valdi/hermes/HermesDebuggerServer.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <bit>
#include <cmath>
#include <limits>
#include <cstring>
#include <mutex>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace Valdi::Hermes {
namespace {
#include "valdi/hermes/HermesJsiHelpers.inc"

// Hermes may retain an evaluated buffer for as long as its code remains live.
class ModuleBuffer final : public jsi::Buffer {
public:
    explicit ModuleBuffer(BytesView bytes) : _bytes(std::move(bytes)) {
        // The module loader strips a four-byte Valdi envelope; the resulting HBC
        // slice need not retain the bytecode alignment required by the public API.
        if (reinterpret_cast<uintptr_t>(_bytes.data()) % alignof(std::max_align_t) != 0) {
            _aligned.resize((_bytes.size() + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            std::memcpy(_aligned.data(), _bytes.data(), _bytes.size());
        }
    }
    size_t size() const override { return _bytes.size(); }
    const uint8_t* data() const override {
        return _aligned.empty() ? _bytes.data() : reinterpret_cast<const uint8_t*>(_aligned.data());
    }
private:
    BytesView _bytes;
    std::vector<std::max_align_t> _aligned;
};

class HelperBuffer final : public jsi::Buffer {
public:
    size_t size() const override { return sizeof(kHermesJsiHelpers); }
    const uint8_t* data() const override { return kHermesJsiHelpers; }
};

hermes::vm::RuntimeConfig makeRuntimeConfig() {
    return hermes::vm::RuntimeConfig::Builder()
        .withMaxNumRegisters(256 * 1024)
        .withMicrotaskQueue(true)
        .withEnableHermesInternal(true)
        .withES6BlockScoping(true)
        .withAsyncBreakCheckInEval(true)
        .build();
}

#ifndef HERMESVM_LEAN
class CompilationDiagnostics final : public hermes::DiagnosticHandler {
public:
    std::string errors;
    void handle(const Diagnostic& diagnostic) override {
        if (diagnostic.kind == Error) {
            if (!errors.empty()) {
                errors += '\n';
            }
            errors += std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.column) + ": " +
                      diagnostic.message;
        }
    }
};

bool compileSource(const std::string& source, const std::string_view& filename,
                   std::string& bytecode, JSExceptionTracker& tracker) {
    hermes::CompileJSOptions options;
    options.optimize = true;
    options.emitAsyncBreakCheck = true;
    options.enableES6BlockScoping = true;
    CompilationDiagnostics diagnostics;
    // The public compiler has no strict-mode option; preserve Valdi's strict
    // evaluation policy without shifting source line numbers.
    const auto strictSource = std::string("\"use strict\"; ") + source;
    if (!hermes::compileJS(strictSource, std::string(filename), bytecode, options, &diagnostics)) {
        tracker.onError(diagnostics.errors.empty() ? "Hermes source compilation failed" : diagnostics.errors.c_str());
        return false;
    }
    return true;
}
#endif
} // namespace

void HermesJsiJavaScriptContext::FinalizerQueue::enqueue(std::function<void()> finalizer) {
    Ref<DispatchQueue> dispatchQueue;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.emplace_back(std::move(finalizer));
        if (ownerQueue != nullptr && !wakeupScheduled) {
            wakeupScheduled = true;
            dispatchQueue = ownerQueue;
        }
    }
    if (dispatchQueue != nullptr) {
        // Never call the scheduler from a Hermes finalizer: retaining its runtime
        // here could cause runtime teardown to join this very finalizer thread.
        dispatchQueue->async([queue = shared_from_this()] {
            HermesJsiJavaScriptContext* context;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->wakeupScheduled = false;
                context = queue->owner;
            }
            // This runs on the same serial queue as context destruction. close()
            // clears owner before teardown; rejected callbacks only drop queue.
            if (context != nullptr) {
                RefCountableAutoreleasePool pool;
                context->drainFinalizers();
            }
        });
    }
}

void HermesJsiJavaScriptContext::FinalizerQueue::close() {
    std::lock_guard<std::mutex> lock(mutex);
    owner = nullptr;
    ownerQueue = nullptr;
}

void HermesJsiJavaScriptContext::drainFinalizers() {
    if (_drainingFinalizers) {
        return;
    }
    _drainingFinalizers = true;
    Defer finish([this] { _drainingFinalizers = false; });
    for (;;) {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(_finalizers->mutex);
            pending.swap(_finalizers->pending);
        }
        if (pending.empty()) {
            return;
        }
        // Destructors can release JS roots or enqueue more cleanup. No queue
        // lock (and no scheduler dispatch) may surround native destruction.
        for (auto& finalizer : pending) {
            finalizer();
        }
    }
}

HermesJsiJavaScriptContext::HermesJsiJavaScriptContext(JavaScriptTaskScheduler* taskScheduler, ILogger& logger)
    : IJavaScriptContext(taskScheduler), _logger(logger),
      _runtime(facebook::hermes::makeHermesRuntime(makeRuntimeConfig())) {
    _finalizers->owner = this;
    // Valdi constructs runtimes on their serial JS dispatch queue. Bare contexts
    // without a queue retain the explicit VM-boundary/GC drain fallback.
    _finalizers->ownerQueue = Ref<DispatchQueue>(DispatchQueue::getCurrent());
}

HermesJsiJavaScriptContext::~HermesJsiJavaScriptContext() {
    _finalizers->close();
#ifdef HERMES_ENABLE_DEBUGGER
    if (_debuggerRuntimeId != kRuntimeIdUndefined) {
        if (auto* server = HermesDebuggerServer::getInstance(_logger)) {
            server->getRegistry()->remove(_debuggerRuntimeId);
        }
    }
#endif
    setListener(nullptr);
    prepareForTeardown();
    _helpers.reset();
    _nativeOwnedPromises.clear(); // Destroy JSI weak handles before their runtime.
    _promiseAdoptions.clear();
    drainFinalizers();
    // WeakState may still be owned by Hermes or awaiting owner-thread deletion.
    // Invalidate every weak pointer before Hermes destroys its pointer storage.
    for (auto* state : _weakStates) {
        state->object.reset();
    }
    // All JSI roots must die while the runtime that owns them is still alive.
    while (!_values.empty()) {
        auto* handle = *_values.begin();
        _values.erase(handle);
        delete handle;
    }
    while (!_properties.empty()) {
        auto* handle = *_properties.begin();
        _properties.erase(handle);
        delete handle;
    }
    _runtime.reset();
    // Pinned Hermes destroys the VM, then drains/joins finalizerExecutor_. All
    // runtime-owned deleters have now published their work. Captured JSValueRef
    // releases see empty handle sets, and weak states contain no JSI pointers.
    drainFinalizers();
    SC_ASSERT(_weakStates.empty());
}

JSValueRef HermesJsiJavaScriptContext::hold(jsi::Value value) {
    if (value.isUndefined()) {
        return JSValueRef(this, JSValue(), true);
    }
    if (value.isNumber()) {
        return JSValueRef(this, JSValue(InlineValue{std::bit_cast<uint64_t>(value.getNumber()), InlineKind::Number}), true);
    }
    if (value.isBool()) {
        return JSValueRef(this, JSValue(InlineValue{value.getBool() ? 1u : 0u, InlineKind::Boolean}), true);
    }
    if (value.isNull()) {
        return JSValueRef(this, JSValue(InlineValue{0, InlineKind::Null}), true);
    }
    auto handle = std::make_unique<ValueHandle>(std::move(value));
    _values.insert(handle.get());
    return JSValueRef(this, JSValue(handle.release()), true);
}

JSPropertyNameRef HermesJsiJavaScriptContext::hold(jsi::PropNameID value) {
    auto handle = std::make_unique<PropertyHandle>(std::move(value));
    _properties.insert(handle.get());
    return JSPropertyNameRef(this, JSPropertyName(handle.release()), true);
}

jsi::Value HermesJsiJavaScriptContext::getValue(const JSValue& value) const {
    const auto encoded = value.bridge<InlineValue>();
    switch (encoded.kind) {
        case InlineKind::Number: return jsi::Value(std::bit_cast<double>(encoded.payload));
        case InlineKind::Boolean: return jsi::Value(encoded.payload != 0);
        case InlineKind::Null: return jsi::Value::null();
        case InlineKind::Handle: {
            auto* handle = value.bridge<ValueHandle*>();
            return handle == nullptr ? jsi::Value::undefined() : jsi::Value(*_runtime, handle->value);
        }
    }
    throw std::invalid_argument("Invalid Hermes JSI value kind");
}

const jsi::PropNameID& HermesJsiJavaScriptContext::getPropertyName(const JSPropertyName& property) const {
    auto* handle = property.bridge<PropertyHandle*>();
    if (handle == nullptr) {
        throw std::invalid_argument("Empty JavaScript property name");
    }
    return handle->value;
}

void HermesJsiJavaScriptContext::retainValue(const JSValue& value) {
    if (value.bridge<InlineValue>().kind != InlineKind::Handle) { return; }
    if (auto* handle = value.bridge<ValueHandle*>(); handle != nullptr && _values.count(handle) != 0) {
        ++handle->references;
    }
}

void HermesJsiJavaScriptContext::releaseValue(const JSValue& value) {
    if (value.bridge<InlineValue>().kind != InlineKind::Handle) { return; }
    if (auto* handle = value.bridge<ValueHandle*>(); handle != nullptr && _values.count(handle) != 0) {
        if (--handle->references == 0) {
            _values.erase(handle);
            delete handle;
        }
    }
}

void HermesJsiJavaScriptContext::retainPropertyName(const JSPropertyName& value) {
    if (auto* handle = value.bridge<PropertyHandle*>(); handle != nullptr && _properties.count(handle) != 0) {
        ++handle->references;
    }
}

void HermesJsiJavaScriptContext::releasePropertyName(const JSPropertyName& value) {
    if (auto* handle = value.bridge<PropertyHandle*>(); handle != nullptr && _properties.count(handle) != 0) {
        if (--handle->references == 0) {
            _properties.erase(handle);
            delete handle;
        }
    }
}

jsi::Value HermesJsiJavaScriptContext::callHelper(const char* name, const jsi::Value* arguments, size_t count) {
    if (!_helpers) {
        throw std::logic_error("Hermes JSI helpers have not been initialized");
    }
    auto function = _helpers->getPropertyAsFunction(*_runtime, name);
    return _runtime->call(function, jsi::Value(*_runtime, *_helpers), arguments, count);
}

void HermesJsiJavaScriptContext::captureException(const jsi::JSError& error, JSExceptionTracker& tracker) {
    tracker.storeException(hold(jsi::Value(*_runtime, error.value())));
}

JSValueRef HermesJsiJavaScriptContext::onNewBool(bool boolean) { return hold(jsi::Value(boolean)); }
JSValueRef HermesJsiJavaScriptContext::onNewNumber(int32_t number) { return hold(jsi::Value(number)); }
JSValueRef HermesJsiJavaScriptContext::onNewNumber(double number) { return hold(jsi::Value(number)); }
JSValueRef HermesJsiJavaScriptContext::onNewNull() { return hold(jsi::Value::null()); }
JSValueRef HermesJsiJavaScriptContext::onNewUndefined() { return hold(jsi::Value::undefined()); }

void HermesJsiJavaScriptContext::onInitialize(JSExceptionTracker& tracker) {
    withExceptionTracker(tracker, [&] {
        _helpers = _runtime->evaluateJavaScript(std::make_shared<HelperBuffer>(), "valdi:hermes-jsi-helpers")
                       .asObject(*_runtime);
        // Patched Hermes builtins call this explicit global after adoption and
        // before tracking/reporting rejection. Register before enabling tracking.
        auto ownership = newFunction(makeShared<JSFunctionWithCallable>(
            ReferenceInfoBuilder().withProperty(STRING_LITERAL("__valdiNativePromiseOwnership")),
            [](JSFunctionNativeCallContext& callContext) -> JSValueRef {
                auto& context = static_cast<HermesJsiJavaScriptContext&>(callContext.getContext());
                const auto& op = context.getValue(callContext.getParameter(0));
                const auto& parent = context.getValue(callContext.getParameter(1));
                if (op.isNumber() && op.getNumber() == 0) {
                    context.recordPromiseAdoption(parent, context.getValue(callContext.getParameter(2)));
                } else if (op.isNumber() && op.getNumber() == 1) {
                    return context.newBool(context.isNativeOwnedPromise(parent));
                }
                return context.newUndefined();
            }), tracker);
        if (!tracker) {
            return;
        }
        auto global = getGlobalObject(tracker);
        IJavaScriptContext::setObjectProperty(
            global.get(), "__valdiNativePromiseOwnership", ownership.get(), false, tracker);
        if (!tracker) {
            return;
        }
        auto callback = newFunction(makeShared<JSFunctionWithCallable>(
            ReferenceInfoBuilder().withProperty(STRING_LITERAL("onUnhandledPromise")),
            [](JSFunctionNativeCallContext& callContext) -> JSValueRef {
                auto& context = static_cast<HermesJsiJavaScriptContext&>(callContext.getContext());
                // Hermes calls onUnhandled(id, reason, promise). Filter only by
                // arg2 identity; arg1 must reach the listener unchanged.
                if (!context.isNativeOwnedPromise(context.getValue(callContext.getParameter(2)))) {
                    if (auto* listener = context.getListener()) {
                        listener->onUnhandledRejectedPromise(context, callContext.getParameter(1));
                    }
                }
                return context.newUndefined();
            }), tracker);
        if (!tracker) {
            return;
        }
        auto callbackValue = getValue(callback.get());
        callHelper("installRejectionTracker", &callbackValue, 1);
    });
}

JSValueRef HermesJsiJavaScriptContext::getGlobalObject(JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] { return hold(_runtime->global()); });
}

bool HermesJsiJavaScriptContext::supportsPreCompilation() const {
#ifdef HERMESVM_LEAN
    return false;
#else
    return true;
#endif
}

BytesView HermesJsiJavaScriptContext::preCompile(const std::string_view& script,
                                               const std::string_view& filename, JSExceptionTracker& tracker) {
#ifdef HERMESVM_LEAN
    tracker.onError("Hermes not compiled with compilation enabled");
    return {};
#else
    return withExceptionTracker(tracker, [&]() -> BytesView {
        std::string bytecode;
        if (!compileSource(formatJsModule(script), filename, bytecode, tracker)) {
            return {};
        }
        return makePreCompiledJsModule(reinterpret_cast<const Byte*>(bytecode.data()), bytecode.size());
    });
#endif
}

JSValueRef HermesJsiJavaScriptContext::evaluate(const std::string& script,
                                              const std::string_view& filename, JSExceptionTracker& tracker) {
#ifdef HERMESVM_LEAN
    tracker.onError("Hermes not compiled with compilation enabled");
    return {};
#else
    return withExceptionTracker(tracker, [&]() -> JSValueRef {
        // Source evaluation through the public runtime retains the debugger's
        // source support. Keep existing emitted strict directives byte-for-byte.
        auto source = script.starts_with("\"use strict\";") || script.starts_with("'use strict';")
            ? script : std::string("\"use strict\"; ") + script;
        return hold(_runtime->evaluateJavaScript(std::make_shared<jsi::StringBuffer>(std::move(source)),
                                                 std::string(filename)));
    });
#endif
}

JSValueRef HermesJsiJavaScriptContext::evaluatePreCompiled(const BytesView& script,
                                                         const std::string_view& filename,
                                                         JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&]() -> JSValueRef {
        // IJavaScriptContext receives raw engine bytecode; ResourceManager has
        // already consumed Valdi's module marker before calling this method.
        auto bytecode = makeFinalized<ModuleBuffer>(script);
        auto* api = jsi::castInterface<facebook::hermes::IHermesRootAPI>(facebook::hermes::makeHermesRootAPI());
        std::string validationError;
        if (api == nullptr || !api->hermesBytecodeSanityCheck(bytecode->data(), bytecode->size(), &validationError)) {
            tracker.onError(validationError.empty() ? "Invalid Hermes bytecode" : validationError.c_str());
            return {};
        }
        return hold(_runtime->evaluateJavaScript(bytecode, std::string(filename)));
    });
}

JSPropertyNameRef HermesJsiJavaScriptContext::newPropertyName(const std::string_view& str) {
    return hold(jsi::PropNameID::forUtf8(*_runtime, reinterpret_cast<const uint8_t*>(str.data()), str.size()));
}

StringBox HermesJsiJavaScriptContext::propertyNameToString(const JSPropertyName& property) {
    auto str = getPropertyName(property).utf16(*_runtime);
    return StringCache::getGlobal().makeStringFromUTF16(str.data(), str.size());
}

JSValueRef HermesJsiJavaScriptContext::newObject(JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] { return hold(jsi::Object(*_runtime)); });
}

JSValueRef HermesJsiJavaScriptContext::newStringUTF8(const std::string_view& str, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        return hold(jsi::String::createFromUtf8(*_runtime, reinterpret_cast<const uint8_t*>(str.data()), str.size()));
    });
}

JSValueRef HermesJsiJavaScriptContext::newStringUTF16(const std::u16string_view& str, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        return hold(jsi::String::createFromUtf16(*_runtime, str.data(), str.size()));
    });
}

JSValueRef HermesJsiJavaScriptContext::newArray(size_t size, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] { return hold(jsi::Array(*_runtime, size)); });
}

JSValueRef HermesJsiJavaScriptContext::getObjectProperty(const JSValue& object, const JSPropertyName& property,
                                                       JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        return hold(getValue(object).asObject(*_runtime).getProperty(*_runtime, getPropertyName(property)));
    });
}

JSValueRef HermesJsiJavaScriptContext::getObjectPropertyForIndex(const JSValue& object, size_t index,
                                                               JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        return hold(getValue(object).asObject(*_runtime).getProperty(*_runtime, jsi::Value(static_cast<double>(index))));
    });
}

bool HermesJsiJavaScriptContext::hasObjectProperty(const JSValue& object, const JSPropertyName& property) {
    try {
        return getValue(object).isObject() &&
               getValue(object).asObject(*_runtime).hasProperty(*_runtime, getPropertyName(property));
    } catch (const std::exception&) {
        // This API has no exception tracker; match the old context's suppressed exception.
        return false;
    }
}

void HermesJsiJavaScriptContext::setObjectProperty(const JSValue& object, const JSPropertyName& property,
                                                  const JSValue& value, bool enumerable, JSExceptionTracker& tracker) {
    withExceptionTracker(tracker, [&] {
        auto target = getValue(object).asObject(*_runtime);
        if (enumerable) {
            target.setProperty(*_runtime, getPropertyName(property), getValue(value));
        } else {
            jsi::Value arguments[] = {jsi::Value(*_runtime, target),
                jsi::String::createFromUtf16(*_runtime, getPropertyName(property).utf16(*_runtime)),
                jsi::Value(*_runtime, getValue(value)), jsi::Value(false), jsi::Value(true), jsi::Value(true)};
            callHelper("defineData", arguments, 6);
        }
    });
}

void HermesJsiJavaScriptContext::setObjectProperty(const JSValue& object, const JSValue& property,
                                                  const JSValue& value, bool enumerable, JSExceptionTracker& tracker) {
    withExceptionTracker(tracker, [&] {
        auto target = getValue(object).asObject(*_runtime);
        if (enumerable) {
            _runtime->setPropertyValue(target, getValue(property), getValue(value));
        } else {
            jsi::Value arguments[] = {jsi::Value(*_runtime, target), jsi::Value(*_runtime, getValue(property)),
                jsi::Value(*_runtime, getValue(value)), jsi::Value(false), jsi::Value(true), jsi::Value(true)};
            callHelper("defineData", arguments, 6);
        }
    });
}

void HermesJsiJavaScriptContext::setObjectPropertyIndex(const JSValue& object, size_t index,
                                                       const JSValue& value, JSExceptionTracker& tracker) {
    withExceptionTracker(tracker, [&] {
        _runtime->setPropertyValue(getValue(object).asObject(*_runtime), jsi::Value(static_cast<double>(index)),
                                   getValue(value));
    });
}

JSValueRef HermesJsiJavaScriptContext::callObjectAsFunction(const JSValue& object, JSFunctionCallContext& callContext) {
    return withExceptionTracker(callContext.getExceptionTracker(), [&] {
        auto function = getValue(object).asObject(*_runtime).asFunction(*_runtime);
        SmallVector<jsi::Value, 8> arguments;
        arguments.reserve(callContext.getParameterSize());
        for (size_t i = 0; i < callContext.getParameterSize(); ++i) {
            arguments.emplace_back(getValue(callContext.getParameter(i)));
        }
        const auto& thisValue = getValue(callContext.getThisValue());
        // Valdi's existing call contract uses the global object for an omitted receiver.
        auto receiver = thisValue.isUndefined() ? jsi::Value(_runtime->global()) : jsi::Value(*_runtime, thisValue);
        return hold(_runtime->call(function, receiver, arguments.data(), arguments.size()));
    });
}

JSValueRef HermesJsiJavaScriptContext::callObjectAsConstructor(const JSValue& object, JSFunctionCallContext& callContext) {
    return withExceptionTracker(callContext.getExceptionTracker(), [&] {
        auto function = getValue(object).asObject(*_runtime).asFunction(*_runtime);
        SmallVector<jsi::Value, 8> arguments;
        arguments.reserve(callContext.getParameterSize());
        for (size_t i = 0; i < callContext.getParameterSize(); ++i) {
            arguments.emplace_back(getValue(callContext.getParameter(i)));
        }
        return hold(_runtime->callAsConstructor(function, arguments.data(), arguments.size()));
    });
}

void HermesJsiJavaScriptContext::visitObjectPropertyNames(const JSValue& object, JSExceptionTracker& tracker,
                                                        IJavaScriptPropertyNamesVisitor& visitor) {
    withExceptionTracker(tracker, [&] {
        auto names = getValue(object).asObject(*_runtime).getPropertyNames(*_runtime);
        auto size = names.size(*_runtime);
        for (size_t i = 0; i < size; ++i) {
            auto name = names.getValueAtIndex(*_runtime, i).asString(*_runtime);
            auto property = hold(jsi::PropNameID::forString(*_runtime, name));
            if (!visitor.visitPropertyName(*this, object, property.get(), tracker) || !tracker) {
                break;
            }
        }
    });
}

StringBox HermesJsiJavaScriptContext::valueToString(const JSValue& value, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        auto input = getValue(value);
        auto str = input.isString() ? input.asString(*_runtime) : callHelper("toString", &input, 1).asString(*_runtime);
        StringBox result;
        auto consume = [&](bool ascii, const void* data, size_t length) {
            auto part = ascii ? StringCache::getGlobal().makeString(static_cast<const char*>(data), length)
                              : StringCache::getGlobal().makeStringFromUTF16(static_cast<const char16_t*>(data), length);
            result = result.isNull() ? part : result.append(part);
        };
        str.getStringData(*_runtime, consume);
        return result.isNull() ? StringCache::getGlobal().makeString(std::string_view()) : result;
    });
}

Ref<StaticString> HermesJsiJavaScriptContext::valueToStaticString(const JSValue& value, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        auto input = getValue(value);
        auto str = (input.isString() ? input.asString(*_runtime) : callHelper("toString", &input, 1).asString(*_runtime))
                       .utf16(*_runtime);
        return StaticString::makeUTF16(str.data(), str.size());
    });
}

bool HermesJsiJavaScriptContext::valueToBool(const JSValue& value, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        auto input = getValue(value);
        if (input.isUndefined() || input.isNull()) { return false; }
        if (input.isBool()) { return input.getBool(); }
        if (input.isNumber()) { return input.getNumber() != 0 && !std::isnan(input.getNumber()); }
        if (input.isString()) { return input.asString(*_runtime).length(*_runtime) != 0; }
        if (input.isObject() || input.isSymbol()) { return true; }
        return callHelper("toBoolean", &input, 1).getBool(); // BigInt zero needs JS coercion.
    });
}

double HermesJsiJavaScriptContext::valueToDouble(const JSValue& value, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        auto input = getValue(value);
        if (input.isNumber()) { return input.getNumber(); }
        if (input.isNull()) { return 0.0; }
        if (input.isBool()) { return input.getBool() ? 1.0 : 0.0; }
        if (input.isUndefined()) { return std::numeric_limits<double>::quiet_NaN(); }
        return callHelper("toNumber", &input, 1).getNumber();
    });
}

int32_t HermesJsiJavaScriptContext::valueToInt(const JSValue& value, JSExceptionTracker& tracker) {
    return withExceptionTracker(tracker, [&] {
        auto input = getValue(value);
        if (input.isNumber()) {
            const auto number = input.getNumber();
            if (number >= -2147483648.0 && number < 2147483648.0) {
                return static_cast<int32_t>(number);
            }
            if (!std::isfinite(number) || number == 0) { return int32_t{0}; }
            // ToInt32 truncates and wraps modulo 2^32. Cast only after reducing
            // into the representable signed range, including huge JS numbers.
            auto reduced = std::fmod(std::trunc(number), 4294967296.0);
            if (reduced >= 2147483648.0) { reduced -= 4294967296.0; }
            if (reduced < -2147483648.0) { reduced += 4294967296.0; }
            return static_cast<int32_t>(reduced);
        }
        return static_cast<int32_t>(callHelper("toInt32", &input, 1).getNumber());
    });
}

ValueType HermesJsiJavaScriptContext::getValueType(const JSValue& value) {
    const auto& input = getValue(value);
    if (input.isUndefined()) { return ValueType::Undefined; }
    if (input.isNull()) { return ValueType::Null; }
    if (input.isBool()) { return ValueType::Bool; }
    if (input.isNumber()) { return ValueType::Double; }
    if (input.isString()) { return ValueType::StaticString; }
    if (!input.isObject()) { return ValueType::Undefined; }
    auto object = input.asObject(*_runtime);
    // Constructors and native functions also carry state, but remain functions.
    if (object.isFunction(*_runtime)) { return ValueType::Function; }
    if (object.isArray(*_runtime)) { return ValueType::Array; }
    if (object.isArrayBuffer(*_runtime)) { return ValueType::TypedArray; }
    try {
        auto kind = callHelper("typedArrayKind", &input, 1);
        if (!kind.isUndefined() && !kind.isNull()) { return ValueType::TypedArray; }
        if (callHelper("isError", &input, 1).getBool()) { return ValueType::Error; }
        if (object.hasNativeState<NativeObjectState>(*_runtime)) { return ValueType::ValdiObject; }
    } catch (const std::exception&) {
        // Proxies and exotic objects can reject inspection.
    }
    if (isValueLong(value)) { return ValueType::Long; }
    return ValueType::Map;
}

bool HermesJsiJavaScriptContext::isValueUndefined(const JSValue& value) { return getValue(value).isUndefined(); }
bool HermesJsiJavaScriptContext::isValueNull(const JSValue& value) { return getValue(value).isNull(); }
bool HermesJsiJavaScriptContext::isValueNumber(const JSValue& value) { return getValue(value).isNumber(); }
bool HermesJsiJavaScriptContext::isValueObject(const JSValue& value) { return getValue(value).isObject(); }
bool HermesJsiJavaScriptContext::isValueFunction(const JSValue& value) {
    return getValue(value).isObject() && getValue(value).asObject(*_runtime).isFunction(*_runtime);
}
bool HermesJsiJavaScriptContext::isValueLong(const JSValue& value) {
    try {
        const auto& constructor = getLongConstructor();
        return getValue(value).isObject() && !constructor.empty() &&
            getValue(value).asObject(*_runtime).instanceOf(*_runtime,
                getValue(constructor.get()).asObject(*_runtime).asFunction(*_runtime));
    } catch (const std::exception&) {
        return false;
    }
}
bool HermesJsiJavaScriptContext::isValueEqual(const JSValue& left, const JSValue& right) {
    return jsi::Value::strictEquals(*_runtime, getValue(left), getValue(right));
}

void HermesJsiJavaScriptContext::prunePromiseOwnership() {
    _nativeOwnedPromises.erase(std::remove_if(_nativeOwnedPromises.begin(), _nativeOwnedPromises.end(),
        [&](const jsi::WeakObject& weak) {
            return weak.lock(*_runtime).isUndefined();
        }), _nativeOwnedPromises.end());
    _promiseAdoptions.erase(std::remove_if(_promiseAdoptions.begin(), _promiseAdoptions.end(),
        [&](const PromiseAdoption& edge) {
            return edge.parent.lock(*_runtime).isUndefined() || edge.child.lock(*_runtime).isUndefined();
        }), _promiseAdoptions.end());
}

bool HermesJsiJavaScriptContext::isNativeOwnedPromise(const jsi::Value& promise) {
    prunePromiseOwnership();
    return promise.isObject() && std::any_of(_nativeOwnedPromises.begin(), _nativeOwnedPromises.end(),
        [&](const jsi::WeakObject& weak) {
            return jsi::Value::strictEquals(*_runtime, weak.lock(*_runtime), promise);
        });
}

void HermesJsiJavaScriptContext::markNativeOwnedPromise(const jsi::Value& promise) {
    prunePromiseOwnership();
    if (!promise.isObject()) {
        return;
    }
    // Temporary strong worklist only: permanent roots would leak rejected
    // promises/reasons after the builtin tracker retires their timer records.
    std::vector<jsi::Value> pending;
    pending.emplace_back(*_runtime, promise);
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (std::any_of(_nativeOwnedPromises.begin(), _nativeOwnedPromises.end(),
            [&](const jsi::WeakObject& weak) {
                return jsi::Value::strictEquals(*_runtime, weak.lock(*_runtime), current);
            })) {
            continue; // Also guards cycles and converging adoption paths.
        }
        _nativeOwnedPromises.emplace_back(*_runtime, current.asObject(*_runtime));
        for (const auto& edge : _promiseAdoptions) {
            if (jsi::Value::strictEquals(*_runtime, edge.parent.lock(*_runtime), current)) {
                auto child = edge.child.lock(*_runtime);
                if (child.isObject()) {
                    pending.emplace_back(std::move(child));
                }
            }
        }
    }
}

void HermesJsiJavaScriptContext::recordPromiseAdoption(const jsi::Value& parent, const jsi::Value& child) {
    prunePromiseOwnership();
    if (!parent.isObject() || !child.isObject()) {
        return;
    }
    const auto duplicate = std::any_of(_promiseAdoptions.begin(), _promiseAdoptions.end(),
        [&](const PromiseAdoption& edge) {
            return jsi::Value::strictEquals(*_runtime, edge.parent.lock(*_runtime), parent) &&
                   jsi::Value::strictEquals(*_runtime, edge.child.lock(*_runtime), child);
        });
    if (!duplicate) {
        _promiseAdoptions.push_back({jsi::WeakObject(*_runtime, parent.asObject(*_runtime)),
                                     jsi::WeakObject(*_runtime, child.asObject(*_runtime))});
    }
    // Transfer immediately: the outer promise may die before the rejection
    // timer fires, at which point a weak edge can no longer recover its identity.
    if (isNativeOwnedPromise(parent)) {
        markNativeOwnedPromise(child);
    }
}

void HermesJsiJavaScriptContext::markPromiseRejectionHandled(const JSValue& promise) {
    // Public JSI has no Promise brand query. Recording other objects is harmless:
    // only a real Promise can be passed back by Hermes' rejection tracker.
    // In particular, do not inspect then/constructor/species or install a handler.
    markNativeOwnedPromise(getValue(promise));
}

void HermesJsiJavaScriptContext::garbageCollect() {
    drainFinalizers();
    _runtime->instrumentation().collectGarbage("Explicit GC");
    (void)isNativeOwnedPromise(jsi::Value::undefined()); // Prune collected identities.
    drainFinalizers();
}

JavaScriptContextMemoryStatistics HermesJsiJavaScriptContext::dumpMemoryStatistics() {
    JavaScriptContextMemoryStatistics statistics;
    auto heap = _runtime->instrumentation().getHeapInfo(false);
    auto allocated = heap.find("hermes_allocatedBytes");
    if (allocated != heap.end() && allocated->second > 0) {
        statistics.memoryUsageBytes = static_cast<size_t>(allocated->second);
    }
    return statistics;
}

BytesView HermesJsiJavaScriptContext::dumpHeap(JSExceptionTracker& tracker) {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    return withExceptionTracker(tracker, [&] {
        std::ostringstream stream;
        _runtime->instrumentation().createSnapshotToStream(stream, {true});
        auto data = stream.str();
        auto bytes = makeShared<Bytes>();
        bytes->assignData(reinterpret_cast<const Byte*>(data.data()), data.size());
        return BytesView(bytes);
    });
#else
    tracker.onError("Memory instrumentation not enabled");
    return {};
#endif
}

void HermesJsiJavaScriptContext::enqueueMicrotask(const JSValue& value, JSExceptionTracker& tracker) {
    withExceptionTracker(tracker, [&] {
        _runtime->queueMicrotask(getValue(value).asObject(*_runtime).asFunction(*_runtime));
    });
}

void HermesJsiJavaScriptContext::willEnterVM() {
    ++_enterVMCount;
    drainFinalizers();
}
void HermesJsiJavaScriptContext::willExitVM(JSExceptionTracker& tracker) {
    SC_ASSERT(_enterVMCount > 0);
    if (_enterVMCount > 1) {
        --_enterVMCount;
        return;
    }
    Defer exitVM([this] { drainFinalizers(); --_enterVMCount; });
    if (!executionTerminationRequested()) {
        withExceptionTracker(tracker, [&] { _runtime->drainMicrotasks(); });
    }
}

void HermesJsiJavaScriptContext::requestExecutionTermination() {
    IJavaScriptContext::requestExecutionTermination();
    _runtime->asyncTriggerTimeout();
}

void HermesJsiJavaScriptContext::startDebugger([[maybe_unused]] bool isWorker) {
#ifdef HERMES_ENABLE_DEBUGGER
    auto* server = HermesDebuggerServer::getInstance(_logger);
    if (server != nullptr && _debuggerRuntimeId == kRuntimeIdUndefined) {
        auto registry = server->getRegistry();
        auto parent = kRuntimeIdUndefined;
        if (isWorker) {
            auto ids = registry->getRuntimeIds();
            if (!ids.empty()) { parent = ids.front(); }
        }
        _debuggerRuntimeId = registry->append(_runtime, makeRuntimeConfig(), getTaskScheduler(), parent);
    }
#endif
}

std::optional<IJavaScriptContextDebuggerInfo> HermesJsiJavaScriptContext::getDebuggerInfo() const {
#ifdef HERMES_ENABLE_DEBUGGER
    if (auto* server = HermesDebuggerServer::getInstance(_logger)) {
        return IJavaScriptContextDebuggerInfo{server->getPort(), server->getWebsocketTargets()};
    }
#endif
    return std::nullopt;
}

void HermesJsiJavaScriptContext::startProfiling() {
#ifdef HERMES_ENABLE_DEBUGGER
    auto* server = HermesDebuggerServer::getInstance(_logger);
    if (server != nullptr && _debuggerRuntimeId != kRuntimeIdUndefined) {
        auto connection = server->getDebuggerConnection(_debuggerRuntimeId);
        if (connection != nullptr) {
            connection->handleCDPMessage(HermesDebuggerConnection::createEvent(STRING_LITERAL("Profiler.start"), -1));
        }
    }
#endif
}

Result<std::vector<std::string>> HermesJsiJavaScriptContext::stopProfiling() {
#ifdef HERMES_ENABLE_DEBUGGER
    auto* server = HermesDebuggerServer::getInstance(_logger);
    if (server == nullptr || _debuggerRuntimeId == kRuntimeIdUndefined) {
        return Error("Profiler uninitialized");
    }
    auto connection = server->getDebuggerConnection(_debuggerRuntimeId);
    if (connection == nullptr) { return Error("Profiler uninitialized"); }
    struct Response {
        std::mutex mutex;
        std::optional<std::string> profile;
    };
    auto response = std::make_shared<Response>();
    connection->setCDPMessageListener([response](const Value& message) {
        if (message.getMapValue("id").toInt() == -2) {
            std::lock_guard<std::mutex> lock(response->mutex);
            response->profile = valueToJsonString(message.getMapValue("result").getMapValue("profile"));
        }
    });
    Defer clearListener([connection] { connection->setCDPMessageListener(nullptr); });
    connection->handleCDPMessage(HermesDebuggerConnection::createEvent(STRING_LITERAL("Profiler.stop"), -2));
    // Default CDP dispatch is inline. Never block the JS thread waiting for work
    // that may instead have been queued back onto that same thread.
    std::lock_guard<std::mutex> lock(response->mutex);
    if (!response->profile) { return Error("Profiler response was not delivered synchronously"); }
    return std::vector<std::string>{std::move(*response->profile)};
#else
    return Error("Profiler disabled");
#endif
}

} // namespace Valdi::Hermes
