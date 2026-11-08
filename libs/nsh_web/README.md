# valdi-nsh-web

NSH's Rust Web API implementations through the engine-neutral C interface in
`include/valdi_nsh_web.h`. The host binds this to Valdi IJavaScriptContext and
its engine; the Rust crate neither embeds a VM nor supplies JS implementations.

Read `ABI.md` for value ownership, tracing, constructor new.target, class Specs,
intrinsic Promise callbacks and byte-only HTTP/WebSocket transport envelopes.
Read `PROVENANCE.md` for the exact upstream pin and every adaptation category.
`net.rs` aliases the independent sibling `valdi-nsh-transport` types; the parent
transport driver drains operations and schedules incoming events on the context
thread. HTTP event 5 is demand for one upload chunk, not a timer/polling trigger.

Build and verify from this directory:

```sh
cargo +1.87.0 check --locked
cargo +1.87.0 test --lib --locked
cargo +1.87.0 build --locked
python3 scripts/verify_sources.py
```

The crate produces both `rlib` and `staticlib`. The header is valid C11 and C++.
Parent-owned Bazel rules, C++ registration and JS type declarations live outside
this crate. Tests use real NSH algorithms with a test-only primitive engine:
they cover the C ABI and Rust state machines; they are not Hermes or mobile
runtime proof. Parent engine and actual-transport checks remain separate.

No Intl implementation is included. Retained NSH behavior and limits remain:
no browser CORS policy, the upstream subset of text encoding labels, and host
responsibility for capability policy, timers, TLS, transport dispatch, and
runtime teardown. Blob UI image-prop integration and NSH reactive/hot-reload
ownership are outside this independent Web API crate.
