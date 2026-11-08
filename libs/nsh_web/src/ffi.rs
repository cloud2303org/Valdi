//! C ABI entrypoints, byte transport envelopes, and GC census.
use crate::{
    NshWebContext,
    engine::{Bytes, Engine, Value, Visit},
    net::*,
    web::{
        self,
        js::{self, Ctx, R},
    },
};
use std::{
    collections::BTreeMap,
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind},
    rc::Rc,
};
fn status(cx: Ctx, f: impl FnOnce() -> R<()>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(_)) => -1,
        Err(_) => {
            js::type_error(cx, "Rust Web ABI panicked");
            -1
        }
    }
}
/// Create a context; the host supplies a complete live vtable and owns its user data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_create(engine: *const Engine) -> Ctx {
    if engine.is_null() {
        return std::ptr::null_mut();
    }
    if unsafe { (*engine).abi_version } != 1
        || unsafe { (*engine).struct_size } < std::mem::size_of::<Engine>()
    {
        return std::ptr::null_mut();
    }
    match catch_unwind(AssertUnwindSafe(|| {
        Box::into_raw(Box::new(NshWebContext {
            engine: unsafe { *engine },
            web: Default::default(),
            shared: Rc::default(),
            owned: Default::default(),
            natives: Default::default(),
            sockets: Default::default(),
        }))
    })) {
        Ok(cx) => cx,
        Err(_) => std::ptr::null_mut(),
    }
}
/// Install the retained NSH classes into the host global object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_install(cx: Ctx) -> i32 {
    if cx.is_null() {
        return -1;
    }
    status(cx, || web::install(cx))
}
/// All attached natives must have been finalized by the host before this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_destroy(cx: Ctx) {
    if cx.is_null() {
        return;
    }
    // Do not create a dangling context if the host violates teardown ordering.
    if !unsafe { (*cx).natives.borrow() }.is_empty() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        // Drain all Rust-owned JS values while the context, ownership census
        // and host vtable are fully live. In particular an open socket keeps
        // its callback even after the host finalized its native attachment.
        let web = std::mem::take(&mut (*cx).web);
        drop(web);
        let sockets = std::mem::take((*cx).sockets.get_mut());
        drop(sockets);
        drop(Box::from_raw(cx));
    }));
}
unsafe extern "C" fn subtract(user: *mut c_void, v: Value) {
    if !v.heap() {
        return;
    }
    let counts = unsafe { &mut *(user as *mut BTreeMap<Value, usize>) };
    if let Some(n) = counts.get_mut(&v) {
        *n = n.saturating_sub(1);
    }
}
/// Roots = Rust-owned references minus Trace edges of all native attachments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_trace_roots(cx: Ctx, visit: Visit, user: *mut c_void) -> i32 {
    if cx.is_null() {
        return -1;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let mut roots = unsafe { (*cx).owned.borrow().clone() };
        let entries: Vec<_> = unsafe { (*cx).natives.borrow() }
            .iter()
            .map(|(p, t)| (*p, *t))
            .collect();
        for (p, trace) in entries {
            if unsafe {
                trace(
                    p as *mut c_void,
                    subtract,
                    (&mut roots as *mut BTreeMap<Value, usize>).cast(),
                )
            } < 0
            {
                // Partial subtraction is invalid. Keep the full ownership census.
                roots = unsafe { (*cx).owned.borrow().clone() };
                break;
            }
        }
        for (v, n) in roots {
            if n > 0 {
                unsafe { visit(user, v) };
            }
        }
    })) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_set_base_url(cx: Ctx, url: Bytes) -> i32 {
    if cx.is_null() {
        return -1;
    }
    status(cx, || {
        let bytes = unsafe { url.slice() };
        let u =
            std::str::from_utf8(bytes).map_err(|_| js::type_error(cx, "base URL is not UTF-8"))?;
        let parsed = if u.is_empty() {
            None
        } else {
            Some(url::Url::parse(u).map_err(|_| js::type_error(cx, "Invalid base URL"))?)
        };
        web::web(cx).base_url = parsed;
        Ok(())
    })
}
#[repr(C)]
pub struct Header {
    pub name: Bytes,
    pub value: Bytes,
}
#[repr(C)]
pub struct HttpOp {
    pub id: u32,
    pub op: u32,
    pub method: Bytes,
    pub url: Bytes,
    pub headers: *const Header,
    pub headers_len: usize,
    pub bytes: Bytes,
    pub body_kind: u32,
    pub follow_redirects: u32,
}
pub type HttpVisit = unsafe extern "C" fn(*mut c_void, *const HttpOp);
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_drain_http(cx: Ctx, visit: HttpVisit, user: *mut c_void) {
    if cx.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        web::forget_dropped_fetches(cx);
        let shared = crate::dom::shared(cx);
        let ops = std::mem::take(&mut shared.net.borrow_mut().ops);
        for (id, op) in ops {
            let mut envelope = HttpOp {
                id,
                op: 0,
                method: Bytes::default(),
                url: Bytes::default(),
                headers: std::ptr::null(),
                headers_len: 0,
                bytes: Bytes::default(),
                body_kind: 0,
                follow_redirects: 0,
            };
            let mut headers = Vec::new();
            match &op {
                FetchOp::Start(req) => {
                    envelope.op = 1;
                    envelope.method = req.method.as_bytes().into();
                    envelope.url = req.url.as_bytes().into();
                    headers.extend(req.headers.iter().map(|(k, v)| Header {
                        name: k.as_bytes().into(),
                        value: v.as_bytes().into(),
                    }));
                    envelope.headers = headers.as_ptr();
                    envelope.headers_len = headers.len();
                    envelope.follow_redirects = req.follow_redirects as u32;
                    match &req.body {
                        HttpBody::Empty => (),
                        HttpBody::Bytes(b) => {
                            envelope.body_kind = 1;
                            envelope.bytes = b.as_slice().into();
                        }
                        HttpBody::Stream => envelope.body_kind = 2,
                    }
                }
                FetchOp::Send(b) => {
                    envelope.op = 2;
                    envelope.bytes = b.as_slice().into();
                }
                FetchOp::Finish => envelope.op = 3,
                FetchOp::Cancel => envelope.op = 4,
                FetchOp::Pause(p) => envelope.op = if *p { 5 } else { 6 },
            }
            unsafe { visit(user, &envelope) };
        }
    }));
}
#[repr(C)]
pub struct HttpEvent {
    pub id: u32,
    pub event: u32,
    pub status: u16,
    pub status_text: Bytes,
    pub url: Bytes,
    pub headers: *const Header,
    pub headers_len: usize,
    pub bytes: Bytes,
    pub redirected: u32,
}
fn text(b: Bytes) -> String {
    String::from_utf8_lossy(unsafe { b.slice() }).into_owned()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_http_event(cx: Ctx, event: *const HttpEvent) -> i32 {
    if cx.is_null() || event.is_null() {
        return -1;
    }
    status(cx, || {
        let e = unsafe { &*event };
        let event = match e.event {
            1 => {
                let headers = if e.headers_len == 0 {
                    &[][..]
                } else {
                    unsafe { std::slice::from_raw_parts(e.headers, e.headers_len) }
                };
                FetchEvent::Head(HttpHead {
                    status: e.status,
                    status_text: text(e.status_text),
                    url: text(e.url),
                    redirected: e.redirected != 0,
                    headers: headers
                        .iter()
                        .map(|h| (text(h.name), text(h.value)))
                        .collect(),
                })
            }
            2 => FetchEvent::Data(unsafe { e.bytes.slice() }.to_vec()),
            3 => FetchEvent::End,
            4 => FetchEvent::Failed(text(e.bytes)),
            5 => FetchEvent::BodyWanted,
            _ => return Err(js::type_error(cx, "Unknown HTTP event")),
        };
        web::fetch_event(cx, e.id, event)
    })
}
/// The host roots the view through this synchronous call. Only the ordinary
/// stream algorithms retain its transferred buffer; Rust never borrows bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_http_data_buffer(cx: Ctx, id: u32, view: Value) -> i32 {
    if cx.is_null() {
        return -1;
    }
    status(cx, || {
        let view = js::V::dup(cx, view);
        web::http_data_buffer(cx, id, view.raw())
    })
}
#[repr(C)]
pub struct SocketOp {
    pub id: u32,
    pub op: u32,
    pub bytes: Bytes,
    pub protocols: *const Bytes,
    pub protocols_len: usize,
    pub code: u16,
}
pub type SocketVisit = unsafe extern "C" fn(*mut c_void, *const SocketOp);
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_drain_ws(cx: Ctx, visit: SocketVisit, user: *mut c_void) {
    if cx.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let ops = std::mem::take(&mut unsafe { (*cx).sockets.borrow_mut() }.ops);
        for op in ops {
            let protocols: Vec<Bytes> = op.protocols.iter().map(|p| p.as_bytes().into()).collect();
            let e = SocketOp {
                id: op.id,
                op: op.op,
                bytes: op.bytes.as_slice().into(),
                protocols: protocols.as_ptr(),
                protocols_len: protocols.len(),
                code: op.code,
            };
            unsafe { visit(user, &e) };
        }
    }));
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_ws_event(
    cx: Ctx,
    id: u32,
    event: u32,
    bytes: Bytes,
    code: u16,
    clean: u32,
) -> i32 {
    if cx.is_null() {
        return -1;
    }
    status(cx, || {
        crate::transport::event(cx, id, event, unsafe { bytes.slice() }, code, clean != 0)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_ws_binary_buffer(cx: Ctx, id: u32, buffer: Value) -> i32 {
    if cx.is_null() {
        return -1;
    }
    status(cx, || {
        let buffer = js::V::dup(cx, buffer);
        crate::transport::binary_buffer(cx, id, buffer.raw())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_web_ws_buffered_amount(cx: Ctx, id: u32, bytes: u64) {
    if cx.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Some(s) = unsafe { (*cx).sockets.borrow_mut() }.active.get_mut(&id) {
            s.buffered = bytes;
        }
    }));
}
