# NSH Rust Web API reuse

Source: longbridge/nsh, commit `701bdcee9512e0f6aed39042d1fe24564dcd0b43`.
Original repository: `https://github.com/longbridge/nsh`.
Copyright 2026 Longbridge. Apache-2.0; see `LICENSE-APACHE` copied verbatim.

`upstream/web/` contains exact `crates/nsh-js/src/web/` originals for event,
URL, encoding, Blob/File/FormData, Fetch/Headers/Request/Response, iterator,
WebSocket, all Streams modules, and their js/mod engine layers.
`upstream/standard/websocket{,_native}.rs` preserves the source for transport
validation and limits that originally lived outside the WebSocket class.
`upstream/SHA256.json` pins every original. Intl is intentionally excluded.
None of the original engine files or their imports is compiled.

`src/web/` retains the Web algorithms and class Specs. The adaptation boundary:

- `js.rs`: retains NSH's Trace/closure/iteration/Promise helpers, with engine
  operations replaced by the C ABI and an ownership census. Context teardown
  drains Web/socket state before releasing census storage. The original
  primitive-number/exception layout is replaced completely with ABI tags/IDs.
- `mod.rs`: retains Object, Class, Key, Spec, ClassTs, Function and declaration
  metadata; replaces engine-specific lazy class registration, native attachment, captured intrinsic Function prototype,
  finalization and tracing with host callbacks. Classes install eagerly once.
  Valdi module registration belongs to the parent C++ integration.
- Event, Fetch, encoding and Streams: replace let chains with nested conditions
  (10 sites) or Option::filter (2 sites) as needed for Rust 1.87. Original branch
  order and mutations remain. Other edits replace typed-array/symbol constants,
  JSON parse, and Promise-state reads with engine-neutral helpers.
- Deferred has Rust-owned shared settlement state for writer ready/closed. The
  two places reconstructing/cloning Deferred use from_parts/clone_ref. Generic
  Promise brand and state operations are reserved and unused. Host Promise.then
  uses captured intrinsics; the engine's Promise implementation remains intact.
- Blob: replaces NSH UI ImageData with a small shared byte owner; removes only
  UI image Prop/ImageSource integration. Blob slicing, URLs, body reads, form
  encoding/parsing and HTTP handling are retained. Date uses system unix time.
- Fetch: minimal shared net operation and dropped-body queues replace the
  nsh_core Host. Transport types are re-exported from valdi-nsh-transport. Access
  denial keeps the same code string; the NSH declarations macro returns metadata.
- `transport.rs`: adapts the upstream WebSocket transport's handle interface to
  ordered raw byte operations and per-context callbacks. Retains synchronous
  ws/wss/credential/fragment and message validation, 8 MiB limits, and a bounded
  8-message outbound command queue. Network policy and actual sockets are host
  transport concerns. No reactive-owner/hot-reload hooks or NSH System quotas
  are imported into the independent crate.

`upstream/port.patch` contains every source difference for the Web modules.
`vendor.py` verifies the source checkout commit and copies exact originals;
`adapt.py`, `adapt_js.py`, `adapt_mod.py` reproduce the ported modules.
`verify_sources.py` checks hashes, patch consistency, absence of QuickJS from
Cargo metadata, and all dependency-declared MSRVs. To refresh the patch after
review, run `python3 scripts/verify_sources.py --write-patch`.

The retained Spec TypeScript strings are declarations only. The crate has no JS
implementation, eval source, VM, JS polyfill, quickjs-jit-stdlib, or nsh_core
runtime dependency. Upstream JS engine references in `upstream/` are archival
provenance, not linked code. Rust and C++ use distinct ABI representations.

Rust 1.87: mechanical syntax backports plus idna_adapter 1.2.0 / ICU 1.5 pinning.
The initial Cargo resolution selected ICU 2.4 (MSRV 1.88); it was replaced in this
crate's Cargo.lock without altering any root toolchain setting. Direct registry
dependencies are exact-pinned and the entire resolved graph is in Cargo.lock.

The four appended port test includes use source-relative ../../tests paths,
so Cargo and parent Bazel rules share the same fixtures without relying on
Cargo_MANIFEST_DIR (which Bazel sets to its rule package).

Review r1 follow-up: six writer ready/closed settlement sites now use a shared
Deferred clone outside the RefCell borrow, so direct resolver/rejector calls
cannot bypass settlement tracking. scripts/writer_settlement.py records these
adaptations and asserts every targeted site matched. Record conversion now
looks up each value through captured intrinsic Reflect.get with the original JS
key before converting only the emitted name to USVString. This reuses existing
CALL operands; the C ABI header and operation IDs did not change. Tests add four
writer release regressions and three WTF-8/record regressions to the primitive
engine suite. tests/hermes_review_r1.js is a test-only fixture for parent-owned
real-engine validation, not a production JS implementation.

Owned receive-buffer adaptation (2026-11-08): original pinned NSH is not a
zero-copy baseline. `upstream/web/js.rs` constructs ArrayBuffers/Uint8Arrays
with `JS_NewArrayBufferCopy`/`JS_NewUint8ArrayCopy`; its `transfer` first snapshots
through `to_vec`, detaches, then copies into a new buffer. The archived
`upstream/standard/websocket.rs` likewise uses `ArrayBuffer::new_copy` for binary
messages. These inherited copies are distinct from the later C ABI envelope
copies; the immutable originals and their hashes remain unchanged.

`adapt_js.py` now delegates `TransferArrayBuffer` to appended ABI opcode 34;
`adapt.py` factors Fetch data enqueue so a host-prebuilt Uint8Array follows the
same controller/backpressure/BYOB algorithms, and makes WS Blob snapshotting
conditional on `binaryType`. `adapt_mod.py` exposes the narrow internal Fetch
value entry. `ffi.rs` and `transport.rs` provide the borrowed-value C entries
and WS callback delivery, while legacy byte entries/send snapshots remain.
The C++ host and Hermes primitive supply actual external ownership/move
semantics; primitive Cargo tests are not proof of that integration. Real
Rust+Hermes tests compare known backing pointers through each value entry and
ordinary streams, with GC/shutdown lifetime checks. BYOB, clone, text decoding,
Blob and intentional send paths still contain semantic copies.
