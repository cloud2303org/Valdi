#include "valdi/runtime/NativeWeb/NativeWeb.hpp"

#include "libs/nsh_web/include/valdi_nsh_web.h"
#include "libs/nsh_web_transport/include/valdi_nsh_transport.h"
#include "valdi/runtime/Interfaces/IJavaScriptNativeWebSupport.hpp"
#include "valdi/runtime/JavaScript/JSPromise.hpp"
#include "valdi/runtime/JavaScript/JavaScriptFunctionCallContext.hpp"
#include "valdi/runtime/JavaScript/JavaScriptTaskScheduler.hpp"
#include "valdi_core/cpp/Constants.hpp"
#include "valdi_core/cpp/Utils/FlatMap.hpp"
#include "valdi_core/cpp/Utils/FlatSet.hpp"
#include "valdi_core/cpp/Utils/StaticString.hpp"
#include "valdi_core/cpp/Utils/StringCache.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace Valdi {
namespace {

constexpr NshWebValue undefinedValue{NSH_UNDEFINED, 0, 0};
constexpr NshWebValue exceptionValue{NSH_EXCEPTION, 0, 0};
bool isHeap(NshWebValue v) { return v.tag >= NSH_STRING && v.tag <= NSH_BIGINT; }
NshWebValue numberValue(double n) {
    NshWebValue out{NSH_NUMBER, 0, 0};
    std::memcpy(&out.payload, &n, sizeof(n));
    return out;
}
NshWebValue boolValue(bool b) { return {NSH_BOOL, 0, b ? 1u : 0u}; }
std::string_view textView(NshWebBytes text) {
    return text.len == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(text.data), text.len);
}
NshTransportBytes transportBytes(NshWebBytes bytes) { return {bytes.data, bytes.len}; }
StringBox nameBox(std::string_view name) { return StringCache::getGlobal().makeString(name); }

class WebState;

// Web IDL legacy codes. Historical constants without an error name still
// exist, but must never match a caller-supplied DOMException name.
constexpr struct {
    std::string_view name, constant;
} domExceptionCodes[] = {
    {"IndexSizeError", "INDEX_SIZE_ERR"}, {"", "DOMSTRING_SIZE_ERR"},
    {"HierarchyRequestError", "HIERARCHY_REQUEST_ERR"}, {"WrongDocumentError", "WRONG_DOCUMENT_ERR"},
    {"InvalidCharacterError", "INVALID_CHARACTER_ERR"}, {"", "NO_DATA_ALLOWED_ERR"},
    {"NoModificationAllowedError", "NO_MODIFICATION_ALLOWED_ERR"}, {"NotFoundError", "NOT_FOUND_ERR"},
    {"NotSupportedError", "NOT_SUPPORTED_ERR"}, {"InUseAttributeError", "INUSE_ATTRIBUTE_ERR"},
    {"InvalidStateError", "INVALID_STATE_ERR"}, {"SyntaxError", "SYNTAX_ERR"},
    {"InvalidModificationError", "INVALID_MODIFICATION_ERR"}, {"NamespaceError", "NAMESPACE_ERR"},
    {"InvalidAccessError", "INVALID_ACCESS_ERR"}, {"", "VALIDATION_ERR"},
    {"TypeMismatchError", "TYPE_MISMATCH_ERR"}, {"SecurityError", "SECURITY_ERR"},
    {"NetworkError", "NETWORK_ERR"}, {"AbortError", "ABORT_ERR"},
    {"URLMismatchError", "URL_MISMATCH_ERR"}, {"QuotaExceededError", "QUOTA_EXCEEDED_ERR"},
    {"TimeoutError", "TIMEOUT_ERR"}, {"InvalidNodeTypeError", "INVALID_NODE_TYPE_ERR"},
    {"DataCloneError", "DATA_CLONE_ERR"},
};

// Immutable native data only, with no VM handles or Rust context. The private
// WeakMap owns the wrapper; collection is safe on a GC worker. StaticString
// retains UTF-16, including unpaired surrogates, without a UTF-8 round trip.
class DOMExceptionData final : public SimpleRefCountable {
  public:
    DOMExceptionData(Ref<StaticString> message, Ref<StaticString> name, uint16_t code)
        : message(std::move(message)), name(std::move(name)), code(code) {}
    const Ref<StaticString> message, name;
    const uint16_t code;
};

class DOMExceptionFunction final : public JSFunction {
  public:
    std::weak_ptr<WebState> state;
    // -1 constructs; 0/1/2 are the message/name/code getters.
    int field = -1;
    JSValueRef operator()(JSFunctionNativeCallContext& context) noexcept final;
};

// The JS thread owns these records until finalization is drained. A Hades GC
// worker may destroy Attachment, but never touches Rust state or JSValueRefs.
struct NativeRecord {
    void* data = nullptr;
    uint32_t kind = 0;
    NshWebFinalize finalize = nullptr;
    NshWebTrace trace = nullptr;
    JSValueRef ownerWeak;
    bool armed = false;
};

class Attachment final : public SimpleRefCountable {
  public:
    std::weak_ptr<WebState> state;
    std::shared_ptr<NativeRecord> record;
    ~Attachment() override;
};

class NativeFunction final : public JSFunction {
  public:
    std::weak_ptr<WebState> state;
    NshWebNative native = nullptr;
    Ref<Attachment> attachment;
    JSValueRef operator()(JSFunctionNativeCallContext& context) noexcept final;
};

struct Handle {
    uint32_t tag = 0;
    size_t references = 1;
    JSValueRef strong;
    JSValueRef weak;
};

// Independent allocation ownership only: destruction may run on a Hermes GC
// worker after WebState/transport teardown. Never capture Rust Web state or JS.
class OwnedReceiveBytes final : public SimpleRefCountable {
  public:
    explicit OwnedReceiveBytes(NshTransportOwnedBytes bytes) : bytes(bytes) {}
    ~OwnedReceiveBytes() override {
        if (bytes.release != nullptr) {
            bytes.release(bytes.owner);
        }
    }
    const NshTransportOwnedBytes bytes;
};

// Transport visitors retain payload ownership and copy borrowed metadata
// without running JS; at most 32 queued events are applied per scheduled turn.
struct Event {
    bool socket = false;
    uint32_t id = 0, kind = 0, clean = 0, redirected = 0;
    uint16_t status = 0, code = 0;
    uint64_t buffered = 0;
    std::string bytes, statusText, url;
    Ref<OwnedReceiveBytes> ownedBytes;
    std::vector<std::pair<std::string, std::string>> headers;
};
std::string copyBytes(NshTransportBytes bytes) {
    return bytes.len == 0 ? std::string() : std::string(reinterpret_cast<const char*>(bytes.data), bytes.len);
}

class WebState final : public std::enable_shared_from_this<WebState> {
  public:
    explicit WebState(IJavaScriptContext& cx)
        : cx(cx), support(dynamic_cast<IJavaScriptNativeWebSupport*>(&cx)), scheduler(cx.getTaskScheduler()) {}

    IJavaScriptContext& cx; // Borrowed; shutdown is required before VM teardown.
    IJavaScriptNativeWebSupport* support;
    JavaScriptTaskScheduler* scheduler;
    NshWebContext* rust = nullptr;
    NshTransport* transport = nullptr;
    FlatMap<uint64_t, Handle> handles;
    FlatMap<NativeRecord*, std::shared_ptr<NativeRecord>> records;
    std::mutex finalizationMutex;
    std::vector<std::shared_ptr<NativeRecord>> finalizations;
    JSValueRef exports;
    JSValueRef pending;
    std::vector<JSValueRef> reports;
    size_t depth = 0;
    uint64_t nextId = 1;
    bool refreshing = false;
    std::atomic<bool> closed{false};
    std::atomic<bool> wakePending{false};
    std::mutex postingMutex;
    std::deque<Event> events;
    FlatSet<uint32_t> failedHttp, failedSockets, httpCancels, socketAborts, activeHttp, activeSockets;
    bool networkFailed = false;
    std::optional<std::chrono::steady_clock::time_point> cancellationDeadline;
    std::deque<std::string> strings;

    JSValueRef global, nativeGlobal, object, defineProperty, createObject, setPrototype, getPrototype, keys, reflectGet;
    JSValueRef weakMapCtor, weakGet, weakSet, metadata, edges;
    JSValueRef promiseCtor, promiseThen, promiseResolve, jsonParse;
    JSValueRef errors[3], symbols[4];
    JSValueRef errorCtor, domExceptionCtor, domExceptionProto, domExceptionMetadata;

    JSValueRef keep(const JSValue& v) { return JSValueRef::makeRetained(cx, v); }
    JSValueRef call(const JSValueRef& function, const JSValueRef& receiver, const std::vector<JSValueRef>& args,
                    JSExceptionTracker& ex, bool construct = false) {
        if (!ex) {
            return cx.newUndefined();
        }
        JSFunctionCallContext context(cx, args.data(), args.size(), ex);
        context.setThisValue(receiver.get());
        return construct ? cx.callObjectAsConstructor(function.get(), context)
                         : cx.callObjectAsFunction(function.get(), context);
    }
    JSValueRef property(const JSValueRef& object, std::string_view key, JSExceptionTracker& ex) {
        return cx.getObjectProperty(object.get(), key, ex);
    }
    JSValueRef string(std::string_view value, JSExceptionTracker& ex) { return cx.newStringUTF8(value, ex); }
    JSValueRef blank(JSExceptionTracker& ex) { return call(createObject, object, {cx.newNull()}, ex); }
    void define(const JSValueRef& target, const JSValueRef& key, const JSValueRef& value, int64_t flags,
                JSExceptionTracker& ex) {
        if (!ex) {
            return;
        }
        auto descriptor = blank(ex);
        if (!ex) {
            return;
        }
        // A null-prototype descriptor cannot invoke an application's setters.
        cx.setObjectProperty(descriptor.get(), "value", value.get(), ex);
        cx.setObjectProperty(descriptor.get(), "writable", cx.newBool((flags & 1) != 0).get(), ex);
        cx.setObjectProperty(descriptor.get(), "enumerable", cx.newBool((flags & 2) != 0).get(), ex);
        cx.setObjectProperty(descriptor.get(), "configurable", cx.newBool((flags & 4) != 0).get(), ex);
        call(defineProperty, object, {target, key, descriptor}, ex);
    }
    void define(const JSValueRef& target, std::string_view key, const JSValueRef& value, int64_t flags,
                JSExceptionTracker& ex) {
        define(target, string(key, ex), value, flags, ex);
    }
    void exportGlobal(std::string_view name, const JSValueRef& value, int64_t flags, JSExceptionTracker& ex) {
        define(nativeGlobal, name, value, flags, ex);
        define(exports, name, value, flags, ex);
    }
    void prototype(const JSValueRef& objectValue, const JSValueRef& proto, JSExceptionTracker& ex) {
        call(setPrototype, object, {objectValue, proto}, ex);
    }
    JSValueRef array(const std::vector<JSValueRef>& values, JSExceptionTracker& ex) {
        auto array = cx.newArray(0, ex);
        for (size_t i = 0; i < values.size() && ex; ++i) {
            define(array, std::to_string(i), values[i], 7, ex);
        }
        return array;
    }
    JSValueRef fail(JSExceptionTracker& ex, std::string_view message, size_t kind = 0) {
        if (!errors[kind].empty()) {
            auto error = call(errors[kind], cx.newUndefined(), {string(message, ex)}, ex, true);
            if (ex) {
                ex.storeException(std::move(error));
            }
        } else {
            ex.onError(std::string(message));
        }
        return cx.newUndefined();
    }
    void capture(JSExceptionTracker& ex) {
        if (!ex) {
            pending = ex.getExceptionAndClear();
        }
    }
    void deliverException(JSExceptionTracker& ex) {
        if (!pending.empty()) {
            ex.storeException(std::move(pending));
        } else {
            fail(ex, "Native Web operation failed without an exception");
        }
    }

    // A returned ABI value owns one ID reference. Primitive bits never expose
    // an engine JSValue. Equal strings share an ID for Rust's SameValue helper.
    NshWebValue toRaw(const JSValueRef& value, JSExceptionTracker& ex) {
        if (!ex) {
            return exceptionValue;
        }
        uint32_t tag;
        auto kind = support->getWebValueKind(value.get());
        if (kind == NativeWebValueKind::Symbol) {
            tag = NSH_SYMBOL;
        } else if (kind == NativeWebValueKind::BigInt) {
            tag = NSH_BIGINT;
        } else if (cx.isValueUndefined(value.get())) {
            return undefinedValue;
        } else if (cx.isValueNull(value.get())) {
            return {NSH_NULL, 0, 0};
        } else if (cx.isValueNumber(value.get())) {
            return numberValue(cx.valueToDouble(value.get(), ex));
        } else if (cx.getValueType(value.get()) == ValueType::Bool) {
            return boolValue(cx.valueToBool(value.get(), ex));
        } else if (cx.isValueObject(value.get())) {
            tag = NSH_OBJECT;
        } else {
            tag = NSH_STRING;
        }

        std::string stringKey;
        if (tag == NSH_STRING || tag == NSH_BIGINT) {
            stringKey = support->valueToWebWTF8(value.get(), ex);
        }
        if (!ex) {
            return exceptionValue;
        }
        // Handle tables are small at each idle boundary: zero-reference entries
        // are retired there. No user code executes during equality/weak deref.
        for (auto& entry : handles) {
            auto& h = entry.second;
            if (h.tag != tag) {
                continue;
            }
            auto existing = !h.strong.empty() ? h.strong : cx.derefWeakRef(h.weak.get(), ex);
            if (!ex) {
                return exceptionValue;
            }
            if (cx.isValueUndefined(existing.get())) {
                continue;
            }
            bool equal = (tag == NSH_STRING || tag == NSH_BIGINT)
                             ? support->valueToWebWTF8(existing.get(), ex) == stringKey
                             : cx.isValueEqual(existing.get(), value.get());
            if (!ex) {
                return exceptionValue;
            }
            if (equal) {
                ++h.references;
                if (h.strong.empty()) {
                    h.strong = keep(existing.get());
                }
                return {tag, 0, entry.first};
            }
        }
        if (nextId == std::numeric_limits<uint64_t>::max()) {
            fail(ex, "Native Web handle IDs exhausted");
            return exceptionValue;
        }
        uint64_t id = nextId++;
        Handle handle;
        handle.tag = tag;
        handle.strong = keep(value.get());
        if (tag == NSH_OBJECT) {
            handle.weak = cx.newWeakRef(value.get(), ex);
        }
        if (!ex) {
            return exceptionValue;
        }
        handles.emplace(id, std::move(handle));
        return {tag, 0, id};
    }
    JSValueRef fromRaw(NshWebValue value, JSExceptionTracker& ex) {
        if (value.flags != 0) {
            return fail(ex, "Invalid Native Web value flags");
        }
        switch (value.tag) {
        case NSH_UNDEFINED:
            return cx.newUndefined();
        case NSH_NULL:
            return cx.newNull();
        case NSH_BOOL:
            return cx.newBool(value.payload != 0);
        case NSH_NUMBER: {
            double number;
            std::memcpy(&number, &value.payload, sizeof(number));
            return cx.newNumber(number);
        }
        case NSH_EXCEPTION:
            return fail(ex, "Exception sentinel used as a JavaScript value");
        default:
            break;
        }
        auto entry = handles.find(value.payload);
        if (entry == handles.end() || entry->second.tag != value.tag || entry->second.references == 0) {
            return fail(ex, "Invalid or stale Native Web value handle");
        }
        if (!entry->second.strong.empty()) {
            return entry->second.strong;
        }
        auto out = cx.derefWeakRef(entry->second.weak.get(), ex);
        if (ex && cx.isValueUndefined(out.get())) {
            return fail(ex, "Collected Native Web ownership edge");
        }
        return out;
    }
    void retain(NshWebValue v) noexcept {
        if (!isHeap(v)) {
            return;
        }
        auto i = handles.find(v.payload);
        if (i != handles.end() && i->second.tag == v.tag) {
            ++i->second.references;
        }
    }
    void release(NshWebValue v) noexcept {
        if (!isHeap(v)) {
            return;
        }
        auto i = handles.find(v.payload);
        if (i != handles.end() && i->second.tag == v.tag && i->second.references != 0) {
            --i->second.references;
        }
        // Do not destroy JS handles or mutate the table from a Rust finalizer.
    }
    bool enter(JSExceptionTracker& ex) {
        if (closed.load()) {
            fail(ex, "Native Web context is closed");
            return false;
        }
        if (depth++ != 0) {
            return true;
        }
        drainFinalizations();
        for (auto& entry : handles) {
            auto& h = entry.second;
            if (h.references != 0 && h.strong.empty()) {
                h.strong = cx.derefWeakRef(h.weak.get(), ex);
            }
            if (!ex) {
                break;
            }
        }
        if (!ex) {
            --depth;
            return false;
        }
        return true;
    }
    void leave(JSExceptionTracker& ex) {
        if (--depth != 0) {
            return;
        }
        strings.clear();
        if (closed.load()) {
            return;
        }
        JSExceptionTracker housekeeping(cx);
        drainFinalizations();
        refreshEdges(housekeeping);
        drainCommands(housekeeping);
        if (!housekeeping && ex) {
            ex.storeException(housekeeping.getExceptionAndClear());
        }
    }
    static void collectValue(void* user, NshWebValue value) noexcept {
        static_cast<Collector*>(user)->values.push_back(value);
    }
    struct Collector {
        std::vector<NshWebValue> values;
    };
    void refreshEdges(JSExceptionTracker& ex) {
        if (refreshing || rust == nullptr || !ex) {
            return;
        }
        refreshing = true;
        struct Reset {
            bool& b;
            ~Reset() { b = false; }
        } reset{refreshing};
        std::vector<std::shared_ptr<NativeRecord>> snapshot;
        snapshot.reserve(records.size());
        for (const auto& entry : records) {
            snapshot.push_back(entry.second);
        }
        struct Graph {
            std::shared_ptr<NativeRecord> attachment;
            Collector edges;
        };
        std::vector<Graph> graph;
        graph.reserve(snapshot.size());
        bool postpone = false;
        for (auto& attachment : snapshot) {
            Graph item{attachment, {}};
            if (attachment->data != nullptr && attachment->trace != nullptr &&
                attachment->trace(attachment->data, collectValue, &item.edges) < 0) {
                postpone = true;
            }
            graph.push_back(std::move(item));
        }
        Collector roots;
        if (nsh_web_trace_roots(rust, collectValue, &roots) < 0) {
            postpone = true;
        }
        // No JS ran above. Every live handle is still strong. Trace failures
        // postpone collection for this turn; a later boundary retries.
        if (postpone) {
            return;
        }
        for (auto& item : graph) {
            if (!item.attachment->armed || item.attachment->ownerWeak.empty()) {
                continue;
            }
            auto owner = cx.derefWeakRef(item.attachment->ownerWeak.get(), ex);
            if (!ex) {
                return;
            }
            if (cx.isValueUndefined(owner.get())) {
                continue;
            }
            auto storage = blank(ex);
            for (size_t index = 0; index < item.edges.values.size() && ex; ++index) {
                auto value = fromRaw(item.edges.values[index], ex);
                if (ex) {
                    cx.setObjectPropertyIndex(storage.get(), index, value.get(), ex);
                }
            }
            call(weakSet, edges, {owner, storage}, ex);
            if (!ex) {
                return;
            }
        }
        FlatSet<uint64_t> rootIds;
        for (auto value : roots.values) {
            if (isHeap(value)) {
                rootIds.insert(value.payload);
            }
        }
        // Commit all ephemeron values before weakening any handle, so a GC in
        // the middle of graph materialization cannot hide a new Rust edge.
        for (auto& entry : handles) {
            auto& h = entry.second;
            if (h.tag == NSH_OBJECT && rootIds.find(entry.first) == rootIds.end()) {
                h.strong = JSValueRef();
            }
        }
        for (auto it = handles.begin(); it != handles.end();) {
            if (it->second.references == 0) {
                auto retired = it++;
                handles.erase(retired);
            } else {
                ++it;
            }
        }
    }

    JSValueRef makeFunction(NshWebNative native, std::string_view name, int32_t length, JSExceptionTracker& ex,
                            bool constructor = false) {
        auto thunk = makeShared<NativeFunction>();
        thunk->state = shared_from_this();
        thunk->native = native;
        auto function =
            constructor ? support->newWebConstructor(thunk, nameBox(name), length, ex) : cx.newFunction(thunk, ex);
        if (!constructor) {
            define(function, "name", string(name, ex), 4, ex);
            define(function, "length", cx.newNumber(length), 4, ex);
        }
        return function;
    }
    void installDOMException(JSExceptionTracker& ex) {
        // Recover %Error% through the captured native TypeError prototype chain,
        // so an application replacing global.Error cannot supply this factory.
        auto errorProto = call(getPrototype, object, {property(errors[0], "prototype", ex)}, ex);
        errorCtor = property(errorProto, "constructor", ex);
        domExceptionProto = cx.newObject(ex);
        prototype(domExceptionProto, errorProto, ex);
        domExceptionMetadata = call(weakMapCtor, cx.newUndefined(), {}, ex, true);
        auto constructor = makeShared<DOMExceptionFunction>();
        constructor->state = shared_from_this();
        domExceptionCtor = support->newWebConstructor(constructor, STRING_LITERAL("DOMException"), 0, ex);
        define(domExceptionCtor, "prototype", domExceptionProto, 0, ex);
        define(domExceptionProto, "constructor", domExceptionCtor, 5, ex);
        constexpr std::string_view fields[] = {"message", "name", "code"};
        for (size_t i = 0; i < 3 && ex; ++i) {
            auto callback = makeShared<DOMExceptionFunction>();
            callback->state = shared_from_this();
            callback->field = static_cast<int>(i);
            auto getter = cx.newFunction(callback, ex);
            define(getter, "name", string(std::string("get ") + std::string(fields[i]), ex), 4, ex);
            define(getter, "length", cx.newNumber(0), 4, ex);
            auto descriptor = blank(ex);
            cx.setObjectProperty(descriptor.get(), "get", getter.get(), ex);
            cx.setObjectProperty(descriptor.get(), "enumerable", cx.newBool(true).get(), ex);
            cx.setObjectProperty(descriptor.get(), "configurable", cx.newBool(true).get(), ex);
            call(defineProperty, object, {domExceptionProto, string(fields[i], ex), descriptor}, ex);
        }
        for (size_t i = 0; i < std::size(domExceptionCodes) && ex; ++i) {
            auto value = cx.newNumber(static_cast<int32_t>(i + 1));
            define(domExceptionCtor, domExceptionCodes[i].constant, value, 2, ex);
            define(domExceptionProto, domExceptionCodes[i].constant, value, 2, ex);
        }
        define(domExceptionProto, symbols[3], string("DOMException", ex), 4, ex);
        exportGlobal("DOMException", domExceptionCtor, 5, ex);
    }
    JSValueRef constructDOMException(JSFunctionNativeCallContext& context) {
        auto& ex = context.getExceptionTracker();
        auto convert = [&](size_t index, std::string_view fallback) {
            auto value = context.getParameter(index);
            return cx.isValueUndefined(value) ? StaticString::makeUTF8(fallback.data(), fallback.size())
                                              : cx.valueToStaticString(value, ex);
        };
        auto message = convert(0, "");
        if (!ex) {
            return cx.newUndefined();
        }
        auto name = convert(1, "Error");
        if (!ex) {
            return cx.newUndefined();
        }
        auto proto = property(keep(context.getThisValue()), "prototype", ex);
        if (!ex) {
            return cx.newUndefined();
        }
        if (!cx.isValueObject(proto.get())) {
            proto = domExceptionProto;
        }
        uint16_t code = 0;
        auto nameText = name->toStdString();
        for (size_t i = 0; i < std::size(domExceptionCodes); ++i) {
            if (!domExceptionCodes[i].name.empty() && domExceptionCodes[i].name == nameText) {
                code = static_cast<uint16_t>(i + 1);
                break;
            }
        }
        // A real native Error provides [[ErrorData]] and the engine's stack.
        // No message argument: it must not shadow our readonly native getter.
        auto instance = call(errorCtor, cx.newUndefined(), {}, ex, true);
        prototype(instance, proto, ex);
        if (ex) {
            auto data = cx.newWrappedObject(makeShared<DOMExceptionData>(std::move(message), std::move(name), code), ex);
            call(weakSet, domExceptionMetadata, {instance, data}, ex);
        }
        return ex ? instance : cx.newUndefined();
    }
    JSValueRef getDOMExceptionField(JSFunctionNativeCallContext& context, int field) {
        auto& ex = context.getExceptionTracker();
        auto receiver = keep(context.getThisValue());
        if (!cx.isValueObject(receiver.get())) {
            return fail(ex, "Illegal DOMException receiver");
        }
        auto wrapped = call(weakGet, domExceptionMetadata, {receiver}, ex);
        if (!ex) {
            return cx.newUndefined();
        }
        if (cx.isValueUndefined(wrapped.get())) {
            return fail(ex, "Illegal DOMException receiver");
        }
        auto opaque = cx.valueToWrappedObject(wrapped.get(), ex);
        auto* data = dynamic_cast<DOMExceptionData*>(opaque.get());
        if (!ex) {
            return cx.newUndefined();
        }
        if (data == nullptr) {
            return fail(ex, "Illegal DOMException receiver");
        }
        return field == 2 ? cx.newNumber(data->code) : cx.newString(*(field == 0 ? data->message : data->name), ex);
    }
    int32_t registerClass(const NshWebClassSpec& spec, NshWebValue parentProto, NshWebValue parentCtor,
                          NshWebValue* protoOut, NshWebValue* ctorOut) {
        JSExceptionTracker ex(cx);
        auto proto = cx.newObject(ex);
        auto ctor = makeFunction(spec.constructor, spec.name, spec.length, ex, true);
        define(ctor, "prototype", proto, 0, ex);
        define(proto, "constructor", ctor, 5, ex);
        if (parentProto.tag != NSH_UNDEFINED) {
            prototype(proto, fromRaw(parentProto, ex), ex);
        }
        if (parentCtor.tag != NSH_UNDEFINED) {
            prototype(ctor, fromRaw(parentCtor, ex), ex);
        }
        for (size_t i = 0; i < spec.methods_len && ex; ++i) {
            const auto& method = spec.methods[i];
            if (method.symbol > 3) {
                fail(ex, "Unknown Native Web well-known symbol");
                break;
            }
            auto name = method.symbol == 1 ? "[Symbol.iterator]" : "[Symbol.asyncIterator]";
            auto key = method.symbol != 0 ? symbols[method.symbol] : string(method.name, ex);
            auto fn = makeFunction(method.call, method.symbol != 0 ? name : method.name, method.length, ex);
            define(proto, key, fn, 5, ex);
        }
        for (size_t i = 0; i < spec.accessors_len && ex; ++i) {
            const auto& accessor = spec.accessors[i];
            auto descriptor = blank(ex);
            auto getter = makeFunction(accessor.get, accessor.name, 0, ex);
            auto setter =
                accessor.set == nullptr ? cx.newUndefined() : makeFunction(accessor.set, accessor.name, 1, ex);
            cx.setObjectProperty(descriptor.get(), "get", getter.get(), ex);
            cx.setObjectProperty(descriptor.get(), "set", setter.get(), ex);
            cx.setObjectProperty(descriptor.get(), "enumerable", cx.newBool(true).get(), ex);
            cx.setObjectProperty(descriptor.get(), "configurable", cx.newBool(true).get(), ex);
            call(defineProperty, object, {proto, string(accessor.name, ex), descriptor}, ex);
        }
        for (size_t i = 0; i < spec.statics_len && ex; ++i) {
            const auto& method = spec.statics[i];
            define(ctor, method.name, makeFunction(method.call, method.name, method.length, ex), 5, ex);
        }
        for (size_t i = 0; i < spec.constants_len && ex; ++i) {
            auto value = cx.newNumber(spec.constants[i].value);
            define(ctor, spec.constants[i].name, value, 2, ex);
            define(proto, spec.constants[i].name, value, 2, ex);
        }
        define(proto, symbols[3], string(spec.name, ex), 4, ex);
        NshWebValue p = undefinedValue, c = undefinedValue;
        if (ex) {
            p = toRaw(proto, ex);
        }
        if (ex) {
            c = toRaw(ctor, ex);
        }
        if (!ex) {
            release(p);
            release(c);
            capture(ex);
            return -1;
        }
        *protoOut = p;
        *ctorOut = c;
        return 0;
    }
    NshWebValue newNative(NshWebValue proto, uint32_t kind, void* data, NshWebFinalize finalize, NshWebTrace trace,
                          NshWebNative native) {
        JSExceptionTracker ex(cx);
        auto attachment = makeShared<Attachment>();
        attachment->state = shared_from_this();
        auto record = std::make_shared<NativeRecord>();
        attachment->record = record;
        // Publish before allocating engine objects. Even a partially-created
        // native attachment must retire its weak handle on the JS thread.
        records.emplace(record.get(), record);
        record->data = data;
        record->kind = kind;
        record->finalize = finalize;
        record->trace = trace;
        auto wrapped = cx.newWrappedObject(attachment, ex);
        JSValueRef objectValue;
        if (native != nullptr) {
            auto function = makeShared<NativeFunction>();
            function->state = shared_from_this();
            function->attachment = attachment;
            function->native = native;
            objectValue = cx.newFunction(function, ex);
        } else {
            objectValue = wrapped;
        }
        if (ex) {
            prototype(objectValue, fromRaw(proto, ex), ex);
        }
        if (ex) {
            record->ownerWeak = cx.newWeakRef(objectValue.get(), ex);
        }
        if (ex) {
            call(weakSet, metadata, {objectValue, wrapped}, ex);
        }
        auto result = ex ? toRaw(objectValue, ex) : exceptionValue;
        if (!ex) {
            record->data = nullptr; // Rust still owns and drops data on failure.
            capture(ex);
            return exceptionValue;
        }
        record->armed = true;
        return result;
    }
    void* getNative(NshWebValue value, uint32_t kind) {
        JSExceptionTracker ex(cx);
        auto objectValue = fromRaw(value, ex);
        if (!ex || !cx.isValueObject(objectValue.get())) {
            capture(ex);
            return nullptr;
        }
        auto wrapped = call(weakGet, metadata, {objectValue}, ex);
        if (!ex || cx.isValueUndefined(wrapped.get())) {
            capture(ex);
            return nullptr;
        }
        auto opaque = cx.valueToWrappedObject(wrapped.get(), ex);
        auto* attachment = dynamic_cast<Attachment*>(opaque.get());
        if (!ex) {
            capture(ex);
        }
        if (attachment == nullptr || !attachment->record->armed || attachment->record->kind != kind) {
            return nullptr;
        }
        return attachment->record->data;
    }

    NshWebValue operate(uint32_t op, NshWebValue receiver, const NshWebValue* args, size_t argc, NshWebBytes text,
                        int64_t aux) {
        JSExceptionTracker ex(cx);
        auto value = [&] { return fromRaw(receiver, ex); };
        auto arg = [&](size_t i) { return i < argc ? fromRaw(args[i], ex) : cx.newUndefined(); };
        JSValueRef result = cx.newUndefined();
        switch (op) {
        case NSH_GLOBAL:
            result = nativeGlobal;
            break;
        case NSH_STRING_NEW:
            result = string(textView(text), ex);
            break;
        case NSH_TO_NUMBER:
            result = cx.newNumber(cx.valueToDouble(value().get(), ex));
            break;
        case NSH_TO_BOOL:
            result = cx.newBool(cx.valueToBool(value().get(), ex));
            break;
        case NSH_IS_FUNCTION:
            result = cx.newBool(cx.isValueFunction(value().get()));
            break;
        case NSH_GET: {
            auto target = value();
            result = property(target, textView(text), ex);
            break;
        }
        case NSH_GET_SYMBOL:
            if (aux < 1 || aux > 3) {
                fail(ex, "Unknown Native Web symbol");
            } else {
                result = call(reflectGet, cx.newUndefined(), {value(), symbols[aux]}, ex);
            }
            break;
        case NSH_DEFINE: {
            auto target = value();
            auto propertyValue = arg(0);
            if (ex && cx.isValueEqual(target.get(), nativeGlobal.get())) {
                exportGlobal(textView(text), propertyValue, aux, ex);
            } else {
                define(target, textView(text), propertyValue, aux, ex);
            }
            break;
        }
        case NSH_CALL:
        case NSH_CONSTRUCT: {
            std::vector<JSValueRef> parameters;
            size_t begin = op == NSH_CALL ? 1 : 0;
            for (size_t i = begin; i < argc && ex; ++i) {
                parameters.push_back(arg(i));
            }
            result = call(value(), op == NSH_CALL ? arg(0) : cx.newUndefined(), parameters, ex, op == NSH_CONSTRUCT);
            break;
        }
        case NSH_OBJECT_NEW:
            result = cx.newObject(ex);
            break;
        case NSH_ARRAY_NEW: {
            std::vector<JSValueRef> values;
            for (size_t i = 0; i < argc && ex; ++i) {
                values.push_back(arg(i));
            }
            result = array(values, ex);
            break;
        }
        case NSH_KEYS:
            result = call(keys, object, {value()}, ex);
            break;
        case NSH_PROMISE_NEW: {
            auto executor = makeShared<PromiseExecutor>();
            auto callback = cx.newFunction(executor, ex);
            auto promise = call(promiseCtor, cx.newUndefined(), {callback}, ex, true);
            if (ex) {
                result = array({promise, executor->extractResolve(), executor->extractReject()}, ex);
            }
            break;
        }
        case NSH_PROMISE_THEN: {
            auto normalized = call(promiseResolve, promiseCtor, {value()}, ex);
            result = call(promiseThen, normalized, {arg(0), arg(1)}, ex);
            break;
        }
        case NSH_PROMISE_HANDLED:
            cx.markPromiseRejectionHandled(value().get());
            break;
        case NSH_THROW:
            pending = value();
            return exceptionValue;
        case NSH_TAKE_EXCEPTION:
            result = pending.empty() ? cx.newUndefined() : std::move(pending);
            break;
        case NSH_ERROR:
            fail(ex, textView(text), aux >= 0 && aux < 3 ? static_cast<size_t>(aux) : 0);
            break;
        case NSH_REPORT:
            reports.push_back(value());
            requestWake();
            break;
        case NSH_JSON_PARSE:
            result = call(jsonParse, cx.newUndefined(), {string(textView(text), ex)}, ex);
            break;
        case NSH_ARRAY_BUFFER_NEW:
        case NSH_UINT8_ARRAY_NEW: {
            result = support->newWebArrayBuffer(text.data, text.len, ex);
            if (op == NSH_UINT8_ARRAY_NEW && ex) {
                result = support->newWebBufferView(result.get(), 0, text.len, 1, ex);
            }
            break;
        }
        case NSH_VIEW_NEW: {
            auto offset = cx.valueToDouble(arg(0).get(), ex);
            auto count = cx.valueToDouble(arg(1).get(), ex);
            if (!std::isfinite(offset) || !std::isfinite(count) || offset < 0 || count < 0 ||
                offset >= static_cast<double>(SIZE_MAX) || count >= static_cast<double>(SIZE_MAX) ||
                std::floor(offset) != offset || std::floor(count) != count) {
                fail(ex, "Invalid buffer view range", 1);
            } else if (ex) {
                result = support->newWebBufferView(value().get(), static_cast<size_t>(offset),
                                                   static_cast<size_t>(count), static_cast<int32_t>(aux), ex);
            }
            break;
        }
        case NSH_DETACH:
            support->detachWebArrayBuffer(value().get(), ex);
            break;
        case NSH_ARRAY_BUFFER_TRANSFER:
            result = support->transferWebArrayBuffer(value().get(), ex);
            break;
        case NSH_IS_DETACHED: {
            NativeWebBufferInfo info;
            bool buffer = support->getWebBufferInfo(value().get(), info, ex);
            result = cx.newBool(buffer && info.detached);
            break;
        }
        case NSH_SET_PROTOTYPE:
            prototype(value(), arg(0), ex);
            break;
        case NSH_GET_PROTOTYPE:
            result = call(getPrototype, object, {value()}, ex);
            break;
        case NSH_IS_UNCATCHABLE:
            result = cx.newBool(cx.executionTerminationRequested());
            break;
        case NSH_PROMISE_STATE:
        case NSH_IS_PROMISE:
        case NSH_FUNCTION_NEW:
        case NSH_TO_WTF8:
        default:
            fail(ex, "Reserved or unsupported Native Web ABI operation");
            break;
        }
        if (!ex) {
            capture(ex);
            return exceptionValue;
        }
        auto raw = toRaw(result, ex);
        if (!ex) {
            capture(ex);
            return exceptionValue;
        }
        return raw;
    }

    // This runtime is built with -fno-exceptions. JavaScript failures travel
    // through JSExceptionTracker and the pending ABI exception, never C++ unwind.
    template <typename F> NshWebValue boundary(F&& f) noexcept { return f(); }
    void setFailure(const char* message) noexcept {
        JSExceptionTracker ex(cx);
        fail(ex, message);
        capture(ex);
    }
    NshWebEngine engine() {
        NshWebEngine table{};
        table.abi_version = NSH_WEB_ABI_VERSION;
        table.struct_size = sizeof(table);
        table.user = this;
        table.operate = [](void* user, NshWebContext*, uint32_t op, NshWebValue receiver, const NshWebValue* args,
                           size_t argc, NshWebBytes text, int64_t aux) noexcept {
            auto& self = *static_cast<WebState*>(user);
            return self.boundary([&] { return self.operate(op, receiver, args, argc, text, aux); });
        };
        table.retain = [](void* user, NshWebValue v) noexcept { static_cast<WebState*>(user)->retain(v); };
        table.release = [](void* user, NshWebValue v) noexcept { static_cast<WebState*>(user)->release(v); };
        table.bytes = [](void* user, NshWebContext*, NshWebValue v, uint32_t mode,
                         NshWebBytes* out) noexcept -> int32_t {
            auto& self = *static_cast<WebState*>(user);
            int32_t status = -1;
            self.boundary([&] {
                JSExceptionTracker ex(self.cx);
                auto value = self.fromRaw(v, ex);
                if (mode == 1) {
                    self.strings.push_back(self.support->valueToWebWTF8(value.get(), ex));
                    if (ex) {
                        const auto& bytes = self.strings.back();
                        *out = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
                        status = 1;
                    }
                } else if (mode == 0) {
                    NativeWebBufferInfo info;
                    bool found = self.support->getWebBufferInfo(value.get(), info, ex);
                    if (ex) {
                        status = found ? 1 : 0;
                        if (found) {
                            *out = {info.data, info.byteLength};
                        }
                    }
                } else {
                    self.fail(ex, "Unknown Native Web bytes mode");
                }
                self.capture(ex);
                return undefinedValue;
            });
            return status;
        };
        table.view = [](void* user, NshWebContext*, NshWebValue v, NshWebView* out) noexcept -> int32_t {
            auto& self = *static_cast<WebState*>(user);
            int32_t status = -1;
            self.boundary([&] {
                JSExceptionTracker ex(self.cx);
                auto value = self.fromRaw(v, ex);
                NativeWebBufferInfo info;
                bool found = self.support->getWebBufferInfo(value.get(), info, ex);
                if (ex && found && info.kind != -2) {
                    auto buffer = self.toRaw(info.buffer, ex);
                    if (ex) {
                        *out = {buffer, info.byteOffset, info.byteLength, info.elementSize, info.kind};
                        status = 1;
                    }
                } else if (ex) {
                    status = 0;
                }
                self.capture(ex);
                return undefinedValue;
            });
            return status;
        };
        table.register_class = [](void* user, NshWebContext*, const NshWebClassSpec* spec, NshWebValue pp,
                                  NshWebValue pc, NshWebValue* proto, NshWebValue* ctor) noexcept -> int32_t {
            auto& self = *static_cast<WebState*>(user);
            int32_t status = -1;
            self.boundary([&] {
                status = self.registerClass(*spec, pp, pc, proto, ctor);
                return undefinedValue;
            });
            return status;
        };
        table.native_new = [](void* user, NshWebContext*, NshWebValue proto, uint32_t kind, void* data,
                              NshWebFinalize finalize, NshWebTrace trace, NshWebNative call) noexcept {
            auto& self = *static_cast<WebState*>(user);
            return self.boundary([&] { return self.newNative(proto, kind, data, finalize, trace, call); });
        };
        table.native_get = [](void* user, NshWebContext*, NshWebValue v, uint32_t kind) noexcept -> void* {
            auto& self = *static_cast<WebState*>(user);
            void* result = nullptr;
            self.boundary([&] {
                result = self.getNative(v, kind);
                return undefinedValue;
            });
            return result;
        };
        return table;
    }

    bool initialize(JSExceptionTracker& ex) {
        if (support == nullptr) {
            ex.onError("JavaScript engine does not implement Native Web primitives");
            return false;
        }
        if (scheduler == nullptr) {
            ex.onError("Native Web requires a JavaScript task scheduler");
            return false;
        }
        global = cx.getGlobalObject(ex);
        object = property(global, "Object", ex);
        defineProperty = property(object, "defineProperty", ex);
        createObject = property(object, "create", ex);
        setPrototype = property(object, "setPrototypeOf", ex);
        getPrototype = property(object, "getPrototypeOf", ex);
        keys = property(object, "keys", ex);
        reflectGet = property(property(global, "Reflect", ex), "get", ex);
        weakMapCtor = property(global, "WeakMap", ex);
        auto weakPrototype = property(weakMapCtor, "prototype", ex);
        weakGet = property(weakPrototype, "get", ex);
        weakSet = property(weakPrototype, "set", ex);
        metadata = call(weakMapCtor, cx.newUndefined(), {}, ex, true);
        edges = call(weakMapCtor, cx.newUndefined(), {}, ex, true);
        promiseCtor = property(global, "Promise", ex);
        promiseThen = property(property(promiseCtor, "prototype", ex), "then", ex);
        promiseResolve = property(promiseCtor, "resolve", ex);
        jsonParse = property(property(global, "JSON", ex), "parse", ex);
        errors[0] = property(global, "TypeError", ex);
        errors[1] = property(global, "RangeError", ex);
        errors[2] = property(global, "SyntaxError", ex);
        auto symbol = property(global, "Symbol", ex);
        symbols[1] = property(symbol, "iterator", ex);
        symbols[2] = property(symbol, "asyncIterator", ex);
        symbols[3] = property(symbol, "toStringTag", ex);
        exports = cx.newObject(ex);
        // Rust's internal globals own the Web definitions and inherit host
        // services (timers, Promise, etc.) without modifying the real global.
        nativeGlobal = call(createObject, object, {global}, ex);
        if (!ex) {
            return false;
        }
        installDOMException(ex);
        if (!ex) {
            return false;
        }
        auto table = engine();
        rust = nsh_web_create(&table);
        if (rust == nullptr) {
            ex.onError("Unable to create Rust Web context");
            return false;
        }
        NshTransportCallbacks callbacks{};
        callbacks.abi_version = NSH_TRANSPORT_ABI_VERSION;
        callbacks.struct_size = sizeof(callbacks);
        auto* weak = new std::weak_ptr<WebState>(shared_from_this());
        callbacks.user = weak;
        callbacks.wake = [](void* user) noexcept {
            auto self = static_cast<std::weak_ptr<WebState>*>(user)->lock();
            if (self != nullptr) {
                self->requestWake();
            }
        };
        callbacks.release_user = [](void* user) noexcept { delete static_cast<std::weak_ptr<WebState>*>(user); };
        transport = nsh_transport_create(&callbacks);
        if (transport == nullptr) {
            delete weak;
            ex.onError("Unable to create Rust Web network transport");
            return false;
        }
        if (!enter(ex)) {
            return false;
        }
        if (nsh_web_install(rust) < 0) {
            deliverException(ex);
        } else {
            pending = {};
        }
        leave(ex);
        return static_cast<bool>(ex);
    }

    void drainCommands(JSExceptionTracker& ex) {
        if (rust == nullptr || transport == nullptr || closed.load()) {
            return;
        }
        nsh_web_drain_http(
            rust,
            [](void* user, const NshWebHttpOp* op) noexcept {
                auto& self = *static_cast<WebState*>(user);
                std::vector<NshTransportHeader> headers;
                headers.reserve(op->headers_len);
                for (size_t i = 0; i < op->headers_len; ++i) {
                    headers.push_back({transportBytes(op->headers[i].name), transportBytes(op->headers[i].value)});
                }
                NshTransportHttpOp request{op->id,
                                           op->op,
                                           transportBytes(op->method),
                                           transportBytes(op->url),
                                           headers.data(),
                                           headers.size(),
                                           transportBytes(op->bytes),
                                           op->body_kind,
                                           op->follow_redirects};
                int32_t accepted = self.networkFailed ? -1 : nsh_transport_http(self.transport, &request);
                if (accepted == 0 && op->op == 1) {
                    self.activeHttp.insert(op->id);
                }
                if (accepted == 0 && op->op == 4) {
                    self.activeHttp.erase(op->id);
                }
                if (accepted < 0) {
                    if (!self.networkFailed && self.activeHttp.find(op->id) != self.activeHttp.end()) {
                        self.httpCancels.insert(op->id);
                        self.startCancellationDeadline();
                    }
                    if (op->op != 4 && self.failedHttp.insert(op->id).second) {
                        Event failure;
                        failure.id = op->id;
                        failure.kind = 4;
                        failure.bytes = "Network transport rejected the request command";
                        self.events.push_back(std::move(failure));
                    }
                    self.requestWake(1);
                }
            },
            this);
        nsh_web_drain_ws(
            rust,
            [](void* user, const NshWebSocketOp* op) noexcept {
                auto& self = *static_cast<WebState*>(user);
                std::vector<NshTransportBytes> protocols;
                for (size_t i = 0; i < op->protocols_len; ++i) {
                    protocols.push_back(transportBytes(op->protocols[i]));
                }
                NshTransportSocketOp request{
                    op->id,  op->op, transportBytes(op->bytes), protocols.data(), protocols.size(), op->code,
                    nullptr, 0};
                int32_t accepted = self.networkFailed ? -1 : nsh_transport_ws(self.transport, &request);
                if (accepted == 0 && op->op == 1) {
                    self.activeSockets.insert(op->id);
                }
                if (accepted < 0) {
                    if (!self.networkFailed && self.activeSockets.find(op->id) != self.activeSockets.end()) {
                        self.socketAborts.insert(op->id);
                        self.startCancellationDeadline();
                    }
                    if (op->op != 5 && self.failedSockets.insert(op->id).second) {
                        Event failure;
                        failure.socket = true;
                        failure.id = op->id;
                        failure.kind = 5;
                        failure.bytes = "Network transport rejected the WebSocket command";
                        self.events.push_back(std::move(failure));
                        Event close;
                        close.socket = true;
                        close.id = op->id;
                        close.kind = 4;
                        close.code = 1006;
                        self.events.push_back(std::move(close));
                    }
                    self.requestWake(1);
                }
            },
            this);
    }
    void startCancellationDeadline() {
        if (!cancellationDeadline) {
            cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    }
    void failNetwork() {
        // ABI v1 does not distinguish a full command queue from a dead driver.
        // A bounded retry window ends by canceling the entire native transport
        // and rejecting every outstanding operation, never by spinning forever.
        networkFailed = true;
        nsh_transport_stop(transport);
        for (auto id : activeHttp) {
            if (failedHttp.insert(id).second) {
                Event e;
                e.id = id;
                e.kind = 4;
                e.bytes = "Network command driver unavailable";
                events.push_back(std::move(e));
            }
        }
        for (auto id : activeSockets) {
            if (failedSockets.insert(id).second) {
                Event e;
                e.socket = true;
                e.id = id;
                e.kind = 5;
                e.bytes = "Network command driver unavailable";
                events.push_back(std::move(e));
                Event close;
                close.socket = true;
                close.id = id;
                close.kind = 4;
                close.code = 1006;
                events.push_back(std::move(close));
            }
        }
        httpCancels.clear();
        socketAborts.clear();
        cancellationDeadline.reset();
    }
    void retryCancellations() {
        if (networkFailed) {
            return;
        }
        if (cancellationDeadline && std::chrono::steady_clock::now() >= *cancellationDeadline) {
            failNetwork();
            return;
        }
        // Driver queue saturation cannot turn cancel/abort into a lost command.
        // Retry only native IDs; no JS value or payload waits on another thread.
        for (auto it = httpCancels.begin(); it != httpCancels.end();) {
            NshTransportHttpOp op{};
            op.id = *it;
            op.op = 4;
            if (nsh_transport_http(transport, &op) < 0) {
                break;
            }
            activeHttp.erase(*it);
            auto accepted = it++;
            httpCancels.erase(accepted);
        }
        for (auto it = socketAborts.begin(); it != socketAborts.end();) {
            NshTransportSocketOp op{};
            op.id = *it;
            op.op = 5;
            if (nsh_transport_ws(transport, &op) < 0) {
                break;
            }
            auto accepted = it++;
            socketAborts.erase(accepted);
        }
        if (httpCancels.empty() && socketAborts.empty()) {
            cancellationDeadline.reset();
        }
    }
    void requestWake(uint32_t delayMs = 0) noexcept {
        // Shutdown marks closed under the same lock, then revokes the transport
        // callback before clearing the borrowed scheduler. No lock waits for JS.
        std::lock_guard<std::mutex> lock(postingMutex);
        if (closed.load() || scheduler == nullptr || wakePending.exchange(true)) {
            return;
        }
        std::weak_ptr<WebState> weak = shared_from_this();
        scheduler->dispatchOnJsThread(JsThreadDispatchReason::NativeWeb,
                                      JavaScriptTaskScheduleTypeAlwaysAsync, delayMs,
                                      [weak](JavaScriptEntryParameters& entry) {
                                          auto self = weak.lock();
                                          if (self == nullptr || self->closed.load() || &self->cx != &entry.jsContext) {
                                              return;
                                          }
                                          self->wakePending = false;
                                          self->pump(entry.exceptionTracker);
                                      });
    }
    void pump(JSExceptionTracker& ex) {
        if (!enter(ex)) {
            return;
        }
        retryCancellations();
        // Keep the native driver's queue bounds after drain: do not repeatedly
        // append snapshots while a previous snapshot is still being delivered.
        if (events.empty()) {
            nsh_transport_drain_http_owned(
                transport,
                [](void* user, const NshTransportHttpEvent* event, NshTransportOwnedBytes owned) noexcept {
                    auto& self = *static_cast<WebState*>(user);
                    Event e;
                    e.ownedBytes = makeShared<OwnedReceiveBytes>(owned);
                    e.id = event->id;
                    e.kind = event->event;
                    e.status = event->status;
                    e.statusText = copyBytes(event->status_text);
                    e.url = copyBytes(event->url);
                    if (event->event != NSH_TRANSPORT_HTTP_DATA) {
                        e.bytes = copyBytes(event->bytes);
                    }
                    e.redirected = event->redirected;
                    for (size_t i = 0; i < event->headers_len; ++i) {
                        e.headers.emplace_back(copyBytes(event->headers[i].name), copyBytes(event->headers[i].value));
                    }
                    self.events.push_back(std::move(e));
                },
                this);
            nsh_transport_drain_ws_owned(
                transport,
                [](void* user, const NshTransportSocketEvent* event, NshTransportOwnedBytes owned) noexcept {
                    auto& self = *static_cast<WebState*>(user);
                    Event e;
                    e.ownedBytes = makeShared<OwnedReceiveBytes>(owned);
                    e.socket = true;
                    e.id = event->id;
                    e.kind = event->event;
                    e.code = event->code;
                    e.clean = event->clean;
                    e.buffered = event->buffered_amount;
                    if (event->event != NSH_TRANSPORT_WS_MESSAGE_TEXT &&
                        event->event != NSH_TRANSPORT_WS_MESSAGE_BINARY) {
                        e.bytes = copyBytes(event->bytes);
                    }
                    self.events.push_back(std::move(e));
                },
                this);
        }
        for (size_t count = 0; count < 32 && !events.empty() && ex; ++count) {
            auto e = std::move(events.front());
            events.pop_front();
            NshWebBytes bytes{reinterpret_cast<const uint8_t*>(e.bytes.data()), e.bytes.size()};
            if (e.ownedBytes != nullptr && e.ownedBytes->bytes.release != nullptr) {
                bytes = {e.ownedBytes->bytes.data, e.ownedBytes->bytes.len};
            }
            int32_t status = 0;
            if ((!e.socket && e.kind == NSH_TRANSPORT_HTTP_DATA) ||
                (e.socket && e.kind == NSH_TRANSPORT_WS_MESSAGE_BINARY)) {
                // Empty buffers must remain distinct native-Web objects. For a
                // nonempty payload the engine retains this independent owner,
                // including through intrinsic TransferArrayBuffer and GC.
                auto buffer = bytes.len == 0
                                  ? support->newWebArrayBuffer(nullptr, 0, ex)
                                  : cx.newArrayBuffer(BytesView(e.ownedBytes, bytes.data, bytes.len), ex);
                auto payload = e.socket || !ex
                                   ? buffer
                                   : support->newWebBufferView(buffer.get(), 0, bytes.len, 1, ex);
                if (ex) {
                    auto raw = toRaw(payload, ex);
                    if (ex) {
                        status = e.socket ? nsh_web_ws_binary_buffer(rust, e.id, raw)
                                          : nsh_web_http_data_buffer(rust, e.id, raw);
                    }
                    release(raw);
                }
            } else if (e.socket) {
                if (e.kind == 6) {
                    nsh_web_ws_buffered_amount(rust, e.id, e.buffered);
                } else {
                    status = nsh_web_ws_event(rust, e.id, e.kind, bytes, e.code, e.clean);
                }
            } else {
                std::vector<NshWebHeader> headers;
                for (auto& header : e.headers) {
                    headers.push_back({{reinterpret_cast<const uint8_t*>(header.first.data()), header.first.size()},
                                       {reinterpret_cast<const uint8_t*>(header.second.data()), header.second.size()}});
                }
                NshWebHttpEvent event{e.id,
                                      e.kind,
                                      e.status,
                                      {reinterpret_cast<const uint8_t*>(e.statusText.data()), e.statusText.size()},
                                      {reinterpret_cast<const uint8_t*>(e.url.data()), e.url.size()},
                                      headers.data(),
                                      headers.size(),
                                      bytes,
                                      e.redirected};
                status = nsh_web_http_event(rust, &event);
            }
            if (status < 0) {
                deliverException(ex);
            } else {
                pending = {};
            }
            if (e.socket && e.kind == 4) {
                failedSockets.erase(e.id);
                activeSockets.erase(e.id);
            }
            if (!e.socket && (e.kind == 3 || e.kind == 4)) {
                failedHttp.erase(e.id);
                activeHttp.erase(e.id);
            }
        }
        if (!reports.empty() && ex) {
            ex.storeException(std::move(reports.front()));
            reports.erase(reports.begin());
        }
        leave(ex);
        if (!events.empty() || !reports.empty() || !httpCancels.empty() || !socketAborts.empty()) {
            requestWake(events.empty() && reports.empty() ? 4 : 0);
        }
    }
    void enqueueFinalization(const std::shared_ptr<NativeRecord>& record) noexcept {
        {
            std::lock_guard<std::mutex> lock(finalizationMutex);
            if (closed.load()) {
                return;
            }
            finalizations.push_back(record);
        }
        requestWake();
    }
    void finishRecord(const std::shared_ptr<NativeRecord>& record) {
        // JS-thread only. Clear first so reentry/shutdown cannot double-drop.
        auto* data = record->data;
        record->data = nullptr;
        if (record->armed && data != nullptr && rust != nullptr && record->finalize != nullptr) {
            record->finalize(rust, data);
        }
        record->ownerWeak = {};
    }
    void drainFinalizations() {
        std::vector<std::shared_ptr<NativeRecord>> pendingRecords;
        {
            std::lock_guard<std::mutex> lock(finalizationMutex);
            pendingRecords.swap(finalizations);
        }
        for (auto& record : pendingRecords) {
            finishRecord(record);
            records.erase(record.get());
        }
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(postingMutex);
            if (closed.exchange(true)) {
                return;
            }
        }
        if (transport != nullptr) {
            nsh_transport_stop(transport);
        }
        scheduler = nullptr;
        events.clear();
        failedHttp.clear();
        failedSockets.clear();
        httpCancels.clear();
        socketAborts.clear();
        activeHttp.clear();
        activeSockets.clear();
        cancellationDeadline.reset();
        reports.clear();
        std::vector<std::shared_ptr<NativeRecord>> snapshot;
        for (const auto& entry : records) {
            snapshot.push_back(entry.second);
        }
        for (auto& record : snapshot) {
            finishRecord(record);
        }
        // closed was published before this snapshot. A concurrent GC release
        // either queued its record already, or observes closed and leaves its
        // JS-thread-owned record to this complete, exactly-once drain.
        {
            std::lock_guard<std::mutex> lock(finalizationMutex);
            finalizations.clear();
        }
        records.clear();
        // Finalizers may enqueue cancellations; stop above cancels all transport
        // tasks, so no finalizer-created cancellation needs a live JS callback.
        if (transport != nullptr) {
            nsh_transport_destroy(transport);
            transport = nullptr;
        }
        if (rust != nullptr) {
            nsh_web_destroy(rust);
            rust = nullptr;
        }
        handles.clear();
        strings.clear();
        exports = {};
        pending = {};
        global = {};
        nativeGlobal = {};
        object = {};
        defineProperty = {};
        createObject = {};
        setPrototype = {};
        getPrototype = {};
        keys = {};
        reflectGet = {};
        weakMapCtor = {};
        weakGet = {};
        weakSet = {};
        metadata = {};
        edges = {};
        promiseCtor = {};
        promiseThen = {};
        promiseResolve = {};
        jsonParse = {};
        errorCtor = {};
        domExceptionCtor = {};
        domExceptionProto = {};
        domExceptionMetadata = {};
        for (auto& error : errors) {
            error = {};
        }
        for (auto& symbol : symbols) {
            symbol = {};
        }
    }
};

Attachment::~Attachment() {
    auto owner = state.lock();
    if (owner != nullptr) {
        owner->enqueueFinalization(record);
    }
    // record remains owned by WebState until the JS thread clears its JSValueRef
    // and finalizes Rust. After shutdown all its engine fields are already empty.
}

JSValueRef DOMExceptionFunction::operator()(JSFunctionNativeCallContext& context) noexcept {
    auto owner = state.lock();
    if (owner == nullptr || owner->closed.load()) {
        return context.throwError(Error("Native Web context is closed"));
    }
    return field < 0 ? owner->constructDOMException(context) : owner->getDOMExceptionField(context, field);
}

JSValueRef NativeFunction::operator()(JSFunctionNativeCallContext& context) noexcept {
    auto owner = state.lock();
    if (owner == nullptr || owner->closed.load()) {
        return context.throwError(Error("Native Web context is closed"));
    }
    auto& ex = context.getExceptionTracker();
    bool entered = false;
    std::vector<NshWebValue> args;
    NshWebValue receiver = undefinedValue, result = undefinedValue;
    JSValueRef out;
    entered = owner->enter(ex);
    if (!entered) {
        return context.getContext().newUndefined();
    }
    auto thisValue = attachment != nullptr ? owner->cx.derefWeakRef(attachment->record->ownerWeak.get(), ex)
                                           : owner->keep(context.getThisValue());
    if (ex) {
        receiver = owner->toRaw(thisValue, ex);
    }
    args.reserve(context.getParameterSize());
    for (size_t i = 0; i < context.getParameterSize() && ex; ++i) {
        args.push_back(owner->toRaw(owner->keep(context.getParameter(i)), ex));
    }
    if (ex && native == nullptr) {
        owner->fail(ex, "Illegal constructor");
    }
    if (ex) {
        result = native(owner->rust, receiver, static_cast<int32_t>(args.size()), args.data());
    }
    if (ex && result.tag == NSH_EXCEPTION) {
        owner->deliverException(ex);
    } else if (ex) {
        // A Rust algorithm may deliberately recover from a failed probe. Only
        // its exception sentinel propagates a throw to the calling JavaScript.
        owner->pending = {};
    }
    if (ex) {
        out = owner->fromRaw(result, ex);
    }
    owner->release(receiver);
    for (auto value : args) {
        owner->release(value);
    }
    owner->release(result);
    if (entered) {
        owner->leave(ex);
    }
    return ex && !out.empty() ? out : context.getContext().newUndefined();
}

struct Registry {
    std::mutex mutex;
    FlatMap<IJavaScriptContext*, std::shared_ptr<WebState>> states;
};
Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace

StringBox NativeWebModuleFactory::getModulePath() const { return STRING_LITERAL("native_web/src/NativeWebNative"); }

JSValueRef NativeWebModuleFactory::loadModule(IJavaScriptContext& context, const ReferenceInfoBuilder&,
                                              JSExceptionTracker& ex) {
    auto& store = registry();
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        auto i = store.states.find(&context);
        if (i != store.states.end()) {
            if (i->second->closed.load()) {
                ex.onError("Native Web context is closed");
                return context.newUndefined();
            }
            return i->second->exports;
        }
    }
    auto state = std::make_shared<WebState>(context);
    bool initialized = state->initialize(ex);
    if (!initialized) {
        state->shutdown();
        return context.newUndefined();
    }
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.states.emplace(&context, state);
    }
    return state->exports;
}

void NativeWeb::shutdown(IJavaScriptContext& context) {
    std::shared_ptr<WebState> state;
    auto& store = registry();
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        auto i = store.states.find(&context);
        if (i == store.states.end()) {
            // Even a first module require queued behind teardown must fail.
            state = std::make_shared<WebState>(context);
            store.states.emplace(&context, state);
        } else {
            state = i->second;
        }
    }
    state->shutdown();
}

void NativeWeb::release(IJavaScriptContext& context) {
    auto& store = registry();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto i = store.states.find(&context);
    if (i == store.states.end()) {
        return;
    }
    // Shutdown must have completed on the owning JS queue, followed by queue
    // quiescence. This path can then retire a partial runtime on its destruction
    // thread without entering Rust or touching any engine-managed handles.
    SC_ASSERT(i->second->closed.load(), "Native Web release requires completed shutdown");
    store.states.erase(i);
}
} // namespace Valdi
