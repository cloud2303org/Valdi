# valdi-nsh-transport

Independent native HTTP and WebSocket extraction from pinned NSH, with no JS
engine, UI, or app dependency. The public Rust contracts are in src/lib.rs and
src/websocket.rs. C++ hosts use include/valdi_nsh_transport.h; Android startup
uses include/valdi_nsh_transport_android.h and the supplied Java class.

Read ABI.md for scheduling, ownership, stop, event mapping and TLS differences.
Read PROVENANCE.md for original hashes, license and extraction changes.

Run local tests with `cargo test --locked`, and lint with
`cargo clippy --locked --all-targets -- -D warnings`. The real loopback tests
exercise delayed HTTP chunks, POST/upload demand, redirects and errors,
cancellation ownership, WS echo/binary/close/back pressure, C ABI input copying,
wakes and nonblocking shutdown. `cargo +1.87.0 test --locked` verifies the
crate against the current Bazel Rust version. `cargo build --locked` also emits
libvaldi_nsh_transport.a; the C++ smoke source is tests/native/smoke.cpp.

Android and iOS use their system HTTP stacks. Desktop HTTP and native WS use
bundled WebPKI roots through rustls. Do not infer system trust/proxy parity for
WS from mobile HTTP behavior. No mobile runtime/device validation is claimed.
