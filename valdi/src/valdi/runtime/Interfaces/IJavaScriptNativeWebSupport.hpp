#pragma once

#include "valdi/runtime/JavaScript/JavaScriptTypes.hpp"
#include "valdi_core/cpp/Utils/StringBox.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Valdi {

enum class NativeWebValueKind {
    Other,
    Symbol,
    BigInt,
};

struct NativeWebBufferInfo {
    JSValueRef buffer;
    uint8_t* data = nullptr;
    size_t byteOffset = 0;
    size_t byteLength = 0;
    size_t elementSize = 1;
    // NSH ABI kinds: -2 ArrayBuffer, -1 DataView, 0..10 numeric/BigInt arrays.
    int32_t kind = -2;
    bool detached = false;
};

// Optional engine primitives for native Web APIs. Existing class/buffer
// bindings keep their original semantics; NativeWeb requests these explicitly.
class IJavaScriptNativeWebSupport {
public:
    virtual ~IJavaScriptNativeWebSupport() = default;

    // A constructor-only function. The callback receives new.target as its
    // this value and returns the actual instance, including for subclasses.
    virtual JSValueRef newWebConstructor(const Ref<JSFunction>& callback,
                                        const StringBox& name,
                                        int32_t length,
                                        JSExceptionTracker& exceptionTracker) = 0;

    virtual NativeWebValueKind getWebValueKind(const JSValue& value) = 0;

    // A false result without an exception means the value is not BufferSource.
    // data covers this view, not the entire backing buffer. It is borrowed
    // while the source remains rooted and no detach or JS call takes place.
    virtual bool getWebBufferInfo(const JSValue& value,
                                  NativeWebBufferInfo& info,
                                  JSExceptionTracker& exceptionTracker) = 0;

    virtual JSValueRef newWebArrayBuffer(const uint8_t* bytes,
                                        size_t size,
                                        JSExceptionTracker& exceptionTracker) = 0;

    virtual JSValueRef newWebBufferView(const JSValue& buffer,
                                       size_t byteOffset,
                                       size_t elementCount,
                                       int32_t kind,
                                       JSExceptionTracker& exceptionTracker) = 0;

    virtual void detachWebArrayBuffer(const JSValue& buffer,
                                     JSExceptionTracker& exceptionTracker) = 0;

    // ECMAScript ToString encoded as WTF-8; lone UTF-16 surrogates survive.
    virtual std::string valueToWebWTF8(const JSValue& value,
                                      JSExceptionTracker& exceptionTracker) = 0;

    // Move into a fresh intrinsic ArrayBuffer, detaching the source and its
    // views. Reject non-ArrayBuffers and already detached buffers.
    virtual JSValueRef transferWebArrayBuffer(const JSValue& buffer,
                                             JSExceptionTracker& exceptionTracker) = 0;
};

} // namespace Valdi
