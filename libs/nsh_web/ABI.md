# Valdi NSH Web ABI v1

The host implements `NshWebEngine` against Valdi `IJavaScriptContext`. This is a
protocol, not a promise that any engine JSValue or raw pointer shares its layout.
The header is the integration contract. All calls run on the owning JS thread;
no Rust unwind or C++ exception may cross C. Host failures set a pending JS
exception and return tag NSH_EXCEPTION (or -1). Rust returns -1 similarly.

Values use explicit tags and bits: numbers hold f64 bits; strings, objects,
symbols and bigints hold context-local IDs. flags is zero. Input values are
borrowed. Each returned heap value owns one host handle reference. retain/release
manage ID lifetime, NOT unconditional GC roots. Native callbacks return owned
values; the host consumes that reference. Rust keeps an ownership ledger and
subtracts each registered native object's Trace edges when enumerating roots.
The collector marks `nsh_web_trace_roots` and each *reachable* native object's
trace edges. Unreachable native cycles can then finalize and release their IDs.
A trace failure (-1) requires conservative retention for this collection.
Retain/release and trace must not execute JS or collect; callbacks that execute
JS must include the active call receiver/arguments/results in their own roots.
The host must not invalidate an ID until both its references and GC object die.

Native object metadata includes finalize/trace and an optional callable entry.
For callable natives, invoke call(ctx, functionObject, argc, argv), not with the
JS receiver. For class constructors invoke constructor(ctx, newTarget, argc, argv).
For methods/getters/setters invoke native(ctx, receiver, argc, argv). The host
must enforce constructor-only invocation, correct descriptor flags, inheritance,
Symbol.iterator/asyncIterator, and configurable Symbol.toStringTag. Specs are
borrowed only during register_class; copy any metadata that will be used later.
Do not expose internal classes globally. Registration returns prototype and
constructor; Rust defines the global itself. A NULL constructor means illegal.

`operate` has fixed operands: receiver, borrowed args, borrowed text bytes, aux.
Unused operands are undefined/empty/0. It returns owned values, including for
property operations (undefined on success). Pending exceptions remain host-side.

| Op | receiver | args / text / aux | result |
|---|---|---|---|
| GLOBAL | - | - | global |
| STRING_NEW | - | text UTF-8 | string |
| TO_WTF8 | value | use bytes callback mode 1 instead | reserved |
| TO_NUMBER / TO_BOOL | value | - | number / boolean |
| IS_FUNCTION / IS_UNCATCHABLE | value | - | boolean |
| GET | object | text property UTF-8 | value |
| GET_SYMBOL | object | aux symbol ID | value |
| DEFINE | object | args[0] value, text key, aux flags | undefined |
| CALL | function | args[0] this, remaining arguments | return |
| CONSTRUCT | constructor | arguments | instance |
| OBJECT_NEW | - | - | object |
| ARRAY_NEW | - | elements | array |
| KEYS | object | - | array of own enumerable string keys |
| FUNCTION_NEW | - | text name, aux length, see below | function |
| PROMISE_NEW | - | - | array [promise, resolve, reject] |
| PROMISE_THEN | promise | [ok, err] | captured intrinsic Promise.resolve then captured intrinsic then |
| PROMISE_HANDLED | promise | - | undefined |
| PROMISE_STATE / IS_PROMISE | - | reserved, never called | - |
| THROW | value | - | exception sentinel |
| TAKE_EXCEPTION | - | - | pending exception, cleared |
| ERROR | - | text message, aux 0 TypeError / 1 RangeError / 2 SyntaxError | exception sentinel |
| REPORT | exception | - | report uncaught asynchronously, undefined |
| JSON_PARSE | - | text UTF-8 | parsed value |
| ARRAY_BUFFER_NEW / UINT8_ARRAY_NEW | - | text raw bytes | copied buffer / view |
| VIEW_NEW | buffer | [offset, elementCount], aux typed kind (-1 DataView) | view |
| DETACH / IS_DETACHED | buffer | - | undefined / boolean |
| ARRAY_BUFFER_TRANSFER (34) | live ArrayBuffer | - | new intrinsic ArrayBuffer with moved backing; old buffer and views detached |
| SET_PROTOTYPE / GET_PROTOTYPE | object | [prototype] / [] | undefined / prototype |

DEFINE flags: 1 writable, 2 enumerable, 4 configurable. Symbol keys are not
required by DEFINE (class metadata handles them). FUNCTION_NEW is reserved:
Rust creates plain callable closures through native_new instead, so hosts may
reject it. Intrinsic Promise operations must ignore overridden user `.then`.
Use the actual engine Promise implementation and microtask queue. `bytes` mode 1
performs ToString (throwing on symbols) and returns WTF-8, preserving lone
surrogates; Rust converts to USVString where the Web algorithm requires it.
Mode 0 returns a mutable BufferSource span respecting view offset/length; null
with zero length is valid. `view` must use engine buffer/view metadata, including
DataView, detached state, element size, and ABI typed kind IDs.

HTTP and WS operations are queued, drained in order, copied by the visitor, then
removed. Events must arrive on the JS thread; they may synchronously execute JS.
No JSON/base64 transport envelope exists. HTTP response bodies are byte chunks;
streaming uploads use separate chunk/end operations and Pause/Resume implement
backpressure. NSH retains its data: URL decoder (including data-URL base64), which
is unrelated to transport encoding. Network capability policy, actual sockets,
TLS, redirects, and callback scheduling are host responsibilities. Reject or
cancel outstanding transport work before destroying the context.

Receive values can instead enter through `nsh_web_http_data_buffer(ctx,id,view)`
and `nsh_web_ws_binary_buffer(ctx,id,buffer)`. These inputs are borrowed handles:
the host roots the Uint8Array/ArrayBuffer through the synchronous call; Rust
duplicates a handle only for the call and whatever the normal algorithms retain.
Neither entry obtains a Rust slice of its payload. HTTP uses the ordinary byte
controller enqueue, transfer, BYOB, queue/backpressure and missing-ID/abort
algorithms. WS invokes its normal binary message callback and `binaryType`
conversion. The legacy byte entries and all existing opcode numbers remain.

The C++ host's owned transport drains retain an independent `RefCountable`
descriptor through the bounded event backlog. Nonempty HTTP/WS binary data
becomes external engine backing via `newArrayBuffer(BytesView)`; transfer reuses
that owner. Empty payloads use distinct native-Web buffers and release their
descriptors independently. Descriptor destruction calls only the independent
allocation release callback, possibly on a GC worker after transport/context
teardown; it never enters Rust Web or JS.

This removes avoidable receive payload copies on the nonempty default-reader
HTTP and WS `binaryType="arraybuffer"` paths. It does not promise all-path zero
copy: metadata and text decoding, text decoder pending UTF-8 bytes, BYOB
destination filling, stream tee/Response clone, Blob materialization and body
aggregation, and intentional send snapshots still copy as required. Socket
`binaryType="blob"` snapshots only on that branch; ArrayBuffer delivery creates
no discarded Rust Vec.

Destroy ordering: stop event delivery; finalize all engine native attachments
while ctx is valid (each exactly once); release host-owned result references;
then destroy ctx. Finalizers may enqueue cancellation, so drain cancellations
before stopping transport or cancel all host requests on teardown. A context is
single-threaded and must outlive every value/native attachment created through it.

HTTP event code 5 is upstream `BodyWanted`: send one streaming upload chunk or
finish the upload. This completes ABI v1 without changing any struct layout.

Writer ready/closed Deferreds track resolution/rejection in Rust; hosts need no
native Promise brand or state API. PROMISE_THEN normalizes its input with the
captured intrinsic Promise.resolve before applying captured intrinsic then.

IDs must be stable and canonical for object identity. Equal string values should
share an ID if the host compares them through raw SameValue; NSH's current raw
SameValue sites use object identity. Functions have OBJECT tag. native_get,
retain, release and trace must not execute JS. Native methods/constructors are
invoked through register_class metadata; callable native attachments are used
for Rust closures/global functions and receive their function object in the
second argument so captures can be located.

The WebSocket adapter validates ws/wss URLs without credentials/fragment, rejects
unsupported send types, and bounds one message and buffered bytes at 8 MiB and
queued outgoing sends at eight. Hosts must preserve these limits after drain,
validate the negotiated subprotocol and deliver error+close for failed handshakes,
and deliver terminal close even after error so active callback roots are released.
A host reports actual outstanding send bytes with nsh_web_ws_buffered_amount;
values are total bytes, not deltas. Transport events must not re-enter native
methods before their queueing call returns.

Record conversion captures intrinsic Reflect.get during installation and passes
original JS string keys through CALL before USVString conversion of the emitted
name. This preserves lone-surrogate property identity without changing GET's
UTF-8 contract or the header. Replacing global Reflect later does not affect it.
Every writer ready/closed settlement clones its Deferred (including shared Rust
settlement state) outside the native data borrow, then calls resolve/reject.
