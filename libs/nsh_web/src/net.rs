//! Transport types are shared verbatim with the independently owned transport.
pub use valdi_nsh_transport::{FetchEvent, FetchOp, HttpBody, HttpHead, HttpRequest};
pub type RequestId = u32;
#[derive(Debug, Default)]
pub struct NetworkQueue {
    pub ops: Vec<(RequestId, FetchOp)>,
}
