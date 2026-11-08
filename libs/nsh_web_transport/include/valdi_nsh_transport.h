#ifndef VALDI_NSH_TRANSPORT_H
#define VALDI_NSH_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NSH_TRANSPORT_ABI_VERSION 1u
typedef struct NshTransport NshTransport;
typedef struct { const uint8_t *data; size_t len; } NshTransportBytes;
typedef struct {
    uint8_t *data;
    size_t len;
    void *owner;
    void (*release)(void *);
} NshTransportOwnedBytes;
typedef struct { NshTransportBytes name, value; } NshTransportHeader;
/* wake MUST be thread-safe and nonblocking. It runs on transport threads, and
 * must only post AlwaysAsync work holding weak host state plus its epoch.
 * Never run JS, drain, stop, or destroy inline from wake. release_user runs once
 * when wake is revoked by stop/destroy (possibly on the calling thread).
 * user must remain valid until release_user. A NULL release_user means caller
 * owns user and must keep it valid through stop. No wake runs after stop returns; a drain visitor already running may finish. */
typedef struct {
    uint32_t abi_version; size_t struct_size; void *user;
    void (*wake)(void *user);
    void (*release_user)(void *user);
} NshTransportCallbacks;
/* create copies callbacks and starts an engine-free driver. Returns NULL on
 * invalid ABI or inability to start the driver. On failure caller retains user.
 * One handle per JS context. The handle contains no JS values. */
NshTransport *nsh_transport_create(const NshTransportCallbacks *);
/* Same numbering/field meanings as NshWebHttpOp. Requests are copied before
 * return. 0 accepted; -1 malformed input, stopped, or driver unavailable, or command queue full. */
enum NshTransportHttpOperation { NSH_TRANSPORT_HTTP_START=1,
    NSH_TRANSPORT_HTTP_SEND=2, NSH_TRANSPORT_HTTP_FINISH=3,
    NSH_TRANSPORT_HTTP_CANCEL=4, NSH_TRANSPORT_HTTP_PAUSE=5,
    NSH_TRANSPORT_HTTP_RESUME=6 };
typedef struct {
    uint32_t id, op; NshTransportBytes method, url;
    const NshTransportHeader *headers; size_t headers_len;
    NshTransportBytes bytes; uint32_t body_kind, follow_redirects;
} NshTransportHttpOp;
int32_t nsh_transport_http(NshTransport *, const NshTransportHttpOp *);
/* Events 1 head, 2 chunk, 3 end, 4 error and 5 streamed-upload BodyWanted
 * match NshWebHttpEvent and can be forwarded to the Web engine.
 * Error text is in bytes. */
enum NshTransportHttpEventKind { NSH_TRANSPORT_HTTP_HEAD=1,
    NSH_TRANSPORT_HTTP_DATA=2, NSH_TRANSPORT_HTTP_END=3,
    NSH_TRANSPORT_HTTP_ERROR=4, NSH_TRANSPORT_HTTP_BODY_WANTED=5 };
typedef struct {
    uint32_t id, event; uint16_t status; NshTransportBytes status_text, url;
    const NshTransportHeader *headers; size_t headers_len; NshTransportBytes bytes;
    uint32_t redirected;
} NshTransportHttpEvent;
typedef void (*NshTransportHttpVisit)(void *, const NshTransportHttpEvent *);
/* Drains a snapshot in order. Visitors run only on the calling (JS) thread,
 * copy borrowed fields synchronously, and may queue commands. No lock is held
 * while visiting. Do not recursively drain or destroy from a visitor. */
void nsh_transport_drain_http(NshTransport *, NshTransportHttpVisit, void *);
typedef void (*NshTransportHttpOwnedVisit)(
    void *, const NshTransportHttpEvent *, NshTransportOwnedBytes);
/* Same snapshot, threading, cancellation and visitor rules as the legacy drain.
 * Data transfers its original allocation exclusively to the visitor; event.bytes
 * describes that allocation. Other events pass an all-NULL/zero descriptor.
 * Metadata remains borrowed through callback return. For each non-NULL release,
 * call release(owner) exactly once, on any thread, even after stop/destroy or
 * context death. The consumer may mutate transferred bytes after delivery.
 * Zero-length Data may still carry an owner and must also be released.
 * A NULL visitor leaves the queue untouched. */
void nsh_transport_drain_http_owned(
    NshTransport *, NshTransportHttpOwnedVisit, void *);
/* WS operations 1..4 match web ABI v1. 5 abort drops a task without close;
 * 6/7 pause/resume reads. Extra handshake headers are optional. */
enum NshTransportSocketOperation { NSH_TRANSPORT_WS_CONNECT=1,
    NSH_TRANSPORT_WS_TEXT=2, NSH_TRANSPORT_WS_BINARY=3,
    NSH_TRANSPORT_WS_CLOSE=4, NSH_TRANSPORT_WS_ABORT=5,
    NSH_TRANSPORT_WS_PAUSE=6, NSH_TRANSPORT_WS_RESUME=7 };
typedef struct {
    uint32_t id, op; NshTransportBytes bytes;
    const NshTransportBytes *protocols; size_t protocols_len; uint16_t code;
    const NshTransportHeader *headers; size_t headers_len;
} NshTransportSocketOp;
int32_t nsh_transport_ws(NshTransport *, const NshTransportSocketOp *);
/* WS events 1 open(protocol), 2 text, 3 binary, 4 close(reason/code/clean),
 * 5 error match web ABI v1. Event 6 carries buffered_amount; route it to
 * nsh_web_ws_buffered_amount rather than nsh_web_ws_event. */
enum NshTransportSocketEventKind { NSH_TRANSPORT_WS_OPEN=1,
    NSH_TRANSPORT_WS_MESSAGE_TEXT=2, NSH_TRANSPORT_WS_MESSAGE_BINARY=3,
    NSH_TRANSPORT_WS_CLOSED=4, NSH_TRANSPORT_WS_ERROR=5,
    NSH_TRANSPORT_WS_BUFFERED=6 };
typedef struct {
    uint32_t id, event; NshTransportBytes bytes; uint16_t code;
    uint32_t clean; uint64_t buffered_amount;
} NshTransportSocketEvent;
typedef void (*NshTransportSocketVisit)(void *, const NshTransportSocketEvent *);
void nsh_transport_drain_ws(NshTransport *, NshTransportSocketVisit, void *);
typedef void (*NshTransportSocketOwnedVisit)(
    void *, const NshTransportSocketEvent *, NshTransportOwnedBytes);
/* Text and binary Message events transfer their original allocation. All other
 * events pass an all-NULL/zero descriptor and keep borrowed metadata. Ownership,
 * release, empty-message and visitor rules match drain_http_owned above. */
void nsh_transport_drain_ws_owned(
    NshTransport *, NshTransportSocketOwnedVisit, void *);
/* Idempotent, does not join I/O: revoke wake/release_user, clear pending events,
 * tell driver to drop/cancel all tasks. A wake already executing may finish
 * before stop returns; wake must only post, so this never waits for I/O.
 * Commands/drains/stop are thread-safe. Concurrent destroy/use is forbidden. */
void nsh_transport_stop(NshTransport *);
/* Stops then frees the handle. Driver cleanup may finish asynchronously.
 * Posted host work must check weak host state and epoch before using a handle. */
void nsh_transport_destroy(NshTransport *);
#ifdef __cplusplus
}
#endif
#endif
