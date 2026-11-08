//! Byte-based WebSocket bridge. The Web API state machine remains upstream's.
use std::collections::BTreeMap;
// NSH standard/websocket{,_native}.rs limits (outside the Web class module).
const MAX_MESSAGE: usize = 8 * 1024 * 1024;
const OUTBOUND_QUEUE: usize = 8;
use crate::web::{
    self,
    js::{self, Ctx, R, UNDEFINED, V},
};
#[derive(Default)]
pub(crate) struct Sockets {
    next: u32,
    pub(crate) active: BTreeMap<u32, Socket>,
    pub(crate) ops: Vec<SocketOp>,
}
pub(crate) struct Socket {
    pub(crate) callback: V,
    pub(crate) buffered: u64,
}
pub(crate) struct SocketOp {
    pub(crate) id: u32,
    pub(crate) op: u32,
    pub(crate) bytes: Vec<u8>,
    pub(crate) protocols: Vec<String>,
    pub(crate) code: u16,
}
fn push(cx: Ctx, id: u32, op: u32, bytes: Vec<u8>, code: u16) {
    unsafe {
        (*cx).sockets.borrow_mut().ops.push(SocketOp {
            id,
            op,
            bytes,
            protocols: Vec::new(),
            code,
        });
    }
}
pub(crate) fn connect(cx: Ctx, url: String, protocols: Vec<String>, callback: V) -> R<V> {
    let parsed =
        url::Url::parse(&url).map_err(|_| js::syntax_error(cx, "Invalid WebSocket URL"))?;
    if !matches!(parsed.scheme(), "ws" | "wss")
        || parsed.fragment().is_some()
        || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return Err(js::syntax_error(
            cx,
            "WebSocket requires a ws/wss URL without credentials or fragment",
        ));
    }
    let id = {
        let mut s = unsafe { (*cx).sockets.borrow_mut() };
        s.next = s
            .next
            .checked_add(1)
            .ok_or_else(|| js::range_error(cx, "WebSocket ID space exhausted"))?;
        s.next
    };
    let handle = js::new_object(cx)?;
    let send = js::closure(cx, Vec::new(), move |cx, _, args| {
        let data = args.get(0);
        let (op, bytes) = if data.tag == 4 {
            (2, js::to_string(cx, data)?.into_bytes())
        } else if let Some(blob) = web::blob::blob_of(cx, data)? {
            if blob.bytes().len() > MAX_MESSAGE {
                return Err(js::range_error(cx, "WebSocket message exceeds 8 MiB"));
            }
            (3, blob.bytes().to_vec())
        } else {
            let bytes = unsafe { js::bytes_of(cx, data) }.ok_or_else(|| {
                js::type_error(cx, "send expects text, Blob, ArrayBuffer or a typed array")
            })?;
            if bytes.len() > MAX_MESSAGE {
                return Err(js::range_error(cx, "WebSocket message exceeds 8 MiB"));
            }
            (3, bytes.to_vec())
        };
        if bytes.len() > MAX_MESSAGE {
            return Err(js::range_error(cx, "WebSocket message exceeds 8 MiB"));
        }
        {
            let mut s = unsafe { (*cx).sockets.borrow_mut() };
            if s.ops
                .iter()
                .filter(|o| o.id == id && (o.op == 2 || o.op == 3))
                .count()
                >= OUTBOUND_QUEUE
            {
                return Err(js::range_error(cx, "WebSocket outbound queue is full"));
            }
            if let Some(entry) = s.active.get_mut(&id) {
                if entry.buffered.saturating_add(bytes.len() as u64) > MAX_MESSAGE as u64 {
                    return Err(js::range_error(cx, "WebSocket buffered data exceeds 8 MiB"));
                }
                entry.buffered += bytes.len() as u64;
            } else {
                return Ok(V::undefined());
            }
        }
        push(cx, id, op, bytes, 0);
        Ok(V::undefined())
    })?;
    let close = js::closure(cx, Vec::new(), move |cx, _, args| {
        let code = js::to_number(cx, args.get(0))? as u16;
        let reason = js::to_string(cx, args.get(1))?;
        push(cx, id, 4, reason.into_bytes(), code);
        Ok(V::undefined())
    })?;
    let amount = js::closure(cx, Vec::new(), move |cx, _, _| {
        let n = unsafe { (*cx).sockets.borrow() }
            .active
            .get(&id)
            .map(|s| s.buffered)
            .unwrap_or(0);
        Ok(V::num(n as f64))
    })?;
    js::define(cx, handle.raw(), "send", send, false)?;
    js::define(cx, handle.raw(), "close", close, false)?;
    js::define(cx, handle.raw(), "bufferedAmount", amount, false)?;
    let mut s = unsafe { (*cx).sockets.borrow_mut() };
    s.active.insert(
        id,
        Socket {
            callback,
            buffered: 0,
        },
    );
    s.ops.push(SocketOp {
        id,
        op: 1,
        bytes: url.into_bytes(),
        protocols,
        code: 0,
    });
    Ok(handle)
}
pub(crate) fn event(cx: Ctx, id: u32, event: u32, bytes: &[u8], code: u16, clean: bool) -> R<()> {
    // Snapshot the callback before invoking JS (listeners may re-enter).
    let callback = unsafe { (*cx).sockets.borrow() }
        .active
        .get(&id)
        .map(|s| s.callback.clone_ref());
    let Some(callback) = callback else {
        return Ok(());
    };
    let (kind, data) = match event {
        1 => {
            let o = js::new_object(cx)?;
            js::define(
                cx,
                o.raw(),
                "protocol",
                js::string(cx, &String::from_utf8_lossy(bytes))?,
                true,
            )?;
            ("open", o)
        }
        2 => ("message", js::string(cx, &String::from_utf8_lossy(bytes))?),
        3 => ("message", js::array_buffer(cx, bytes)?),
        4 => {
            let o = js::new_object(cx)?;
            js::define(cx, o.raw(), "code", V::num(code as f64), true)?;
            js::define(
                cx,
                o.raw(),
                "reason",
                js::string(cx, &String::from_utf8_lossy(bytes))?,
                true,
            )?;
            js::define(cx, o.raw(), "wasClean", V::bool(clean), true)?;
            let gone = unsafe { (*cx).sockets.borrow_mut() }.active.remove(&id);
            drop(gone);
            ("close", o)
        }
        5 => ("error", js::string(cx, &String::from_utf8_lossy(bytes))?),
        _ => return Err(js::type_error(cx, "Unknown WebSocket event")),
    };
    let kind = js::string(cx, kind)?;
    js::call(cx, callback.raw(), UNDEFINED, &[kind.raw(), data.raw()])?;
    Ok(())
}

/// Prebuilt host-rooted binary payload, delivered through the same callback as
/// the legacy byte entry. The WebSocket class still implements binaryType.
pub(crate) fn binary_buffer(cx: Ctx, id: u32, buffer: js::Raw) -> R<()> {
    let callback = unsafe { (*cx).sockets.borrow() }
        .active
        .get(&id)
        .map(|s| s.callback.clone_ref());
    let Some(callback) = callback else {
        return Ok(());
    };
    let kind = js::string(cx, "message")?;
    js::call(cx, callback.raw(), UNDEFINED, &[kind.raw(), buffer])?;
    Ok(())
}
