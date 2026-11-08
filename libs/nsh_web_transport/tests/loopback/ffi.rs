use std::{
    ffi::c_void,
    io::{Read, Write},
    net::TcpListener,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};
use valdi_nsh_transport::ffi::*;
#[path = "../owned/loopback.rs"]
mod owned;
fn bytes(b: &[u8]) -> NshTransportBytes {
    NshTransportBytes {
        data: b.as_ptr(),
        len: b.len(),
    }
}
fn empty() -> NshTransportBytes {
    bytes(&[])
}
struct Wake {
    tx: mpsc::Sender<()>,
    count: AtomicUsize,
    releases: AtomicUsize,
}
unsafe extern "C" fn wake(user: *mut c_void) {
    // SAFETY: test retains Arc<Wake> through stop and destroy.
    let user = unsafe { &*(user as *const Wake) };
    user.count.fetch_add(1, Ordering::SeqCst);
    let _ = user.tx.send(());
}
unsafe extern "C" fn release(user: *mut c_void) {
    // SAFETY: create gives the C transport one strong Arc reference; release
    // consumes it once, independently of the test's own observation reference.
    let owner = unsafe { Arc::from_raw(user as *const Wake) };
    owner.releases.fetch_add(1, Ordering::SeqCst);
}

struct Handle(*mut NshTransport);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { nsh_transport_destroy(self.0) }
    }
}
fn create() -> (Handle, Arc<Wake>, mpsc::Receiver<()>) {
    let (tx, rx) = mpsc::channel();
    let owner = Arc::new(Wake {
        tx,
        count: AtomicUsize::new(0),
        releases: AtomicUsize::new(0),
    });
    let cb = NshTransportCallbacks {
        abi_version: 1,
        struct_size: std::mem::size_of::<NshTransportCallbacks>(),
        user: Arc::into_raw(owner.clone()) as *mut c_void,
        wake: Some(wake),
        release_user: Some(release),
    };
    let transport = unsafe { nsh_transport_create(&cb) };
    assert!(!transport.is_null(), "C ABI driver not implemented");
    (Handle(transport), owner, rx)
}
#[derive(Debug)]
struct HttpEvent {
    id: u32,
    kind: u32,
    status: u16,
    data: Vec<u8>,
}
unsafe extern "C" fn http_visit(user: *mut c_void, event: *const NshTransportHttpEvent) {
    // SAFETY: synchronous drain visitor borrows a live Vec and event data.
    unsafe {
        let events = &mut *(user as *mut Vec<HttpEvent>);
        let e = &*event;
        let data = if e.bytes.len == 0 {
            vec![]
        } else {
            std::slice::from_raw_parts(e.bytes.data, e.bytes.len).to_vec()
        };
        events.push(HttpEvent {
            id: e.id,
            kind: e.event,
            status: e.status,
            data,
        });
    }
}
fn drain_http(h: &Handle, events: &mut Vec<HttpEvent>) {
    unsafe { nsh_transport_drain_http(h.0, Some(http_visit), events as *mut _ as *mut c_void) }
}
fn http_op(id: u32, op: u32) -> NshTransportHttpOp {
    NshTransportHttpOp {
        id,
        op,
        method: empty(),
        url: empty(),
        headers: std::ptr::null(),
        headers_len: 0,
        bytes: empty(),
        body_kind: 0,
        follow_redirects: 1,
    }
}
#[test]
fn ffi_http_copies_post_bytes_wakes_and_drains_on_caller_thread() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut request = Vec::new();
        let mut byte = [0];
        while !request.ends_with(b"\r\n\r\n") {
            stream.read_exact(&mut byte).unwrap();
            request.push(byte[0]);
        }
        assert!(request.starts_with(b"POST "));
        let mut body = [0; 4];
        stream.read_exact(&mut body).unwrap();
        assert_eq!(&body, b"post");
        stream
            .write_all(
                b"HTTP/1.1 201 Created\r\nContent-Length: 4\r\nConnection: close\r\n\r\npost",
            )
            .unwrap();
    });
    let (h, owner, rx) = create();
    let mut op = http_op(7, 1);
    op.method = bytes(b"POST");
    op.url = bytes(url.as_bytes());
    op.body_kind = 1;
    let mut body = b"post".to_vec();
    op.bytes = bytes(&body);
    assert_eq!(unsafe { nsh_transport_http(h.0, &op) }, 0);
    body.fill(0);
    drop(body);
    drop(url);
    let mut events = Vec::new();
    let until = Instant::now() + Duration::from_secs(5);
    while !events.iter().any(|e: &HttpEvent| e.kind == 3) {
        drain_http(&h, &mut events);
        assert!(Instant::now() < until, "{events:?}");
        let _ = rx.recv_timeout(Duration::from_millis(20));
    }
    assert!(events.iter().all(|e| e.id == 7));
    assert_eq!(events[0].status, 201);
    assert_eq!(
        events
            .iter()
            .flat_map(|e| e.data.clone())
            .collect::<Vec<_>>(),
        b"post"
    );
    unsafe { nsh_transport_stop(h.0) };
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
    assert_eq!(unsafe { nsh_transport_http(h.0, &http_op(7, 4)) }, -1);
    drop(h);
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
}
#[test]
fn ffi_stop_is_nonblocking_during_handshake_and_revokes_wakes() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let (h, owner, _) = create();
    let op = NshTransportSocketOp {
        id: 1,
        op: 1,
        bytes: bytes(url.as_bytes()),
        protocols: std::ptr::null(),
        protocols_len: 0,
        code: 0,
        headers: std::ptr::null(),
        headers_len: 0,
    };
    assert_eq!(unsafe { nsh_transport_ws(h.0, &op) }, 0);
    let (mut peer, _) = listener.accept().unwrap();
    peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let start = Instant::now();
    unsafe { nsh_transport_stop(h.0) };
    assert!(start.elapsed() < Duration::from_millis(100));
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
    let count = owner.count.load(Ordering::SeqCst);
    let mut buf = [0; 4096];
    loop {
        match peer.read(&mut buf) {
            Ok(0) => break,
            Ok(_) => {}
            Err(e) => {
                assert_ne!(e.kind(), std::io::ErrorKind::WouldBlock);
                break;
            }
        }
    }
    assert_eq!(owner.count.load(Ordering::SeqCst), count);
}

#[derive(Debug)]
struct SocketEvent {
    kind: u32,
    data: Vec<u8>,
    code: u16,
    clean: u32,
    buffered: u64,
}
unsafe extern "C" fn socket_visit(user: *mut c_void, event: *const NshTransportSocketEvent) {
    // SAFETY: synchronous drain visitor borrows a live Vec and event data.
    unsafe {
        let events = &mut *(user as *mut Vec<SocketEvent>);
        let e = &*event;
        let data = if e.bytes.len == 0 {
            vec![]
        } else {
            std::slice::from_raw_parts(e.bytes.data, e.bytes.len).to_vec()
        };
        events.push(SocketEvent {
            kind: e.event,
            data,
            code: e.code,
            clean: e.clean,
            buffered: e.buffered_amount,
        });
    }
}
fn socket_op(id: u32, op: u32) -> NshTransportSocketOp {
    NshTransportSocketOp {
        id,
        op,
        bytes: empty(),
        protocols: std::ptr::null(),
        protocols_len: 0,
        code: 0,
        headers: std::ptr::null(),
        headers_len: 0,
    }
}
fn socket_wait(
    h: &Handle,
    events: &mut Vec<SocketEvent>,
    rx: &mpsc::Receiver<()>,
    condition: impl Fn(&[SocketEvent]) -> bool,
) {
    let until = Instant::now() + Duration::from_secs(5);
    while !condition(events) {
        unsafe { nsh_transport_drain_ws(h.0, Some(socket_visit), events as *mut _ as *mut c_void) };
        assert!(Instant::now() < until, "{events:?}");
        let _ = rx.recv_timeout(Duration::from_millis(20));
    }
}
#[test]
fn ffi_websocket_echo_binary_and_clean_close() {
    use tokio_tungstenite::tungstenite::{Message, accept};
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        let (peer, _) = listener.accept().unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut socket = accept(peer).unwrap();
        loop {
            match socket.read() {
                Ok(Message::Close(_)) => {
                    let _ = socket.flush();
                    break;
                }
                Ok(message) => {
                    if socket.send(message).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    });
    let (h, _owner, rx) = create();
    let mut op = socket_op(19, 1);
    op.bytes = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_ws(h.0, &op) }, 0);
    let mut events = Vec::new();
    socket_wait(&h, &mut events, &rx, |e| e.iter().any(|e| e.kind == 1));
    let mut text = socket_op(19, 2);
    text.bytes = bytes(b"hello ffi");
    assert_eq!(unsafe { nsh_transport_ws(h.0, &text) }, 0);
    let mut binary = socket_op(19, 3);
    binary.bytes = bytes(&[0, 1, 255]);
    assert_eq!(unsafe { nsh_transport_ws(h.0, &binary) }, 0);
    socket_wait(&h, &mut events, &rx, |e| e.iter().any(|e| e.kind == 3));
    assert!(events.iter().any(|e| e.kind == 2 && e.data == b"hello ffi"));
    assert!(events.iter().any(|e| e.kind == 3 && e.data == [0, 1, 255]));
    let mut close = socket_op(19, 4);
    close.code = 1000;
    close.bytes = bytes(b"C close");
    assert_eq!(unsafe { nsh_transport_ws(h.0, &close) }, 0);
    socket_wait(&h, &mut events, &rx, |e| e.iter().any(|e| e.kind == 4));
    assert!(
        events
            .iter()
            .any(|e| e.kind == 4 && e.code == 1000 && e.clean == 1 && e.data == b"C close")
    );
}
#[test]
fn ffi_upload_demand_is_exposed_before_response() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        use std::io::{BufRead, BufReader};
        let (peer, _) = listener.accept().unwrap();
        let mut peer = BufReader::new(peer);
        loop {
            let mut line = String::new();
            peer.read_line(&mut line).unwrap();
            if line == "\r\n" {
                break;
            }
        }
        let mut body = Vec::new();
        loop {
            let mut line = String::new();
            peer.read_line(&mut line).unwrap();
            let len = usize::from_str_radix(line.trim(), 16).unwrap();
            let mut chunk = vec![0; len + 2];
            peer.read_exact(&mut chunk).unwrap();
            if len == 0 {
                break;
            }
            body.extend_from_slice(&chunk[..len]);
        }
        assert_eq!(body, b"streamed");
        peer.get_mut()
            .write_all(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
            .unwrap();
    });
    let (h, _owner, rx) = create();
    let mut start = http_op(3, 1);
    start.method = bytes(b"POST");
    start.url = bytes(url.as_bytes());
    start.body_kind = 2;
    assert_eq!(unsafe { nsh_transport_http(h.0, &start) }, 0);
    let mut events = Vec::new();
    let mut next = 0;
    let mut wanted = 0;
    let until = Instant::now() + Duration::from_secs(5);
    while !events.iter().any(|e: &HttpEvent| e.kind == 3) {
        drain_http(&h, &mut events);
        for event in &events[next..] {
            if event.kind == 5 {
                let mut op = http_op(3, if wanted == 0 { 2 } else { 3 });
                op.bytes = bytes(b"streamed");
                assert_eq!(unsafe { nsh_transport_http(h.0, &op) }, 0);
                wanted += 1;
            }
        }
        next = events.len();
        assert!(Instant::now() < until, "{events:?}");
        let _ = rx.recv_timeout(Duration::from_millis(10));
    }
    assert_eq!(wanted, 2);
    assert!(events.iter().any(|e| e.kind == 1 && e.status == 204));
}
#[test]
fn ffi_buffered_amount_returns_to_zero_without_an_incoming_message() {
    use tokio_tungstenite::tungstenite::accept;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let (read, read_rx) = mpsc::channel();
    let (release, release_rx) = mpsc::channel();
    std::thread::spawn(move || {
        let (peer, _) = listener.accept().unwrap();
        let mut socket = accept(peer).unwrap();
        let result = socket.read();
        let _ = read.send(result.is_ok());
        let _ = release_rx.recv_timeout(Duration::from_secs(5));
    });
    let (h, _owner, rx) = create();
    let mut connect = socket_op(4, 1);
    connect.bytes = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_ws(h.0, &connect) }, 0);
    let mut events = Vec::new();
    socket_wait(&h, &mut events, &rx, |e| e.iter().any(|e| e.kind == 1));
    // A large write cannot complete in the same driver instruction as enqueue.
    let payload = vec![7; 1024 * 1024];
    let mut send = socket_op(4, 3);
    send.bytes = bytes(&payload);
    assert_eq!(unsafe { nsh_transport_ws(h.0, &send) }, 0);
    assert!(read_rx.recv_timeout(Duration::from_secs(5)).unwrap());
    socket_wait(&h, &mut events, &rx, |e| {
        e.iter().any(|e| e.kind == 6 && e.buffered == 0)
    });
    release.send(()).unwrap();
}
#[test]
fn ffi_invalid_inputs_and_idempotent_stop() {
    let (h, owner, _) = create();
    let mut invalid = http_op(1, 1);
    invalid.method = bytes(b"GET");
    invalid.body_kind = 99;
    assert_eq!(unsafe { nsh_transport_http(h.0, &invalid) }, -1);
    assert_eq!(unsafe { nsh_transport_http(h.0, std::ptr::null()) }, -1);
    assert_eq!(unsafe { nsh_transport_ws(h.0, &socket_op(1, 99)) }, -1);
    unsafe {
        nsh_transport_stop(h.0);
        nsh_transport_stop(h.0);
    }
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
}

#[test]
fn ffi_stop_releases_a_full_http_event_queue_without_draining() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        let (mut peer, _) = listener.accept().unwrap();
        peer.set_write_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut byte = [0];
        let mut head = Vec::new();
        while !head.ends_with(b"\r\n\r\n") {
            if peer.read_exact(&mut byte).is_err() {
                return;
            }
            head.push(byte[0]);
        }
        if peer
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 33554432\r\nConnection: close\r\n\r\n")
            .is_err()
        {
            return;
        }
        let chunk = vec![1; 65536];
        for _ in 0..512 {
            if peer.write_all(&chunk).is_err() {
                break;
            }
        }
    });
    let (h, owner, rx) = create();
    let mut start = http_op(55, 1);
    start.method = bytes(b"GET");
    start.url = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_http(h.0, &start) }, 0);
    // Head plus 127 chunks fill the bridge queue. No visitor has drained it.
    for _ in 0..128 {
        rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let start = Instant::now();
    unsafe { nsh_transport_stop(h.0) };
    assert!(start.elapsed() < Duration::from_millis(100));
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
    let mut events = Vec::new();
    drain_http(&h, &mut events);
    assert!(events.is_empty());
}

#[test]
fn ffi_send_completion_reports_zero_even_when_byte_count_never_changes() {
    use tokio_tungstenite::tungstenite::accept;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let (read, read_rx) = mpsc::channel();
    let (release, release_rx) = mpsc::channel();
    std::thread::spawn(move || {
        let (peer, _) = listener.accept().unwrap();
        let mut socket = accept(peer).unwrap();
        let result = socket.read();
        let _ = read.send(result.is_ok());
        let _ = release_rx.recv_timeout(Duration::from_secs(5));
    });
    let (h, _owner, rx) = create();
    let mut connect = socket_op(4, 1);
    connect.bytes = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_ws(h.0, &connect) }, 0);
    let mut events = Vec::new();
    socket_wait(&h, &mut events, &rx, |e| e.iter().any(|e| e.kind == 1));
    // An empty binary message is valid; byte count stays zero throughout.
    let payload = Vec::<u8>::new();
    let mut send = socket_op(4, 3);
    send.bytes = bytes(&payload);
    assert_eq!(unsafe { nsh_transport_ws(h.0, &send) }, 0);
    assert!(read_rx.recv_timeout(Duration::from_secs(5)).unwrap());
    socket_wait(&h, &mut events, &rx, |e| {
        e.iter().any(|e| e.kind == 6 && e.buffered == 0)
    });
    release.send(()).unwrap();
}

fn saturated_http_context() -> (Handle, Arc<Wake>, mpsc::Receiver<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        let (mut peer, _) = listener.accept().unwrap();
        peer.set_write_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut byte = [0];
        let mut head = Vec::new();
        while !head.ends_with(b"\r\n\r\n") {
            if peer.read_exact(&mut byte).is_err() {
                return;
            }
            head.push(byte[0]);
        }
        if peer
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 33554432\r\nConnection: close\r\n\r\n")
            .is_err()
        {
            return;
        }
        let chunk = vec![1; 65536];
        for _ in 0..512 {
            if peer.write_all(&chunk).is_err() {
                break;
            }
        }
    });
    let (h, owner, rx) = create();
    let mut start = http_op(700, 1);
    start.method = bytes(b"GET");
    start.url = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_http(h.0, &start) }, 0);
    for _ in 0..128 {
        rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    assert!(
        matches!(
            rx.recv_timeout(Duration::from_millis(100)),
            Err(mpsc::RecvTimeoutError::Timeout)
        ),
        "event queue did not settle at capacity"
    );
    (h, owner, rx)
}
fn http_server(body: &'static [u8]) -> (String, mpsc::Receiver<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let (mut peer, _) = listener.accept().unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut head = Vec::new();
        let mut byte = [0];
        while !head.ends_with(b"\r\n\r\n") {
            if peer.read_exact(&mut byte).is_err() {
                return;
            }
            head.push(byte[0]);
        }
        let _ = write!(
            peer,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            body.len()
        );
        let _ = peer.write_all(body);
        let _ = tx.send(());
    });
    (url, rx)
}
fn start_get(h: &Handle, id: u32, url: &str) {
    let mut start = http_op(id, 1);
    start.method = bytes(b"GET");
    start.url = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_http(h.0, &start) }, 0);
}
fn waiting_request_cancel_or_replace(replace: bool) {
    let (h, _owner, wakes) = saturated_http_context();
    let (old, old_sent) = http_server(b"old blocked response");
    start_get(&h, 701, &old);
    old_sent.recv_timeout(Duration::from_secs(5)).unwrap();
    // All queued events belong to 700. The 701 callback cannot be admitted;
    // removing only 701's queued events therefore cannot create any space.
    assert!(matches!(
        wakes.recv_timeout(Duration::from_millis(100)),
        Err(mpsc::RecvTimeoutError::Timeout)
    ));
    let (next, next_sent) = http_server(b"successor");
    if replace {
        start_get(&h, 701, &next);
    } else {
        assert_eq!(unsafe { nsh_transport_http(h.0, &http_op(701, 4)) }, 0);
        start_get(&h, 702, &next);
    }
    let progressed = next_sent.recv_timeout(Duration::from_secs(2));
    // Always stop before asserting, so a RED case releases blocked producers.
    unsafe { nsh_transport_stop(h.0) };
    assert!(
        progressed.is_ok(),
        "accepted cancel/replacement stalled the driver behind a full unrelated queue: {progressed:?}"
    );
}
#[test]
fn ffi_cancel_waiting_request_escapes_another_requests_full_queue() {
    waiting_request_cancel_or_replace(false);
}
#[test]
fn ffi_replace_waiting_request_escapes_another_requests_full_queue() {
    waiting_request_cancel_or_replace(true);
}
#[test]
fn ffi_start_reusing_completed_id_discards_old_queued_response() {
    let (h, _owner, wakes) = create();
    let (old, old_sent) = http_server(b"stale");
    start_get(&h, 703, &old);
    old_sent.recv_timeout(Duration::from_secs(5)).unwrap();
    // Observe Head/Data/End without draining. A driver command after completion
    // provides a turn for pruning the old task independently of its event queue.
    for _ in 0..3 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let marker = socket_op(799, 1);
    assert_eq!(unsafe { nsh_transport_ws(h.0, &marker) }, 0);
    for _ in 0..2 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let (new, new_sent) = http_server(b"fresh");
    start_get(&h, 703, &new);
    new_sent.recv_timeout(Duration::from_secs(5)).unwrap();
    for _ in 0..3 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let mut events = Vec::new();
    drain_http(&h, &mut events);
    let data = events
        .iter()
        .filter(|e| e.id == 703 && e.kind == 2)
        .flat_map(|e| e.data.clone())
        .collect::<Vec<_>>();
    assert_eq!(
        data, b"fresh",
        "completed ID reuse delivered stale response events: {events:?}"
    );
    assert_eq!(
        events.iter().filter(|e| e.id == 703 && e.kind == 3).count(),
        1
    );
}

struct ReentrantDrain {
    handle: *mut NshTransport,
    events: Vec<HttpEvent>,
    replacement: Option<String>,
    replacement_started: Option<mpsc::Receiver<()>>,
    wakes: mpsc::Receiver<()>,
    operation_processed: bool,
    failure: Option<String>,
}
unsafe extern "C" fn reentrant_http_visit(user: *mut c_void, event: *const NshTransportHttpEvent) {
    // SAFETY: the synchronous caller lends this state and event for the visit.
    let state = unsafe { &mut *(user as *mut ReentrantDrain) };
    unsafe { http_visit(&mut state.events as *mut _ as *mut c_void, event) };
    if unsafe { (*event).event } != 1 || state.operation_processed || state.failure.is_some() {
        return;
    }
    let mut op = http_op(810, if state.replacement.is_some() { 1 } else { 4 });
    if let Some(url) = &state.replacement {
        op.method = bytes(b"GET");
        op.url = bytes(url.as_bytes());
    }
    if unsafe { nsh_transport_http(state.handle, &op) } != 0 {
        state.failure = Some("reentrant operation was not accepted".into());
        return;
    }
    if let Some(started) = &state.replacement_started {
        // The replacement server cannot receive a request until Start has been
        // processed. Keep the old Head visitor running through that point.
        if started.recv_timeout(Duration::from_secs(5)).is_err() {
            state.failure = Some("replacement did not start while Head visitor was running".into());
            return;
        }
    } else {
        // Commands are FIFO. Synchronous invalid-connect events acknowledge a
        // driver turn after Cancel without recursively draining the transport.
        if unsafe { nsh_transport_ws(state.handle, &socket_op(811, 1)) } != 0 {
            state.failure = Some("cancel barrier was not accepted".into());
            return;
        }
        for _ in 0..2 {
            if state.wakes.recv_timeout(Duration::from_secs(5)).is_err() {
                state.failure = Some("cancel did not finish before Head visitor returned".into());
                return;
            }
        }
    }
    state.operation_processed = true;
}
fn reentrant_completed_response(replace: bool) {
    let (h, _owner, wakes) = create();
    let (old, sent) = http_server(b"stale snapshot");
    start_get(&h, 810, &old);
    sent.recv_timeout(Duration::from_secs(5)).unwrap();
    for _ in 0..3 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    // Let the driver prune the completed task while its response remains queued.
    assert_eq!(unsafe { nsh_transport_ws(h.0, &socket_op(812, 1)) }, 0);
    for _ in 0..2 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let (replacement, replacement_started) = if replace {
        let (url, started) = http_server(b"fresh snapshot");
        (Some(url), Some(started))
    } else {
        (None, None)
    };
    let mut state = ReentrantDrain {
        handle: h.0,
        events: vec![],
        replacement,
        replacement_started,
        wakes,
        operation_processed: false,
        failure: None,
    };
    unsafe {
        nsh_transport_drain_http(
            h.0,
            Some(reentrant_http_visit),
            &mut state as *mut _ as *mut c_void,
        )
    };
    assert!(state.failure.is_none(), "{:?}", state.failure);
    assert!(state.operation_processed);
    assert_eq!(
        state.events.iter().map(|e| e.kind).collect::<Vec<_>>(),
        [1],
        "old snapshot Data/End were delivered after reentrant {}: {:?}",
        if replace { "Start" } else { "Cancel" },
        state.events
    );
    if replace {
        let deadline = Instant::now() + Duration::from_secs(5);
        while !state.events.iter().any(|e| e.kind == 3) {
            drain_http(&h, &mut state.events);
            assert!(Instant::now() < deadline, "{:?}", state.events);
            let _ = state.wakes.recv_timeout(Duration::from_millis(10));
        }
        assert_eq!(
            state
                .events
                .iter()
                .flat_map(|e| e.data.clone())
                .collect::<Vec<_>>(),
            b"fresh snapshot"
        );
        assert_eq!(state.events.iter().filter(|e| e.kind == 3).count(), 1);
    }
}
#[test]
fn ffi_reentrant_start_invalidates_completed_response_already_in_drain_snapshot() {
    reentrant_completed_response(true);
}
#[test]
fn ffi_reentrant_cancel_invalidates_completed_response_already_in_drain_snapshot() {
    reentrant_completed_response(false);
}
