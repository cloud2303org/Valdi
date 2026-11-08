//! NSH's native Web API algorithms, hosted through Valdi's C ABI.
#![allow(dead_code)] // Upstream declaration metadata and reusable support APIs.
pub mod engine;
mod ffi;
pub mod net;
mod support;
mod transport;
mod web;
use engine::{Engine, Trace, Value};
pub use ffi::*;
use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

#[macro_export]
macro_rules! decl {
    ($s:expr) => {
        $s
    };
}

pub struct NshWebContext {
    engine: Engine,
    web: web::Web,
    shared: Rc<host::Shared>,
    owned: RefCell<BTreeMap<Value, usize>>,
    natives: RefCell<BTreeMap<usize, Trace>>,
    sockets: RefCell<transport::Sockets>,
}
impl NshWebContext {
    fn track(&self, v: Value) {
        *self.owned.borrow_mut().entry(v).or_default() += 1;
    }
    fn untrack(&self, v: Value) {
        let mut m = self.owned.borrow_mut();
        if let Some(n) = m.get_mut(&v) {
            *n -= 1;
            if *n == 0 {
                m.remove(&v);
            }
        }
    }
}
mod state {
    pub(crate) fn state(cx: *mut crate::NshWebContext) -> *mut crate::NshWebContext {
        cx
    }
}
mod host {
    #[derive(Default)]
    pub(crate) struct Shared {
        pub(crate) net: std::cell::RefCell<crate::net::NetworkQueue>,
        pub(crate) dropped_fetches: std::cell::RefCell<Vec<crate::net::RequestId>>,
    }
}
mod dom {
    pub(crate) fn shared(cx: *mut crate::NshWebContext) -> std::rc::Rc<crate::host::Shared> {
        unsafe { (*cx).shared.clone() }
    }
}
mod standard {
    pub(crate) mod websocket {
        pub(crate) use crate::transport::connect;
    }
}
#[cfg(test)]
mod engine_tests;
