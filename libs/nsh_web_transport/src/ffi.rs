//! C ABI v1. Hosts only exchange owned transport data, never engine handles.
#![allow(missing_docs)]
use std::ffi::c_void;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NshTransportBytes {
    pub data: *const u8,
    pub len: usize,
}
#[repr(C)]
pub struct NshTransportOwnedBytes {
    pub data: *mut u8,
    pub len: usize,
    pub owner: *mut c_void,
    pub release: Option<unsafe extern "C" fn(*mut c_void)>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NshTransportHeader {
    pub name: NshTransportBytes,
    pub value: NshTransportBytes,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NshTransportCallbacks {
    pub abi_version: u32,
    pub struct_size: usize,
    pub user: *mut c_void,
    pub wake: Option<unsafe extern "C" fn(*mut c_void)>,
    pub release_user: Option<unsafe extern "C" fn(*mut c_void)>,
}
#[repr(C)]
pub struct NshTransportHttpOp {
    pub id: u32,
    pub op: u32,
    pub method: NshTransportBytes,
    pub url: NshTransportBytes,
    pub headers: *const NshTransportHeader,
    pub headers_len: usize,
    pub bytes: NshTransportBytes,
    pub body_kind: u32,
    pub follow_redirects: u32,
}
#[repr(C)]
pub struct NshTransportHttpEvent {
    pub id: u32,
    pub event: u32,
    pub status: u16,
    pub status_text: NshTransportBytes,
    pub url: NshTransportBytes,
    pub headers: *const NshTransportHeader,
    pub headers_len: usize,
    pub bytes: NshTransportBytes,
    pub redirected: u32,
}
#[repr(C)]
pub struct NshTransportSocketOp {
    pub id: u32,
    pub op: u32,
    pub bytes: NshTransportBytes,
    pub protocols: *const NshTransportBytes,
    pub protocols_len: usize,
    pub code: u16,
    pub headers: *const NshTransportHeader,
    pub headers_len: usize,
}
#[repr(C)]
pub struct NshTransportSocketEvent {
    pub id: u32,
    pub event: u32,
    pub bytes: NshTransportBytes,
    pub code: u16,
    pub clean: u32,
    pub buffered_amount: u64,
}
pub type NshTransportHttpVisit =
    Option<unsafe extern "C" fn(*mut c_void, *const NshTransportHttpEvent)>;
pub type NshTransportSocketVisit =
    Option<unsafe extern "C" fn(*mut c_void, *const NshTransportSocketEvent)>;

pub type NshTransportHttpOwnedVisit =
    Option<unsafe extern "C" fn(*mut c_void, *const NshTransportHttpEvent, NshTransportOwnedBytes)>;
pub type NshTransportSocketOwnedVisit = Option<
    unsafe extern "C" fn(*mut c_void, *const NshTransportSocketEvent, NshTransportOwnedBytes),
>;

use crate::{
    FetchEvent, FetchTask, HttpBody, HttpRequest, WebSocketData, WebSocketEvent, WebSocketRequest,
    WebSocketTask,
};
use std::{
    collections::{HashMap, VecDeque},
    sync::{
        Arc, Condvar, Mutex, Weak,
        atomic::{AtomicBool, Ordering},
        mpsc::{SyncSender, sync_channel},
    },
};

const QUEUE: usize = 128;
const QUEUE_BYTES: usize = 8 * 1024 * 1024;
// SAFETY: ABI callers promise user and callbacks are safe on any worker thread.
unsafe impl Send for NshTransportCallbacks {}
struct Pending<T> {
    entries: VecDeque<(u32, T)>,
    bytes: usize,
}
impl<T> Default for Pending<T> {
    fn default() -> Self {
        Self {
            entries: VecDeque::new(),
            bytes: 0,
        }
    }
}
struct Shared {
    stopped: AtomicBool,
    wake: Mutex<Option<NshTransportCallbacks>>,
    http: Mutex<Pending<QueuedHttp>>,
    // Weak entries remain discoverable while any task, queue or drain snapshot
    // owns a generation, without retaining completed response bodies or tasks.
    http_generations: Mutex<HashMap<u32, Weak<HttpGeneration>>>,
    ws: Mutex<Pending<SocketEvent>>,
    http_space: Condvar,
    ws_space: Condvar,
}
struct HttpGeneration {
    active: AtomicBool,
    id: u32,
    owner: Weak<Shared>,
}
impl Drop for HttpGeneration {
    fn drop(&mut self) {
        if let Some(owner) = self.owner.upgrade() {
            let mut generations = owner.http_generations.lock().expect("http generations");
            // An old snapshot can outlive a replacement. Its cleanup must not
            // remove the replacement's index entry for the same public ID.
            if generations
                .get(&self.id)
                .is_some_and(|entry| std::ptr::eq(entry.as_ptr(), self))
            {
                generations.remove(&self.id);
            }
        }
    }
}
struct QueuedHttp {
    event: FetchEvent,
    generation: Arc<HttpGeneration>,
}
#[derive(Clone)]
enum SocketEvent {
    Event(WebSocketEvent),
    Buffered(usize),
}
fn http_size(event: &FetchEvent) -> usize {
    match event {
        FetchEvent::Data(b) => b.len(),
        FetchEvent::Failed(s) => s.len(),
        FetchEvent::Head(h) => {
            h.url.len()
                + h.status_text.len()
                + h.headers
                    .iter()
                    .map(|(k, v)| k.len() + v.len())
                    .sum::<usize>()
        }
        _ => 0,
    }
}
fn socket_size(event: &SocketEvent) -> usize {
    match event {
        SocketEvent::Event(WebSocketEvent::Message(d)) => d.len(),
        SocketEvent::Event(WebSocketEvent::Error(s)) => s.len(),
        SocketEvent::Event(WebSocketEvent::Close { reason, .. }) => reason.len(),
        SocketEvent::Event(WebSocketEvent::Open { protocol }) => protocol.len(),
        _ => 0,
    }
}
impl Shared {
    fn notify(&self) {
        let guard = self.wake.lock().expect("wake");
        if !self.stopped.load(Ordering::Acquire) {
            if let Some(cb) = *guard {
                if let Some(wake) = cb.wake {
                    // SAFETY: host callback/user lifetime is serialized with stop.
                    unsafe { wake(cb.user) };
                }
            }
        }
    }
    fn push<T>(
        &self,
        queue: &Mutex<Pending<T>>,
        space: &Condvar,
        id: u32,
        event: T,
        size: usize,
        active: Option<&AtomicBool>,
    ) {
        let mut queue = queue.lock().expect("events");
        let admitted = || {
            !self.stopped.load(Ordering::Acquire)
                && active.is_none_or(|active| active.load(Ordering::Acquire))
        };
        while admitted()
            && (queue.entries.len() >= QUEUE
                || (!queue.entries.is_empty() && queue.bytes.saturating_add(size) > QUEUE_BYTES))
        {
            queue = space.wait(queue).expect("events");
        }
        if !admitted() {
            return;
        }
        queue.bytes = queue.bytes.saturating_add(size);
        queue.entries.push_back((id, event));
        drop(queue);
        self.notify();
    }
    fn begin_http(self: &Arc<Self>, id: u32) -> Arc<HttpGeneration> {
        let generation = Arc::new(HttpGeneration {
            active: AtomicBool::new(true),
            id,
            owner: Arc::downgrade(self),
        });
        self.http_generations
            .lock()
            .expect("http generations")
            .insert(id, Arc::downgrade(&generation));
        generation
    }
    fn push_http(&self, id: u32, event: FetchEvent, generation: &Arc<HttpGeneration>) {
        let size = http_size(&event);
        let queued = QueuedHttp {
            event,
            generation: generation.clone(),
        };
        self.push(
            &self.http,
            &self.http_space,
            id,
            queued,
            size,
            Some(&generation.active),
        );
    }
    fn push_ws(&self, id: u32, event: SocketEvent) {
        let size = socket_size(&event);
        self.push(&self.ws, &self.ws_space, id, event, size, None);
    }
    fn stop(&self) {
        let cb = {
            let mut wake = self.wake.lock().expect("wake");
            self.stopped.store(true, Ordering::Release);
            wake.take()
        };
        *self.http.lock().expect("http") = Pending::default();
        *self.ws.lock().expect("ws") = Pending::default();
        self.http_space.notify_all();
        self.ws_space.notify_all();
        if let Some(cb) = cb {
            if let Some(release) = cb.release_user {
                // SAFETY: wake has been revoked, host gave us exactly one release.
                unsafe { release(cb.user) };
            }
        }
    }
    fn remove_http(&self, id: u32) {
        let mut q = self.http.lock().expect("http");
        // Serialize invalidation with admission/wait, retaining R1's escape
        // from a full queue. The index also finds completed generations whose
        // remaining events have already moved into a drain's local snapshot.
        let generation = self
            .http_generations
            .lock()
            .expect("http generations")
            .remove(&id)
            .and_then(|generation| generation.upgrade());
        if let Some(generation) = &generation {
            generation.active.store(false, Ordering::Release);
        }
        q.entries.retain(|(key, _)| *key != id);
        q.bytes = q.entries.iter().map(|(_, e)| http_size(&e.event)).sum();
        self.http_space.notify_all();
        self.ws_space.notify_all();
    }
    fn remove_ws(&self, id: u32) {
        let mut q = self.ws.lock().expect("ws");
        q.entries.retain(|(key, _)| *key != id);
        q.bytes = q.entries.iter().map(|(_, e)| socket_size(e)).sum();
        self.http_space.notify_all();
        self.ws_space.notify_all();
    }
}
pub struct NshTransport {
    shared: Arc<Shared>,
    commands: SyncSender<Command>,
}
enum HttpOp {
    Start(HttpRequest),
    Send(Vec<u8>),
    Finish,
    Cancel,
    Pause(bool),
}
enum SocketOp {
    Connect(WebSocketRequest),
    Send(WebSocketData),
    Close(u16, String),
    Abort,
    Pause(bool),
}
enum Command {
    Http(u32, HttpOp),
    Socket(u32, SocketOp),
    Pump,
    Stop,
}
struct HttpTask {
    task: Box<dyn FetchTask>,
    done: Arc<AtomicBool>,
}
struct SocketTask {
    task: Box<dyn WebSocketTask>,
    done: std::rc::Rc<std::cell::Cell<bool>>,
    last_buffered: usize,
}

/// # Safety
/// callbacks must point to a readable ABI struct. Its user and callback lifetime
/// must obey include/valdi_nsh_transport.h, including thread-safe, nonblocking wake.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_create(
    callbacks: *const NshTransportCallbacks,
) -> *mut NshTransport {
    // SAFETY: caller supplies a readable callback struct or NULL.
    let Some(cb) = (unsafe { callbacks.as_ref() }) else {
        return std::ptr::null_mut();
    };
    if cb.abi_version != 1 || cb.struct_size < std::mem::size_of::<NshTransportCallbacks>() {
        return std::ptr::null_mut();
    }
    let shared = Arc::new(Shared {
        stopped: AtomicBool::new(false),
        wake: Mutex::new(Some(*cb)),
        http: Mutex::new(Pending::default()),
        http_generations: Mutex::new(HashMap::new()),
        ws: Mutex::new(Pending::default()),
        http_space: Condvar::new(),
        ws_space: Condvar::new(),
    });
    let (tx, rx) = sync_channel(QUEUE);
    let state = shared.clone();
    let pump = tx.clone();
    let spawned = std::thread::Builder::new()
        .name("valdi-nsh-transport".into())
        .spawn(move || driver(state, rx, pump));
    if spawned.is_err() {
        shared.wake.lock().expect("wake").take();
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(NshTransport {
        shared,
        commands: tx,
    }))
}
fn driver(
    shared: Arc<Shared>,
    commands: std::sync::mpsc::Receiver<Command>,
    pump: SyncSender<Command>,
) {
    let network = crate::network();
    let notify = pump.clone();
    let sockets = crate::websocket::transport_with_wake(Arc::new(move || {
        let _ = notify.try_send(Command::Pump);
    }));
    let mut http: HashMap<u32, HttpTask> = HashMap::new();
    let mut ws: HashMap<u32, SocketTask> = HashMap::new();
    while let Ok(command) = commands.recv() {
        if shared.stopped.load(Ordering::Acquire) {
            break;
        }
        match command {
            Command::Stop => break,
            Command::Pump => {}
            Command::Http(id, op) => match op {
                HttpOp::Start(request) => {
                    let old = http.remove(&id);
                    // Always discard a previous response, including one whose
                    // completed task was already pruned from the live map.
                    shared.remove_http(id);
                    if let Some(old) = old {
                        old.task.cancel();
                    }
                    let admission = shared.begin_http(id);
                    let weak = Arc::downgrade(&shared);
                    let done = Arc::new(AtomicBool::new(false));
                    let terminal = done.clone();
                    let notify = pump.clone();
                    let task = network.fetch(
                        request,
                        Box::new(move |event| {
                            let finished = matches!(event, FetchEvent::End | FetchEvent::Failed(_));
                            if let Some(shared) = weak.upgrade() {
                                shared.push_http(id, event, &admission);
                            }
                            if finished {
                                terminal.store(true, Ordering::Release);
                                let _ = notify.try_send(Command::Pump);
                            }
                        }),
                    );
                    http.insert(id, HttpTask { task, done });
                }
                HttpOp::Cancel => {
                    let old = http.remove(&id);
                    // Wake rejected producers before backend cancel waits for
                    // their per-request callback mutex to become available.
                    shared.remove_http(id);
                    if let Some(old) = old {
                        old.task.cancel();
                    }
                }
                op => {
                    if let Some(entry) = http.get_mut(&id) {
                        match op {
                            HttpOp::Send(b) => entry.task.send(b),
                            HttpOp::Finish => entry.task.finish(),
                            HttpOp::Pause(p) => entry.task.pause(p),
                            _ => {}
                        }
                    }
                }
            },
            Command::Socket(id, op) => match op {
                SocketOp::Connect(request) => {
                    ws.remove(&id);
                    shared.remove_ws(id);
                    let weak = Arc::downgrade(&shared);
                    let done = std::rc::Rc::new(std::cell::Cell::new(false));
                    let terminal = done.clone();
                    match sockets.connect(
                        request,
                        Box::new(move |event| {
                            let finished = matches!(event, WebSocketEvent::Close { .. });
                            if let Some(shared) = weak.upgrade() {
                                shared.push_ws(id, SocketEvent::Event(event));
                            }
                            if finished {
                                terminal.set(true);
                            }
                        }),
                    ) {
                        Ok(task) => {
                            ws.insert(
                                id,
                                SocketTask {
                                    task,
                                    done,
                                    last_buffered: 0,
                                },
                            );
                        }
                        Err(error) => {
                            shared.push_ws(id, SocketEvent::Event(WebSocketEvent::Error(error)));
                            shared.push_ws(
                                id,
                                SocketEvent::Event(WebSocketEvent::Close {
                                    code: 1006,
                                    reason: String::new(),
                                    was_clean: false,
                                }),
                            );
                        }
                    }
                }
                SocketOp::Abort => {
                    ws.remove(&id);
                    shared.remove_ws(id);
                }
                op => {
                    if let Some(entry) = ws.get_mut(&id) {
                        let sending = matches!(&op, SocketOp::Send(_));
                        let result = match op {
                            SocketOp::Send(b) => entry.task.send(b),
                            SocketOp::Close(c, r) => entry.task.close(c, &r),
                            SocketOp::Pause(p) => {
                                entry.task.pause(p);
                                Ok(())
                            }
                            _ => Ok(()),
                        };
                        if let Err(error) = result {
                            shared.push_ws(id, SocketEvent::Event(WebSocketEvent::Error(error)));
                        }
                        // Publish an authoritative amount for each send even if
                        // a fast/empty write completed before the first sample.
                        if sending {
                            let amount = entry.task.buffered_amount();
                            shared.push_ws(id, SocketEvent::Buffered(amount));
                            entry.last_buffered = amount;
                        }
                    }
                }
            },
        }
        for (id, entry) in &mut ws {
            entry.task.dispatch();
            let buffered = if entry.done.get() {
                0
            } else {
                entry.task.buffered_amount()
            };
            if buffered != entry.last_buffered {
                shared.push_ws(*id, SocketEvent::Buffered(buffered));
                entry.last_buffered = buffered;
            }
        }
        ws.retain(|_, entry| !entry.done.get());
        http.retain(|_, entry| !entry.done.load(Ordering::Acquire));
    }
    // No user callback is retained by cleanup. Cancelling Java or Foundation
    // may enter platform code; only this driver does it, never the caller of stop.
    for (_, entry) in http {
        entry.task.cancel();
    }
    drop(ws);
}

unsafe fn copy_bytes(bytes: NshTransportBytes) -> Result<Vec<u8>, ()> {
    if bytes.len == 0 {
        return Ok(Vec::new());
    }
    if bytes.data.is_null() || bytes.len > isize::MAX as usize {
        return Err(());
    }
    // SAFETY: caller promises readable bytes for the complete input call.
    Ok(unsafe { std::slice::from_raw_parts(bytes.data, bytes.len) }.to_vec())
}
unsafe fn copy_string(bytes: NshTransportBytes) -> Result<String, ()> {
    // SAFETY: forwarded ABI input contract.
    String::from_utf8(unsafe { copy_bytes(bytes) }?).map_err(|_| ())
}
unsafe fn copy_headers(
    ptr: *const NshTransportHeader,
    len: usize,
) -> Result<Vec<(String, String)>, ()> {
    if len == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() || len > isize::MAX as usize / std::mem::size_of::<NshTransportHeader>() {
        return Err(());
    }
    // SAFETY: caller promises a readable header array for this call.
    unsafe { std::slice::from_raw_parts(ptr, len) }
        .iter()
        .map(|h| {
            // SAFETY: header fields borrow readable UTF-8 inputs for this call.
            unsafe { Ok((copy_string(h.name)?, copy_string(h.value)?)) }
        })
        .collect()
}
/// # Safety
/// handle must be live; op and its arrays/bytes must be readable until return.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_http(
    handle: *mut NshTransport,
    op: *const NshTransportHttpOp,
) -> i32 {
    // SAFETY: caller provides live inputs or NULL, without concurrent destroy.
    let (Some(h), Some(op)) = (unsafe { handle.as_ref() }, unsafe { op.as_ref() }) else {
        return -1;
    };
    if h.shared.stopped.load(Ordering::Acquire) {
        return -1;
    }
    let copied = (|| -> Result<HttpOp, ()> {
        // SAFETY: input op's arrays and bytes are live for this call.
        unsafe {
            Ok(match op.op {
                1 => HttpOp::Start(HttpRequest {
                    method: copy_string(op.method)?,
                    url: copy_string(op.url)?,
                    headers: copy_headers(op.headers, op.headers_len)?,
                    body: match op.body_kind {
                        0 => HttpBody::Empty,
                        1 => HttpBody::Bytes(copy_bytes(op.bytes)?),
                        2 => HttpBody::Stream,
                        _ => return Err(()),
                    },
                    follow_redirects: op.follow_redirects != 0,
                }),
                2 => HttpOp::Send(copy_bytes(op.bytes)?),
                3 => HttpOp::Finish,
                4 => HttpOp::Cancel,
                5 => HttpOp::Pause(true),
                6 => HttpOp::Pause(false),
                _ => return Err(()),
            })
        }
    })();
    match copied {
        Ok(command) => {
            if h.commands.try_send(Command::Http(op.id, command)).is_ok() {
                0
            } else {
                -1
            }
        }
        Err(()) => -1,
    }
}
/// # Safety
/// handle must be live; op and its arrays/bytes must be readable until return.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_ws(
    handle: *mut NshTransport,
    op: *const NshTransportSocketOp,
) -> i32 {
    // SAFETY: caller provides live inputs or NULL, without concurrent destroy.
    let (Some(h), Some(op)) = (unsafe { handle.as_ref() }, unsafe { op.as_ref() }) else {
        return -1;
    };
    if h.shared.stopped.load(Ordering::Acquire) {
        return -1;
    }
    let copied = (|| -> Result<SocketOp, ()> {
        // SAFETY: input op's arrays and bytes are live for this call.
        unsafe {
            Ok(match op.op {
                1 => {
                    let protocols = if op.protocols_len == 0 {
                        Vec::new()
                    } else {
                        if op.protocols.is_null()
                            || op.protocols_len
                                > isize::MAX as usize / std::mem::size_of::<NshTransportBytes>()
                        {
                            return Err(());
                        }
                        std::slice::from_raw_parts(op.protocols, op.protocols_len)
                            .iter()
                            .map(|b| copy_string(*b))
                            .collect::<Result<Vec<_>, _>>()?
                    };
                    SocketOp::Connect(WebSocketRequest {
                        url: copy_string(op.bytes)?,
                        protocols,
                        headers: copy_headers(op.headers, op.headers_len)?,
                    })
                }
                2 => SocketOp::Send(WebSocketData::Text(copy_string(op.bytes)?)),
                3 => SocketOp::Send(WebSocketData::Bytes(copy_bytes(op.bytes)?)),
                4 => SocketOp::Close(op.code, copy_string(op.bytes)?),
                5 => SocketOp::Abort,
                6 => SocketOp::Pause(true),
                7 => SocketOp::Pause(false),
                _ => return Err(()),
            })
        }
    })();
    match copied {
        Ok(command) => {
            if h.commands.try_send(Command::Socket(op.id, command)).is_ok() {
                0
            } else {
                -1
            }
        }
        Err(()) => -1,
    }
}
fn borrowed(bytes: &[u8]) -> NshTransportBytes {
    NshTransportBytes {
        data: bytes.as_ptr(),
        len: bytes.len(),
    }
}
fn borrowed_headers(headers: &[(String, String)]) -> Vec<NshTransportHeader> {
    headers
        .iter()
        .map(|(name, value)| NshTransportHeader {
            name: borrowed(name.as_bytes()),
            value: borrowed(value.as_bytes()),
        })
        .collect()
}
/// # Safety
/// handle must be live; visitor/user must be valid through this synchronous call.
/// No recursive drain, destroy, or concurrent destroy of the handle is allowed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_drain_http(
    handle: *mut NshTransport,
    visit: NshTransportHttpVisit,
    user: *mut c_void,
) {
    // SAFETY: caller supplies a live handle or NULL, without concurrent destroy.
    let (Some(h), Some(visit)) = (unsafe { handle.as_ref() }, visit) else {
        return;
    };
    drain_http(h, false, |event, _| {
        // SAFETY: all borrowed storage stays live through this synchronous call.
        unsafe { visit(user, event) };
    });
}
fn drain_http(
    h: &NshTransport,
    transfer: bool,
    mut visit: impl FnMut(&NshTransportHttpEvent, NshTransportOwnedBytes),
) {
    if h.shared.stopped.load(Ordering::Acquire) {
        return;
    }
    let snapshot = std::mem::take(&mut *h.shared.http.lock().expect("http"));
    h.shared.http_space.notify_all();
    for (
        id,
        QueuedHttp {
            mut event,
            generation,
        },
    ) in snapshot.entries
    {
        if h.shared.stopped.load(Ordering::Acquire) {
            break;
        }
        if !generation.active.load(Ordering::Acquire) {
            continue;
        }
        let mut e = NshTransportHttpEvent {
            id,
            event: 0,
            status: 0,
            status_text: borrowed(&[]),
            url: borrowed(&[]),
            headers: std::ptr::null(),
            headers_len: 0,
            bytes: borrowed(&[]),
            redirected: 0,
        };
        let headers;
        match &event {
            FetchEvent::Head(head) => {
                e.event = 1;
                e.status = head.status;
                e.status_text = borrowed(head.status_text.as_bytes());
                e.url = borrowed(head.url.as_bytes());
                headers = borrowed_headers(&head.headers);
                e.headers = headers.as_ptr();
                e.headers_len = headers.len();
                e.redirected = u32::from(head.redirected);
            }
            FetchEvent::Data(bytes) => {
                e.event = 2;
                e.bytes = borrowed(bytes);
            }
            FetchEvent::End => e.event = 3,
            FetchEvent::Failed(error) => {
                e.event = 4;
                e.bytes = borrowed(error.as_bytes());
            }
            FetchEvent::BodyWanted => e.event = 5,
        }
        // No lock crosses a visitor. A prior visitor may have submitted an
        // operation and waited for retirement while this snapshot was owned.
        if h.shared.stopped.load(Ordering::Acquire) {
            break;
        }
        if !generation.active.load(Ordering::Acquire) {
            continue;
        }
        // Only create the token after the last stop/generation check. Retired
        // snapshot entries therefore drop normally without orphaning a token.
        let owned = if transfer {
            match &mut event {
                FetchEvent::Data(bytes) => owned_bytes(std::mem::take(bytes)),
                _ => empty_owned(),
            }
        } else {
            empty_owned()
        };
        if owned.release.is_some() {
            e.bytes = NshTransportBytes {
                data: owned.data,
                len: owned.len,
            };
        }
        // e.bytes is now a raw description of recipient-owned storage. No Rust
        // slice/reference or owning Vec to those bytes survives this handoff.
        visit(&e, owned);
    }
}
/// # Safety
/// Same visitor and handle requirements as nsh_transport_drain_http.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_drain_ws(
    handle: *mut NshTransport,
    visit: NshTransportSocketVisit,
    user: *mut c_void,
) {
    // SAFETY: caller supplies a live handle or NULL, without concurrent destroy.
    let (Some(h), Some(visit)) = (unsafe { handle.as_ref() }, visit) else {
        return;
    };
    drain_ws(h, false, |event, _| {
        // SAFETY: all borrowed storage stays live through this synchronous call.
        unsafe { visit(user, event) };
    });
}
fn drain_ws(
    h: &NshTransport,
    transfer: bool,
    mut visit: impl FnMut(&NshTransportSocketEvent, NshTransportOwnedBytes),
) {
    if h.shared.stopped.load(Ordering::Acquire) {
        return;
    }
    let snapshot = std::mem::take(&mut *h.shared.ws.lock().expect("ws"));
    h.shared.ws_space.notify_all();
    for (id, mut event) in snapshot.entries {
        if h.shared.stopped.load(Ordering::Acquire) {
            break;
        }
        let mut e = NshTransportSocketEvent {
            id,
            event: 0,
            bytes: borrowed(&[]),
            code: 0,
            clean: 0,
            buffered_amount: 0,
        };
        match &event {
            SocketEvent::Buffered(amount) => {
                e.event = 6;
                e.buffered_amount = *amount as u64;
            }
            SocketEvent::Event(WebSocketEvent::Open { protocol }) => {
                e.event = 1;
                e.bytes = borrowed(protocol.as_bytes());
            }
            SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Text(text))) => {
                e.event = 2;
                e.bytes = borrowed(text.as_bytes());
            }
            SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Bytes(bytes))) => {
                e.event = 3;
                e.bytes = borrowed(bytes);
            }
            SocketEvent::Event(WebSocketEvent::Close {
                code,
                reason,
                was_clean,
            }) => {
                e.event = 4;
                e.code = *code;
                e.bytes = borrowed(reason.as_bytes());
                e.clean = u32::from(*was_clean);
            }
            SocketEvent::Event(WebSocketEvent::Error(error)) => {
                e.event = 5;
                e.bytes = borrowed(error.as_bytes());
            }
        }
        let owned = if transfer {
            match &mut event {
                SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Text(text))) => {
                    owned_bytes(std::mem::take(text).into_bytes())
                }
                SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Bytes(bytes))) => {
                    owned_bytes(std::mem::take(bytes))
                }
                _ => empty_owned(),
            }
        } else {
            empty_owned()
        };
        if owned.release.is_some() {
            e.bytes = NshTransportBytes {
                data: owned.data,
                len: owned.len,
            };
        }
        // Strings and Vecs have been emptied before entering recipient code.
        // Borrowed metadata remains in event through this synchronous call.
        visit(&e, owned);
    }
}
/// # Safety
/// handle must be NULL or a live handle with no concurrent destroy. A wake may
/// only post asynchronous host work; it must never call stop inline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_stop(handle: *mut NshTransport) {
    // SAFETY: caller supplies a live handle or NULL, without concurrent destroy.
    if let Some(h) = unsafe { handle.as_ref() } {
        h.shared.stop();
        let _ = h.commands.try_send(Command::Stop);
    }
}
/// # Safety
/// handle must be NULL or the sole live allocation returned by create. No
/// concurrent use, recursive visitor destroy, or subsequent use is permitted.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_destroy(handle: *mut NshTransport) {
    if handle.is_null() {
        return;
    }
    // SAFETY: caller transfers sole ownership of this handle, after all users.
    unsafe {
        nsh_transport_stop(handle);
        drop(Box::from_raw(handle));
    }
}

// The token owns only Vec metadata and its original allocation, with no shared
// transport state. Moving String::into_bytes or Vec here never copies payloads.
// A recipient must release it exactly once, on any thread after any lifetime.
fn owned_bytes(bytes: Vec<u8>) -> NshTransportOwnedBytes {
    let mut owner = Box::new(bytes);
    NshTransportOwnedBytes {
        data: owner.as_mut_ptr(),
        len: owner.len(),
        owner: Box::into_raw(owner).cast(),
        release: Some(release_bytes),
    }
}
unsafe extern "C" fn release_bytes(owner: *mut c_void) {
    // SAFETY: the recipient returns the sole token produced by owned_bytes.
    // Vec<u8> has no thread affinity and owns no context or engine resources.
    unsafe { drop(Box::from_raw(owner.cast::<Vec<u8>>())) };
}
fn empty_owned() -> NshTransportOwnedBytes {
    NshTransportOwnedBytes {
        data: std::ptr::null_mut(),
        len: 0,
        owner: std::ptr::null_mut(),
        release: None,
    }
}
/// # Safety
/// Same handle/visitor rules as nsh_transport_drain_http. The visitor assumes
/// exclusive ownership of each payload token and must release it exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_drain_http_owned(
    handle: *mut NshTransport,
    visit: NshTransportHttpOwnedVisit,
    user: *mut c_void,
) {
    // SAFETY: caller supplies a live handle or NULL, without concurrent destroy.
    let (Some(h), Some(visit)) = (unsafe { handle.as_ref() }, visit) else {
        return;
    };
    drain_http(h, true, |event, owned| {
        // SAFETY: metadata is live through this call; owned has no Rust aliases.
        unsafe { visit(user, event, owned) };
    });
}
/// # Safety
/// Same handle/visitor and release rules as nsh_transport_drain_http_owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsh_transport_drain_ws_owned(
    handle: *mut NshTransport,
    visit: NshTransportSocketOwnedVisit,
    user: *mut c_void,
) {
    // SAFETY: caller supplies a live handle or NULL, without concurrent destroy.
    let (Some(h), Some(visit)) = (unsafe { handle.as_ref() }, visit) else {
        return;
    };
    drain_ws(h, true, |event, owned| {
        // SAFETY: metadata is live through this call; owned has no Rust aliases.
        unsafe { visit(user, event, owned) };
    });
}
#[cfg(test)]
#[path = "../tests/owned/unit.rs"]
mod owned_tests;
