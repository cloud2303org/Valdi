# valdi-nsh-transport ABI v1

## Rust consumer

Use `valdi-nsh-transport = { path = "../nsh_web_transport" }`.
HTTP names are exported at the root and through `platform::{FetchEvent, FetchOp,
HttpBody, HttpHead, HttpRequest, FetchEvents, FetchTask, Network}`. WebSocket names
are exported at the root and through `websocket::{WebSocketData,
WebSocketRequest, WebSocketEvent, WebSocketEvents, WebSocketTask,
WebSocketTransport}`. The fields, enum variants, HTTP constructors, and original
trait signatures match NSH commit 701bdcee9512e0f6aed39042d1fe24564dcd0b43.
There is no JS engine or UI dependency.

`network() -> Arc<dyn Network>` selects HttpURLConnection on Android,
NSURLSession on iOS, and NSH's ureq backend on desktop. FetchEvents remains
`Box<dyn FnMut(FetchEvent) + Send>`. Callbacks can run on I/O threads (or
synchronously for early platform errors): enqueue owned data onto the context's
queue. Do not execute JS or reenter a task method from a callback. Callbacks must
not panic. Task cancel consumes the box; native task drop also cancels. The
callback is released during cancellation even if a desktop socket read remains
blocked. The underlying ureq request thread cannot interrupt an in-progress
read/write; it stops before another chunk. This is inherited from NSH.

WebSocketEvents retains upstream's engine-local `Box<dyn FnMut(WebSocketEvent)>`
without Send. Use `websocket::transport()` or the additive
`websocket::transport_with_wake(Arc<dyn Fn() + Send + Sync>)`. A notification may
run on a worker and must only schedule a later host turn. On that turn call the
additive `WebSocketTask::dispatch() -> usize` (at most 32 events per call).
No callback runs inside connect/send/close/pause. The host owns polling or wake
scheduling. Pause stops socket reads; already queued messages may still arrive.
Sends, close and cancellation remain live while paused. Drop or the additive
`abort(self: Box<Self>)` aborts owned Tokio I/O and releases the callback
synchronously; socket cleanup follows when Tokio polls the aborted task. Native
command/event queues have eight slots, messages and queued outgoing bytes are
limited to 8 MiB, as in NSH. Handshake/write waits are 30 s; close waits are 5 s.

## C++ consumer

Include `include/valdi_nsh_transport.h` and link the `staticlib` output (or the
Rust web crate's staticlib when it references/reexports these functions).
This crate exports every `nsh_transport_*` declaration in that header, separately
from the sibling web engine header `nsh_web/include/valdi_nsh_web.h`.

Create one NshTransport per context. `nsh_transport_http` and `nsh_transport_ws`
copy all input bytes/headers/protocols before returning, then enqueue a command.
0 means accepted, -1 means malformed input, a full command queue, stopped, or an
unavailable driver. A successful enqueue can still produce an asynchronous
transport failure event. Use valid readable pointers for nonempty inputs; NULL
is allowed for zero-length bytes/arrays. Names, URLs, protocols, reason and text
frames are UTF-8; bodies/binary frames are bytes. IDs belong to the host context.
Use unique live IDs; reusing a Start/Connect ID cancels its old task and discards
its queued events. Commands are processed in their enqueue order.

The worker wake callback MUST be thread-safe and nonblocking. It must post
AlwaysAsync scheduler work containing only weak host state and the context's
captured epoch. That work checks weak state and epoch before using the handle,
then drains events on the JS thread. Never call JS, drain, stop or destroy inline
from wake. The transport serializes wake against stop; no wake executes after
stop returns. release_user is called once when stop revokes wake. On failed
create, caller retains user ownership. A null release_user leaves ownership with
the caller through stop. These are opaque host data, never JS handles.

`nsh_transport_drain_http/ws` drain a snapshot. Visitors run on the calling
thread, borrow each event only for their synchronous call, and copy bytes into
the web engine event input. No internal lock is held while visiting. Visitors
may enqueue commands; they must not recursively drain or destroy. Pending event
queues are bounded to 128 entries and 8 MiB per kind (one larger platform head
may occupy an otherwise empty queue). Full event queues apply back pressure to
the producer. Draining or stop wakes blocked producers. The command queue has
128 entries. Context stop does not wait for any platform cancellation or I/O:
it revokes wake, clears queued events, wakes producers and asks the driver to
cancel/drop its tasks. destroy calls stop and frees the handle; driver cleanup
may finish later. Operations/drains/stop may run concurrently; concurrent
use/destroy is forbidden. An already-running visitor may finish during stop.

Operation/event numbers match web ABI v1:

- HTTP operations: 1 Start, 2 Send upload chunk, 3 Finish upload, 4 Cancel,
  5 Pause, 6 Resume. Body kind: 0 Empty, 1 Bytes, 2 Stream.
- HTTP events: 1 Head, 2 Data, 3 End, 4 Failed (text in bytes), 5 BodyWanted.
  Web ABI v1 lacks BodyWanted input: the host must pump upload chunks on demand.
- WS operations: 1 Connect (URL in bytes), 2 Text, 3 Binary, 4 Close
  (code/reason), plus 5 Abort, 6 Pause, 7 Resume. Additional headers are optional.
- WS events: 1 Open (protocol in bytes), 2 Text, 3 Binary, 4 Close
  (reason/code/clean), 5 Error. Event 6 carries buffered_amount; route it to
  `nsh_web_ws_buffered_amount`, not `nsh_web_ws_event`.

The host drains web ops at outer bridge exit into transport operations. Transport
visitors only feed owned event inputs back on the JS thread. Destroy/stop must
invalidate the host epoch before any future posted drain can use the handle.
No engine handle or JS value crosses the transport boundary.

### Additive owned receive drains (2026-11-08)

ABI version 1 and all existing struct layouts, symbols, operation/event numbers
and legacy borrowed drains remain unchanged. Optional consumers can instead use:

```c
typedef struct {
    uint8_t *data;
    size_t len;
    void *owner;
    void (*release)(void *);
} NshTransportOwnedBytes;
typedef void (*NshTransportHttpOwnedVisit)(
    void *, const NshTransportHttpEvent *, NshTransportOwnedBytes);
typedef void (*NshTransportSocketOwnedVisit)(
    void *, const NshTransportSocketEvent *, NshTransportOwnedBytes);
void nsh_transport_drain_http_owned(
    NshTransport *, NshTransportHttpOwnedVisit, void *);
void nsh_transport_drain_ws_owned(
    NshTransport *, NshTransportSocketOwnedVisit, void *);
```

HTTP Data and WS Text/Binary Message callbacks receive exclusive ownership of
the original queued payload allocation. `event.bytes` describes the same data
and length as the descriptor. The allocation can be mutated by the recipient
or released immediately, including before the callback returns. Rust empties
the event's Vec/String before calling the visitor and never reads, writes or
frees the transferred payload afterward. WS text becomes bytes with the
allocation-preserving `String::into_bytes`. The release token is a separate
Box owning Vec metadata, including its original capacity; no payload is copied,
shrunk or converted into a boxed slice.

For **every descriptor with non-NULL release**, the recipient must eventually
call `release(owner)` **exactly once**. It can do so on any thread, independently
of transport stop/destroy and host context lifetime. This function frees only
Rust allocation storage and holds no transport, wake, JS or context state.
Do not free `data` separately or change Vec metadata. The descriptor is not an
idempotent handle: calling release twice is invalid. An empty payload also has
a token (and may retain capacity), so zero length does not waive release.
No bytes may be accessed after release. Retaining ownership can retain memory
beyond the queue's accounting bounds; the host owns the lifetime of delivered
allocations.

Other events receive an all-NULL/zero descriptor. Their existing event fields
(HTTP head/status/headers/URL/error text; WS protocol/reason/error and scalar
metadata) remain borrowed through callback return. A NULL visitor leaves the
queue untouched. Owned and legacy drains consume the same queues: an event is
delivered through only the drain that takes it.

The legacy and owned entries share the same snapshot implementation. HTTP
generation ownership and both pre-visitor invalidation checks are preserved;
reentrant Cancel/Start suppresses retired events already in a taken snapshot.
Tokens are created only for events actually admitted to an owned visitor.
Canceled, stale, unvisited and stopped events drop their Rust payloads normally.
Stop inside a visitor lets that call finish and discards the remaining snapshot.
Queue bounds, producer notification, callback threading, wake revocation,
asynchronous stop/driver teardown and the prohibition on recursive drains or
visitor/concurrent destruction are unchanged.

Private tests compare original queued Vec/String addresses with callback data,
including spare-capacity and zero-length allocations, and observe one actual
deallocation on a different thread after handle destruction. They introduce no
public production test API. Real loopback tests separately exercise split UTF-8
HTTP receive, WS text/binary/empty frames and cancel/replacement through a drain
snapshot. Rust 1.87 host evidence and source freezes are recorded under
`/tmp/valdi-nsh-owned-buffers-20261108/transport`. These are transport tests,
not C++/Hermes integration, device or TLS-policy proof. No TLS implementation,
dependency or trust configuration is changed by the owned receive API.

## Android startup

Ship `java/com/snap/valdi/nsh/Net.java` and `consumer-rules.pro` in the parent
Android library. The crate exports no JNI_OnLoad. Call
`valdi_nsh_transport_init_android(JNIEnv*)` (0 success, -1 failure) from the
parent JNI_OnLoad or an app native call with the app's class loader, before
starting fetch. `include/valdi_nsh_transport_android.h` declares this function.
The Rust equivalent is `init_android(&mut jni::JNIEnv)`.
Only the Net class and its five static methods are cached; no NSH UI/bootstrap
classes are needed. JNI callbacks are renamed `Java_com_snap_valdi_nsh_Net_*`.
Retain these exported native functions when linking the final shared library
(e.g. whole-archive/alwayslink for a standalone staticlib). Initialization is
idempotent, reports missing initialization as FetchEvent::Failed, and clears
pending JNI exceptions on C bootstrap failure. The parent owns all application
policy/permissions; this crate adds no AndroidManifest or network policy.

## TLS, proxies, redirects and validation boundaries

Android fetch uses HttpURLConnection and its default system trust/proxy settings,
30 s connect and 60 s read deadlines. iOS fetch uses default NSURLSession
configuration and Foundation system trust, proxies, cookies/cache and ATS.
It uses delegate suspend/resume for back pressure and a bound stream for uploads.
No trust delegate override or ATS exception is installed. The Objective-C class
name is ValdiNshUrlSessionDelegate, avoiding collision with an embedded NSH.

Desktop fetch uses NSH's ureq algorithm and ureq defaults: rustls with the bundled
WebPKI roots, not the macOS Keychain or Linux system CA store; ureq's environment
proxy detection remains in force. It follows at most 20 redirects, or returns
3xx for manual mode; HTTP 4xx/5xx are responses. Connect deadline is 30 s,
response-head deadline 60 s, with no response-body deadline. Full bodies are not
buffered. No custom roots, certificates, verifier or trust bypass are set.

All native WebSockets, including mobile, use NSH's tokio-tungstenite/rustls path
with bundled WebPKI roots and ring, not NSURLSession/Android system trust stores.
This path does not automatically adopt HTTP platform cookies, system proxies,
client identities or private enterprise CAs, and its handshake does not follow
redirects. Additional headers and selected subprotocols are preserved/checked.
These differences already exist between NSH's HTTP and WS backends.

The crate declares MSRV 1.87.0, uses edition 2024 and keeps Cargo.lock version 4.
The independent lock was resolved with Rust-version fallback, excluding newer
ICU versions requiring 1.88. Native host tests pass under Cargo/Rust 1.94 and
1.87; Android/iOS Rust target checks and Java compilation are source/build
validation only. They do not establish device networking, mobile TLS, JNI load,
platform streaming/proxy parity, or C++/Hermes integration. Linux uses the same
ureq source but has not been executed in this macOS session.

## Review revision R2

HTTP queue admission is invalidated for the old request before backend cancel
or replacement waits for callback teardown. The blocked producer observes this
invalidation even while another ID keeps the shared event queue full. Every
Start clears queued events for that ID, including completed/pruned requests.
Mobile registries release their process map lock before invoking per-request
callbacks. This changes no ABI field, symbol, operation, event or enqueue result;
a full command queue still returns -1 for the host's existing error/retry path.

## Review revision R3: visitor reentrancy and response generations

Queued HTTP events retain their request generation through drain snapshots.
Start/Cancel retire that generation before backend teardown, even when the task
has completed and been pruned. Each candidate event checks generation validity
again immediately before its visitor; retirement while an earlier visitor is
running suppresses the remaining old Data/End/Failed in the taken snapshot.
A visitor may still enqueue commands and wait for a later driver turn without
holding an internal lock. A visitor already admitted may finish concurrently
with retirement; this does not serialize or revoke an executing host callback.

The generation index holds weak references. Queue entries, producer callbacks
and drain snapshots own the generation while needed; the last owner removes its
index entry, with identity checking so old cleanup cannot erase a replacement.
No completed-ID tombstone or response is retained indefinitely by that index.
Public C/Rust signatures, operation/event numbers and queue results are unchanged.
