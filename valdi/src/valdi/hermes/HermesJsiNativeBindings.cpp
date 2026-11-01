#include "valdi/hermes/HermesJsiJavaScriptContext.hpp"

#include "valdi/runtime/JavaScript/JavaScriptFunctionCallContext.hpp"
#include "valdi_core/cpp/Utils/SmallVector.hpp"

#include <functional>
#include <utility>

namespace Valdi::Hermes {
namespace {

class ExternalBuffer final : public jsi::MutableBuffer {
public:
    explicit ExternalBuffer(BytesView bytes) : _bytes(std::move(bytes)) {}
    size_t size() const override { return _bytes.size(); }
    uint8_t* data() override { return const_cast<uint8_t*>(_bytes.data()); }

private:
    BytesView _bytes;
};

// Used only behind invokeNative, which supplies argument roots, this, interrupt
// handling and the exception bridge for all native class callbacks as well.
class NativeCallback final : public JSFunction {
public:
    using Callback = std::function<JSValueRef(JSFunctionNativeCallContext&)>;
    NativeCallback(ReferenceInfo info, Callback callback) : _info(std::move(info)), _callback(std::move(callback)) {}
    const ReferenceInfo& getReferenceInfo() const override { return _info; }
    JSValueRef operator()(JSFunctionNativeCallContext& context) noexcept override { return _callback(context); }

private:
    ReferenceInfo _info;
    Callback _callback;
};

constexpr const char* kTypedArrayNames[] = {
    "Int8Array", "Int16Array", "Int32Array", "Uint8Array", "Uint8ClampedArray",
    "Uint16Array", "Uint32Array", "Float32Array", "Float64Array",
};

} // namespace

jsi::Value HermesJsiJavaScriptContext::invokeNative(const Ref<JSFunction>& function,
                                                   const jsi::Value& thisValue,
                                                   const jsi::Value* arguments,
                                                   size_t count) {
    auto receiver = hold(jsi::Value(*_runtime, thisValue));
    SmallVector<JSValueRef, 8> parameters;
    parameters.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        parameters.emplace_back(hold(jsi::Value(*_runtime, arguments[i])));
    }
    JSExceptionTracker tracker(*this);
    JSFunctionNativeCallContext context(*this, parameters.empty() ? nullptr : parameters.data(), count,
                                        tracker, function->getReferenceInfo());
    context.setThisValue(receiver.get());
    JSValueRef result;
    if (interruptRequested() && onInterrupt()) {
        tracker.onError("JavaScript execution terminated");
    } else {
        result = (*function)(context);
    }
    if (!tracker) {
        auto exception = tracker.getExceptionAndClear();
        throw jsi::JSError(*_runtime, jsi::Value(*_runtime, getValue(exception.get())));
    }
    return getValue(result.get());
}

JSValueRef HermesJsiJavaScriptContext::newFunction(const Ref<JSFunction>& callable,
                                                  JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        if (callable == nullptr) {
            throw jsi::JSError(*_runtime, "Native function cannot be null");
        }
        auto state = makeFinalized<NativeFunctionState>(callable);
        const auto name = callable->getReferenceInfo().getName().toStringView();
        auto function = jsi::Function::createFromHostFunction(
            *_runtime, jsi::PropNameID::forUtf8(*_runtime, reinterpret_cast<const uint8_t*>(name.data()), name.size()),
            0, [this, state](jsi::Runtime&, const jsi::Value& receiver, const jsi::Value* args, size_t count) {
                return invokeNative(state->function, receiver, args, count);
            });
        function.setNativeState(*_runtime, state);
        return hold(std::move(function));
    });
}

Ref<JSFunction> HermesJsiJavaScriptContext::valueToFunction(const JSValue& value,
                                                           JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> Ref<JSFunction> {
        const auto& input = getValue(value);
        if (!input.isObject()) {
            return nullptr;
        }
        auto object = input.asObject(*_runtime);
        if (!object.isFunction(*_runtime) || !object.hasNativeState<NativeFunctionState>(*_runtime)) {
            return nullptr;
        }
        auto function = object.getNativeState<NativeFunctionState>(*_runtime)->function;
        // Class callbacks require their receiver/class checks and must not be
        // exported as receiver-free native functions by the generic marshaller.
        return dynamic_cast<NativeCallback*>(function.get()) == nullptr ? function : Ref<JSFunction>();
    });
}

JSValueRef HermesJsiJavaScriptContext::newWrappedObject(const Ref<RefCountable>& wrappedObject,
                                                       JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        jsi::Object object(*_runtime);
        object.setNativeState(*_runtime, makeFinalized<NativeObjectState>(wrappedObject));
        return hold(std::move(object));
    });
}

Ref<RefCountable> HermesJsiJavaScriptContext::valueToWrappedObject(const JSValue& value,
                                                                JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> Ref<RefCountable> {
        const auto& input = getValue(value);
        if (!input.isObject()) {
            return nullptr;
        }
        auto object = input.asObject(*_runtime);
        if (!object.hasNativeState<NativeObjectState>(*_runtime)) {
            return nullptr;
        }
        return object.getNativeState<NativeObjectState>(*_runtime)->opaque;
    });
}

JSValueRef HermesJsiJavaScriptContext::newNativeClass(const Ref<RefCountable>& classOpaque,
                                                     const JSClassDefinition& definition,
                                                     JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        auto data = makeShared<JSNativeClassData>(definition.getName(), classOpaque, definition.getConstructor());
        auto initializerCallback = makeShared<NativeCallback>(data->getConstructorReferenceInfo(),
            [this, data](JSFunctionNativeCallContext& context) -> JSValueRef {
                return withExceptionTracker(context.getExceptionTracker(), [&]() -> JSValueRef {
                    auto receiver = getValue(context.getThisValue()).asObject(*_runtime);
                    if (data->getConstructor() == nullptr) {
                        throw jsi::JSError::createTypeError(*_runtime, "Native class cannot be constructed from JavaScript");
                    }
                    auto opaque = data->getConstructor()(data->getOpaque().get(), context);
                    if (!context.getExceptionTracker()) {
                        return newUndefined();
                    }
                    if (opaque == nullptr) {
                        context.getExceptionTracker().onError("Native class constructor returned a null opaque object");
                        return newUndefined();
                    }
                    receiver.setNativeState(*_runtime, makeFinalized<NativeObjectState>(std::move(opaque), data));
                    return newUndefined();
                });
            });
        auto initializer = newFunction(initializerCallback, exceptionTracker);
        if (!exceptionTracker) {
            return newUndefined();
        }
        const auto name = definition.getName().toStringView();
        jsi::Value constructorArgs[] = {
            jsi::Value(*_runtime, getValue(initializer.get())),
            jsi::String::createFromUtf8(*_runtime, reinterpret_cast<const uint8_t*>(name.data()), name.size()),
        };
        // The JS closure owns initializer. Neither initializer nor its native
        // callback captures constructor, so there is no native/JS root cycle.
        auto constructor = callHelper("makeConstructor", constructorArgs, 2).asObject(*_runtime);
        constructor.setNativeState(*_runtime, makeFinalized<NativeClassState>(data));
        auto prototype = constructor.getPropertyAsObject(*_runtime, "prototype");

        auto makeMember = [&](const JSClassEntry& entry, JSClassCallback callback) -> jsi::Value {
            if (callback == nullptr) {
                return jsi::Value::undefined();
            }
            auto function = makeShared<NativeCallback>(data->makeMemberReferenceInfo(entry.getName()),
                [this, data, callback, isStatic = entry.isClassMember()](JSFunctionNativeCallContext& context) {
                    return withExceptionTracker(context.getExceptionTracker(), [&]() -> JSValueRef {
                        Ref<RefCountable> opaque;
                        if (isStatic) {
                            opaque = data->getOpaque();
                        } else {
                            const auto& receiver = getValue(context.getThisValue());
                            if (!receiver.isObject()) {
                                throw jsi::JSError::createTypeError(*_runtime, "Native class member called with an incompatible receiver");
                            }
                            auto object = receiver.asObject(*_runtime);
                            if (!object.hasNativeState<NativeObjectState>(*_runtime)) {
                                throw jsi::JSError::createTypeError(*_runtime, "Native class member called with an incompatible receiver");
                            }
                            auto state = object.getNativeState<NativeObjectState>(*_runtime);
                            if (state->nativeClass != data || state->opaque == nullptr) {
                                throw jsi::JSError::createTypeError(*_runtime, "Native class member called with an incompatible receiver");
                            }
                            opaque = state->opaque;
                        }
                        return callback(opaque.get(), context);
                    });
                });
            auto value = newFunction(function, exceptionTracker);
            if (!exceptionTracker) {
                return jsi::Value::undefined();
            }
            return jsi::Value(*_runtime, getValue(value.get()));
        };
        for (const auto& entry : definition.getEntries()) {
            const auto key = entry.getName().toStringView();
            auto& target = entry.isClassMember() ? constructor : prototype;
            if (entry.getKind() == JSClassEntryKind::Accessor) {
                if (entry.getGetterCallback() == nullptr && entry.getSetterCallback() == nullptr) {
                    throw jsi::JSError(*_runtime, "Native class accessor must have a getter or setter");
                }
                auto getter = makeMember(entry, entry.getGetterCallback());
                auto setter = makeMember(entry, entry.getSetterCallback());
                if (!exceptionTracker) {
                    return newUndefined();
                }
                jsi::Value args[] = {
                    jsi::Value(*_runtime, target),
                    jsi::String::createFromUtf8(*_runtime, reinterpret_cast<const uint8_t*>(key.data()), key.size()),
                    std::move(getter), std::move(setter), jsi::Value(entry.isEnumerable()),
                    jsi::Value(entry.isConfigurable()),
                };
                callHelper("defineAccessor", args, 6);
            } else {
                if (entry.getKind() == JSClassEntryKind::Method && entry.getMethodCallback() == nullptr) {
                    throw jsi::JSError(*_runtime, "Native class method callback cannot be null");
                }
                auto value = entry.getKind() == JSClassEntryKind::Constant
                    ? jsi::Value(*_runtime, getValue(entry.getValue().get()))
                    : makeMember(entry, entry.getMethodCallback());
                if (!exceptionTracker) {
                    return newUndefined();
                }
                jsi::Value args[] = {
                    jsi::Value(*_runtime, target),
                    jsi::String::createFromUtf8(*_runtime, reinterpret_cast<const uint8_t*>(key.data()), key.size()),
                    std::move(value), jsi::Value(entry.isEnumerable()), jsi::Value(entry.isWritable()),
                    jsi::Value(entry.isConfigurable()),
                };
                callHelper("defineData", args, 6);
            }
        }
        return hold(std::move(constructor));
    });
}

JSValueRef HermesJsiJavaScriptContext::newObjectFromNativeClass(const Ref<RefCountable>& opaque,
                                                               const JSValue& cls,
                                                               JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        const auto& value = getValue(cls);
        if (!value.isObject()) {
            throw jsi::JSError(*_runtime, "Value is not a native class");
        }
        auto constructor = value.asObject(*_runtime);
        if (!constructor.hasNativeState<NativeClassState>(*_runtime)) {
            throw jsi::JSError(*_runtime, "Value is not a native class");
        }
        if (opaque == nullptr) {
            throw jsi::JSError(*_runtime, "Native class opaque object cannot be null");
        }
        auto prototype = constructor.getProperty(*_runtime, "prototype");
        if (!prototype.isObject()) {
            throw jsi::JSError(*_runtime, "Native class prototype is not an object");
        }
        jsi::Object object(*_runtime);
        object.setPrototype(*_runtime, prototype);
        object.setNativeState(*_runtime, makeFinalized<NativeObjectState>(
            opaque, constructor.getNativeState<NativeClassState>(*_runtime)->data));
        return hold(std::move(object));
    });
}

JSValueRef HermesJsiJavaScriptContext::newWeakRef(const JSValue& value, JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        auto target = getValue(value).asObject(*_runtime);
        jsi::Object wrapper(*_runtime);
        auto state = std::shared_ptr<WeakState>(new WeakState(jsi::WeakObject(*_runtime, target)),
            [queue = _finalizers, this](WeakState* object) {
                // The raw context is used only inside the owner-thread drain,
                // including its final drain after Hermes has joined finalizers.
                queue->enqueue([this, object] {
                    _weakStates.erase(object);
                    delete object;
                });
            });
        _weakStates.insert(state.get());
        wrapper.setNativeState(*_runtime, std::move(state));
        return hold(std::move(wrapper));
    });
}

JSValueRef HermesJsiJavaScriptContext::derefWeakRef(const JSValue& value, JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        const auto& input = getValue(value);
        if (input.isObject()) {
            auto wrapper = input.asObject(*_runtime);
            if (wrapper.hasNativeState<WeakState>(*_runtime)) {
                return hold(wrapper.getNativeState<WeakState>(*_runtime)->object->lock(*_runtime));
            }
        }
        throw jsi::JSError(*_runtime, "Not a WeakReference");
    });
}

JSValueRef HermesJsiJavaScriptContext::newArrayBuffer(const BytesView& buffer, JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        return hold(jsi::ArrayBuffer(*_runtime, makeFinalized<ExternalBuffer>(buffer)));
    });
}

JSValueRef HermesJsiJavaScriptContext::newTypedArrayFromArrayBuffer(const TypedArrayType& type,
                                                                  const JSValue& arrayBuffer,
                                                                  JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSValueRef {
        if (type == TypedArrayType::ArrayBuffer) {
            return hold(jsi::Value(*_runtime, getValue(arrayBuffer)));
        }
        const auto index = static_cast<size_t>(type);
        if (index >= sizeof(kTypedArrayNames) / sizeof(kTypedArrayNames[0])) {
            throw jsi::JSError(*_runtime, "Unsupported TypedArray type");
        }
        jsi::Value args[] = {jsi::String::createFromUtf8(*_runtime, std::string(kTypedArrayNames[index])),
                             jsi::Value(*_runtime, getValue(arrayBuffer))};
        return hold(callHelper("makeTypedArray", args, 2));
    });
}

JSTypedArray HermesJsiJavaScriptContext::valueToTypedArray(const JSValue& value,
                                                         JSExceptionTracker& exceptionTracker) {
    return withExceptionTracker(exceptionTracker, [&]() -> JSTypedArray {
        const auto& input = getValue(value);
        if (!input.isObject()) {
            throw jsi::JSError(*_runtime, "Not a TypedArray or ArrayBuffer");
        }
        auto object = input.asObject(*_runtime);
        if (object.isArrayBuffer(*_runtime)) {
            auto buffer = object.getArrayBuffer(*_runtime);
            if (buffer.detached(*_runtime)) {
                return {};
            }
            auto* data = buffer.data(*_runtime);
            const auto size = buffer.size(*_runtime);
            return JSTypedArray(TypedArrayType::ArrayBuffer, data, size, hold(std::move(buffer)));
        }
        if (!object.isTypedArray(*_runtime)) {
            throw jsi::JSError(*_runtime, "Not a TypedArray or ArrayBuffer");
        }
        auto array = object.getTypedArray(*_runtime);
        auto buffer = array.buffer(*_runtime);
        if (buffer.detached(*_runtime)) {
            return {};
        }
        jsi::Value argument(*_runtime, object);
        const auto kind = callHelper("typedArrayKind", &argument, 1).asString(*_runtime).utf8(*_runtime);
        std::optional<TypedArrayType> type;
        for (size_t i = 0; i < sizeof(kTypedArrayNames) / sizeof(kTypedArrayNames[0]); ++i) {
            if (kind == kTypedArrayNames[i]) {
                type = static_cast<TypedArrayType>(i);
                break;
            }
        }
        if (!type) {
            throw jsi::JSError(*_runtime,
                "Unsupported TypedArray type (BigInt64Array and BigUint64Array are not supported)");
        }
        const auto offset = array.byteOffset(*_runtime);
        const auto size = array.byteLength(*_runtime);
        const auto length = array.length(*_runtime);
        const auto bufferSize = buffer.size(*_runtime);
        if (buffer.detached(*_runtime)) {
            return {};
        }
        if (offset > bufferSize || size > bufferSize - offset || length > size) {
            throw jsi::JSError(*_runtime, "TypedArray is out of bounds");
        }
        auto* data = buffer.data(*_runtime);
        if (data != nullptr) {
            data += offset;
        } else if (size != 0 || offset != 0) {
            throw jsi::JSError(*_runtime, "TypedArray has no backing storage");
        }
        return JSTypedArray(*type, data, size, hold(std::move(buffer)));
    });
}

} // namespace Valdi::Hermes
