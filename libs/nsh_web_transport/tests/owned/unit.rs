//! Private transport tests: original allocation identity, never a public test API.
use super::*;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static WATCH_FREE: Cell<(usize, usize)> = const { Cell::new((0, 0)) };
}
struct ObservedAllocator;
// Observe only the chosen allocation on the current test/release thread.
// Other allocations and parallel tests use System unchanged.
unsafe impl GlobalAlloc for ObservedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        let _ = WATCH_FREE.try_with(|watch| {
            let (address, count) = watch.get();
            if address != 0 && address == ptr as usize {
                watch.set((address, count + 1));
            }
        });
        unsafe { System.dealloc(ptr, layout) };
    }
}
#[global_allocator]
static ALLOCATOR: ObservedAllocator = ObservedAllocator;

struct Handle(*mut NshTransport);
impl Handle {
    fn new() -> Self {
        let callbacks = NshTransportCallbacks {
            abi_version: 1,
            struct_size: std::mem::size_of::<NshTransportCallbacks>(),
            user: std::ptr::null_mut(),
            wake: None,
            release_user: None,
        };
        let handle = unsafe { nsh_transport_create(&callbacks) };
        assert!(!handle.is_null());
        Self(handle)
    }
    fn shared(&self) -> &Arc<Shared> {
        unsafe { &(*self.0).shared }
    }
}
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { nsh_transport_destroy(self.0) };
    }
}

struct Delivery {
    id: u32,
    kind: u32,
    borrowed: NshTransportBytes,
    owned: NshTransportOwnedBytes,
    metadata: Vec<u8>,
}
impl Drop for Delivery {
    fn drop(&mut self) {
        if let Some(release) = self.owned.release.take() {
            unsafe { release(self.owned.owner) };
        }
    }
}
unsafe extern "C" fn http_visit(
    user: *mut c_void,
    event: *const NshTransportHttpEvent,
    owned: NshTransportOwnedBytes,
) {
    let event = unsafe { &*event };
    let deliveries = unsafe { &mut *(user as *mut Vec<Delivery>) };
    let metadata = if event.event == 4 && event.bytes.len != 0 {
        unsafe { std::slice::from_raw_parts(event.bytes.data, event.bytes.len) }.to_vec()
    } else {
        vec![]
    };
    deliveries.push(Delivery {
        id: event.id,
        kind: event.event,
        borrowed: event.bytes,
        owned,
        metadata,
    });
}
unsafe extern "C" fn ws_visit(
    user: *mut c_void,
    event: *const NshTransportSocketEvent,
    owned: NshTransportOwnedBytes,
) {
    let event = unsafe { &*event };
    let deliveries = unsafe { &mut *(user as *mut Vec<Delivery>) };
    let metadata = if !matches!(event.event, 2 | 3) && event.bytes.len != 0 {
        unsafe { std::slice::from_raw_parts(event.bytes.data, event.bytes.len) }.to_vec()
    } else {
        vec![]
    };
    deliveries.push(Delivery {
        id: event.id,
        kind: event.event,
        borrowed: event.bytes,
        owned,
        metadata,
    });
}
fn http_drain(h: &Handle) -> Vec<Delivery> {
    let mut deliveries = Vec::new();
    unsafe {
        nsh_transport_drain_http_owned(
            h.0,
            Some(http_visit),
            &mut deliveries as *mut _ as *mut c_void,
        )
    };
    deliveries
}
fn ws_drain(h: &Handle) -> Vec<Delivery> {
    let mut deliveries = Vec::new();
    unsafe {
        nsh_transport_drain_ws_owned(
            h.0,
            Some(ws_visit),
            &mut deliveries as *mut _ as *mut c_void,
        )
    };
    deliveries
}
fn assert_transfer(mut delivery: Delivery, original: usize, expected: &[u8]) {
    assert_eq!(
        delivery.owned.data as usize, original,
        "owned drain copied the original receive allocation"
    );
    assert_eq!(delivery.borrowed.data, delivery.owned.data.cast_const());
    assert_eq!(delivery.borrowed.len, expected.len());
    assert_eq!(delivery.owned.len, expected.len());
    assert!(!delivery.owned.owner.is_null());
    let release = delivery.owned.release.take().expect("payload release");
    let owner = delivery.owned.owner as usize;
    let data = delivery.owned.data as usize;
    let expected = expected.to_vec();
    // Handle is already destroyed. Move only the C token to another thread;
    // read and mutate the allocation there, then observe its one real free.
    std::thread::spawn(move || {
        WATCH_FREE.with(|watch| watch.set((original, 0)));
        let bytes = unsafe { std::slice::from_raw_parts_mut(data as *mut u8, expected.len()) };
        assert_eq!(bytes, expected);
        bytes.fill(0xa5);
        unsafe { release(owner as *mut c_void) };
        WATCH_FREE.with(|watch| {
            assert_eq!(
                watch.get().1,
                1,
                "release must free the original allocation once"
            );
            watch.set((0, 0));
        });
    })
    .join()
    .unwrap();
}

#[test]
fn owned_http_preserves_original_allocation_after_destroy_and_releases_once_elsewhere() {
    let h = Handle::new();
    let mut bytes = Vec::with_capacity(4096);
    bytes.extend_from_slice(b"HTTP original");
    let original = bytes.as_ptr() as usize;
    let generation = h.shared().begin_http(1);
    h.shared()
        .push_http(1, FetchEvent::Data(bytes), &generation);
    WATCH_FREE.with(|watch| watch.set((original, 0)));
    let mut deliveries = http_drain(&h);
    assert_eq!(deliveries.len(), 1);
    assert_eq!(
        deliveries[0].owned.data as usize, original,
        "HTTP payload allocation was copied"
    );
    drop(generation);
    drop(h);
    WATCH_FREE.with(|watch| {
        assert_eq!(watch.get().1, 0, "Rust freed handed-off bytes");
        watch.set((0, 0));
    });
    assert_transfer(deliveries.remove(0), original, b"HTTP original");
}

fn websocket_identity(text: bool) {
    let h = Handle::new();
    let mut bytes = Vec::with_capacity(4096);
    bytes.extend_from_slice("WS split €\0".as_bytes());
    let original = bytes.as_ptr() as usize;
    let data = if text {
        WebSocketData::Text(String::from_utf8(bytes).unwrap())
    } else {
        WebSocketData::Bytes(bytes)
    };
    h.shared()
        .push_ws(2, SocketEvent::Event(WebSocketEvent::Message(data)));
    WATCH_FREE.with(|watch| watch.set((original, 0)));
    let mut deliveries = ws_drain(&h);
    assert_eq!(deliveries.len(), 1);
    assert_eq!(
        deliveries[0].owned.data as usize, original,
        "WS payload allocation was copied"
    );
    assert_eq!(deliveries[0].kind, if text { 2 } else { 3 });
    drop(h);
    WATCH_FREE.with(|watch| {
        assert_eq!(watch.get().1, 0, "Rust freed handed-off bytes");
        watch.set((0, 0));
    });
    assert_transfer(deliveries.remove(0), original, "WS split €\0".as_bytes());
}
#[test]
fn owned_ws_text_moves_original_string_allocation() {
    websocket_identity(true);
}
#[test]
fn owned_ws_binary_moves_original_vec_allocation() {
    websocket_identity(false);
}

#[test]
fn owned_empty_payloads_with_reserved_storage_still_transfer_and_release() {
    for kind in 0..3 {
        let h = Handle::new();
        let bytes = Vec::with_capacity(64);
        let original = bytes.as_ptr() as usize;
        if kind == 0 {
            let generation = h.shared().begin_http(3);
            h.shared()
                .push_http(3, FetchEvent::Data(bytes), &generation);
        } else {
            let data = if kind == 1 {
                WebSocketData::Text(String::from_utf8(bytes).unwrap())
            } else {
                WebSocketData::Bytes(bytes)
            };
            h.shared()
                .push_ws(3, SocketEvent::Event(WebSocketEvent::Message(data)));
        }
        let mut deliveries = if kind == 0 {
            http_drain(&h)
        } else {
            ws_drain(&h)
        };
        assert_eq!(deliveries.len(), 1);
        drop(h);
        assert_transfer(deliveries.remove(0), original, b"");
    }
}

#[test]
fn owned_metadata_events_have_no_release_token_and_keep_borrowed_fields() {
    let h = Handle::new();
    let generation = h.shared().begin_http(4);
    for event in [
        FetchEvent::Head(crate::HttpHead::default()),
        FetchEvent::Failed("HTTP error".into()),
        FetchEvent::End,
        FetchEvent::BodyWanted,
    ] {
        h.shared().push_http(4, event, &generation);
    }
    for event in [
        SocketEvent::Event(WebSocketEvent::Open {
            protocol: "chat".into(),
        }),
        SocketEvent::Event(WebSocketEvent::Close {
            code: 1000,
            reason: "done".into(),
            was_clean: true,
        }),
        SocketEvent::Event(WebSocketEvent::Error("WS error".into())),
        SocketEvent::Buffered(42),
    ] {
        h.shared().push_ws(4, event);
    }
    let http = http_drain(&h);
    let ws = ws_drain(&h);
    assert_eq!(
        http.iter().map(|e| e.kind).collect::<Vec<_>>(),
        [1, 4, 3, 5]
    );
    assert_eq!(ws.iter().map(|e| e.kind).collect::<Vec<_>>(), [1, 4, 5, 6]);
    assert_eq!(http[1].metadata, b"HTTP error");
    assert_eq!(ws[0].metadata, b"chat");
    assert_eq!(ws[1].metadata, b"done");
    assert_eq!(ws[2].metadata, b"WS error");
    for delivery in http.iter().chain(&ws) {
        assert_eq!(delivery.id, 4);
        assert!(delivery.owned.data.is_null());
        assert_eq!(delivery.owned.len, 0);
        assert!(delivery.owned.owner.is_null());
        assert!(delivery.owned.release.is_none());
    }
}

#[test]
fn owned_null_visitors_keep_queues_and_stop_discards_unclaimed_payloads() {
    let h = Handle::new();
    let generation = h.shared().begin_http(5);
    h.shared()
        .push_http(5, FetchEvent::Data(vec![1]), &generation);
    h.shared().push_ws(
        5,
        SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Bytes(vec![2]))),
    );
    unsafe {
        nsh_transport_drain_http_owned(
            std::ptr::null_mut(),
            Some(http_visit),
            std::ptr::null_mut(),
        );
        nsh_transport_drain_ws_owned(std::ptr::null_mut(), Some(ws_visit), std::ptr::null_mut());
        nsh_transport_drain_http_owned(h.0, None, std::ptr::null_mut());
        nsh_transport_drain_ws_owned(h.0, None, std::ptr::null_mut());
    }
    assert_eq!(h.shared().http.lock().unwrap().entries.len(), 1);
    assert_eq!(h.shared().ws.lock().unwrap().entries.len(), 1);
    unsafe { nsh_transport_stop(h.0) };
    assert!(http_drain(&h).is_empty());
    assert!(ws_drain(&h).is_empty());
    assert!(h.shared().http.lock().unwrap().entries.is_empty());
    assert!(h.shared().ws.lock().unwrap().entries.is_empty());
}

struct StopOnFirst {
    handle: *mut NshTransport,
    visits: usize,
}
unsafe extern "C" fn stop_http(
    user: *mut c_void,
    _: *const NshTransportHttpEvent,
    owned: NshTransportOwnedBytes,
) {
    let state = unsafe { &mut *(user as *mut StopOnFirst) };
    state.visits += 1;
    if let Some(release) = owned.release {
        unsafe { release(owned.owner) };
    }
    unsafe { nsh_transport_stop(state.handle) };
}
unsafe extern "C" fn stop_ws(
    user: *mut c_void,
    _: *const NshTransportSocketEvent,
    owned: NshTransportOwnedBytes,
) {
    let state = unsafe { &mut *(user as *mut StopOnFirst) };
    state.visits += 1;
    if let Some(release) = owned.release {
        unsafe { release(owned.owner) };
    }
    unsafe { nsh_transport_stop(state.handle) };
}
#[test]
fn owned_stop_inside_visitor_discards_remaining_snapshot() {
    for http in [true, false] {
        let h = Handle::new();
        let generation = h.shared().begin_http(6);
        for byte in 0..3 {
            if http {
                h.shared()
                    .push_http(6, FetchEvent::Data(vec![byte]), &generation);
            } else {
                h.shared().push_ws(
                    6,
                    SocketEvent::Event(WebSocketEvent::Message(WebSocketData::Bytes(vec![byte]))),
                );
            }
        }
        let mut state = StopOnFirst {
            handle: h.0,
            visits: 0,
        };
        unsafe {
            if http {
                nsh_transport_drain_http_owned(
                    h.0,
                    Some(stop_http),
                    &mut state as *mut _ as *mut c_void,
                );
            } else {
                nsh_transport_drain_ws_owned(
                    h.0,
                    Some(stop_ws),
                    &mut state as *mut _ as *mut c_void,
                );
            }
        }
        assert_eq!(state.visits, 1);
    }
}

struct ImmediateRelease {
    original: usize,
    exclusive_bytes_match: bool,
    calls: usize,
}
unsafe fn mutate_and_release(
    user: *mut c_void,
    borrowed: NshTransportBytes,
    owned: NshTransportOwnedBytes,
) {
    let state = unsafe { &mut *user.cast::<ImmediateRelease>() };
    state.calls += 1;
    state.exclusive_bytes_match = owned.data as usize == state.original
        && owned.len == 3
        && borrowed.data == owned.data.cast_const()
        && borrowed.len == owned.len;
    if state.exclusive_bytes_match {
        // Mutation is legal while the callback is still running. Drop every
        // temporary slice before release; the event itself holds only raw data.
        unsafe { std::slice::from_raw_parts_mut(owned.data, owned.len) }.fill(0xff);
        state.exclusive_bytes_match =
            unsafe { std::slice::from_raw_parts(borrowed.data, borrowed.len) } == [0xff; 3];
    }
    if let Some(release) = owned.release {
        unsafe { release(owned.owner) };
    }
}
unsafe extern "C" fn immediate_http(
    user: *mut c_void,
    event: *const NshTransportHttpEvent,
    owned: NshTransportOwnedBytes,
) {
    unsafe { mutate_and_release(user, (*event).bytes, owned) };
}
unsafe extern "C" fn immediate_ws(
    user: *mut c_void,
    event: *const NshTransportSocketEvent,
    owned: NshTransportOwnedBytes,
) {
    unsafe { mutate_and_release(user, (*event).bytes, owned) };
}
#[test]
fn owned_payload_is_exclusive_and_can_be_mutated_and_released_inside_callback() {
    for kind in 0..3 {
        let h = Handle::new();
        let bytes = b"abc".to_vec();
        let original = bytes.as_ptr() as usize;
        if kind == 0 {
            let generation = h.shared().begin_http(7);
            h.shared()
                .push_http(7, FetchEvent::Data(bytes), &generation);
        } else {
            let data = if kind == 1 {
                WebSocketData::Text(String::from_utf8(bytes).unwrap())
            } else {
                WebSocketData::Bytes(bytes)
            };
            h.shared()
                .push_ws(7, SocketEvent::Event(WebSocketEvent::Message(data)));
        }
        let mut state = ImmediateRelease {
            original,
            exclusive_bytes_match: false,
            calls: 0,
        };
        WATCH_FREE.with(|watch| watch.set((original, 0)));
        unsafe {
            if kind == 0 {
                nsh_transport_drain_http_owned(
                    h.0,
                    Some(immediate_http),
                    &mut state as *mut _ as *mut c_void,
                );
            } else {
                nsh_transport_drain_ws_owned(
                    h.0,
                    Some(immediate_ws),
                    &mut state as *mut _ as *mut c_void,
                );
            }
        }
        drop(h);
        let freed = WATCH_FREE.with(|watch| {
            let count = watch.get().1;
            watch.set((0, 0));
            count
        });
        assert_eq!(state.calls, 1);
        assert!(
            state.exclusive_bytes_match,
            "callback did not own original writable bytes"
        );
        assert_eq!(
            freed, 1,
            "drain/destroy must not free already released bytes again"
        );
    }
}
