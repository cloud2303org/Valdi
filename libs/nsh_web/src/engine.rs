//! Engine-neutral ABI primitives. No engine's representation or symbols are used.
use crate::web::js::{Ctx, Native, Raw};
use std::ffi::{c_char, c_int, c_void};
pub const UINT8: i32 = 1;
pub const ITERATOR: u32 = 1;
pub const ASYNC_ITERATOR: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Value {
    pub tag: u32,
    pub flags: u32,
    pub payload: u64,
}
impl Value {
    pub const UNDEFINED: Self = Self {
        tag: 0,
        flags: 0,
        payload: 0,
    };
    pub const NULL: Self = Self {
        tag: 1,
        flags: 0,
        payload: 0,
    };
    pub const EXCEPTION: Self = Self {
        tag: 8,
        flags: 0,
        payload: 0,
    };
    pub const fn boolean(v: bool) -> Self {
        Self {
            tag: 2,
            flags: 0,
            payload: v as u64,
        }
    }
    pub fn number(n: f64) -> Self {
        Self {
            tag: 3,
            flags: 0,
            payload: n.to_bits(),
        }
    }
    pub const fn heap(self) -> bool {
        self.tag >= 4 && self.tag <= 7
    }
    pub fn as_number(self) -> f64 {
        f64::from_bits(self.payload)
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Bytes {
    pub data: *const u8,
    pub len: usize,
}
impl Default for Bytes {
    fn default() -> Self {
        Self::from(&[][..])
    }
}
impl<'a> From<&'a [u8]> for Bytes {
    fn from(b: &'a [u8]) -> Self {
        Self {
            data: b.as_ptr(),
            len: b.len(),
        }
    }
}
impl Bytes {
    /// The caller guarantees valid bytes for the duration of the returned borrow.
    pub unsafe fn slice<'a>(self) -> &'a [u8] {
        if self.len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.data, self.len) }
        }
    }
}
pub type Visit = unsafe extern "C" fn(*mut c_void, Raw);
pub type Finalize = unsafe extern "C" fn(Ctx, *mut c_void);
pub type Trace = unsafe extern "C" fn(*mut c_void, Visit, *mut c_void) -> i32;
#[repr(C)]
pub struct Method {
    pub name: *const c_char,
    pub symbol: u32,
    pub call: Native,
    pub length: c_int,
}
#[repr(C)]
pub struct Accessor {
    pub name: *const c_char,
    pub get: Native,
    pub set: Option<Native>,
}
#[repr(C)]
pub struct Constant {
    pub name: *const c_char,
    pub value: f64,
}
#[repr(C)]
pub struct ClassSpec {
    pub id: u32,
    pub name: *const c_char,
    pub parent_id: i32,
    pub constructor: Option<Native>,
    pub length: i32,
    pub methods: *const Method,
    pub methods_len: usize,
    pub accessors: *const Accessor,
    pub accessors_len: usize,
    pub statics: *const Method,
    pub statics_len: usize,
    pub constants: *const Constant,
    pub constants_len: usize,
    pub global: u32,
}
#[repr(C)]
pub struct View {
    pub buffer: Raw,
    pub offset: usize,
    pub length: usize,
    pub element: usize,
    pub kind: i32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Engine {
    pub abi_version: u32,
    pub struct_size: usize,
    pub user: *mut c_void,
    pub operate:
        unsafe extern "C" fn(*mut c_void, Ctx, u32, Raw, *const Raw, usize, Bytes, i64) -> Raw,
    pub retain: unsafe extern "C" fn(*mut c_void, Raw),
    pub release: unsafe extern "C" fn(*mut c_void, Raw),
    pub bytes: unsafe extern "C" fn(*mut c_void, Ctx, Raw, u32, *mut Bytes) -> i32,
    pub view: unsafe extern "C" fn(*mut c_void, Ctx, Raw, *mut View) -> i32,
    pub register_class: unsafe extern "C" fn(
        *mut c_void,
        Ctx,
        *const ClassSpec,
        Raw,
        Raw,
        *mut Raw,
        *mut Raw,
    ) -> i32,
    pub native_new: unsafe extern "C" fn(
        *mut c_void,
        Ctx,
        Raw,
        u32,
        *mut c_void,
        Finalize,
        Trace,
        Option<Native>,
    ) -> Raw,
    pub native_get: unsafe extern "C" fn(*mut c_void, Ctx, Raw, u32) -> *mut c_void,
}
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum Op {
    Global = 1,
    StringNew,
    ToWtf8,
    ToNumber,
    ToBool,
    IsFunction,
    Get,
    GetSymbol,
    Define,
    Call,
    Construct,
    ObjectNew,
    ArrayNew,
    Keys,
    FunctionNew,
    PromiseNew,
    PromiseThen,
    PromiseHandled,
    PromiseState,
    IsPromise,
    Throw,
    TakeException,
    Error,
    Report,
    JsonParse,
    ArrayBufferNew,
    Uint8ArrayNew,
    ViewNew,
    Detach,
    IsDetached,
    SetPrototype,
    GetPrototype,
    IsUncatchable,
    ArrayBufferTransfer,
}
pub(crate) fn op(cx: Ctx, op: Op, receiver: Raw, args: &[Raw], text: &[u8], aux: i64) -> Raw {
    // Copy the vtable before entering JS; never borrow context state across it.
    let e = unsafe { (*cx).engine };
    unsafe {
        (e.operate)(
            e.user,
            cx,
            op as u32,
            receiver,
            args.as_ptr(),
            args.len(),
            text.into(),
            aux,
        )
    }
}
pub(crate) fn parse_json(cx: Ctx, text: &[u8]) -> Raw {
    op(cx, Op::JsonParse, Value::UNDEFINED, &[], text, 0)
}
pub(crate) fn retain(cx: Ctx, v: Raw) {
    if v.heap() {
        let e = unsafe { (*cx).engine };
        unsafe { (e.retain)(e.user, v) }
    }
}
pub(crate) fn release(cx: Ctx, v: Raw) {
    if v.heap() {
        let e = unsafe { (*cx).engine };
        unsafe { (e.release)(e.user, v) }
    }
}
pub(crate) fn native_get(cx: Ctx, v: Raw, kind: u32) -> *mut c_void {
    let e = unsafe { (*cx).engine };
    unsafe { (e.native_get)(e.user, cx, v, kind) }
}
