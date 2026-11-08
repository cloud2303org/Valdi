use super::*;

struct Received {
    kind: u32,
    data: usize,
    len: usize,
    owner: usize,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
    borrowed_matches: bool,
}
impl Received {
    fn new(kind: u32, borrowed: NshTransportBytes, owned: NshTransportOwnedBytes) -> Self {
        Self {
            kind,
            data: owned.data as usize,
            len: owned.len,
            owner: owned.owner as usize,
            release: owned.release,
            borrowed_matches: borrowed.data == owned.data.cast_const() && borrowed.len == owned.len,
        }
    }
    fn bytes(&self) -> &[u8] {
        if self.len == 0 {
            return &[];
        }
        assert!(self.release.is_some());
        unsafe { std::slice::from_raw_parts(self.data as *const u8, self.len) }
    }
}
impl Drop for Received {
    fn drop(&mut self) {
        if let Some(release) = self.release.take() {
            unsafe { release(self.owner as *mut c_void) };
        }
    }
}
unsafe extern "C" fn owned_http_visit(
    user: *mut c_void,
    event: *const NshTransportHttpEvent,
    owned: NshTransportOwnedBytes,
) {
    let event = unsafe { &*event };
    unsafe { &mut *user.cast::<Vec<Received>>() }.push(Received::new(
        event.event,
        event.bytes,
        owned,
    ));
}
unsafe extern "C" fn owned_ws_visit(
    user: *mut c_void,
    event: *const NshTransportSocketEvent,
    owned: NshTransportOwnedBytes,
) {
    let event = unsafe { &*event };
    unsafe { &mut *user.cast::<Vec<Received>>() }.push(Received::new(
        event.event,
        event.bytes,
        owned,
    ));
}
fn drain_owned_http(h: &Handle, received: &mut Vec<Received>) {
    unsafe {
        nsh_transport_drain_http_owned(
            h.0,
            Some(owned_http_visit),
            received as *mut _ as *mut c_void,
        )
    };
}
fn drain_owned_ws(h: &Handle, received: &mut Vec<Received>) {
    unsafe {
        nsh_transport_drain_ws_owned(h.0, Some(owned_ws_visit), received as *mut _ as *mut c_void)
    };
}
fn wait_owned(
    h: &Handle,
    received: &mut Vec<Received>,
    wakes: &mpsc::Receiver<()>,
    http: bool,
    predicate: impl Fn(&[Received]) -> bool,
) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while !predicate(received) {
        if http {
            drain_owned_http(h, received);
        } else {
            drain_owned_ws(h, received);
        }
        assert!(Instant::now() < deadline, "owned receive timed out");
        if !predicate(received) {
            let _ = wakes.recv_timeout(Duration::from_millis(10));
        }
    }
}
fn assert_descriptors(received: &[Received], http: bool) {
    for event in received {
        if event.kind == 2 || (!http && event.kind == 3) {
            assert!(event.release.is_some());
            assert_ne!(event.owner, 0);
            assert!(event.borrowed_matches);
        } else {
            assert!(event.release.is_none());
            assert_eq!((event.data, event.len, event.owner), (0, 0, 0));
        }
    }
}

#[test]
fn owned_real_http_split_utf8_survives_destroy_and_cross_thread_release() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/", listener.local_addr().unwrap());
    let (continue_tx, continue_rx) = mpsc::channel();
    let server = std::thread::spawn(move || {
        let (mut peer, _) = listener.accept().unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut head = Vec::new();
        let mut byte = [0];
        while !head.ends_with(b"\r\n\r\n") {
            peer.read_exact(&mut byte).unwrap();
            head.push(byte[0]);
        }
        peer.write_all(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n7\r\ndata: \xe2\r\n").unwrap();
        continue_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        peer.write_all(b"4\r\n\x82\xac\n\n\r\n0\r\n\r\n").unwrap();
    });
    let (h, owner, wakes) = create();
    start_get(&h, 910, &url);
    let mut received = Vec::new();
    wait_owned(&h, &mut received, &wakes, true, |events| {
        events.iter().any(|e| e.kind == 2)
    });
    assert_eq!(
        received
            .iter()
            .flat_map(|e| e.bytes())
            .copied()
            .collect::<Vec<_>>(),
        b"data: \xe2"
    );
    continue_tx.send(()).unwrap();
    wait_owned(&h, &mut received, &wakes, true, |events| {
        events.iter().any(|e| e.kind == 3)
    });
    server.join().unwrap();
    drop(h);
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
    std::thread::spawn(move || {
        assert_descriptors(&received, true);
        assert_eq!(
            received
                .iter()
                .flat_map(|e| e.bytes())
                .copied()
                .collect::<Vec<_>>(),
            "data: €\n\n".as_bytes()
        );
        // The last owners release on this thread after transport destruction.
        drop(received);
    })
    .join()
    .unwrap();
}

#[test]
fn owned_real_ws_text_binary_and_empty_messages_survive_destroy() {
    use tokio_tungstenite::tungstenite::{Message, accept};
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let server = std::thread::spawn(move || {
        let (peer, _) = listener.accept().unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut socket = accept(peer).unwrap();
        loop {
            match socket.read() {
                Ok(Message::Close(_)) => {
                    let _ = socket.flush();
                    return;
                }
                Ok(message) => socket.send(message).unwrap(),
                Err(_) => return,
            }
        }
    });
    let (h, owner, wakes) = create();
    let mut connect = socket_op(911, 1);
    connect.bytes = bytes(url.as_bytes());
    assert_eq!(unsafe { nsh_transport_ws(h.0, &connect) }, 0);
    let mut received = Vec::new();
    wait_owned(&h, &mut received, &wakes, false, |events| {
        events.iter().any(|e| e.kind == 1)
    });
    for (op, data) in [
        (2, "hello €".as_bytes()),
        (3, &[0, 255, 1][..]),
        (2, &[]),
        (3, &[]),
    ] {
        let mut send = socket_op(911, op);
        send.bytes = bytes(data);
        assert_eq!(unsafe { nsh_transport_ws(h.0, &send) }, 0);
    }
    wait_owned(&h, &mut received, &wakes, false, |events| {
        events.iter().filter(|e| matches!(e.kind, 2 | 3)).count() == 4
    });
    let mut close = socket_op(911, 4);
    close.code = 1000;
    close.bytes = bytes(b"owned close");
    assert_eq!(unsafe { nsh_transport_ws(h.0, &close) }, 0);
    wait_owned(&h, &mut received, &wakes, false, |events| {
        events.iter().any(|e| e.kind == 4)
    });
    server.join().unwrap();
    drop(h);
    assert_eq!(owner.releases.load(Ordering::SeqCst), 1);
    std::thread::spawn(move || {
        assert_descriptors(&received, false);
        let messages = received
            .iter()
            .filter(|e| matches!(e.kind, 2 | 3))
            .collect::<Vec<_>>();
        assert_eq!(
            messages.iter().map(|e| e.kind).collect::<Vec<_>>(),
            [2, 3, 2, 3]
        );
        assert_eq!(messages[0].bytes(), "hello €".as_bytes());
        assert_eq!(messages[1].bytes(), [0, 255, 1]);
        assert!(messages[2].bytes().is_empty());
        assert!(messages[3].bytes().is_empty());
    })
    .join()
    .unwrap();
}

unsafe extern "C" fn reentrant_owned_http_visit(
    user: *mut c_void,
    event: *const NshTransportHttpEvent,
    owned: NshTransportOwnedBytes,
) {
    unsafe { reentrant_http_visit(user, event) };
    if let Some(release) = owned.release {
        unsafe { release(owned.owner) };
    }
}
fn reentrant_owned_snapshot(replace: bool) {
    let (h, _owner, wakes) = create();
    let (old, sent) = http_server(b"stale snapshot");
    start_get(&h, 810, &old);
    sent.recv_timeout(Duration::from_secs(5)).unwrap();
    for _ in 0..3 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    assert_eq!(unsafe { nsh_transport_ws(h.0, &socket_op(812, 1)) }, 0);
    for _ in 0..2 {
        wakes.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    let (replacement, replacement_started) = if replace {
        let (url, started) = http_server(b"fresh owned");
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
        nsh_transport_drain_http_owned(
            h.0,
            Some(reentrant_owned_http_visit),
            &mut state as *mut _ as *mut c_void,
        )
    };
    assert!(state.failure.is_none(), "{:?}", state.failure);
    assert!(state.operation_processed);
    assert_eq!(
        state.events.iter().map(|e| e.kind).collect::<Vec<_>>(),
        [1],
        "retired snapshot was delivered"
    );
    if replace {
        let mut received = Vec::new();
        wait_owned(&h, &mut received, &state.wakes, true, |events| {
            events.iter().any(|e| e.kind == 3)
        });
        assert_descriptors(&received, true);
        assert_eq!(
            received
                .iter()
                .flat_map(|e| e.bytes())
                .copied()
                .collect::<Vec<_>>(),
            b"fresh owned"
        );
    }
}
#[test]
fn owned_real_reentrant_cancel_invalidates_completed_snapshot() {
    reentrant_owned_snapshot(false);
}
#[test]
fn owned_real_reentrant_replacement_reuses_completed_id_without_stale_snapshot() {
    reentrant_owned_snapshot(true);
}
