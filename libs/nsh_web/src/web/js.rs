//! The toolkit the web APIs are written with: owned values, errors as
//! `Result`, natives, closures, promises and byte access over the Valdi engine-neutral C ABI.
//!
//! Rules: a [`V`] owns one reference and frees it when dropped (through the
//! runtime, so values held by objects are freed by their finalizers); a
//! native returns `R<V>`, `Err(Throw)` meaning a JS exception is pending.

use crate::engine::{self, Op};
use std::{
    ffi::{CStr, c_int},
    ptr,
};
pub(crate) type Ctx = *mut crate::NshWebContext;
pub(crate) type Raw = engine::Value;
pub(crate) type Native = unsafe extern "C" fn(Ctx, Raw, c_int, *mut Raw) -> Raw;
pub(crate) const UNDEFINED: Raw = Raw::UNDEFINED;
pub(crate) const NULL: Raw = Raw::NULL;
#[derive(Debug, Clone, Copy)]
pub(crate) struct Throw;
pub(crate) type R<T> = Result<T, Throw>;

/// Owns one host handle and one entry in the Rust ownership census. Handle
/// references preserve ID lifetime; the census and Trace provide GC roots/edges.
pub(crate) struct V {
    cx: Ctx,
    raw: Raw,
}
impl Drop for V {
    fn drop(&mut self) {
        if self.raw.heap() {
            unsafe {
                (*self.cx).untrack(self.raw);
            }
            engine::release(self.cx, self.raw);
        }
    }
}
impl V {
    pub(crate) fn own(cx: Ctx, raw: Raw) -> V {
        if raw.heap() {
            unsafe {
                (*cx).track(raw);
            }
        }
        V { cx, raw }
    }
    pub(crate) fn dup(cx: Ctx, raw: Raw) -> V {
        engine::retain(cx, raw);
        Self::own(cx, raw)
    }
    pub(crate) fn checked(cx: Ctx, raw: Raw) -> R<V> {
        if raw.tag == 8 {
            Err(Throw)
        } else {
            Ok(Self::own(cx, raw))
        }
    }
    pub(crate) const fn undefined() -> V {
        V {
            cx: ptr::null_mut(),
            raw: UNDEFINED,
        }
    }
    pub(crate) const fn null() -> V {
        V {
            cx: ptr::null_mut(),
            raw: NULL,
        }
    }
    pub(crate) fn bool(b: bool) -> V {
        V {
            cx: ptr::null_mut(),
            raw: Raw::boolean(b),
        }
    }
    pub(crate) fn num(n: f64) -> V {
        V {
            cx: ptr::null_mut(),
            raw: Raw::number(n),
        }
    }
    pub(crate) fn raw(&self) -> Raw {
        self.raw
    }
    pub(crate) fn take(self) -> Raw {
        if self.raw.heap() {
            unsafe {
                (*self.cx).untrack(self.raw);
            }
        }
        let raw = self.raw;
        std::mem::forget(self);
        raw
    }
    pub(crate) fn clone_ref(&self) -> V {
        Self::dup(self.cx, self.raw)
    }
    pub(crate) fn take_out(&mut self) -> V {
        std::mem::take(self)
    }
    pub(crate) fn is_undefined(&self) -> bool {
        is_undefined(self.raw)
    }
    pub(crate) fn is_nullish(&self) -> bool {
        is_nullish(self.raw)
    }
}
impl Default for V {
    fn default() -> V {
        V::undefined()
    }
}
impl std::fmt::Debug for V {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.raw.fmt(f)
    }
}
fn operation(cx: Ctx, op: Op, receiver: Raw, args: &[Raw], text: &[u8], aux: i64) -> R<V> {
    V::checked(cx, engine::op(cx, op, receiver, args, text, aux))
}
/// What values an object owns, for the cycle collector (every owned [`V`]
/// must be reported exactly once; one forgotten only keeps cycles alive).
pub(crate) trait Trace {
    fn trace(&self, mark: &mut dyn FnMut(Raw));
}

impl Trace for V {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        mark(self.raw);
    }
}

/// Plain Rust data holds no values.
macro_rules! no_values {
    ($($ty:ty),*) => {$(
        impl Trace for $ty {
            fn trace(&self, _: &mut dyn FnMut(Raw)) {}
        }
    )*};
}

no_values!(String, bool, u8, u16, u32, u64, usize, i32, i64, f64);

impl<T: Trace> Trace for Option<T> {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        if let Some(t) = self {
            t.trace(mark);
        }
    }
}

impl<T: Trace> Trace for Vec<T> {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        for t in self {
            t.trace(mark);
        }
    }
}

impl<T: Trace> Trace for std::collections::VecDeque<T> {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        for t in self {
            t.trace(mark);
        }
    }
}

impl<A: Trace, B: Trace> Trace for (A, B) {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        self.0.trace(mark);
        self.1.trace(mark);
    }
}

impl<A: Trace, B: Trace, C: Trace> Trace for (A, B, C) {
    fn trace(&self, mark: &mut dyn FnMut(Raw)) {
        self.0.trace(mark);
        self.1.trace(mark);
        self.2.trace(mark);
    }
}

/// Implements [`Trace`] for a struct by its fields holding values.
macro_rules! trace {
    ($ty:ty { $($field:ident),* $(,)? }) => {
        impl $crate::web::js::Trace for $ty {
            #[allow(unused_variables)]
            fn trace(&self, mark: &mut dyn FnMut($crate::web::js::Raw)) {
                $($crate::web::js::Trace::trace(&self.$field, mark);)*
            }
        }
    };
}
pub(crate) use trace;

// --- Values ---------------------------------------------------------------

#[inline]
pub(crate) fn is_undefined(v: Raw) -> bool {
    v.tag == 0
}

#[inline]
pub(crate) fn is_nullish(v: Raw) -> bool {
    v.tag <= 1
}

#[inline]
pub(crate) fn is_object(v: Raw) -> bool {
    v.tag == 5
}

pub(crate) fn is_function(cx: Ctx, v: Raw) -> bool {
    engine::op(cx, Op::IsFunction, v, &[], &[], 0).payload == 1
}

pub(crate) fn same(a: Raw, b: Raw) -> bool {
    if a.tag == 3 && b.tag == 3 {
        let (a, b) = (a.as_number(), b.as_number());
        return (a.is_nan() && b.is_nan()) || (a == b && (a != 0.0 || a.to_bits() == b.to_bits()));
    }
    a == b
}

/// A new string.
pub(crate) fn string(cx: Ctx, s: &str) -> R<V> {
    operation(cx, Op::StringNew, UNDEFINED, &[], s.as_bytes(), 0)
}

/// `ToString(v)` as a USVString (throws for symbols): lone surrogates,
/// which QuickJS hands out as 3-byte WTF-8 sequences, become one U+FFFD
/// each.
pub(crate) fn to_string(cx: Ctx, v: Raw) -> R<String> {
    Ok(from_wtf8(&to_wtf8(cx, v)?))
}

/// `ToString(v)` as WTF-8: UTF-8 with lone surrogates as 3-byte
/// sequences (for code that pairs surrogates across strings).
pub(crate) fn to_wtf8(cx: Ctx, v: Raw) -> R<Vec<u8>> {
    let e = unsafe { (*cx).engine };
    let mut b = engine::Bytes::default();
    if unsafe { (e.bytes)(e.user, cx, v, 1, &mut b) } < 1 {
        return Err(Throw);
    }
    Ok(unsafe { b.slice() }.to_vec())
}

/// WTF-8 (UTF-8 with encoded surrogates) as UTF-8, a U+FFFD for each
/// surrogate.
pub(crate) fn from_wtf8(bytes: &[u8]) -> String {
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == 0xED
            && i + 2 < bytes.len()
            && (0xA0..=0xBF).contains(&bytes[i + 1])
            && (0x80..=0xBF).contains(&bytes[i + 2])
        {
            out.extend_from_slice("\u{FFFD}".as_bytes());
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// `ToString(v)` unless `v` is undefined (then `None`).
pub(crate) fn opt_string(cx: Ctx, v: Raw) -> R<Option<String>> {
    if is_undefined(v) {
        Ok(None)
    } else {
        to_string(cx, v).map(Some)
    }
}

/// `ToNumber(v)`.
pub(crate) fn to_number(cx: Ctx, v: Raw) -> R<f64> {
    operation(cx, Op::ToNumber, v, &[], &[], 0).map(|n| n.raw().as_number())
}

/// `ToBoolean(v)`.
pub(crate) fn to_bool(cx: Ctx, v: Raw) -> bool {
    engine::op(cx, Op::ToBool, v, &[], &[], 0).payload == 1
}

/// WebIDL `unsigned long long` (enforce range off): a non-negative integer.
pub(crate) fn to_index(cx: Ctx, v: Raw) -> R<u64> {
    let n = to_number(cx, v)?;
    Ok(if n.is_finite() && n > 0.0 {
        n.trunc() as u64
    } else {
        0
    })
}

pub(crate) fn new_object(cx: Ctx) -> R<V> {
    operation(cx, Op::ObjectNew, UNDEFINED, &[], &[], 0)
}

pub(crate) fn new_array(cx: Ctx, items: Vec<V>) -> R<V> {
    let raws: Vec<_> = items.iter().map(V::raw).collect();
    operation(cx, Op::ArrayNew, UNDEFINED, &raws, &[], 0)
}

pub(crate) fn global(cx: Ctx) -> V {
    V::own(cx, engine::op(cx, Op::Global, UNDEFINED, &[], &[], 0))
}

/// `obj[key]`.
pub(crate) fn get(cx: Ctx, obj: Raw, key: &str) -> R<V> {
    operation(cx, Op::Get, obj, &[], key.as_bytes(), 0)
}

/// `obj[atom]`.
pub(crate) fn get_atom(cx: Ctx, obj: Raw, atom: u32) -> R<V> {
    operation(cx, Op::GetSymbol, obj, &[], &[], atom as i64)
}

/// Defines data property `key` (writable, configurable; `enumerable`).
pub(crate) fn define(cx: Ctx, obj: Raw, key: &str, value: V, enumerable: bool) -> R<()> {
    define_flags(cx, obj, key, value, 5 | if enumerable { 2 } else { 0 })
}
pub(crate) fn define_flags(cx: Ctx, obj: Raw, key: &str, value: V, flags: i64) -> R<()> {
    operation(cx, Op::Define, obj, &[value.raw()], key.as_bytes(), flags).map(drop)
}

/// `{ value, done }`.
pub(crate) fn iter_result(cx: Ctx, value: V, done: bool) -> R<V> {
    let o = new_object(cx)?;
    define(cx, o.raw(), "value", value, true)?;
    define(cx, o.raw(), "done", V::bool(done), true)?;
    Ok(o)
}

/// `f.call(this, ...args)`.
pub(crate) fn call(cx: Ctx, f: Raw, this: Raw, args: &[Raw]) -> R<V> {
    let mut a = Vec::with_capacity(args.len() + 1);
    a.push(this);
    a.extend_from_slice(args);
    operation(cx, Op::Call, f, &a, &[], 0)
}

/// `obj[name](...args)`.
pub(crate) fn invoke(cx: Ctx, obj: Raw, name: &str, args: &[Raw]) -> R<V> {
    let f = get(cx, obj, name)?;
    call(cx, f.raw(), obj, args)
}

/// `new f(...args)`.
pub(crate) fn construct(cx: Ctx, f: Raw, args: &[Raw]) -> R<V> {
    operation(cx, Op::Construct, f, args, &[], 0)
}

/// `GetMethod(obj, name)`: the function, `None` when undefined or null;
/// throws when it is something else.
pub(crate) fn get_method(cx: Ctx, obj: Raw, name: &str) -> R<Option<V>> {
    let f = get(cx, obj, name)?;
    if f.is_nullish() {
        return Ok(None);
    }
    if !is_function(cx, f.raw()) {
        return Err(type_error(cx, &format!("{name} is not a function")));
    }
    Ok(Some(f))
}

// --- Errors ---------------------------------------------------------------

pub(crate) fn type_error(cx: Ctx, message: &str) -> Throw {
    engine::op(cx, Op::Error, UNDEFINED, &[], message.as_bytes(), 0);
    Throw
}

pub(crate) fn range_error(cx: Ctx, message: &str) -> Throw {
    engine::op(cx, Op::Error, UNDEFINED, &[], message.as_bytes(), 1);
    Throw
}

#[cfg_attr(target_arch = "wasm32", allow(dead_code))]
pub(crate) fn syntax_error(cx: Ctx, message: &str) -> Throw {
    engine::op(cx, Op::Error, UNDEFINED, &[], message.as_bytes(), 2);
    Throw
}

/// Throws `value`.
pub(crate) fn throw(cx: Ctx, value: V) -> Throw {
    engine::op(cx, Op::Throw, value.raw(), &[], &[], 0);
    Throw
}

/// The pending exception, cleared.
pub(crate) fn take_exception(cx: Ctx) -> V {
    V::own(
        cx,
        engine::op(cx, Op::TakeException, UNDEFINED, &[], &[], 0),
    )
}

/// A new `DOMException(message, name)`.
pub(crate) fn dom_exception(cx: Ctx, message: &str, name: &str) -> R<V> {
    let g = global(cx);
    let class = get(cx, g.raw(), "DOMException")?;
    let (m, n) = (string(cx, message)?, string(cx, name)?);
    construct(cx, class.raw(), &[m.raw(), n.raw()])
}

/// Throws `DOMException(message, name)`.
pub(crate) fn throw_dom(cx: Ctx, message: &str, name: &str) -> Throw {
    match dom_exception(cx, message, name) {
        Ok(e) => throw(cx, e),
        Err(t) => t,
    }
}

/// Reports `error` as uncaught without interrupting the caller (the
/// specs' "report an exception": a listener that threw, ...).
pub(crate) fn report(cx: Ctx, error: V) {
    let _ = operation(cx, Op::Report, error.raw(), &[], &[], 0);
}

/// Reports the pending exception.
pub(crate) fn report_exception(cx: Ctx) {
    report(cx, take_exception(cx));
}

/// A `TypeError(message)` value (not thrown).
pub(crate) fn type_error_value(cx: Ctx, message: &str) -> V {
    let _ = type_error(cx, message);
    take_exception(cx)
}

// --- Natives --------------------------------------------------------------

/// The arguments of a native call.
#[derive(Clone, Copy)]
pub(crate) struct Args<'a>(&'a [Raw]);

impl<'a> Args<'a> {
    /// # Safety
    ///
    /// `argv` holds `argc` live values for the call.
    pub(crate) unsafe fn new(argc: c_int, argv: *mut Raw) -> Args<'a> {
        if argc <= 0 || argv.is_null() {
            Args(&[])
        } else {
            // SAFETY: per the contract.
            Args(unsafe { std::slice::from_raw_parts(argv, argc as usize) })
        }
    }

    /// Argument `i` (undefined when missing).
    #[inline]
    pub(crate) fn get(&self, i: usize) -> Raw {
        self.0.get(i).copied().unwrap_or(UNDEFINED)
    }

    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }
}

/// Defines native `$name` from a body returning `R<V>`.
macro_rules! native {
    ($(#[$m:meta])* fn $name:ident($cx:ident, $this:ident, $args:ident) $body:block) => {
        $(#[$m])*
        pub(crate) unsafe extern "C" fn $name(
            cx: $crate::web::js::Ctx,
            this: $crate::web::js::Raw,
            argc: std::ffi::c_int,
            argv: *mut $crate::web::js::Raw,
        ) -> $crate::web::js::Raw {
            #[allow(unused_variables)]
            fn body(
                $cx: $crate::web::js::Ctx,
                $this: $crate::web::js::Raw,
                $args: $crate::web::js::Args<'_>,
            ) -> $crate::web::js::R<$crate::web::js::V> $body
            // SAFETY: The engine passes `argc` live arguments.
            let args = unsafe { $crate::web::js::Args::new(argc, argv) };
            match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(cx,this,args))) {
                Ok(Ok(v)) => v.take(),
                Ok(Err(_)) => $crate::engine::Value::EXCEPTION,
                Err(_) => { $crate::web::js::type_error(cx,"Rust native panicked"); $crate::engine::Value::EXCEPTION }
            }
        }
    };
}
pub(crate) use native;

/// A plain function `name`.
pub(crate) fn function(cx: Ctx, name: &CStr, f: Native, length: c_int) -> R<V> {
    super::new_function(cx, name, f, length)
}

// --- Closures -------------------------------------------------------------

/// A closure's code: its captured values and the call's arguments.
pub(crate) type ClosureFn = Box<dyn Fn(Ctx, &[V], Args<'_>) -> R<V>>;

/// The data of a closure object.
pub(crate) struct Closure {
    pub(crate) f: ClosureFn,
    /// JS values it uses (marked; Rust state goes in `f`'s captures).
    pub(crate) captures: Vec<V>,
}

trace!(Closure { captures });

/// A function running `f` with `captures`.
pub(crate) fn closure(
    cx: Ctx,
    captures: Vec<V>,
    f: impl Fn(Ctx, &[V], Args<'_>) -> R<V> + 'static,
) -> R<V> {
    super::new_closure(
        cx,
        Closure {
            f: Box::new(f),
            captures,
        },
    )
}

// --- Promises -------------------------------------------------------------

/// A new promise and its resolving functions.
pub(crate) struct Deferred {
    pub(crate) promise: V,
    pub(crate) resolve: V,
    pub(crate) reject: V,
    pending: std::rc::Rc<std::cell::Cell<bool>>,
}

trace!(Deferred {
    promise,
    resolve,
    reject
});

impl Deferred {
    pub(crate) fn new(cx: Ctx) -> R<Deferred> {
        let parts = operation(cx, Op::PromiseNew, UNDEFINED, &[], &[], 0)?;
        Ok(Deferred {
            promise: get(cx, parts.raw(), "0")?,
            resolve: get(cx, parts.raw(), "1")?,
            reject: get(cx, parts.raw(), "2")?,
            pending: std::rc::Rc::new(std::cell::Cell::new(true)),
        })
    }

    pub(crate) fn from_parts(promise: V, resolve: V, reject: V) -> Self {
        Self {
            promise,
            resolve,
            reject,
            pending: std::rc::Rc::new(std::cell::Cell::new(true)),
        }
    }
    pub(crate) fn clone_ref(&self) -> Self {
        Self {
            promise: self.promise.clone_ref(),
            resolve: self.resolve.clone_ref(),
            reject: self.reject.clone_ref(),
            pending: self.pending.clone(),
        }
    }

    /// Used only for ready/closed capabilities settled with undefined or rejection.
    pub(crate) fn is_pending(&self) -> bool {
        self.pending.get()
    }

    /// Resolves with `value` (later calls do nothing).
    pub(crate) fn resolve(&self, cx: Ctx, value: Raw) {
        self.pending.set(false);
        let _ = call(cx, self.resolve.raw(), UNDEFINED, &[value]);
    }

    /// Rejects with `reason`.
    pub(crate) fn reject(&self, cx: Ctx, reason: Raw) {
        self.pending.set(false);
        let _ = call(cx, self.reject.raw(), UNDEFINED, &[reason]);
    }
}

/// `Promise.resolve(value)` (a new promise, also for promises).
pub(crate) fn resolved(cx: Ctx, value: Raw) -> R<V> {
    let d = Deferred::new(cx)?;
    d.resolve(cx, value);
    Ok(d.promise)
}

/// A promise rejected with `reason`.
pub(crate) fn rejected(cx: Ctx, reason: Raw) -> R<V> {
    let d = Deferred::new(cx)?;
    d.reject(cx, reason);
    Ok(d.promise)
}

/// A promise rejected with the pending exception.
pub(crate) fn rejected_with_exception(cx: Ctx) -> R<V> {
    let e = take_exception(cx);
    rejected(cx, e.raw())
}

/// `r`, what a native returning a promise made, as a promise: an exception
/// (a wrong argument, an op past the limit) rejects it instead, as every
/// call returning a promise does; an uncatchable one (an interrupt) stays
/// thrown.
pub(crate) fn rejecting(cx: Ctx, r: Raw) -> Raw {
    if r.tag != 8 {
        return r;
    }
    let e = take_exception(cx);
    if engine::op(cx, Op::IsUncatchable, e.raw(), &[], &[], 0).payload == 1 {
        throw(cx, e);
        return Raw::EXCEPTION;
    }
    match rejected(cx, e.raw()) {
        Ok(p) => p.take(),
        Err(_) => Raw::EXCEPTION,
    }
}

/// The result of `f` as a promise: its value, or rejected with what it
/// threw.
pub(crate) fn promise_of(cx: Ctx, f: impl FnOnce() -> R<V>) -> R<V> {
    match f() {
        Ok(v) => resolved(cx, v.raw()),
        Err(Throw) => rejected_with_exception(cx),
    }
}

/// Marks `promise` handled (no unhandled-rejection report).
pub(crate) fn mark_handled(cx: Ctx, promise: Raw) {
    let _ = operation(cx, Op::PromiseHandled, promise, &[], &[], 0);
}

/// `promise.then(on_ok, on_err)`, through the intrinsic `then` (as the
/// specs' "react to" does, unaffected by an overridden `then`).
pub(crate) fn then(cx: Ctx, promise: Raw, on_ok: Raw, on_err: Raw) -> R<V> {
    operation(cx, Op::PromiseThen, promise, &[on_ok, on_err], &[], 0)
}

/// Reacts to `promise` with Rust code (its result discarded and handled).
pub(crate) fn upon(
    cx: Ctx,
    promise: Raw,
    captures: Vec<V>,
    on_ok: impl Fn(Ctx, &[V], Raw) -> R<()> + 'static,
    on_err: impl Fn(Ctx, &[V], Raw) -> R<()> + 'static,
) -> R<()> {
    let ok = closure(
        cx,
        captures.iter().map(V::clone_ref).collect(),
        move |cx, c, args| {
            on_ok(cx, c, args.get(0))?;
            Ok(V::undefined())
        },
    )?;
    let err = closure(cx, captures, move |cx, c, args| {
        on_err(cx, c, args.get(0))?;
        Ok(V::undefined())
    })?;
    let p = then(cx, promise, ok.raw(), err.raw())?;
    mark_handled(cx, p.raw());
    Ok(())
}

// --- Iteration ------------------------------------------------------------

/// Calls `f` with each value `iterable` yields (sync iteration protocol).
pub(crate) fn for_each(cx: Ctx, iterable: Raw, mut f: impl FnMut(V) -> R<()>) -> R<()> {
    let method = get_atom(cx, iterable, engine::ITERATOR)?;
    if !is_function(cx, method.raw()) {
        return Err(type_error(cx, "value is not iterable"));
    }
    let iterator = call(cx, method.raw(), iterable, &[])?;
    if !is_object(iterator.raw()) {
        return Err(type_error(cx, "iterator is not an object"));
    }
    let next = get(cx, iterator.raw(), "next")?;
    loop {
        let r = call(cx, next.raw(), iterator.raw(), &[])?;
        if !is_object(r.raw()) {
            return Err(type_error(cx, "iterator result is not an object"));
        }
        let done = get(cx, r.raw(), "done")?;
        if to_bool(cx, done.raw()) {
            return Ok(());
        }
        let value = get(cx, r.raw(), "value")?;
        if let Err(t) = f(value) {
            // Close the iterator, keeping the first error.
            let e = take_exception(cx);
            if let Ok(Some(ret)) = get_method(cx, iterator.raw(), "return") {
                let _ = call(cx, ret.raw(), iterator.raw(), &[]);
            }
            let _ = take_exception(cx);
            let _ = throw(cx, e);
            return Err(t);
        }
    }
}

/// Whether `v` has a callable `Symbol.iterator`.
pub(crate) fn is_iterable(cx: Ctx, v: Raw) -> R<bool> {
    if !is_object(v) {
        return Ok(false);
    }
    let m = get_atom(cx, v, engine::ITERATOR)?;
    Ok(is_function(cx, m.raw()))
}

/// The own enumerable string keys of `obj`, with their values (a WebIDL
/// record).
pub(crate) fn record(cx: Ctx, obj: Raw) -> R<Vec<(String, V)>> {
    let keys = operation(cx, Op::Keys, obj, &[], &[], 0)?;
    let len = to_number(cx, get(cx, keys.raw(), "length")?.raw())? as usize;
    let mut out = Vec::with_capacity(len);
    let getter = super::intrinsic_get(cx)?;
    for i in 0..len {
        // Lookup must use the original JS key: converting to USVString first
        // changes lone surrogates and can select a different property/getter.
        let key = get(cx, keys.raw(), &i.to_string())?;
        let value = call(cx, getter.raw(), UNDEFINED, &[obj, key.raw()])?;
        let name = to_string(cx, key.raw())?;
        out.push((name, value));
    }
    Ok(out)
}

// --- Bytes ----------------------------------------------------------------

/// The bytes of an ArrayBuffer or a view (borrowed until JS runs).
///
/// # Safety
///
/// The slice is valid until the buffer is detached, resized or freed: use
/// it before running any JS.
pub(crate) unsafe fn bytes_of<'a>(cx: Ctx, v: Raw) -> Option<&'a [u8]> {
    let e = unsafe { (*cx).engine };
    let mut b = engine::Bytes::default();
    if unsafe { (e.bytes)(e.user, cx, v, 0, &mut b) } != 1 {
        return None;
    }
    Some(unsafe { b.slice() })
}

/// The bytes of an ArrayBuffer or a view, writable.
///
/// # Safety
///
/// As [`bytes_of`], and no other slice of the same bytes is in use.
pub(crate) unsafe fn bytes_mut<'a>(cx: Ctx, v: Raw) -> Option<&'a mut [u8]> {
    // SAFETY: per the contract.
    unsafe {
        bytes_of(cx, v).map(|b| std::slice::from_raw_parts_mut(b.as_ptr().cast_mut(), b.len()))
    }
}

/// Copies the bytes of a BufferSource (`None`: not one).
pub(crate) fn buffer_source(cx: Ctx, v: Raw) -> Option<Vec<u8>> {
    // SAFETY: copied before any JS runs.
    unsafe { bytes_of(cx, v) }.map(<[u8]>::to_vec)
}

/// An ArrayBufferView's buffer, byte offset and length; its element size
/// and typed array kind (`None` for a DataView).
pub(crate) struct View {
    pub(crate) buffer: V,
    pub(crate) offset: usize,
    pub(crate) length: usize,
    pub(crate) element: usize,
    pub(crate) kind: Option<c_int>,
}

/// `v` as an ArrayBufferView.
pub(crate) fn view(cx: Ctx, v: Raw) -> Option<View> {
    let e = unsafe { (*cx).engine };
    let mut info = engine::View {
        buffer: UNDEFINED,
        offset: 0,
        length: 0,
        element: 0,
        kind: -1,
    };
    if unsafe { (e.view)(e.user, cx, v, &mut info) } != 1 {
        return None;
    }
    Some(View {
        buffer: V::own(cx, info.buffer),
        offset: info.offset,
        length: info.length,
        element: info.element,
        kind: if info.kind < 0 { None } else { Some(info.kind) },
    })
}

/// A new ArrayBuffer holding `bytes`.
pub(crate) fn array_buffer(cx: Ctx, bytes: &[u8]) -> R<V> {
    operation(cx, Op::ArrayBufferNew, UNDEFINED, &[], bytes, 0)
}

/// A new Uint8Array holding `bytes`.
pub(crate) fn uint8_array(cx: Ctx, bytes: &[u8]) -> R<V> {
    operation(cx, Op::Uint8ArrayNew, UNDEFINED, &[], bytes, 0)
}

/// A new typed array of `kind` over `buffer[offset..]` with `length`
/// elements (`kind` `None`: a DataView of `length` bytes).
pub(crate) fn new_view(
    cx: Ctx,
    kind: Option<c_int>,
    buffer: Raw,
    offset: usize,
    length: usize,
) -> R<V> {
    operation(
        cx,
        Op::ViewNew,
        buffer,
        &[Raw::number(offset as f64), Raw::number(length as f64)],
        &[],
        kind.unwrap_or(-1) as i64,
    )
}

/// Moves `buffer`'s bytes to a new ArrayBuffer and detaches it (the
/// specs' TransferArrayBuffer).
pub(crate) fn transfer(cx: Ctx, buffer: Raw) -> R<V> {
    // The engine moves the backing store and detaches every old view. Never
    // borrow payload bytes across this operation or snapshot through a Vec.
    operation(cx, Op::ArrayBufferTransfer, buffer, &[], &[], 0)
}

/// Whether ArrayBuffer `buffer` is detached.
pub(crate) fn is_detached(cx: Ctx, buffer: Raw) -> bool {
    engine::op(cx, Op::IsDetached, buffer, &[], &[], 0).payload == 1
}
