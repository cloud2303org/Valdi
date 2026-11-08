use std::{
    cell::RefCell,
    io,
    net::TcpListener,
    rc::Rc,
    time::{Duration, Instant},
};
use tokio_tungstenite::tungstenite::{Message, accept};
use valdi_nsh_transport::websocket::*;
fn fixture() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    std::thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(10)))
            .unwrap();
        let mut ws = accept(stream).unwrap();
        loop {
            match ws.read() {
                Ok(Message::Close(_)) => {
                    let _ = ws.flush();
                    break;
                }
                Ok(message) => {
                    if ws.send(message).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    });
    format!("ws://{addr}/echo")
}
fn open(url: String) -> (Box<dyn WebSocketTask>, Rc<RefCell<Vec<WebSocketEvent>>>) {
    let events = Rc::new(RefCell::new(Vec::new()));
    let sink = events.clone();
    let task = transport()
        .connect(
            WebSocketRequest {
                url,
                protocols: vec![],
                headers: vec![],
            },
            Box::new(move |e| sink.borrow_mut().push(e)),
        )
        .unwrap();
    assert!(events.borrow().is_empty(), "connect cannot run callbacks");
    (task, events)
}
fn wait(
    task: &mut dyn WebSocketTask,
    events: &Rc<RefCell<Vec<WebSocketEvent>>>,
    predicate: impl Fn(&[WebSocketEvent]) -> bool,
) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !predicate(&events.borrow()) {
        task.dispatch();
        assert!(Instant::now() < deadline, "events: {:?}", events.borrow());
        std::thread::park_timeout(Duration::from_millis(2));
    }
}
#[test]
fn echo_text_binary_and_clean_close() {
    let (mut task, events) = open(fixture());
    wait(&mut *task, &events, |e| {
        matches!(e.first(), Some(WebSocketEvent::Open { .. }))
    });
    task.send(WebSocketData::Text("hello".into())).unwrap();
    task.send(WebSocketData::Bytes(vec![0, 1, 255])).unwrap();
    wait(&mut *task, &events, |e| e.len() >= 3);
    assert!(
        matches!(&events.borrow()[1], WebSocketEvent::Message(WebSocketData::Text(t)) if t=="hello")
    );
    assert!(
        matches!(&events.borrow()[2], WebSocketEvent::Message(WebSocketData::Bytes(b)) if b==&[0,1,255])
    );
    assert_eq!(task.buffered_amount(), 0);
    task.close(1000, "done").unwrap();
    wait(&mut *task, &events, |e| {
        matches!(
            e.last(),
            Some(WebSocketEvent::Close {
                was_clean: true,
                ..
            })
        )
    });
    assert!(
        matches!(events.borrow().last(),Some(WebSocketEvent::Close{code:1000,reason,..}) if reason=="done")
    );
}
#[test]
fn abort_pending_handshake_releases_callback_and_socket() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let owner = Rc::new(());
    let weak = Rc::downgrade(&owner);
    let task = transport()
        .connect(
            WebSocketRequest {
                url,
                protocols: vec![],
                headers: vec![],
            },
            Box::new(move |_| {
                let _ = &owner;
            }),
        )
        .unwrap();
    let (mut stream, _) = listener.accept().unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    task.abort();
    assert!(weak.upgrade().is_none(), "abort retains callback");
    let mut byte = [0];
    use io::Read;
    loop {
        match stream.read(&mut byte) {
            Ok(0) => break,
            Ok(_) => {}
            Err(e) => {
                assert_ne!(e.kind(), io::ErrorKind::WouldBlock);
                break;
            }
        }
    }
}
#[test]
fn pause_suppresses_delivery_until_resume() {
    let (mut task, events) = open(fixture());
    wait(&mut *task, &events, |e| !e.is_empty());
    task.pause(true);
    task.send(WebSocketData::Text("paused".into())).unwrap();
    let until = Instant::now() + Duration::from_millis(150);
    while Instant::now() < until {
        task.dispatch();
        std::thread::park_timeout(Duration::from_millis(2));
    }
    assert_eq!(events.borrow().len(), 1);
    task.pause(false);
    wait(&mut *task, &events, |e| e.len() == 2);
}

#[test]
fn failed_handshake_reports_error_then_abnormal_close_and_releases_callback() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    drop(listener);
    let owner = Rc::new(());
    let weak = Rc::downgrade(&owner);
    let events = Rc::new(RefCell::new(Vec::new()));
    let out = events.clone();
    let mut task = transport()
        .connect(
            WebSocketRequest {
                url,
                protocols: vec![],
                headers: vec![],
            },
            Box::new(move |event| {
                let _ = &owner;
                out.borrow_mut().push(event);
            }),
        )
        .unwrap();
    wait(&mut *task, &events, |e| e.len() >= 2);
    assert!(matches!(&events.borrow()[0], WebSocketEvent::Error(_)));
    assert!(matches!(
        &events.borrow()[1],
        WebSocketEvent::Close {
            code: 1006,
            was_clean: false,
            ..
        }
    ));
    assert!(weak.upgrade().is_none(), "terminal close retained callback");
}

#[test]
fn close_still_completes_while_reads_are_paused() {
    let (mut task, events) = open(fixture());
    wait(&mut *task, &events, |e| !e.is_empty());
    task.pause(true);
    task.close(1000, "paused close").unwrap();
    wait(&mut *task, &events, |e| {
        matches!(
            e.last(),
            Some(WebSocketEvent::Close {
                was_clean: true,
                ..
            })
        )
    });
}

#[test]
fn abort_after_inbound_queue_fills_closes_the_peer() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let (filled, fill_rx) = std::sync::mpsc::channel();
    let (closed, close_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut ws = accept(stream).unwrap();
        for _ in 0..32 {
            if ws.send(Message::Text("flood".into())).is_err() {
                break;
            }
        }
        let _ = filled.send(());
        let result = ws.read();
        let _ = closed.send(result.is_err());
    });
    let owner = Rc::new(());
    let weak = Rc::downgrade(&owner);
    let task = transport()
        .connect(
            WebSocketRequest {
                url,
                protocols: vec![],
                headers: vec![],
            },
            Box::new(move |_| {
                let _ = &owner;
            }),
        )
        .unwrap();
    fill_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    // No dispatch: worker fills its bounded event queue and eventually waits.
    drop(task);
    assert!(weak.upgrade().is_none());
    assert!(close_rx.recv_timeout(Duration::from_secs(5)).unwrap());
}

#[test]
fn rejects_credentials_fragments_and_non_websocket_schemes() {
    for url in [
        "http://localhost/",
        "ws://user@localhost/",
        "ws://localhost/#fragment",
    ] {
        assert!(
            transport()
                .connect(
                    WebSocketRequest {
                        url: url.into(),
                        protocols: vec![],
                        headers: vec![]
                    },
                    Box::new(|_| {})
                )
                .is_err()
        );
    }
}

#[test]
// tungstenite defines this callback error as an unboxed HTTP response.
#[allow(clippy::result_large_err)]
fn handshake_preserves_additional_headers_and_negotiates_requested_protocol() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    let (checked, check) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        use tokio_tungstenite::tungstenite::handshake::server::{Request, Response};
        let (stream, _) = listener.accept().unwrap();
        let mut socket = tokio_tungstenite::tungstenite::accept_hdr(
            stream,
            |request: &Request, mut response: Response| {
                assert_eq!(request.headers()["X-Test"], "header");
                assert_eq!(
                    request.headers()["Sec-WebSocket-Protocol"],
                    "test.v1, test.v2"
                );
                response
                    .headers_mut()
                    .insert("Sec-WebSocket-Protocol", "test.v2".parse().unwrap());
                Ok(response)
            },
        )
        .unwrap();
        checked.send(()).unwrap();
        let _ = socket.close(None);
    });
    let events = Rc::new(RefCell::new(Vec::new()));
    let out = events.clone();
    let mut task = transport()
        .connect(
            WebSocketRequest {
                url,
                protocols: vec!["test.v1".into(), "test.v2".into()],
                headers: vec![("X-Test".into(), "header".into())],
            },
            Box::new(move |e| out.borrow_mut().push(e)),
        )
        .unwrap();
    check.recv_timeout(Duration::from_secs(5)).unwrap();
    wait(&mut *task, &events, |e| !e.is_empty());
    assert!(matches!(&events.borrow()[0],WebSocketEvent::Open{protocol} if protocol=="test.v2"));
}

#[test]
fn close_while_paused_does_not_deadlock_behind_inflight_messages() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("ws://{}/", listener.local_addr().unwrap());
    std::thread::spawn(move || {
        let (peer, _) = listener.accept().unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut socket = accept(peer).unwrap();
        for _ in 0..16 {
            if socket.send(Message::Text("inflight".into())).is_err() {
                return;
            }
        }
        while let Ok(message) = socket.read() {
            if matches!(message, Message::Close(_)) {
                let _ = socket.flush();
                break;
            }
        }
    });
    let (wake, ready) = std::sync::mpsc::channel();
    let factory = transport_with_wake(std::sync::Arc::new(move || {
        let _ = wake.send(());
    }));
    let events = Rc::new(RefCell::new(Vec::new()));
    let sink = events.clone();
    let mut task = factory
        .connect(
            WebSocketRequest {
                url,
                protocols: vec![],
                headers: vec![],
            },
            Box::new(move |e| sink.borrow_mut().push(e)),
        )
        .unwrap();
    // The Open plus seven messages filled the bounded native event queue.
    for _ in 0..8 {
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    task.pause(true);
    task.close(1000, "paused backlog").unwrap();
    wait(&mut *task, &events, |e| {
        matches!(
            e.last(),
            Some(WebSocketEvent::Close {
                was_clean: true,
                ..
            })
        )
    });
}
