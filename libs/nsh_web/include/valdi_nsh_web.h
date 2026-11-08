#ifndef VALDI_NSH_WEB_H
#define VALDI_NSH_WEB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NSH_WEB_ABI_VERSION 1u
typedef struct NshWebContext NshWebContext;
/* Never an engine JSValue or pointer. payload is an opaque ID for heap values,
 * IEEE-754 bits for NUMBER, and 0/1 for BOOL. flags must be zero. */
typedef struct { uint32_t tag, flags; uint64_t payload; } NshWebValue;
enum NshWebTag { NSH_UNDEFINED, NSH_NULL, NSH_BOOL, NSH_NUMBER, NSH_STRING,
    NSH_OBJECT, NSH_SYMBOL, NSH_BIGINT, NSH_EXCEPTION };
typedef struct { const uint8_t *data; size_t len; } NshWebBytes;
typedef NshWebValue (*NshWebNative)(NshWebContext *, NshWebValue, int32_t, NshWebValue *);
typedef void (*NshWebVisit)(void *, NshWebValue);
typedef void (*NshWebFinalize)(NshWebContext *, void *);
/* Return -1 to conservatively postpone collection if native state is borrowed. */
typedef int32_t (*NshWebTrace)(void *, NshWebVisit, void *);
typedef struct { const char *name; uint32_t symbol; NshWebNative call; int32_t length; } NshWebMethod;
typedef struct { const char *name; NshWebNative get, set; } NshWebAccessor;
typedef struct { const char *name; double value; } NshWebConstant;
typedef struct {
    uint32_t id; const char *name; int32_t parent_id; NshWebNative constructor; int32_t length;
    const NshWebMethod *methods; size_t methods_len;
    const NshWebAccessor *accessors; size_t accessors_len;
    const NshWebMethod *statics; size_t statics_len;
    const NshWebConstant *constants; size_t constants_len;
    uint32_t global;
} NshWebClassSpec;
/* operate() operands and return ownership are specified in ABI.md. */
enum NshWebOp {
    NSH_GLOBAL=1, NSH_STRING_NEW, NSH_TO_WTF8, NSH_TO_NUMBER, NSH_TO_BOOL,
    NSH_IS_FUNCTION, NSH_GET, NSH_GET_SYMBOL, NSH_DEFINE, NSH_CALL, NSH_CONSTRUCT,
    NSH_OBJECT_NEW, NSH_ARRAY_NEW, NSH_KEYS, NSH_FUNCTION_NEW, NSH_PROMISE_NEW,
    NSH_PROMISE_THEN, NSH_PROMISE_HANDLED,
    NSH_PROMISE_STATE /* reserved, never called */, NSH_IS_PROMISE /* reserved */,
    NSH_THROW, NSH_TAKE_EXCEPTION, NSH_ERROR, NSH_REPORT, NSH_JSON_PARSE,
    NSH_ARRAY_BUFFER_NEW, NSH_UINT8_ARRAY_NEW, NSH_VIEW_NEW, NSH_DETACH,
    NSH_IS_DETACHED, NSH_SET_PROTOTYPE, NSH_GET_PROTOTYPE, NSH_IS_UNCATCHABLE,
    NSH_ARRAY_BUFFER_TRANSFER
};
/* Symbol IDs: 1 iterator, 2 asyncIterator, 3 toStringTag.
 * Typed-array IDs: 0 Int8, 1 Uint8, 2 Uint8Clamped, 3 Int16, 4 Uint16,
 * 5 Int32, 6 Uint32, 7 Float32, 8 Float64, 9 BigInt64, 10 BigUint64.
 * DataView has kind -1. These are this ABI's constants, not engine enums. */
typedef struct { NshWebValue buffer; size_t offset, length, element; int32_t kind; } NshWebView;
typedef struct {
    uint32_t abi_version; size_t struct_size; void *user;
    NshWebValue (*operate)(void *, NshWebContext *, uint32_t op, NshWebValue receiver,
        const NshWebValue *args, size_t argc, NshWebBytes text, int64_t aux);
    void (*retain)(void *, NshWebValue);
    void (*release)(void *, NshWebValue);
    /* mode 0: BufferSource mutable bytes; mode 1: ToString WTF-8 bytes.
     * Return 1 success, 0 not BufferSource, -1 exception. Bytes valid until JS runs. */
    int32_t (*bytes)(void *, NshWebContext *, NshWebValue, uint32_t mode, NshWebBytes *);
    /* 1 view, 0 not view, -1 exception; buffer result is owned on success. */
    int32_t (*view)(void *, NshWebContext *, NshWebValue, NshWebView *);
    /* register_class copies metadata synchronously; results owned. Constructor
     * invokes spec.constructor with new.target as its second argument. */
    int32_t (*register_class)(void *, NshWebContext *, const NshWebClassSpec *,
        NshWebValue parent_proto, NshWebValue parent_ctor, NshWebValue *proto, NshWebValue *ctor);
    /* kind 1 = Rust object, kind 2 = callable Rust closure. On success host owns
     * data, calls finalize exactly once, and exposes trace to its GC. On failure
     * Rust still owns data. call may be NULL (noncallable). */
    NshWebValue (*native_new)(void *, NshWebContext *, NshWebValue proto, uint32_t kind,
        void *data, NshWebFinalize, NshWebTrace, NshWebNative call);
    void *(*native_get)(void *, NshWebContext *, NshWebValue, uint32_t kind);
} NshWebEngine;
NshWebContext *nsh_web_create(const NshWebEngine *);
int32_t nsh_web_install(NshWebContext *);
/* Host must finalize all attached native objects and release its owned returned
 * handles before destroy. No host callback may use ctx after destroy. */
void nsh_web_destroy(NshWebContext *);
/* Enumerates temporary/per-context roots, excluding object-owned Trace edges.
 * Call with JS stopped, before marking; also traverse each reachable native's
 * trace callback. This distinction permits collecting native<->JS cycles. */
int32_t nsh_web_trace_roots(NshWebContext *, NshWebVisit, void *);
int32_t nsh_web_set_base_url(NshWebContext *, NshWebBytes);
/* Transport is an ordered byte protocol. Visitors copy synchronously. No JS is
 * run while draining. HTTP op 1 Start, 2 Upload chunk, 3 Upload end, 4 Cancel,
 * 5 Pause, 6 Resume. Body kind 0 empty, 1 bytes, 2 streaming. */
typedef struct { NshWebBytes name, value; } NshWebHeader;
typedef struct {
    uint32_t id, op; NshWebBytes method, url;
    const NshWebHeader *headers; size_t headers_len;
    NshWebBytes bytes; uint32_t body_kind, follow_redirects;
} NshWebHttpOp;
typedef void (*NshWebHttpVisit)(void *, const NshWebHttpOp *);
void nsh_web_drain_http(NshWebContext *, NshWebHttpVisit, void *);
/* Event 1 response head, 2 chunk, 3 end, 4 error, 5 upload BodyWanted. Other fields unused per event. */
typedef struct {
    uint32_t id, event; uint16_t status; NshWebBytes status_text, url;
    const NshWebHeader *headers; size_t headers_len; NshWebBytes bytes;
    uint32_t redirected;
} NshWebHttpEvent;
int32_t nsh_web_http_event(NshWebContext *, const NshWebHttpEvent *);
/* Borrowed JS values, rooted by the host through this synchronous call. */
int32_t nsh_web_http_data_buffer(NshWebContext *, uint32_t id, NshWebValue view);
/* WS op 1 connect(url, protocols), 2 send text, 3 send binary, 4 close.
 * event 1 open(protocol), 2 text, 3 binary, 4 close(code,reason,clean), 5 error.
 * IDs are per context; transport never receives a JS handle. */
typedef struct {
    uint32_t id, op; NshWebBytes bytes; const NshWebBytes *protocols; size_t protocols_len;
    uint16_t code;
} NshWebSocketOp;
typedef void (*NshWebSocketVisit)(void *, const NshWebSocketOp *);
void nsh_web_drain_ws(NshWebContext *, NshWebSocketVisit, void *);
int32_t nsh_web_ws_event(NshWebContext *, uint32_t id, uint32_t event, NshWebBytes,
    uint16_t code, uint32_t clean);
int32_t nsh_web_ws_binary_buffer(NshWebContext *, uint32_t id, NshWebValue buffer);
void nsh_web_ws_buffered_amount(NshWebContext *, uint32_t id, uint64_t bytes);
#ifdef __cplusplus
}
#endif
#endif
