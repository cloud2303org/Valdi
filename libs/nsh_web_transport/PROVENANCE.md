# NSH provenance and modifications

Upstream: https://github.com/longbridge/nsh
Pinned commit: `701bdcee9512e0f6aed39042d1fe24564dcd0b43`.
Local read-only source: `/tmp/valdi-nsh-integration-source-20261107`.
Copyright 2026 Longbridge. Upstream license: Apache-2.0; original notice is
preserved verbatim in LICENSE-APACHE. No upstream NOTICE file was present.

This crate extracts only transport contracts and the actual native HTTP/WS
algorithms. It does not vendor the NSH workspace, UI, engine, or QuickJS.
The web algorithm port and C++/Hermes bindings are separately owned siblings.
All modifications here are transport-only and described below.

## Original source hashes

- `crates/nsh-core/src/platform.rs`
  - SHA-256: `12bf89652e1ed9a73cf6e7f3e4c06e574b05c33987ea6446e0d9015c9048ea1f`
  - Extracted use: src/lib.rs HTTP request/body/head/event/op types.
- `crates/nsh-core/src/websocket.rs`
  - SHA-256: `a0b33809b289dc2199593bf0187c98ffaa7acf496d07bd0fa748cee98e3d04c4`
  - Extracted use: src/websocket.rs contracts.
- `crates/nsh-engine/src/lib.rs`
  - SHA-256: `a4ccc521e6bad379c74876807c52d3ab4c62db53a1f371f60715ec8c65bea4b9`
  - Extracted use: src/lib.rs FetchEvents/FetchTask/Network.
- `crates/nsh-engine/src/net.rs`
  - SHA-256: `3a1fe7de92639eb17aac9e97156b7a9c6cf9384bbd9a973548c12434b48910f9`
  - Extracted use: src/ureq_backend.rs and tests/loopback/http.rs.
- `crates/nsh-platform-android/src/net.rs`
  - SHA-256: `e51b9e715af5652a6d0697beaa707d021935128c06751010e773c0bda9baaa49`
  - Extracted use: src/android.rs.
- `crates/nsh-platform-android/src/java.rs`
  - SHA-256: `0bf5af374a0e15902d0833db94f01032ff67155a40ffe77816231aeb6f7e5329`
  - Extracted use: src/java.rs HTTP-only bootstrap subset.
- `crates/nsh-platform-android/java/com/longbridge/nsh/Net.java`
  - SHA-256: `891adea10cb4f8cf0020ed3854eac241572b00b32506e129987715db2a183149`
  - Extracted use: java/com/snap/valdi/nsh/Net.java.
- `crates/nsh-platform-ios/src/net.rs`
  - SHA-256: `289365dc5d62b09d9a2da14fc1bcc041cf37f6fed409db24916be830ac850760`
  - Extracted use: src/ios.rs.
- `crates/nsh-js/src/standard/websocket_native.rs`
  - SHA-256: `b455a36c9d0c765fc7c3bd46b0be8db0695cc6a4dc372b3babaf819e4b85e8e6`
  - Extracted use: src/websocket/native.rs I/O algorithm.
- `Cargo.toml`
  - SHA-256: `7e5da7d0c7b9235b2a3c9a66cbf30dd696a5a2e16e0f2a79cf5b5877e855676a`
  - Extracted use: dependency declarations reviewed for extraction.
- `Cargo.lock`
  - SHA-256: `6fa3f7662411d54236fd1fe2bf0b2a73f85397f059a00233e4fcf0bee875bb8b`
  - Extracted use: upstream dependency version reference.
- `LICENSE-APACHE`
  - SHA-256: `ec75427ad955e3c740f22e7dc3f6db6fc5b2cc1d9135c2af91cece8dd47a2d38`
  - Extracted use: LICENSE-APACHE original notice.

## Modification ledger

- HTTP contracts copied from the indicated modules, with imports/doc links
  redirected to this independent crate. No enum variant or field was removed.
- ureq: original request/response/streaming/redirect algorithm retained. A shared
  optional callback sink is cleared on terminal delivery and cancel/drop, so
  callback ownership can end before a blocked I/O syscall. Drop now cancels.
- Android: original net.rs JNI and Net.java HttpURLConnection algorithm retained;
  package and native names become com.snap.valdi.nsh. java.rs retains only the
  VM, Net global class reference and its five method IDs. Added explicit Rust/C
  initialization, missing-init failure, JNI error delivery, cancel-on-drop, and
  shrinker keep rules. No UI bootstrap or JNI_OnLoad export is copied.
- iOS: original objc2 NSURLSession session/delegate/bound-stream code retained.
  Imports use local contracts; delegate class renamed ValdiNshUrlSessionDelegate.
  Callback sink and drop teardown mirror desktop cancellation ownership.
- WebSocket contracts retain upstream non-Send callbacks. Add dispatch, abort,
  native factory and thread-safe notification factory. The original tungstenite
  handshake, limits, send/read/ping/close timeouts, subprotocol check and clean/
  abnormal close algorithm remain, with Rust events replacing VM values. Tokio
  owns cancellable tasks on a two-worker runtime; bounded queues carry commands
  and events; task drop releases the engine-local callback synchronously.
- C ABI/driver, headers, documentation and added tests are new transport glue.
  A context driver owns tasks; callbacks carry only data, not JS handles. Wake
  revocation and asynchronous cleanup make stop independent of platform I/O.
- HTTP tests originate from nsh-engine net.rs, supplemented by delayed chunk,
  byte POST, drop and callback lifetime cases. WS/C ABI tests are independent
  real loopback fixtures, without JS or mocks. RED logs precede backend and
  lifecycle/ABI fixes under the external report directory.

## Dependency lock and MSRV

Cargo.toml declares Rust 1.87 / edition 2024. Cargo.lock version 4 is included
as source, with no NSH path dependencies. Direct version ranges follow the
upstream relevant libraries. The extraction's own lock resolves compatible
patches (notably ureq 3.5.1, Tokio 1.54.1, rustls 0.23.46), versus upstream's
ureq 3.4.2 / Tokio 1.53.3 / rustls 0.23.45. tokio-tungstenite/tungstenite stay
0.28.0. Rust-version fallback selected ICU 2.2 rather than versions requiring
1.88. Cargo/Rust 1.87 and 1.94 host tests pass. A consuming Cargo workspace uses
its own lockfile; the parent must preserve compatible resolutions.

The Cargo dependencies include ring/WebPKI for native WS and desktop ureq.
Mobile HTTP dependencies are cfg-gated (JNI on Android, objc2/Foundation on iOS).
There are no NSH, rquickjs, UI toolkit, JS runtime, HTTP policy, custom trust,
or framework permission dependencies. See ABI.md for the system-vs-WebPKI
trust-store/proxy differences and the bounded validation claims.

## Review revision R2 (2026-11-07)

Review transport-r1 findings reproduced before fixes in behavioral RED logs.
Added per-request queue admission invalidation, coupled to the queue mutex and
condition wait, before cancel/replacement backend teardown. Start now clears
old queued events unconditionally, including after terminal task pruning.
Android and iOS use the same private EventRegistry: it clones/removes a request's
Arc<EventSink> under the process-map lock and invokes the callback after releasing
that lock. Per-request callback ordering and synchronous cancellation ownership
remain. The actual mobile HTTP algorithms and default trust policies are unchanged.

Regression coverage: real C ABI HTTP loopback Cancel/replacement while another
ID owns a full queue, completed-ID reuse before a drain, and a portable test of
the exact shared mobile registry's concurrent registration/delivery/cancellation.
The registry test was RED with the original lock-across-callback pattern before
it was changed and wired into both mobile backends. It is not mobile runtime
proof. Headers, Cargo manifests/lock and command queue result behavior are unchanged.
All 25 pre-review files are preserved and hash-verified in the external r2/before
snapshot; new hashes and validation results are recorded in r2/REPORT.md.

## Review revision R3 (2026-11-07)

The actual C ABI Head visitor can enqueue same-ID Start/Cancel and wait for its
processing before returning. Two new loopback regressions reproduced old Data
and End continuing from an already-taken snapshot. HTTP envelopes now retain an
Arc<HttpGeneration>; a weak per-ID index permits retiring completed generations
still owned by a queue/snapshot after task pruning. Drain rechecks validity
immediately before each visitor, while preserving lock-free visitor calls.
Generation destruction removes only its matching weak index entry. R1/R2
cancel-aware admission and registry lock fixes are preserved. No headers,
Cargo dependencies/lock, platform I/O algorithms, WS behavior or public ABI
changed. The complete prior R2 freeze and new RED/GREEN evidence are preserved
under the external transports/r3 report directory.

## Additive owned receive buffers (2026-11-08)

`src/ffi.rs` adds the contract's `NshTransportOwnedBytes`, owned visitor types and
`nsh_transport_drain_http_owned` / `nsh_transport_drain_ws_owned` exports.
Existing layouts and borrowed drains are preserved. Both forms use the same
snapshot walker, including the R2 queue behavior and R3 HTTP generation checks.
Only HTTP Data and WS text/binary Message allocations move into a standalone
Box<Vec<u8>> release token; text uses String::into_bytes without reallocating.
The event is emptied before handoff. Event bytes describe recipient storage,
while other metadata stays borrowed through callback return. Release owns no
transport/context state and may run on any thread after transport destruction.
The token allocates Vec metadata, never another receive-payload buffer.

Task-start transport files matched the parent's `before/fork` snapshot exactly.
New private ownership tests first ran against an explicitly permitted copy-only
ABI scaffold: HTTP, WS text, WS binary and reserved-empty allocation pointer
assertions failed behaviorally before the allocation-moving implementation.
That RED source/log is preserved, not shipped. The final private tests observe
original allocation identity, retained capacity at zero length, writable bytes
after destruction and one actual deallocation on another thread. There is no
test-only public production API; allocator observations exist only in cfg(test).
Real loopback tests separately cover HTTP split UTF-8, WS text/binary/empty
messages and reentrant Cancel/Start invalidating a completed drain snapshot.
All original 35 host regressions remain present and pass with the additions.

Evidence, commands and exact frozen sources/hashes are under
`/tmp/valdi-nsh-owned-buffers-20261108/transport`. Validation uses isolated,
offline/locked Cargo host runs with Rust 1.87 and real local loopback servers.
It does not claim C++/Hermes, mobile execution or end-to-end zero-copy behavior.
No upstream platform networking source, TLS source, Cargo dependency/lockfile,
BUILD file, Bazel execution or canceled system-TLS test is part of this change.
