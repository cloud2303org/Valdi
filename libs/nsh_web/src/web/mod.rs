pub(crate) mod js;

pub(crate) mod blob;
mod encoding;
mod event;
mod fetch;
mod iterator;
mod streams;
mod url;
mod websocket;

use std::{
    any::Any,
    cell::{RefCell, RefMut},
    ffi::{CStr, c_int, c_void},
    ptr,
};

use crate::engine;
use js::{Args, Closure, Ctx, Native, R, Raw, Throw, Trace, UNDEFINED, V};

/// The Rust data of an object of a web class.
pub(crate) trait Object: Trace + Any {
    /// The `EventTarget` part (for `EventTarget`'s methods on subclasses).
    fn target(&mut self) -> Option<&mut event::Target> {
        None
    }

    /// The `Blob` part (`File`s).
    fn blob(&mut self) -> Option<&mut blob::Blob> {
        None
    }

    /// The body (`Request`, `Response`).
    fn body(&mut self) -> Option<&mut fetch::Body> {
        None
    }
}

/// The classes, in registration order (parents first).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(usize)]
pub(crate) enum Class {
    EventTarget,
    Event,
    CustomEvent,
    AbortSignal,
    AbortController,
    Iterator,
    UrlSearchParams,
    Url,
    TextEncoder,
    TextDecoder,
    Blob,
    File,
    FormData,
    ByteLengthQueuingStrategy,
    CountQueuingStrategy,
    ReadableStream,
    ReadableStreamDefaultReader,
    ReadableStreamBYOBReader,
    ReadableStreamDefaultController,
    ReadableByteStreamController,
    ReadableStreamBYOBRequest,
    ReadableStreamAsyncIterator,
    Internal,
    WritableStream,
    WritableStreamDefaultWriter,
    WritableStreamDefaultController,
    TransformStream,
    TransformStreamDefaultController,
    TextEncoderStream,
    TextDecoderStream,
    Headers,
    Request,
    Response,
    WebSocket,
}

const CLASS_COUNT: usize = Class::WebSocket as usize + 1;

/// A method key: a name or a well-known symbol.
pub(crate) enum Key {
    Name(&'static CStr),
    Iterator,
    AsyncIterator,
}

/// A class: its constructor, prototype members and static members.
pub(crate) struct Spec {
    pub(crate) class: Class,
    pub(crate) name: &'static CStr,
    pub(crate) parent: Option<Class>,
    /// `None`: an illegal constructor (objects are made internally).
    pub(crate) constructor: Option<(Native, c_int)>,
    pub(crate) methods: &'static [(Key, Native, c_int)],
    /// Accessors: getter and optional setter.
    pub(crate) accessors: &'static [(&'static CStr, Native, Option<Native>)],
    pub(crate) statics: &'static [(&'static CStr, Native, c_int)],
    /// Constants on the constructor and the prototype.
    pub(crate) constants: &'static [(&'static CStr, f64)],
    /// Defined on the global object.
    pub(crate) global: bool,
    /// Its TypeScript declaration (`globals.d.ts`).
    pub(crate) ts: ClassTs,
}

/// The TypeScript declaration of a global class: `declare class
/// <name><head> { <members> }`, with its constants (`readonly NONE:
/// number`) added from the spec. A test checks that the members are the
/// spec's.
pub(crate) struct ClassTs {
    /// Documentation (plain text).
    pub(crate) doc: &'static str,
    /// What follows the name: type parameters and heritage (`<R = any>`,
    /// ` extends EventTarget`).
    pub(crate) head: &'static str,
    /// The members' declarations, one per line as in a class body.
    pub(crate) members: &'static str,
}

impl ClassTs {
    /// Not declared: a class apps never name (`Iterator`'s prototype, the
    /// streams' internals).
    pub(crate) const NONE: ClassTs = ClassTs {
        doc: "",
        head: "",
        members: "",
    };

    /// This declaration, or [`ClassTs::NONE`] in builds without
    /// declarations (`nsh_core::element::DECLARATIONS`): the text is then not in
    /// the binary.
    pub(crate) const fn declared(self) -> ClassTs {
        if true { self } else { ClassTs::NONE }
    }
}

fn specs() -> impl Iterator<Item = &'static Spec> {
    [
        event::SPECS,
        iterator::SPECS,
        url::SPECS,
        encoding::SPECS,
        blob::SPECS,
        fetch::SPECS,
        websocket::SPECS,
    ]
    .into_iter()
    .flatten()
    .chain(streams::specs())
}

/// A global function (not a class) and its declaration.
pub(crate) struct Function {
    pub(crate) name: &'static CStr,
    pub(crate) native: Native,
    pub(crate) length: c_int,
    /// Documentation (plain text).
    pub(crate) doc: &'static str,
    /// What follows the name in its declaration.
    pub(crate) signature: &'static str,
}

/// Global functions (not classes).
pub(crate) fn functions() -> impl Iterator<Item = &'static Function> {
    fetch::FUNCTIONS.iter()
}

/// The global classes, as declared.
pub(crate) fn declared_specs() -> impl Iterator<Item = &'static Spec> {
    specs().filter(|s| s.global)
}

/// The classes apps never name.
#[cfg(test)]
pub(crate) fn internal_specs() -> impl Iterator<Item = &'static Spec> {
    specs().filter(|s| !s.global)
}

/// The TypeScript types the classes use (`BodyInit`, `EventInit`, ...).
pub(crate) const TYPES: &[&str] = &[
    encoding::TYPES,
    event::TYPES,
    blob::TYPES,
    streams::TYPES,
    streams::readable::TYPES,
    streams::writable::TYPES,
    streams::transform::TYPES,
    fetch::TYPES,
    websocket::TYPES,
];

/// Engine-independent per-context Web state.
#[derive(Default)]
pub(crate) struct Web {
    built: bool,
    protos: Vec<V>,
    ctors: Vec<V>,
    cached: Vec<V>,
    function_proto: V,
    reflect_get: V,
    pub(crate) blob_urls: blob::BlobUrls,
    pub(crate) fetches: rustc_hash::FxHashMap<crate::net::RequestId, fetch::Inflight>,
    pub(crate) next_fetch: crate::net::RequestId,
    pub(crate) base_url: Option<::url::Url>,
}
pub(crate) fn web<'a>(cx: Ctx) -> &'a mut Web {
    unsafe { &mut (*cx).web }
}
struct Slot(RefCell<Box<dyn Object>>);

unsafe extern "C" fn finalize_slot(cx: Ctx, p: *mut c_void) {
    unsafe {
        (*cx).natives.borrow_mut().remove(&(p as usize));
    }
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        drop(Box::from_raw(p as *mut Slot))
    }));
}
unsafe extern "C" fn trace_slot(p: *mut c_void, visit: engine::Visit, user: *mut c_void) -> i32 {
    let slot = unsafe { &*(p as *const Slot) };
    let Ok(data) = slot.0.try_borrow() else {
        return -1;
    };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        data.trace(&mut |v| unsafe { visit(user, v) });
    })) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}
unsafe extern "C" fn finalize_closure(cx: Ctx, p: *mut c_void) {
    unsafe {
        (*cx).natives.borrow_mut().remove(&(p as usize));
    }
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        drop(Box::from_raw(p as *mut Closure))
    }));
}
unsafe extern "C" fn trace_closure(p: *mut c_void, visit: engine::Visit, user: *mut c_void) -> i32 {
    let c = unsafe { &*(p as *const Closure) };
    c.trace(&mut |v| unsafe { visit(user, v) });
    0
}
unsafe extern "C" fn call_closure(cx: Ctx, func: Raw, argc: c_int, argv: *mut Raw) -> Raw {
    let p = engine::native_get(cx, func, 2) as *const Closure;
    let Some(c) = (unsafe { p.as_ref() }) else {
        js::type_error(cx, "Invalid native closure");
        return Raw::EXCEPTION;
    };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        (c.f)(cx, &c.captures, unsafe { Args::new(argc, argv) })
    })) {
        Ok(Ok(v)) => v.take(),
        Ok(Err(_)) => Raw::EXCEPTION,
        Err(_) => {
            js::type_error(cx, "Rust closure panicked");
            Raw::EXCEPTION
        }
    }
}
pub(crate) fn new_closure(cx: Ctx, closure: Closure) -> R<V> {
    let proto = web(cx).function_proto.clone_ref();
    let p = Box::into_raw(Box::new(closure)).cast();
    let e = unsafe { (*cx).engine };
    let raw = unsafe {
        (e.native_new)(
            e.user,
            cx,
            proto.raw(),
            2,
            p,
            finalize_closure,
            trace_closure,
            Some(call_closure),
        )
    };
    if raw.tag == 8 {
        unsafe { drop(Box::from_raw(p as *mut Closure)) };
        return Err(Throw);
    }
    unsafe {
        (*cx).natives.borrow_mut().insert(p as usize, trace_closure);
    }
    V::checked(cx, raw)
}
pub(crate) fn new_function(cx: Ctx, name: &CStr, f: Native, length: c_int) -> R<V> {
    let func = js::closure(cx, Vec::new(), move |cx, _, args| {
        let mut raws: Vec<_> = (0..args.len()).map(|i| args.get(i)).collect();
        V::checked(cx, unsafe {
            f(cx, UNDEFINED, raws.len() as c_int, raws.as_mut_ptr())
        })
    })?;
    let name = js::string(cx, &name.to_string_lossy())?;
    js::define_flags(cx, func.raw(), "name", name, 4)?;
    js::define_flags(cx, func.raw(), "length", V::num(length as f64), 4)?;
    Ok(func)
}
/// Property lookup with the original JS key, unaffected by global overrides.
pub(crate) fn intrinsic_get(cx: Ctx) -> R<V> {
    build(cx)?;
    Ok(web(cx).reflect_get.clone_ref())
}
pub(crate) fn cached_function(cx: Ctx, index: usize, name: &CStr, f: Native) -> R<V> {
    if let Some(v) = web(cx).cached.get(index).filter(|v| !v.is_undefined()) {
        return Ok(v.clone_ref());
    }
    let func = js::function(cx, name, f, 1)?;
    let w = web(cx);
    if w.cached.len() <= index {
        w.cached.resize_with(index + 1, V::undefined);
    }
    w.cached[index] = func.clone_ref();
    Ok(func)
}
pub(crate) fn proto(cx: Ctx, class: Class) -> R<V> {
    build(cx)?;
    Ok(web(cx).protos[class as usize].clone_ref())
}
pub(crate) fn constructor(cx: Ctx, class: Class) -> R<V> {
    build(cx)?;
    Ok(web(cx).ctors[class as usize].clone_ref())
}
pub(crate) fn instance(cx: Ctx, new_target: Raw, class: Class, data: impl Object) -> R<V> {
    let proto = if js::is_object(new_target) {
        let p = js::get(cx, new_target, "prototype")?;
        if js::is_object(p.raw()) {
            p
        } else {
            proto(cx, class)?
        }
    } else {
        proto(cx, class)?
    };
    instance_of_proto(cx, proto.raw(), data)
}
pub(crate) fn instance_of_proto(cx: Ctx, proto: Raw, data: impl Object) -> R<V> {
    let p = Box::into_raw(Box::new(Slot(RefCell::new(Box::new(data))))).cast();
    let e = unsafe { (*cx).engine };
    let raw = unsafe { (e.native_new)(e.user, cx, proto, 1, p, finalize_slot, trace_slot, None) };
    if raw.tag == 8 {
        unsafe { drop(Box::from_raw(p as *mut Slot)) };
        return Err(Throw);
    }
    unsafe {
        (*cx).natives.borrow_mut().insert(p as usize, trace_slot);
    }
    V::checked(cx, raw)
}
fn slot<'a>(cx: Ctx, v: Raw) -> Option<&'a Slot> {
    unsafe { (engine::native_get(cx, v, 1) as *const Slot).as_ref() }
}
/// The data of `v` as a `T`, borrowed (`TypeError` for other values, and
/// for a re-entrant use).
pub(crate) fn data<'a, T: Object>(cx: Ctx, v: Raw, what: &str) -> R<RefMut<'a, T>> {
    let illegal = |cx| js::type_error(cx, &format!("Illegal invocation: not a {what}"));
    let slot = slot(cx, v).ok_or_else(|| illegal(cx))?;
    let b = slot
        .0
        .try_borrow_mut()
        .map_err(|_| js::type_error(cx, &format!("{what} used re-entrantly")))?;
    RefMut::filter_map(b, |d| (d.as_mut() as &mut dyn Any).downcast_mut::<T>())
        .map_err(|_| illegal(cx))
}

/// `f` of the data of `v` as a `T`, without throwing (`None`: another
/// value, or one in use).
pub(crate) fn peek<T: Object, R>(cx: Ctx, v: Raw, f: impl FnOnce(&T) -> R) -> Option<R> {
    let slot = slot(cx, v)?;
    let data = slot.0.try_borrow().ok()?;
    (data.as_ref() as &dyn Any).downcast_ref::<T>().map(f)
}

/// Whether `v` holds a `T`.
pub(crate) fn is<T: Object>(cx: Ctx, v: Raw) -> bool {
    slot(cx, v).is_some_and(|s| {
        s.0.try_borrow()
            .is_ok_and(|d| (d.as_ref() as &dyn Any).is::<T>())
    })
}

/// The data of `v` through a role (`EventTarget` of subclasses).
pub(crate) fn role<'a, T>(
    cx: Ctx,
    v: Raw,
    what: &str,
    f: impl FnOnce(&mut dyn Object) -> Option<&mut T>,
) -> R<RefMut<'a, T>> {
    let illegal = |cx| js::type_error(cx, &format!("Illegal invocation: not a {what}"));
    let slot = slot(cx, v).ok_or_else(|| illegal(cx))?;
    let b = slot
        .0
        .try_borrow_mut()
        .map_err(|_| js::type_error(cx, &format!("{what} used re-entrantly")))?;
    RefMut::filter_map(b, |d| f(d.as_mut())).map_err(|_| illegal(cx))
}

fn build(cx: Ctx) -> R<()> {
    if web(cx).built {
        return Ok(());
    }
    // Build in local vectors and publish only after success. Re-entrant host
    // registration must not call Rust Web constructors during installation.
    let global = js::global(cx);
    let function = js::get(cx, global.raw(), "Function")?;
    let function_proto = js::get(cx, function.raw(), "prototype")?;
    let reflect = js::get(cx, global.raw(), "Reflect")?;
    let reflect_get = js::get(cx, reflect.raw(), "get")?;
    let mut protos: Vec<V> = (0..CLASS_COUNT).map(|_| V::undefined()).collect();
    let mut ctors: Vec<V> = (0..CLASS_COUNT).map(|_| V::undefined()).collect();
    for spec in specs() {
        let methods: Vec<_> = spec
            .methods
            .iter()
            .map(|(k, f, n)| {
                let (name, symbol) = match k {
                    Key::Name(n) => (n.as_ptr(), 0),
                    Key::Iterator => (ptr::null(), 1),
                    Key::AsyncIterator => (ptr::null(), 2),
                };
                engine::Method {
                    name,
                    symbol,
                    call: *f,
                    length: *n,
                }
            })
            .collect();
        let accessors: Vec<_> = spec
            .accessors
            .iter()
            .map(|(n, g, s)| engine::Accessor {
                name: n.as_ptr(),
                get: *g,
                set: *s,
            })
            .collect();
        let statics: Vec<_> = spec
            .statics
            .iter()
            .map(|(n, f, l)| engine::Method {
                name: n.as_ptr(),
                symbol: 0,
                call: *f,
                length: *l,
            })
            .collect();
        let constants: Vec<_> = spec
            .constants
            .iter()
            .map(|(n, v)| engine::Constant {
                name: n.as_ptr(),
                value: *v,
            })
            .collect();
        let (f, length) = spec
            .constructor
            .map(|(f, l)| (Some(f), l))
            .unwrap_or((None, 0));
        let abi = engine::ClassSpec {
            id: spec.class as u32,
            name: spec.name.as_ptr(),
            parent_id: spec.parent.map(|p| p as i32).unwrap_or(-1),
            constructor: f,
            length,
            methods: methods.as_ptr(),
            methods_len: methods.len(),
            accessors: accessors.as_ptr(),
            accessors_len: accessors.len(),
            statics: statics.as_ptr(),
            statics_len: statics.len(),
            constants: constants.as_ptr(),
            constants_len: constants.len(),
            global: spec.global as u32,
        };
        let (pp, pc) = spec
            .parent
            .map(|p| (protos[p as usize].raw(), ctors[p as usize].raw()))
            .unwrap_or((UNDEFINED, UNDEFINED));
        let (mut p, mut c) = (UNDEFINED, UNDEFINED);
        let e = unsafe { (*cx).engine };
        if unsafe { (e.register_class)(e.user, cx, &abi, pp, pc, &mut p, &mut c) } < 0 {
            return Err(Throw);
        }
        protos[spec.class as usize] = V::own(cx, p);
        ctors[spec.class as usize] = V::own(cx, c);
    }
    // Match NSH's iterator helper inheritance, without any engine-private atom.
    let array = js::new_array(cx, Vec::new())?;
    let iterator = js::invoke(cx, array.raw(), "values", &[])?;
    let parent = V::checked(
        cx,
        engine::op(cx, engine::Op::GetPrototype, iterator.raw(), &[], &[], 0),
    )?;
    let parent = V::checked(
        cx,
        engine::op(cx, engine::Op::GetPrototype, parent.raw(), &[], &[], 0),
    )?;
    V::checked(
        cx,
        engine::op(
            cx,
            engine::Op::SetPrototype,
            protos[Class::Iterator as usize].raw(),
            &[parent.raw()],
            &[],
            0,
        ),
    )?;
    let w = web(cx);
    w.protos = protos;
    w.ctors = ctors;
    w.function_proto = function_proto;
    w.reflect_get = reflect_get;
    w.built = true;
    Ok(())
}
pub(crate) fn install(cx: Ctx) -> R<()> {
    build(cx)?;
    let g = js::global(cx);
    for spec in declared_specs() {
        js::define(
            cx,
            g.raw(),
            &spec.name.to_string_lossy(),
            constructor(cx, spec.class)?,
            false,
        )?;
    }
    for f in functions() {
        js::define(
            cx,
            g.raw(),
            &f.name.to_string_lossy(),
            js::function(cx, f.name, f.native, f.length)?,
            false,
        )?;
    }
    Ok(())
}
pub(crate) fn fetch_event(
    cx: Ctx,
    id: crate::net::RequestId,
    event: crate::net::FetchEvent,
) -> R<()> {
    fetch::event(cx, id, event)
}
pub(crate) fn http_data_buffer(cx: Ctx, id: crate::net::RequestId, view: Raw) -> R<()> {
    fetch::data_buffer(cx, id, view)
}
pub(crate) fn forget_dropped_fetches(cx: Ctx) {
    fetch::forget_dropped(cx);
}
