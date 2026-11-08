# Native Web APIs: opt-in NSH Rust integration

This fork ports native Web APIs by hosting NSH's Rust algorithms
in Valdi's C++ runtime and existing Hermes VM. The feature is opt-in:
`--define=valdi_native_web=true`. It provides Fetch, streaming bodies,
WebSocket, encoding, URL, events, and related classes without adding an NSH
QuickJS VM or JavaScript implementations of those APIs.

This document describes the local implementation on November 8, 2026. The
validation section distinguishes host execution, native builds and device
execution. The consuming ValdiNativeRoutes
app's `valdi/MODULE.bazel` and `module-build/MODULE.bazel` still name
`f5b41aed4780dab6899d74ca755acb411879a315`; that pin alone does not select this
implementation.

## Enable and rebuild the native host

Select a fork containing this implementation in the consuming build, include
the `native_web` Valdi module in the application's module dependencies, and
build the native runtime with:

```text
--define=valdi_native_web=true
```

The module's Bazel target inside this repository is
`//src/valdi_modules/src/valdi/native_web:native_web`
(`@valdi//src/valdi_modules/src/valdi/native_web:native_web` from a consumer
using the `valdi` repository name).

### ValdiNativeRoutes opt-in runtime export

The consuming app has a separate `valdi/native_web/BUILD.bazel` package. Its
`valdi_exported_library(name = "valdi_runtime", ...)` includes the native_web
module and exposes `//native_web:valdi_runtime_android` and
`//native_web:valdi_runtime_ios` in the app's `valdi/` Bazel workspace. The
artifacts retain the runtime names `valdi_runtime.aar` /
`libvaldi_runtime.so` and `ValdiRuntime.xcframework`.

Select these targets explicitly with the CLI's `--library`, and pass the
local fork override and feature define together through `--bazel_args`.
For a checkout layout with `ValdiNativeRoutes` and `Valdi-native-web` as
siblings, the commands below run from the app's `valdi/` directory and stage
outputs separately for parent validation:

```sh
pnpm exec valdi export android \
  --library //native_web:valdi_runtime_android \
  --build_config debug \
  --bazel_args="--override_module=valdi=../../Valdi-native-web --define=valdi_native_web=true" \
  --output_path ../.native-web-export/android/valdi_runtime.aar

pnpm exec valdi export ios \
  --library //native_web:valdi_runtime_ios \
  --build_config debug \
  --bazel_args="--override_module=valdi=../../Valdi-native-web --define=valdi_native_web=true" \
  --output_path ../.native-web-export/ios/ValdiRuntime.xcframework
```

These are export instructions, not commands executed or validated for this
document. Adjust the relative checkout path for your layout. Keep Valdi's
separately overridden internal modules and any required consumer patches
consistent with the selected source; a local source override does not apply
the archive override's patches to that checkout. A local override selects
development inputs and does not publish a revision or update the app's pin.

The app's default root export targets `//:valdi_runtime_android` and
`//:valdi_runtime_ios` remain unchanged. Its export scripts also accept an
optional third argument selecting the library, for example
`scripts/export-android.sh release /tmp/native-web-runtime/android //native_web:valdi_runtime_android`
from the application root. Supply the same override and define through
`VALDI_BAZEL_ARGS`; see the application's `docs/native-web.md`.

Rebuild and relink the native host for each target platform/architecture and
package its Java/Objective-C/native dependencies as appropriate, as well as the
TypeScript module. Replacing an OTA bundle or adding the import cannot add Rust
or C++ code to an already installed host. A host built without the define does
not register the native module factory.

In [valdi/BUILD.bazel](../valdi/BUILD.bazel), the define selects the C++ binding,
`VALDI_NATIVE_WEB=1`, and `//third-party/nsh_web:native_web`. On Android it also
selects the transport's Java source and shrinker rules. The guarded startup in
`valdi/src/valdi/android/RegisterJNI.cpp` calls
`valdi_nsh_transport_init_android` using the parent JNI environment. The
transport does not provide a second `JNI_OnLoad`. The host still owns Android
Internet permission, cleartext/network security policy, and iOS ATS policy.

The Bazel module setup uses Rust **1.87.0**, edition 2024, and
`rules_rust` **crate-universe**. `MODULE.bazel` resolves `nsh_web_crates` from
`third-party/nsh_web/Cargo.toml` and its lockfile. That dependency graph is
separate from the sibling crates' standalone Cargo lockfiles. The port includes
mechanical syntax backports and URL/IDNA dependency constraints for Rust 1.87.
Preserve the consuming Bazel lock/configuration when resolving these inputs;
do not assume a newer standalone Cargo resolution is interchangeable. The
native link uses the Rust `web_static` target and its transport dependency.

## Imports, entry point, and installation timing

Use the exact module path backed by the native factory and declaration file:

```typescript
import {
  fetch, WebSocket, TextDecoder, ReadableStream,
} from 'native_web/src/NativeWeb';
```

The C++ factory registers **exactly `native_web/src/NativeWeb`** and supplies
that module's runtime exports. `src/NativeWeb.d.ts` provides its TypeScript
declarations and emits no JavaScript implementation; the factory is its
runtime counterpart. Every runnable API example here uses this exact path,
with no additional resolver mapping required.

The optional barrel `native_web/src/index` only reexports `./NativeWeb`.
It is not a separate native factory. No package-root entry is supplied for
the bare name `native_web`. Consuming-app packaging and runtime loading of
the exact path still need validation in the rebuilt host.

The first actual load creates the per-context Rust state, transport and named
exports. It leaves existing globals unchanged. Named imports work immediately,
including their internal AbortSignal/AbortController dependencies. Later loads
reuse those exports. A type-only import does not load the module.

For global APIs, call the native installer once from the application entry,
before code that uses those globals:

```typescript
import { installNativeWeb } from 'native_web/src/NativeWeb';

installNativeWeb();
// Other code in this JS context can now use global fetch, WebSocket,
// Response, ReadableStream, AbortController, and the other Web APIs.
```

The installer defines the original native exports on the current JS global,
replacing existing writable/configurable Web definitions. A nonconfigurable
conflict throws. Repeated successful calls are no-ops; they do not rebuild
objects or restore globals changed afterward. The installer itself stays a
module export. Installation is per JS context and cannot revive a shut-down
context. No JS implementation or polyfill is evaluated.

Module loading captures the **then-current** engine intrinsics, including Object
helpers, WeakMap helpers, Promise/resolve/then, JSON.parse, error constructors,
well-known Symbols, and Function.prototype. Load the module before application
code replaces those intrinsics. Capturing at import time does not recover
pristine intrinsics that were changed earlier. Native Promise operations use
the captured Promise helpers and the existing VM microtask machinery.

`DOMException` is exported at module load and installed globally only by
`installNativeWeb()`. Its
constructor and branded readonly `message`, `name`, and `code` getters are
C++ implementations backed by immutable native data. Instances are real VM
Errors with the DOMException prototype (including for subclasses), preserving
native Error branding and stack support. The captured Error constructor is
recovered from the parent of the captured `TypeError.prototype`, so replacing
the application's global `Error` does not replace this factory. Load before
modifying that intrinsic prototype chain.

Rust resolves Web definitions through a private namespace inheriting host
services such as timers and Promise. Internal abort/timeout reasons and other
Web dependencies use the native definitions before global installation and
after application replacement of globals or module export properties. Direct
application reads still observe the application's public properties.

The C++ host works through `IJavaScriptContext` plus the optional
`IJavaScriptNativeWebSupport` primitives implemented by
`HermesJavaScriptContext`. An engine missing that interface, or a context
without a JS task scheduler, fails installation; this document does not claim
other Valdi engines support it.

## Implementation and API scope

The layers are:

1. `src/valdi_modules/src/valdi/native_web`: generated TypeScript declarations
   and an entry reexport only.
2. `valdi/src/valdi/runtime/NativeWeb/NativeWeb.cpp` and `.hpp`: native module
   registration, engine operations, native classes, value lifetime/GC tracing,
   Promise interaction, scheduler delivery, and teardown.
3. `libs/nsh_web`: NSH's Rust Web algorithms adapted to an engine-neutral
   C ABI, including class specifications and declaration metadata.
4. `libs/nsh_web_transport`: native HTTP and WebSocket I/O behind a separate
   C ABI, with owned byte/header events and bounded queues.

The public port includes DOMException, EventTarget/Event/CustomEvent,
AbortController/AbortSignal, URL/URLSearchParams, TextEncoder/TextDecoder,
Blob/File/FormData, Headers/Request/Response/fetch, WebSocket,
ReadableStream including BYOB readers, WritableStream, TransformStream,
queuing strategies, TextEncoderStream, and TextDecoderStream. See
[NativeWeb.d.ts](../src/valdi_modules/src/valdi/native_web/src/NativeWeb.d.ts)
for the declared surface.

This is a broad native port, not a claim of full WHATWG or browser
compatibility. Browser origin, CORS, cookie, credential, cache, proxy, and TLS
policy must not be inferred from the presence of similarly named Request
properties. The transport's HTTP request ABI carries method, URL, headers,
body kind, and redirect-following choice, not a complete browser policy
environment. No DOM or EventSource API is provided by this module.

There is no JS Web API polyfill payload, evaluated implementation source, or
additional JS VM in this feature. The archival upstream engine files under
`libs/nsh_web/upstream/` are provenance material and are not compiled. The
Rust port uses Valdi/Hermes values through explicit ABI tags and context-local
handle IDs; it does not reinterpret QuickJS values as Hermes values. Network
bodies and WS binary frames cross as bytes, with no JSON/base64 transport
envelope. The receive path transfers the transport's owned allocation into
Hermes external ArrayBuffer storage. Ordinary response-body reads and binary
WebSocket ArrayBuffer delivery reuse that storage; byte-stream buffer transfer
moves the backing store and detaches the old buffer rather than copying it.

This is not an end-to-end zero-copy claim. Platform/JNI adapters can still copy
network data into the Rust receive allocation. Text decoding creates JS
strings; BYOB reads fill the caller's destination, clones/tees may copy, and
outgoing mutable buffers retain their submission-time snapshots. Receive
owners contain no JS handles or Rust Web context and can release their
allocation from a GC thread after the network context has shut down.

## Fetch, streaming text, and abort

Fetch returns a Response, and `response.body` is a
`ReadableStream<Uint8Array> | null`. Use streaming reads for long-lived
responses; `text()`, `json()`, and other whole-body helpers wait for body
completion. Check `response.ok` explicitly when an HTTP error status should be
an application error.

An SSE consumer can pass decoded fragments into its existing incremental
parser. Network chunks are neither UTF-8 character boundaries nor SSE event
boundaries:

```typescript
import {
  AbortController, AbortSignal, fetch, TextDecoder,
} from 'native_web/src/NativeWeb';

async function consumeEventStream(
  url: string,
  signal: AbortSignal,
  acceptText: (fragment: string) => void,
): Promise<void> {
  const response = await fetch(url, {
    headers: { accept: 'text/event-stream' },
    signal,
  });
  if (!response.ok) {
    if (response.body) await response.body.cancel();
    throw new Error(`HTTP ${response.status}`);
  }
  if (!response.body) throw new Error('Missing response body');

  const reader = response.body.getReader();
  const decoder = new TextDecoder('utf-8');
  let reachedEnd = false;
  try {
    for (;;) {
      const chunk = await reader.read();
      if (chunk.done) {
        reachedEnd = true;
        break;
      }
      const fragment = decoder.decode(chunk.value, { stream: true });
      if (fragment) acceptText(fragment);
    }
    const tail = decoder.decode(); // Flush only after normal EOF.
    if (tail) acceptText(tail);
  } finally {
    try {
      if (!reachedEnd) await reader.cancel();
    } finally {
      reader.releaseLock();
    }
  }
}

// Application callbacks, supplied by the caller:
declare const eventsUrl: string;
declare function acceptSseText(fragment: string): void;
declare function reportFailure(error: unknown): void;
declare function registerCleanup(cleanup: () => void): void;

const abort = new AbortController();
registerCleanup(() => abort.abort());
void consumeEventStream(eventsUrl, abort.signal, acceptSseText).catch(error => {
  if (!abort.signal.aborted) reportFailure(error);
});
```

`acceptSseText` must retain partial lines/events between calls and handle SSE
framing. This example intentionally does not implement an SSE parser,
reconnection, Last-Event-ID handling, or retry policy. It also does not claim
that a server or intermediary will flush data promptly. The declarations
include `AbortSignal.abort`, `timeout`, and `any`; abort can stop a pending
fetch or its body consumption. An application-supplied abort reason need not
be an Error with a particular name.

Calling `abort()` without a reason (or with `undefined`) creates a native
DOMException named `AbortError`, with legacy code **20**. Timeout signals use
`TimeoutError`, with code **23**. `new DOMException()` defaults to an empty
message, name `Error`, and code 0. The constructor accepts arbitrary names;
unmapped names have code 0. All 25 legacy constants are available on both
the constructor and its prototype.

Streaming uploads are represented separately from response streaming. Supply
`duplex: 'half'` for a ReadableStream body:

```typescript
import { fetch, ReadableStream, TextEncoder } from 'native_web/src/NativeWeb';

declare const uploadUrl: string;
const encoder = new TextEncoder();
const body = new ReadableStream<Uint8Array>({
  start(controller) {
    controller.enqueue(encoder.encode('hello\n'));
    controller.close();
  },
});

async function upload(): Promise<string> {
  const response = await fetch(uploadUrl, {
    method: 'POST',
    body,
    duplex: 'half',
  });
  if (!response.ok) {
    if (response.body) await response.body.cancel();
    throw new Error(`HTTP ${response.status}`);
  }
  return response.text();
}
```

The bridge forwards upload demand (`BodyWanted`), chunks, upload completion,
and response pause/resume operations. These are source-level streaming and
backpressure mechanisms; platform behavior still needs host validation.

## WebSocket text, binary, and close

```typescript
import { WebSocket } from 'native_web/src/NativeWeb';

declare const socketUrl: string;
declare function acceptText(text: string): void;
declare function acceptBytes(bytes: Uint8Array): void;
declare function reportSocketError(message: string): void;
declare function reportClose(code: number, reason: string, clean: boolean): void;
declare function registerCleanup(cleanup: () => void): void;

const socket = new WebSocket(socketUrl, ['updates']);
socket.binaryType = 'arraybuffer';
socket.onopen = () => {
  socket.send('subscribe');
  socket.send(new Uint8Array([1, 2, 3]));
};
socket.onmessage = event => {
  if (typeof event.data === 'string') {
    acceptText(event.data);
  } else if (event.data instanceof ArrayBuffer) {
    acceptBytes(new Uint8Array(event.data));
  }
};
socket.onerror = event => reportSocketError(event.message);
socket.onclose = event => reportClose(event.code, event.reason, event.wasClean);
registerCleanup(() => socket.close(1000, 'done'));
```

Send only after open. `send` accepts string, Blob, ArrayBuffer, or
ArrayBufferView. Binary receive defaults to Blob; setting
`binaryType = 'arraybuffer'` selects ArrayBuffer delivery. Use
`bufferedAmount` to inform application send pacing; queued sends and bytes are
bounded, so callers must also handle send failures.

The port rejects invalid ws/wss URLs, credentials/fragments in those URLs, and
unsupported send types. Individual messages and buffered outgoing bytes are
limited to 8 MiB; the outbound message queue has eight slots. `close` accepts
1000 or integer application codes 3000–4999; reasons are limited to 123 UTF-8
bytes. `onerror` is followed by a terminal close on failed connections, and
`onclose` provides the actual code/reason/clean flag. A graceful `close()` is
distinct from native context destruction. Native handshake/write waits are
30 seconds and close waits are 5 seconds.

## Platform transports, TLS, and proxies

| Operation | Selected backend | Trust and policy |
| --- | --- | --- |
| Android Fetch | Rust adapter to `HttpURLConnection` through `com.snap.valdi.nsh.Net` | Default Android system trust/proxy settings; host network policy applies. Connect/read deadlines are 30/60 seconds. |
| iOS Fetch | Rust `objc2` adapter to default `NSURLSession` | Foundation system trust, proxies, cookies/cache, and ATS; no custom trust delegate or ATS exception is installed. |
| Desktop Fetch | NSH's `ureq` backend | rustls with bundled WebPKI roots, not macOS Keychain/Linux system CA stores; ureq environment proxy detection remains enabled. Connect/head deadlines are 30/60 seconds, with no response-body deadline. |
| WebSocket on all platforms | NSH's Tokio/tokio-tungstenite path | rustls/ring with bundled WebPKI roots, including on mobile. |

Desktop Fetch follows up to 20 redirects, or returns the 3xx response for
manual mode, and treats HTTP 4xx/5xx as responses. Backend selection is by Rust
target OS; desktop macOS follows the desktop path, not the iOS path.

Native WS does not automatically inherit platform HTTP cookies, system
proxies, client identities, or private enterprise CAs, and its handshake does
not follow redirects. The transport supports additional headers, but the
current C++ WebSocket bridge passes no additional headers and the JS
constructor takes only URL and optional protocols. Selected subprotocols are
checked. Do not infer HTTP/WS trust or proxy parity, or install a trust bypass
to hide a mismatch.

## Scheduling, cancellation, and lifetime limits

All engine/NSH Web operations belong to the owning JS thread. Transport
workers carry owned data, never JS handles. Worker wake callbacks schedule
`AlwaysAsync` JS work through weak host state; transport visitors transfer
receive-buffer ownership and copy borrowed metadata before feeding events
into the Web engine. The C++ pump applies at most 32
events per turn and retains a pending snapshot before draining another.

The transport command queue is bounded to 128 entries. HTTP and WS pending
event queues are each bounded to 128 entries and 8 MiB, with an exception
allowing one oversized platform head into an otherwise empty queue. Full
event queues apply producer backpressure. Accepted enqueue does not guarantee
network success.

Abort and body cancellation initiate native cancellation. Rejected
cancel/abort commands are retried by native ID; if the driver's queue remains
unavailable beyond the C++ two-second retry deadline, the bridge stops the
context's transport and fails its outstanding operations. This bounds retry
work; it does not impose a universal two-second socket shutdown guarantee.

Context shutdown marks the state closed, revokes transport wakes and requests
task cancellation, clears pending deliveries, finalizes native attachments
while Rust is alive, destroys transport/Rust state, and releases engine
handles before VM teardown. Already posted work checks weak/closed state.
`JavaScriptRuntime::teardownOnJsThread` calls `NativeWeb::shutdown` before
tearing down the JS queue. With the feature enabled, application-termination
partial teardown queues this cleanup behind any in-flight JS work and returns
without waiting for that work on the caller's thread.

Shutdown leaves a closed registry entry so late installation cannot recreate
Web state in a terminating context. Full context destruction calls
`NativeWeb::release` after disposing callbacks and module roots; release only
retires an already-closed entry and performs no Rust or JS work. Partial
teardown follows Valdi's existing process-exit lifetime and can retain the VM.
If that runtime is eventually destroyed, its destructor retires the closed
entry after queue quiescence. No JS may run after release.

Transport stop does **not wait for platform cancellation or I/O completion**.
Driver cleanup may finish later. WS abort releases callback ownership and
aborts Tokio work; socket cleanup follows when the task is polled. Desktop
ureq cancellation releases the callback, but cannot interrupt an already
blocked read/write; that worker stops before another chunk. With no desktop
response-body deadline, blocked body I/O can outlive JS-visible cancellation.
Thus nonblocking host teardown means it avoids waiting for native I/O, not
that every socket or worker has already terminated.

## Provenance and evidence boundary

The upstream source is **longbridge/nsh**, pinned to
`701bdcee9512e0f6aed39042d1fe24564dcd0b43`, copyright 2026 Longbridge,
**Apache-2.0**. Preserve the copied licenses/notices for both extracted
crates. Valdi's repository license does not replace the vendored source's
license.

The Web crate retains exact originals and a source-difference patch under
`libs/nsh_web/upstream/`. Its provenance documents the Rust algorithm port,
engine-neutral adaptation, excluded UI/Intl/runtime pieces, syntax backports,
and declaration generation. The transport provenance records its separate
HTTP/WS extraction and glue changes.

Primary contracts and implementation:

- [Web ABI](../libs/nsh_web/ABI.md) and
  [Web provenance](../libs/nsh_web/PROVENANCE.md).
- [Transport ABI](../libs/nsh_web_transport/ABI.md) and
  [transport provenance](../libs/nsh_web_transport/PROVENANCE.md).
- [C++ binding](../valdi/src/valdi/runtime/NativeWeb/NativeWeb.cpp) and
  [factory/lifetime interface](../valdi/src/valdi/runtime/NativeWeb/NativeWeb.hpp).
- [Module build](../src/valdi_modules/src/valdi/native_web/BUILD.bazel),
  [entry reexport](../src/valdi_modules/src/valdi/native_web/src/index.ts), and
  [Rust/Bazel targets](../third-party/nsh_web/BUILD.bazel).

Local validation on November 7, 2026 includes:

- 30 Rust Web algorithm tests and 35 transport tests on Rust 1.87.
- 33 linked Rust/C++/Hermes tests, including loopback HTTP streaming, streamed
  upload, abort, WebSocket text/binary/subprotocol/close, GC and BYOB.
- One actual RuntimeWrapper partial-teardown regression, including an
  in-flight Rust stream callback, prompt termination return, JS-thread exit,
  refused late dispatch and an independent fresh-runtime import.
- Android Release runtime export for arm32, arm64 and x86_64. Each linked
  library defines all five HTTP JNI callbacks; the AAR contains the Java
  network class, its shrinker rules and the native module's HBC99 entry.
- iOS Debug XCFramework export with a device arm64 slice and a simulator
  arm64/x86_64 slice. The dynamic runtimes define the checked Web/transport
  symbols and contain the native module's Debug JS reexport entry.
- A consuming application module resolves the complete generated declarations
  and compiles to an external native-module require and Release HBC99.

The November 8 receive-buffer update passed 51 linked Rust/C++/Hermes tests,
19 dedicated engine tests and 47 transport tests. Added cases check backing
allocation identity, detachment and exact-once release, default reads, BYOB,
clone independence, split-UTF8 SSE, cancellation and queued shutdown cleanup.
The real-runtime lifecycle regression remains passing. These tests verify
ownership and behavior; they are not mobile throughput or latency measurements.

The November 8 DOMException fix passed all 57 linked tests (including 19 engine
tests) and the real-runtime lifecycle regression. A new no-reason `abort()`
regression first reproduced the Android failure in host Hermes:
`Value is not a function`. Six added cases cover default abort/fetch rejection
identity, timeout reasons, native Error branding, readonly getter brands,
constructor/subclass conversion, all legacy constants/codes, and captured
constructor use after global replacement and GC. Declaration regeneration and
strict TypeScript usage checks also pass. The replacement test supplies the
native monotonic `performance.now` primitive normally provided by Runtime
bootstrap; a bare JSContextWrapper's missing clock otherwise triggers a
recovered event-timestamp probe through generic Error conversion.

The runtime regression follows the existing retained termination fixture.
It does not establish destruction or address reuse of the terminated VM.
The November 8 ownerless-wake correction adds a dedicated native scheduling
reason without weakening the context-owner assertion. All 57 linked cases
and two actual-runtime regressions pass after that correction.

A consuming application was also built and run under a separate QA bundle
identifier against a local Node HTTP/WebSocket server. The Android API 36
emulator completed two fresh-process runs of one final APK with all 21
assertions passing in each. On the iOS 26.5
simulator, two unchanged runs each pass 16 of 21 assertions: GET, binary
POST, streamed upload, split-UTF8 SSE and WebSocket protocol/text/binary/
close pass. The three headers-before-body checks and two dependent abort
checks fail while waiting for the Fetch response; these are retained
failures, not skipped tests.

Independent real macOS Foundation probes, including byte-identical copies
of the iOS transport adapter, reproduce delayed head delivery with no initial
body bytes and with small plain-text bodies lacking `nosniff`. EOF controls
release the callbacks, while nonempty SSE/octet-stream controls deliver
incrementally. This is evidence of a sufficient platform-stack cause, not
proof of every iOS-specific mechanism. No private API or alternate transport
has been introduced to mask the limitation.

The consuming project's acceptance scope excludes the artificial
headers-only acknowledgment gate. Ordinary body/JSON, upload, incremental
SSE and WebSocket checks passed on both platforms in both final runs
(16 checks per run). The original 21-case outcomes are retained rather
than rewritten as passes. No transport replacement is planned for that
excluded boundary.

The consuming application's revised 19-case business probe replaces the
three headers-only cases with one ordinary complete text response and
cancels an already-streaming SSE response. The rebuilt iOS App and the
Windows-hosted Android emulator App each pass 19/19 twice, including exact
native abort-reason identity and server-side
client-disconnect verification. These are new executions with a frozen
client/server pair; the native runtime bytes are unchanged.

Host tests and simulator HTTP/WS probes do not establish platform TLS policy,
physical-device behavior, worker isolation, throughput or complete
browser/WHATWG compatibility. Native export checks remain a separate layer
of evidence from executing an application.
