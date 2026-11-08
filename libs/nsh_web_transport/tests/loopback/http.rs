#[cfg(test)]
mod tests {
    use std::{
        io::{BufRead, BufReader, Write},
        net::{TcpListener, TcpStream},
        sync::mpsc::RecvTimeoutError,
    };

    use std::{
        io::Read,
        sync::mpsc::{Receiver, channel},
        time::Duration,
    };
    use valdi_nsh_transport::*;

    const WAIT: Duration = Duration::from_secs(10);

    /// A local HTTP server: `handle` gets each connection with its request
    /// line and headers already read (lower-case names). Returns the base URL.
    fn serve(
        handle: impl Fn(&mut BufReader<TcpStream>, &str, &[(String, String)]) + Send + 'static,
    ) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
        let port = listener.local_addr().expect("addr").port();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(stream) = stream else { return };
                let mut reader = BufReader::new(stream);
                let mut line = String::new();
                if reader.read_line(&mut line).is_err() {
                    continue;
                }
                let mut headers = Vec::new();
                loop {
                    let mut h = String::new();
                    reader.read_line(&mut h).expect("header");
                    let h = h.trim_end();
                    if h.is_empty() {
                        break;
                    }
                    let (k, v) = h.split_once(':').expect("header line");
                    headers.push((k.trim().to_ascii_lowercase(), v.trim().to_owned()));
                }
                handle(&mut reader, line.trim_end(), &headers);
            }
        });
        format!("http://127.0.0.1:{port}")
    }

    fn respond(stream: &mut BufReader<TcpStream>, head: &str, body: &[u8]) {
        let s = stream.get_mut();
        let _ = write!(
            s,
            "HTTP/1.1 {head}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            body.len()
        );
        let _ = s.write_all(body);
    }

    fn request(method: &str, url: &str) -> HttpRequest {
        HttpRequest {
            method: method.to_owned(),
            url: url.to_owned(),
            headers: Vec::new(),
            body: HttpBody::Empty,
            follow_redirects: true,
        }
    }

    fn start(request: HttpRequest) -> (Box<dyn FetchTask>, Receiver<FetchEvent>) {
        let (tx, rx) = channel();
        let task = network().fetch(
            request,
            Box::new(move |e| {
                let _ = tx.send(e);
            }),
        );
        (task, rx)
    }

    fn next(rx: &Receiver<FetchEvent>) -> FetchEvent {
        rx.recv_timeout(WAIT).expect("event")
    }

    /// The rest of the body up to `End`.
    fn body(rx: &Receiver<FetchEvent>) -> Vec<u8> {
        let mut body = Vec::new();
        loop {
            match next(rx) {
                FetchEvent::Data(d) => body.extend(d),
                FetchEvent::End => return body,
                e => panic!("unexpected {e:?}"),
            }
        }
    }

    /// The head, then the whole body.
    fn response(rx: &Receiver<FetchEvent>) -> (HttpHead, Vec<u8>) {
        let FetchEvent::Head(head) = next(rx) else {
            panic!("no head")
        };
        (head, body(rx))
    }

    fn values<'a>(headers: &'a [(String, String)], name: &str) -> Vec<&'a str> {
        headers
            .iter()
            .filter(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
            .collect()
    }

    #[test]
    fn get_with_headers_in_order() {
        let base = serve(|s, line, headers| {
            assert_eq!(line, "GET /path?q=1 HTTP/1.1");
            assert_eq!(values(headers, "x-a"), ["1", "2"]);
            let _ = s.get_mut().write_all(
                b"HTTP/1.1 404 Not Found\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\
                  Content-Length: 4\r\nConnection: close\r\n\r\nnope",
            );
        });
        let mut r = request("GET", &format!("{base}/path?q=1"));
        r.headers = vec![("X-A".into(), "1".into()), ("X-A".into(), "2".into())];
        let (_task, rx) = start(r);
        let (head, body) = response(&rx);
        assert_eq!(head.status, 404);
        assert_eq!(head.status_text, "Not Found");
        assert!(!head.redirected);
        assert_eq!(head.url, format!("{base}/path?q=1"));
        assert_eq!(values(&head.headers, "set-cookie"), ["a=1", "b=2"]);
        assert_eq!(body, b"nope");
    }

    #[test]
    fn post_streamed_body_is_chunked() {
        let base = serve(|s, line, headers| {
            assert_eq!(line, "POST /up HTTP/1.1");
            assert_eq!(values(headers, "transfer-encoding"), ["chunked"]);
            let mut body = Vec::new();
            loop {
                let mut size = String::new();
                s.read_line(&mut size).expect("size");
                let size = usize::from_str_radix(size.trim(), 16).expect("hex");
                let mut chunk = vec![0; size + 2];
                s.read_exact(&mut chunk).expect("chunk");
                if size == 0 {
                    break;
                }
                body.extend_from_slice(&chunk[..size]);
            }
            respond(s, "200 OK", &body);
        });
        let mut r = request("POST", &format!("{base}/up"));
        r.body = HttpBody::Stream;
        let (mut task, rx) = start(r);
        let parts: [&[u8]; 3] = [b"hello ", b"streamed ", b"world"];
        let mut wanted = 0;
        let head = loop {
            match next(&rx) {
                FetchEvent::BodyWanted => {
                    match parts.get(wanted) {
                        Some(p) => task.send(p.to_vec()),
                        None => task.finish(),
                    }
                    wanted += 1;
                }
                FetchEvent::Head(h) => break h,
                e => panic!("unexpected {e:?}"),
            }
        };
        assert_eq!(wanted, 4);
        assert_eq!(head.status, 200);
        assert_eq!(body(&rx), b"hello streamed world");
    }

    fn redirecting() -> String {
        serve(|s, line, _| {
            if line.starts_with("GET /a ") {
                let _ = s.get_mut().write_all(
                    b"HTTP/1.1 302 Found\r\nLocation: /b\r\nContent-Length: 0\r\n\
                      Connection: close\r\n\r\n",
                );
            } else {
                respond(s, "200 OK", b"at b");
            }
        })
    }

    #[test]
    fn follows_redirects() {
        let base = redirecting();
        let (_task, rx) = start(request("GET", &format!("{base}/a")));
        let (head, body) = response(&rx);
        assert_eq!(head.status, 200);
        assert!(head.redirected);
        assert_eq!(head.url, format!("{base}/b"));
        assert_eq!(body, b"at b");
    }

    #[test]
    fn manual_redirects_hand_on_the_3xx() {
        let base = redirecting();
        let mut r = request("GET", &format!("{base}/a"));
        r.follow_redirects = false;
        let (_task, rx) = start(r);
        let (head, _) = response(&rx);
        assert_eq!(head.status, 302);
        assert!(!head.redirected);
        assert_eq!(head.url, format!("{base}/a"));
        assert_eq!(values(&head.headers, "location"), ["/b"]);
    }

    #[test]
    fn pause_stops_reading_until_resumed() {
        const LEN: usize = 8 << 20;
        let base = serve(|s, _, _| respond(s, "200 OK", &vec![7; LEN]));
        let (mut task, rx) = start(request("GET", &base));
        let FetchEvent::Head(_) = next(&rx) else {
            panic!("no head")
        };
        let FetchEvent::Data(first) = next(&rx) else {
            panic!("no data")
        };
        task.pause(true);
        std::thread::sleep(Duration::from_millis(300));
        // Chunks read before the pause took effect are already queued: drain
        // them, then nothing more may arrive while paused.
        let mut total = first.len();
        let drain = |total: &mut usize| {
            while let Ok(e) = rx.try_recv() {
                let FetchEvent::Data(d) = e else {
                    panic!("unexpected {e:?}")
                };
                *total += d.len();
            }
        };
        drain(&mut total);
        std::thread::sleep(Duration::from_millis(300));
        let settled = total;
        drain(&mut total);
        assert!(
            total - settled <= 1 << 16,
            "{} bytes while paused",
            total - settled
        );
        assert!(total < LEN);
        task.pause(false);
        total += body(&rx).len();
        assert_eq!(total, LEN);
    }

    #[test]
    fn cancel_mid_body_stops_events() {
        let base = serve(|s, _, _| {
            let s = s.get_mut();
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n");
            let _ = s.write_all(&[1; 1000]);
            let _ = s.flush();
            std::thread::sleep(Duration::from_millis(200));
            let _ = s.write_all(&[2; 99000]);
        });
        let (task, rx) = start(request("GET", &base));
        let FetchEvent::Head(_) = next(&rx) else {
            panic!("no head")
        };
        let FetchEvent::Data(_) = next(&rx) else {
            panic!("no data")
        };
        task.cancel();
        match rx.recv_timeout(Duration::from_millis(600)) {
            Err(RecvTimeoutError::Timeout | RecvTimeoutError::Disconnected) => {}
            Ok(e) => panic!("event after cancel: {e:?}"),
        }
    }

    #[test]
    fn connection_refused_fails() {
        let port = {
            let l = TcpListener::bind("127.0.0.1:0").expect("bind");
            l.local_addr().expect("addr").port()
        };
        let (_task, rx) = start(request("GET", &format!("http://127.0.0.1:{port}/")));
        assert!(matches!(next(&rx), FetchEvent::Failed(_)));
    }

    #[test]
    fn invalid_url_fails() {
        let (_task, rx) = start(request("GET", "not a url"));
        assert!(matches!(next(&rx), FetchEvent::Failed(_)));
    }
    #[test]
    fn cancel_drops_callback_while_desktop_read_is_blocked() {
        let (release, hold) = channel();
        let hold = std::sync::Mutex::new(hold);
        let base = serve(move |s, _, _| {
            s.get_mut()
                .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n")
                .unwrap();
            s.get_mut().flush().unwrap();
            let _ = hold.lock().unwrap().recv_timeout(WAIT);
            let _ = s.get_mut().write_all(b"finish");
        });
        let owner = std::sync::Arc::new(());
        let weak = std::sync::Arc::downgrade(&owner);
        let (tx, rx) = channel();
        let task = network().fetch(
            HttpRequest::get(base),
            Box::new(move |e| {
                let _ = &owner;
                let _ = tx.send(e);
            }),
        );
        assert!(matches!(next(&rx), FetchEvent::Head(_)));
        task.cancel();
        let retained = weak.upgrade().is_some();
        release.send(()).unwrap();
        assert!(
            !retained,
            "cancel retained the callback until a blocked socket read completed"
        );
        assert!(rx.recv_timeout(WAIT).is_err());
    }

    #[test]
    fn dropping_a_streaming_upload_cancels_and_releases_callback() {
        let base = serve(|_, _, _| {});
        let owner = std::sync::Arc::new(());
        let weak = std::sync::Arc::downgrade(&owner);
        let (tx, rx) = channel();
        let mut r = HttpRequest::new("POST", base);
        r.body = HttpBody::Stream;
        let task = network().fetch(
            r,
            Box::new(move |e| {
                let _ = &owner;
                let _ = tx.send(e);
            }),
        );
        assert_eq!(next(&rx), FetchEvent::BodyWanted);
        drop(task);
        assert!(weak.upgrade().is_none(), "drop retained upload callback");
        assert!(rx.recv_timeout(WAIT).is_err());
    }

    #[test]
    fn delayed_chunk_is_delivered_before_server_finishes() {
        let (release, hold) = channel();
        let hold = std::sync::Mutex::new(hold);
        let base = serve(move |s, _, _| {
            s.get_mut().write_all(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\none\r\n").unwrap();
            s.get_mut().flush().unwrap();
            let _ = hold.lock().unwrap().recv_timeout(WAIT);
            let _ = s.get_mut().write_all(b"3\r\ntwo\r\n0\r\n\r\n");
        });
        let (_task, rx) = start(HttpRequest::get(base));
        assert!(matches!(next(&rx), FetchEvent::Head(_)));
        assert_eq!(next(&rx), FetchEvent::Data(b"one".to_vec()));
        release.send(()).unwrap();
        assert_eq!(body(&rx), b"two");
    }

    #[test]
    fn bytes_post_is_sent_without_a_stream() {
        let base = serve(|s, line, headers| {
            assert!(line.starts_with("POST "));
            assert_eq!(values(headers, "content-length"), ["4"]);
            let mut bytes = [0; 4];
            s.read_exact(&mut bytes).unwrap();
            respond(s, "201 Created", &bytes);
        });
        let (_task, rx) = start(HttpRequest::post(base, b"post".to_vec()));
        let (head, data) = response(&rx);
        assert_eq!(head.status, 201);
        assert_eq!(data, b"post");
    }
}
