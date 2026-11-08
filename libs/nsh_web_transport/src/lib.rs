//! Engine-independent NSH HTTP and WebSocket transport contracts.
//! Hosts marshal owned events onto their per-context execution queues.
#![warn(missing_docs)]

/// An HTTP request for the platform's network stack (made by JS `fetch`
/// and Rust consumers).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpRequest {
    /// Upper-case method (`GET`, `POST`, ...).
    pub method: String,
    /// Absolute URL.
    pub url: String,
    /// Header names and values, in order; names may repeat.
    pub headers: Vec<(String, String)>,
    /// Request body.
    pub body: HttpBody,
    /// Follow redirects (`redirect: "follow"`, the default).
    pub follow_redirects: bool,
}

impl HttpRequest {
    /// A request of `method` (`GET`, `POST`, ...; upper-cased) to `url`,
    /// with no headers or body, following redirects.
    pub fn new(method: impl Into<String>, url: impl Into<String>) -> Self {
        Self {
            method: method.into().to_ascii_uppercase(),
            url: url.into(),
            headers: Vec::new(),
            body: HttpBody::Empty,
            follow_redirects: true,
        }
    }

    /// A `GET` of `url`.
    pub fn get(url: impl Into<String>) -> Self {
        Self::new("GET", url)
    }

    /// A `POST` of `body` to `url`.
    pub fn post(url: impl Into<String>, body: impl Into<Vec<u8>>) -> Self {
        Self::new("POST", url).body(body)
    }

    /// Adds the header `name: value`.
    #[must_use]
    pub fn header(mut self, name: impl Into<String>, value: impl Into<String>) -> Self {
        self.headers.push((name.into(), value.into()));
        self
    }

    /// Sends `body`.
    #[must_use]
    pub fn body(mut self, body: impl Into<Vec<u8>>) -> Self {
        self.body = HttpBody::Bytes(body.into());
        self
    }
}

impl From<&str> for HttpRequest {
    fn from(url: &str) -> Self {
        Self::get(url)
    }
}

impl From<String> for HttpRequest {
    fn from(url: String) -> Self {
        Self::get(url)
    }
}

/// The body of an [`HttpRequest`].
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum HttpBody {
    /// No body.
    #[default]
    Empty,
    /// All of it, known up front.
    Bytes(Vec<u8>),
    /// Produced while the request is sent (a `ReadableStream`), in chunks
    /// the stack asks for one at a time ([`FetchEvent::BodyWanted`]); sent
    /// with chunked transfer encoding.
    Stream,
}

impl HttpBody {
    /// No body at all.
    pub fn is_empty(&self) -> bool {
        match self {
            HttpBody::Empty => true,
            HttpBody::Bytes(b) => b.is_empty(),
            HttpBody::Stream => false,
        }
    }
}

/// The status line and headers of a response, before its body.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HttpHead {
    /// Status code.
    pub status: u16,
    /// Reason phrase, if the stack reports one.
    pub status_text: String,
    /// Final URL, after redirects.
    pub url: String,
    /// True if a redirect was followed.
    pub redirected: bool,
    /// Header names and values.
    pub headers: Vec<(String, String)>,
}

/// What happens to a request, in order: (for a streamed body) any number
/// of `BodyWanted`; then `Head`, `Data`… and `End` — or `Failed` at any
/// point.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FetchEvent {
    /// A streamed body's next chunk is wanted: first when the stack is
    /// ready to send, then each time the last chunk was handed on.
    BodyWanted,
    /// The response's status and headers arrived.
    Head(HttpHead),
    /// Bytes of the response body, in order.
    Data(Vec<u8>),
    /// The body is complete.
    End,
    /// The request failed (no response, or the body broke off).
    Failed(String),
}

/// What JS asks of a request it made (the JS host queues these for the
/// engine, which hands them to the stack).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FetchOp {
    /// Make the request.
    Start(HttpRequest),
    /// The next chunk of a streamed body (after [`FetchEvent::BodyWanted`]).
    Send(Vec<u8>),
    /// A streamed body is complete.
    Finish,
    /// Stop (`true`) or go on (`false`) reading the response body: nobody
    /// reads what arrived.
    Pause(bool),
    /// Abort the request (`AbortController`, or the body was cancelled).
    Cancel,
}

/// Receives what happens to a request started by [`Network::fetch`], in
/// order, on any thread. Native backends release this callback during cancel.
/// Enqueue owned events; never run JS or reenter task control from a callback.
pub type FetchEvents = Box<dyn FnMut(FetchEvent) + Send>;

/// A request in flight, driven from the engine thread.
pub trait FetchTask: Send {
    /// The next chunk of a streamed body ([`HttpBody::Stream`]), after
    /// [`FetchEvent::BodyWanted`].
    fn send(&mut self, chunk: Vec<u8>);
    /// A streamed body is complete.
    fn finish(&mut self);
    /// Stops (`true`) or resumes (`false`) reading the response body: JS
    /// does not read what arrived (back pressure). Bytes already on their
    /// way may still arrive.
    fn pause(&mut self, paused: bool);
    /// Aborts the request.
    fn cancel(self: Box<Self>);
}

/// The platform's HTTP stack, used by JS `fetch`.
pub trait Network: Send + Sync + 'static {
    /// Starts `request`: its response head, then its body in chunks as they
    /// arrive, are reported to `events`.
    fn fetch(&self, request: HttpRequest, events: FetchEvents) -> Box<dyn FetchTask>;
}

/// Upstream-compatible HTTP names, also exported at the crate root.
pub mod platform {
    pub use crate::{
        FetchEvent, FetchEvents, FetchOp, FetchTask, HttpBody, HttpHead, HttpRequest, Network,
    };
}
/// Upstream-compatible WebSocket contracts and owned native transport.
pub mod websocket;
pub use websocket::{
    WebSocketData, WebSocketEvent, WebSocketEvents, WebSocketRequest, WebSocketTask,
    WebSocketTransport,
};

#[cfg(target_os = "android")]
mod android;
#[cfg(target_os = "ios")]
mod ios;
#[cfg(target_os = "android")]
mod java;
#[cfg(not(any(target_os = "android", target_os = "ios")))]
mod ureq_backend;

/// Process-shared platform HTTP backend (system mobile stacks; desktop ureq).
pub fn network() -> std::sync::Arc<dyn Network> {
    #[cfg(target_os = "android")]
    {
        android::network()
    }
    #[cfg(target_os = "ios")]
    {
        ios::network()
    }
    #[cfg(not(any(target_os = "android", target_os = "ios")))]
    {
        ureq_backend::network()
    }
}
#[cfg(target_os = "android")]
pub use java::init_android;

// A shared optional callback lets cancellation release the host context even
// when a platform socket read or streamed-body writer has not returned yet.
struct EventSink(std::sync::Mutex<Option<FetchEvents>>);
impl EventSink {
    fn new(events: FetchEvents) -> Self {
        Self(std::sync::Mutex::new(Some(events)))
    }
    fn send(&self, event: FetchEvent) {
        let terminal = matches!(event, FetchEvent::End | FetchEvent::Failed(_));
        let mut sink = self.0.lock().expect("fetch events");
        if let Some(callback) = sink.as_mut() {
            callback(event);
        }
        if terminal {
            sink.take();
        }
    }
    fn clear(&self) {
        self.0.lock().expect("fetch events").take();
    }
}

/// Native C transport interface; matching declarations are in include/valdi_nsh_transport.h.
pub mod ffi;

#[cfg(any(target_os = "android", target_os = "ios", test))]
mod registry;
