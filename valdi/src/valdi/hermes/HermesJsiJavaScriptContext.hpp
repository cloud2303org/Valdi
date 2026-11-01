#pragma once

#include "valdi/hermes/HermesDebuggerRegistry.hpp"
#include "valdi/runtime/Interfaces/IJavaScriptContext.hpp"
#include "valdi/runtime/JavaScript/JSNativeClassData.hpp"
#include "valdi_core/cpp/Threading/DispatchQueue.hpp"
#include <hermes/hermes.h>
#include <jsi/jsi.h>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Valdi::Hermes {
namespace jsi = facebook::jsi;

// Implements the existing Valdi engine contract using public JSI/Hermes APIs.
class HermesJsiJavaScriptContext final : public IJavaScriptContext {
public:
    explicit HermesJsiJavaScriptContext(JavaScriptTaskScheduler* taskScheduler, ILogger& logger);
    ~HermesJsiJavaScriptContext() override;

    JSValueRef getGlobalObject(JSExceptionTracker& exceptionTracker) final;

    BytesView preCompile(const std::string_view& script,
                         const std::string_view& sourceFilename,
                         JSExceptionTracker& exceptionTracker) final;

    bool supportsPreCompilation() const final;

    JSValueRef evaluate(const std::string& script,
                        const std::string_view& sourceFilename,
                        JSExceptionTracker& exceptionTracker) final;

    JSValueRef evaluatePreCompiled(const BytesView& script,
                                   const std::string_view& sourceFilename,
                                   JSExceptionTracker& exceptionTracker) final;

    JSPropertyNameRef newPropertyName(const std::string_view& str) final;

    StringBox propertyNameToString(const JSPropertyName& propertyName) final;

    JSValueRef newObject(JSExceptionTracker& exceptionTracker) final;

    JSValueRef newFunction(const Ref<JSFunction>& callable, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newStringUTF8(const std::string_view& str, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newStringUTF16(const std::u16string_view& str, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newArray(size_t initialSize, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newArrayBuffer(const BytesView& buffer, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newTypedArrayFromArrayBuffer(const TypedArrayType& type,
                                            const JSValue& arrayBuffer,
                                            JSExceptionTracker& exceptionTracker) final;

    JSValueRef newWrappedObject(const Ref<RefCountable>& wrappedObject, JSExceptionTracker& exceptionTracker) final;

    JSValueRef newNativeClass(const Ref<RefCountable>& classOpaque,
                              const JSClassDefinition& classDefinition,
                              JSExceptionTracker& exceptionTracker) final;

    JSValueRef newObjectFromNativeClass(const Ref<RefCountable>& opaque,
                                        const JSValue& cls,
                                        JSExceptionTracker& exceptionTracker) final;

    JSValueRef newWeakRef(const JSValue& object, JSExceptionTracker& exceptionTracker) final;

    JSValueRef getObjectProperty(const JSValue& object,
                                 const JSPropertyName& propertyName,
                                 JSExceptionTracker& exceptionTracker) final;

    JSValueRef getObjectPropertyForIndex(const JSValue& object,
                                         size_t index,
                                         JSExceptionTracker& exceptionTracker) final;

    bool hasObjectProperty(const JSValue& object, const JSPropertyName& propertyName) final;

    void setObjectProperty(const JSValue& object,
                           const JSPropertyName& propertyName,
                           const JSValue& propertyValue,
                           bool enumerable,
                           JSExceptionTracker& exceptionTracker) final;

    void setObjectProperty(const JSValue& object,
                           const JSValue& propertyName,
                           const JSValue& propertyValue,
                           bool enumerable,
                           JSExceptionTracker& exceptionTracker) final;

    JSValueRef callObjectAsFunction(const JSValue& object, JSFunctionCallContext& callContext) final;

    JSValueRef callObjectAsConstructor(const JSValue& object, JSFunctionCallContext& callContext) final;

    void setObjectPropertyIndex(const JSValue& object,
                                size_t index,
                                const JSValue& value,
                                JSExceptionTracker& exceptionTracker) final;

    void visitObjectPropertyNames(const JSValue& object,
                                  JSExceptionTracker& exceptionTracker,
                                  IJavaScriptPropertyNamesVisitor& visitor) final;

    JSValueRef derefWeakRef(const JSValue& weakRef, JSExceptionTracker& exceptionTracker) final;

    StringBox valueToString(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    Ref<StaticString> valueToStaticString(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    bool valueToBool(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    double valueToDouble(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    int32_t valueToInt(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    Ref<RefCountable> valueToWrappedObject(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    Ref<JSFunction> valueToFunction(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    JSTypedArray valueToTypedArray(const JSValue& value, JSExceptionTracker& exceptionTracker) final;

    ValueType getValueType(const JSValue& value) final;

    bool isValueUndefined(const JSValue& value) final;
    bool isValueNull(const JSValue& value) final;
    bool isValueFunction(const JSValue& value) final;
    bool isValueNumber(const JSValue& value) final;
    bool isValueLong(const JSValue& value) final;
    bool isValueObject(const JSValue& value) final;

    bool isValueEqual(const JSValue& left, const JSValue& right) final;

    void retainValue(const JSValue& value) final;
    void releaseValue(const JSValue& value) final;

    void retainPropertyName(const JSPropertyName& value) final;
    void releasePropertyName(const JSPropertyName& value) final;

    void garbageCollect() final;
    JavaScriptContextMemoryStatistics dumpMemoryStatistics() final;
    BytesView dumpHeap(JSExceptionTracker& exceptionTracker);

    void enqueueMicrotask(const JSValue& value, JSExceptionTracker& exceptionTracker) final;
    void markPromiseRejectionHandled(const JSValue& promise) final;

    void willEnterVM() final;
    void willExitVM(JSExceptionTracker& exceptionTracker) final;
    void requestExecutionTermination() final;

    void startDebugger(bool isWorker) final;
    std::optional<IJavaScriptContextDebuggerInfo> getDebuggerInfo() const final;

    void startProfiling() final;
    Result<std::vector<std::string>> stopProfiling() final;

protected:
    JSValueRef onNewBool(bool boolean) final;

    JSValueRef onNewNumber(int32_t number) final;

    JSValueRef onNewNumber(double number) final;

    JSValueRef onNewNull() final;

    JSValueRef onNewUndefined() final;

    void onInitialize(JSExceptionTracker& exceptionTracker) final;


private:
    // Encode primitive payloads in Valdi's own storage, without depending on the
    // private layout of jsi::Value. Only pointer-valued JSI handles need rooting.
    enum class InlineKind : uint64_t { Handle = 0, Number, Boolean, Null };
    struct InlineValue {
        uint64_t payload = 0;
        InlineKind kind = InlineKind::Handle;
    };
    static_assert(sizeof(InlineValue) == kJSValueStorageSize);

    struct ValueHandle {
        size_t references = 1;
        jsi::Value value;
        explicit ValueHandle(jsi::Value input) : value(std::move(input)) {}
    };
    struct PropertyHandle {
        size_t references = 1;
        jsi::PropNameID value;
        explicit PropertyHandle(jsi::PropNameID input) : value(std::move(input)) {}
    };
    struct NativeObjectState final : jsi::NativeState {
        Ref<RefCountable> opaque;
        Ref<JSNativeClassData> nativeClass;
        NativeObjectState(Ref<RefCountable> input, Ref<JSNativeClassData> cls = nullptr)
            : opaque(std::move(input)), nativeClass(std::move(cls)) {}
    };
    struct NativeClassState final : jsi::NativeState {
        Ref<JSNativeClassData> data;
        explicit NativeClassState(Ref<JSNativeClassData> input) : data(std::move(input)) {}
    };
    struct NativeFunctionState final : jsi::NativeState {
        Ref<JSFunction> function;
        explicit NativeFunctionState(Ref<JSFunction> input) : function(std::move(input)) {}
    };
    struct WeakState final : jsi::NativeState {
        std::optional<jsi::WeakObject> object;
        explicit WeakState(jsi::WeakObject input) : object(std::move(input)) {}
    };

    // Background finalizers publish ownership and wake the independent owner
    // queue. They never retain the runtime/context or call its task scheduler.
    struct FinalizerQueue : std::enable_shared_from_this<FinalizerQueue> {
        std::mutex mutex;
        std::vector<std::function<void()>> pending;
        Ref<DispatchQueue> ownerQueue;
        HermesJsiJavaScriptContext* owner = nullptr;
        bool wakeupScheduled = false;
        void enqueue(std::function<void()> finalizer);
        void close();
    };

    template<typename T, typename... Args>
    std::shared_ptr<T> makeFinalized(Args&&... args) {
        return std::shared_ptr<T>(new T(std::forward<Args>(args)...),
            [queue = _finalizers](T* object) {
                queue->enqueue([object] { delete object; });
            });
    }

    void drainFinalizers();

    ILogger& _logger;
    std::shared_ptr<FinalizerQueue> _finalizers = std::make_shared<FinalizerQueue>();
    std::unordered_set<WeakState*> _weakStates;
    // Native ownership must neither execute JS nor keep the promise alive.
    // These handles belong solely to the JS thread, never to a Hermes finalizer.
    std::vector<jsi::WeakObject> _nativeOwnedPromises;
    struct PromiseAdoption {
        jsi::WeakObject parent;
        jsi::WeakObject child;
    };
    std::vector<PromiseAdoption> _promiseAdoptions;
    void prunePromiseOwnership();
    bool isNativeOwnedPromise(const jsi::Value& promise);
    void markNativeOwnedPromise(const jsi::Value& promise);
    void recordPromiseAdoption(const jsi::Value& parent, const jsi::Value& child);
    bool _drainingFinalizers = false;
    std::shared_ptr<facebook::hermes::HermesRuntime> _runtime;
    std::unordered_set<ValueHandle*> _values;
    std::unordered_set<PropertyHandle*> _properties;
    std::optional<jsi::Object> _helpers;
    size_t _enterVMCount = 0;
    HermesRuntimeId _debuggerRuntimeId = kRuntimeIdUndefined;

    JSValueRef hold(jsi::Value value);
    JSPropertyNameRef hold(jsi::PropNameID value);
    jsi::Value getValue(const JSValue& value) const;
    const jsi::PropNameID& getPropertyName(const JSPropertyName& property) const;
    jsi::Value callHelper(const char* name, const jsi::Value* arguments, size_t count);
    void captureException(const jsi::JSError& error, JSExceptionTracker& tracker);
    jsi::Value invokeNative(const Ref<JSFunction>& function, const jsi::Value& thisValue,
                            const jsi::Value* arguments, size_t count);
    template<typename F>
    auto withExceptionTracker(JSExceptionTracker& tracker, F&& operation) -> decltype(operation()) {
        using Result = decltype(operation());
        try {
            return operation();
        } catch (const jsi::JSError& error) {
            captureException(error, tracker);
        } catch (const std::exception& error) {
            tracker.onError(error.what());
        }
        if constexpr (!std::is_void_v<Result>) {
            return Result{};
        }
    }
};
} // namespace Valdi::Hermes
