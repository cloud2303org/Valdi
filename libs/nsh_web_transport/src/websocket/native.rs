//! NSH websocket_native.rs I/O loop without VM values or the VM scheduler.
use std::sync::{
    Arc, OnceLock,
    atomic::{AtomicUsize, Ordering},
};
use std::time::Duration;

use super::*;
use futures_util::{SinkExt, StreamExt};
use tokio::sync::{mpsc, watch};
use tokio_tungstenite::tungstenite::{
    Message,
    client::IntoClientRequest,
    protocol::{CloseFrame, WebSocketConfig, frame::coding::CloseCode},
};

const MAX_MESSAGE: usize = 8 * 1024 * 1024;
const QUEUE: usize = 8;
const MESSAGES_PER_TURN: usize = 32;

fn runtime() -> Result<&'static tokio::runtime::Runtime, String> {
    static RUNTIME: OnceLock<Result<tokio::runtime::Runtime, String>> = OnceLock::new();
    RUNTIME
        .get_or_init(|| {
            tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .thread_name("valdi-nsh-ws")
                .enable_all()
                .build()
                .map_err(|e| e.to_string())
        })
        .as_ref()
        .map_err(Clone::clone)
}

pub(super) fn transport() -> Arc<dyn WebSocketTransport> {
    Arc::new(Native { wake: None })
}
pub(super) fn transport_with_wake(
    wake: Arc<dyn Fn() + Send + Sync>,
) -> Arc<dyn WebSocketTransport> {
    Arc::new(Native { wake: Some(wake) })
}
struct Native {
    wake: Option<Arc<dyn Fn() + Send + Sync>>,
}
struct EventSender {
    sender: mpsc::Sender<WebSocketEvent>,
    wake: Option<Arc<dyn Fn() + Send + Sync>>,
}
impl EventSender {
    async fn send(&self, event: WebSocketEvent) -> Result<(), &'static str> {
        self.sender
            .send(event)
            .await
            .map_err(|_| "event receiver dropped")?;
        if let Some(wake) = &self.wake {
            wake();
        }
        Ok(())
    }
}
enum Command {
    Send(Message),
    Close(Option<CloseFrame>),
}
struct Task {
    commands: mpsc::Sender<Command>,
    paused: watch::Sender<bool>,
    buffered: Arc<AtomicUsize>,
    incoming: mpsc::Receiver<WebSocketEvent>,
    events: Option<WebSocketEvents>,
    job: tokio::task::JoinHandle<()>,
    closing: bool,
}
impl Drop for Task {
    fn drop(&mut self) {
        // Cancellation is independent of both bounded queues and socket state.
        self.job.abort();
        self.events.take();
    }
}
impl WebSocketTask for Task {
    fn send(&mut self, data: WebSocketData) -> Result<(), String> {
        if self.closing || self.job.is_finished() {
            return Ok(());
        }
        let length = data.len();
        if length > MAX_MESSAGE {
            return Err("WebSocket message exceeds 8 MiB".into());
        }
        self.buffered
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| {
                n.checked_add(length).filter(|n| *n <= MAX_MESSAGE)
            })
            .map_err(|_| "WebSocket buffered data exceeds 8 MiB")?;
        let message = match data {
            WebSocketData::Text(t) => Message::Text(t.into()),
            WebSocketData::Bytes(b) => Message::Binary(b.into()),
        };
        match self.commands.try_send(Command::Send(message)) {
            Ok(()) => Ok(()),
            Err(error) => {
                self.buffered.fetch_sub(length, Ordering::AcqRel);
                match error {
                    mpsc::error::TrySendError::Full(_) => {
                        Err("WebSocket outbound queue is full".into())
                    }
                    mpsc::error::TrySendError::Closed(_) => Ok(()),
                }
            }
        }
    }
    fn close(&mut self, code: u16, reason: &str) -> Result<(), String> {
        if self.closing {
            return Ok(());
        }
        if reason.len() > 123 {
            return Err("WebSocket close reason exceeds 123 bytes".into());
        }
        match self.commands.try_send(Command::Close(Some(CloseFrame {
            code: CloseCode::from(code),
            reason: reason.to_owned().into(),
        }))) {
            Ok(()) => {
                self.closing = true;
                Ok(())
            }
            Err(mpsc::error::TrySendError::Closed(_)) => Ok(()),
            Err(_) => Err("WebSocket outbound queue is full".into()),
        }
    }
    fn buffered_amount(&self) -> usize {
        if self.job.is_finished() {
            0
        } else {
            self.buffered.load(Ordering::Acquire)
        }
    }
    fn pause(&mut self, paused: bool) {
        self.paused.send_replace(paused);
    }
    fn dispatch(&mut self) -> usize {
        let mut delivered = 0;
        while delivered < MESSAGES_PER_TURN {
            let event = match self.incoming.try_recv().ok() {
                Some(event) => event,
                None => break,
            };
            // Already queued messages remain deliverable while paused. Draining
            // them lets close/abort progress even when the native queue was full.

            let terminal = matches!(event, WebSocketEvent::Close { .. });
            if let Some(events) = self.events.as_mut() {
                events(event);
                delivered += 1;
            }
            if terminal {
                self.events.take();
                break;
            }
        }
        delivered
    }
}
impl WebSocketTransport for Native {
    fn connect(
        &self,
        request: WebSocketRequest,
        events: WebSocketEvents,
    ) -> Result<Box<dyn WebSocketTask>, String> {
        let parsed = url::Url::parse(&request.url).map_err(|e| e.to_string())?;
        if !matches!(parsed.scheme(), "ws" | "wss")
            || parsed.fragment().is_some()
            || !parsed.username().is_empty()
            || parsed.password().is_some()
        {
            return Err("WebSocket requires a ws/wss URL without credentials or fragment".into());
        }
        let mut handshake = parsed
            .as_str()
            .into_client_request()
            .map_err(|e| e.to_string())?;
        for (name, value) in request.headers {
            let name = tokio_tungstenite::tungstenite::http::header::HeaderName::from_bytes(
                name.as_bytes(),
            )
            .map_err(|e| e.to_string())?;
            handshake.headers_mut().append(
                name,
                value.parse().map_err(
                    |e: tokio_tungstenite::tungstenite::http::header::InvalidHeaderValue| {
                        e.to_string()
                    },
                )?,
            );
        }
        if !request.protocols.is_empty() {
            handshake.headers_mut().insert(
                "Sec-WebSocket-Protocol",
                request.protocols.join(", ").parse().map_err(
                    |e: tokio_tungstenite::tungstenite::http::header::InvalidHeaderValue| {
                        e.to_string()
                    },
                )?,
            );
        }
        let (commands, rx) = mpsc::channel(QUEUE);
        let (sender, incoming) = mpsc::channel(QUEUE);
        let outgoing = EventSender {
            sender,
            wake: self.wake.clone(),
        };
        let (paused, pause_rx) = watch::channel(false);
        let buffered = Arc::new(AtomicUsize::new(0));
        let worker_buffered = buffered.clone();
        let job = runtime()?.spawn(async move {
            let result = run(
                handshake,
                request.protocols,
                rx,
                pause_rx,
                &outgoing,
                &worker_buffered,
            )
            .await;
            if let Err(error) = result {
                let _ = outgoing.send(WebSocketEvent::Error(error)).await;
                let _ = outgoing.send(closed(None, false)).await;
            }
        });
        Ok(Box::new(Task {
            commands,
            paused,
            buffered,
            incoming,
            events: Some(events),
            job,
            closing: false,
        }))
    }
}
async fn run(
    request: tokio_tungstenite::tungstenite::handshake::client::Request,
    protocols: Vec<String>,
    mut rx: mpsc::Receiver<Command>,
    mut paused: watch::Receiver<bool>,
    events: &EventSender,
    buffered: &AtomicUsize,
) -> Result<(), String> {
    let config = WebSocketConfig::default()
        .max_message_size(Some(MAX_MESSAGE))
        .max_frame_size(Some(MAX_MESSAGE));
    let handshake = tokio::time::timeout(
        Duration::from_secs(30),
        tokio_tungstenite::connect_async_with_config(request, Some(config), false),
    );
    let (mut socket, response) = tokio::select! {
        result = handshake => result.map_err(|_| "WebSocket handshake timed out")?.map_err(|e|e.to_string())?,
        command = rx.recv() => {
            if let Some(Command::Close(_)) = command { return Err("WebSocket closed while connecting".into()); }
            return Err("WebSocket send before handshake completed".into());
        }
    };
    let protocol = response
        .headers()
        .get("Sec-WebSocket-Protocol")
        .and_then(|h| h.to_str().ok())
        .unwrap_or("");
    if !protocol.is_empty() && !protocols.iter().any(|p| p == protocol) {
        return Err("Server selected an unrequested protocol".into());
    }
    events
        .send(WebSocketEvent::Open {
            protocol: protocol.into(),
        })
        .await
        .map_err(|_| "event receiver dropped")?;
    let mut delivered = 0;
    loop {
        if delivered >= MESSAGES_PER_TURN {
            delivered = 0;
            tokio::task::yield_now().await;
        }
        let reading = !*paused.borrow();
        tokio::select! {
            _ = paused.changed() => {},
            command = rx.recv() => match command {
                Some(Command::Send(message)) => {
                    let length = message.len();
                    tokio::time::timeout(Duration::from_secs(30),socket.send(message)).await.map_err(|_|"WebSocket write timed out")?.map_err(|e|e.to_string())?;
                    buffered.fetch_sub(length,Ordering::AcqRel);
                    if let Some(wake)=&events.wake {wake();}
                },
                Some(Command::Close(frame)) => {
                    tokio::time::timeout(Duration::from_secs(5),socket.close(frame)).await.map_err(|_|"WebSocket close timed out")?.map_err(|e|e.to_string())?;
                    let frame = tokio::time::timeout(Duration::from_secs(5),async {
                        while let Some(next)=socket.next().await {
                            if let Message::Close(frame)=next.map_err(|e|e.to_string())? { return Ok(frame); }
                        }
                        Err::<Option<CloseFrame>,String>("Peer disconnected before completing the WebSocket close handshake".into())
                    }).await.map_err(|_|"WebSocket close timed out")??;
                    let _=events.send(closed(frame,true)).await;
                    return Ok(());
                },
                None => return Ok(()),
            },
            incoming = socket.next(), if reading => match incoming {
                Some(Ok(Message::Text(text))) => { delivered+=1; events.send(WebSocketEvent::Message(WebSocketData::Text(text.to_string()))).await.map_err(|_|"event receiver dropped")?; },
                Some(Ok(Message::Binary(bytes))) => { delivered+=1; events.send(WebSocketEvent::Message(WebSocketData::Bytes(bytes.to_vec()))).await.map_err(|_|"event receiver dropped")?; },
                Some(Ok(Message::Close(frame))) => {
                    tokio::time::timeout(Duration::from_secs(5),socket.flush()).await.map_err(|_|"WebSocket close timed out")?.map_err(|e|e.to_string())?;
                    let _=events.send(closed(frame,true)).await; return Ok(());
                },
                Some(Ok(Message::Ping(_))) => { tokio::time::timeout(Duration::from_secs(30),socket.flush()).await.map_err(|_|"WebSocket write timed out")?.map_err(|e|e.to_string())?; },
                Some(Ok(_)) => {},
                Some(Err(error)) => return Err(error.to_string()),
                None => return Err("WebSocket connection ended without a close frame".into()),
            }
        }
    }
}
fn closed(frame: Option<CloseFrame>, clean: bool) -> WebSocketEvent {
    WebSocketEvent::Close {
        code: frame
            .as_ref()
            .map(|f| u16::from(f.code))
            .unwrap_or(if clean { 1005 } else { 1006 }),
        reason: frame.map(|f| f.reason.to_string()).unwrap_or_default(),
        was_clean: clean,
    }
}
