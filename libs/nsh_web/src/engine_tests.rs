//! Deterministic test engine for the C ABI. Web algorithms under test are the
//! real retained NSH modules; this supplies only engine primitives, not Web APIs.
use super::*;
use crate::{
    engine::*,
    web::js::{self, Ctx, Native, V},
};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    ffi::{CStr, c_void},
};
const U: Value = Value::UNDEFINED;
#[derive(Clone)]
enum F {
    Native(Native),
    Ctor(Option<Native>),
    Attached(Native),
    Builtin(fn(Ctx, Value, &[Value]) -> Value),
    Resolve(Value, bool),
}
#[derive(Clone)]
struct Attachment {
    data: usize,
    kind: u32,
    finalize: Finalize,
    trace: Trace,
}
#[derive(Clone, Default)]
struct Entry {
    text: Option<Vec<u8>>,
    props: BTreeMap<String, Value>,
    raw_props: BTreeMap<Vec<u8>, Value>,
    getters: BTreeMap<String, Native>,
    setters: BTreeMap<String, Native>,
    proto: Value,
    call: Option<F>,
    attachment: Option<Attachment>,
    bytes: Option<Vec<u8>>,
    view: Option<(Value, usize, usize, usize, i32)>,
    promise: Option<(u8, Value, Vec<(Value, Value, Value)>)>,
    detached: bool,
}
#[derive(Default)]
struct TestEngine {
    next: u64,
    heap: BTreeMap<u64, Entry>,
    refs: BTreeMap<u64, isize>,
    global: Value,
    object_proto: Value,
    function_proto: Value,
    array_proto: Value,
    exception: Value,
    scratch: Vec<u8>,
    jobs: VecDeque<(Value, Value, Value, bool)>,
    reserved_ops: usize,
}
unsafe fn h<'a>(cx: Ctx) -> &'a mut TestEngine {
    unsafe { &mut *((*cx).engine.user as *mut TestEngine) }
}
fn allocate(cx: Ctx, tag: u32, e: Entry) -> Value {
    let h = unsafe { h(cx) };
    h.next += 1;
    let id = h.next;
    h.heap.insert(id, e);
    h.refs.insert(id, 1);
    Value {
        tag,
        flags: 0,
        payload: id,
    }
}
fn entry(cx: Ctx, v: Value) -> Entry {
    unsafe { h(cx) }
        .heap
        .get(&v.payload)
        .cloned()
        .unwrap_or_default()
}
fn put(cx: Ctx, o: Value, k: &str, v: Value) {
    unsafe { h(cx) }
        .heap
        .get_mut(&o.payload)
        .unwrap()
        .props
        .insert(k.into(), v);
}
fn object(cx: Ctx) -> Value {
    let proto = unsafe { h(cx) }.object_proto;
    allocate(
        cx,
        5,
        Entry {
            proto,
            ..Default::default()
        },
    )
}
fn string(cx: Ctx, s: &str) -> Value {
    allocate(
        cx,
        4,
        Entry {
            text: Some(s.as_bytes().to_vec()),
            ..Default::default()
        },
    )
}
fn function(cx: Ctx, f: F) -> Value {
    let proto = unsafe { h(cx) }.function_proto;
    allocate(
        cx,
        5,
        Entry {
            proto,
            call: Some(f),
            ..Default::default()
        },
    )
}
fn array(cx: Ctx, items: &[Value]) -> Value {
    let proto = unsafe { h(cx) }.array_proto;
    let o = allocate(
        cx,
        5,
        Entry {
            proto,
            ..Default::default()
        },
    );
    for (i, v) in items.iter().enumerate() {
        put(cx, o, &i.to_string(), *v);
    }
    put(cx, o, "length", Value::number(items.len() as f64));
    o
}
fn text(cx: Ctx, v: Value) -> String {
    match v.tag {
        0 => "undefined".into(),
        1 => "null".into(),
        2 => (v.payload != 0).to_string(),
        3 => v.as_number().to_string(),
        4 => String::from_utf8_lossy(&entry(cx, v).text.unwrap()).into_owned(),
        _ => "[object Object]".into(),
    }
}
fn get(cx: Ctx, o: Value, k: &str) -> Value {
    get_with_receiver(cx, o, k, o)
}
fn get_with_receiver(cx: Ctx, o: Value, k: &str, this: Value) -> Value {
    if o.tag != 5 && o.tag != 4 {
        return U;
    }
    let e = entry(cx, o);
    if let Some(v) = e.props.get(k) {
        return *v;
    }
    if let Some(f) = e.getters.get(k) {
        return unsafe { f(cx, this, 0, std::ptr::null_mut()) };
    }
    if e.proto.tag == 5 {
        return get_with_receiver(cx, e.proto, k, this);
    }
    U
}
fn call(cx: Ctx, f: Value, this: Value, args: &[Value]) -> Value {
    let mut args = args.to_vec();
    match entry(cx, f).call {
        Some(F::Native(n)) => unsafe { n(cx, this, args.len() as i32, args.as_mut_ptr()) },
        Some(F::Attached(n)) => unsafe { n(cx, f, args.len() as i32, args.as_mut_ptr()) },
        Some(F::Builtin(n)) => n(cx, this, &args),
        Some(F::Resolve(p, reject)) => {
            settle(cx, p, args.first().copied().unwrap_or(U), reject);
            U
        }
        _ => {
            unsafe { h(cx) }.exception = string(cx, "not callable");
            Value::EXCEPTION
        }
    }
}
fn construct(cx: Ctx, f: Value, args: &[Value]) -> Value {
    let mut args = args.to_vec();
    match entry(cx, f).call {
        Some(F::Ctor(Some(n))) => unsafe { n(cx, f, args.len() as i32, args.as_mut_ptr()) },
        Some(F::Builtin(n)) => n(cx, f, &args),
        _ => {
            unsafe { h(cx) }.exception = string(cx, "illegal constructor");
            Value::EXCEPTION
        }
    }
}
fn settle(cx: Ctx, p: Value, v: Value, reject: bool) {
    let state = entry(cx, p).promise.unwrap();
    if state.0 != 0 {
        return;
    }
    if !reject && entry(cx, v).promise.is_some() {
        // Engine-level promise adoption, test-only.
        let ok = function(cx, F::Resolve(p, false));
        let err = function(cx, F::Resolve(p, true));
        let _ = react(cx, v, ok, err);
        return;
    }
    unsafe { h(cx) }.heap.get_mut(&p.payload).unwrap().promise =
        Some((if reject { 2 } else { 1 }, v, Vec::new()));
    for (ok, err, target) in state.2 {
        unsafe { h(cx) }
            .jobs
            .push_back((if reject { err } else { ok }, v, target, reject));
    }
}
fn promise(cx: Ctx) -> (Value, Value, Value) {
    let p = allocate(
        cx,
        5,
        Entry {
            promise: Some((0, U, Vec::new())),
            ..Default::default()
        },
    );
    (
        p,
        function(cx, F::Resolve(p, false)),
        function(cx, F::Resolve(p, true)),
    )
}
fn react(cx: Ctx, p: Value, ok: Value, err: Value) -> Value {
    let (target, _, _) = promise(cx);
    if let Some((s, v, _)) = entry(cx, p).promise {
        if s == 0 {
            unsafe { h(cx) }
                .heap
                .get_mut(&p.payload)
                .unwrap()
                .promise
                .as_mut()
                .unwrap()
                .2
                .push((ok, err, target));
        } else {
            unsafe { h(cx) }
                .jobs
                .push_back((if s == 2 { err } else { ok }, v, target, s == 2));
        }
    } else {
        unsafe { h(cx) }.jobs.push_back((ok, p, target, false));
    }
    target
}
fn pump(cx: Ctx) {
    for _ in 0..2000 {
        let job = unsafe { h(cx) }.jobs.pop_front();
        let Some((f, v, target, reject)) = job else {
            return;
        };
        if f.tag == 0 {
            settle(cx, target, v, reject);
        } else {
            let result = call(cx, f, U, &[v]);
            if result.tag == 8 {
                let e = unsafe { h(cx) }.exception;
                unsafe { h(cx) }.exception = U;
                settle(cx, target, e, true);
            } else {
                settle(cx, target, result, false);
            }
        }
    }
    panic!("microtask loop did not settle");
}
fn array_values(cx: Ctx, this: Value, _: &[Value]) -> Value {
    let it = object(cx);
    put(cx, it, "source", this);
    put(cx, it, "index", Value::number(0.));
    let n = function(
        cx,
        F::Builtin(|cx, this, _| {
            let src = get(cx, this, "source");
            let i = get(cx, this, "index").as_number() as usize;
            let len = get(cx, src, "length").as_number() as usize;
            put(cx, this, "index", Value::number((i + 1) as f64));
            let out = object(cx);
            put(cx, out, "done", Value::boolean(i >= len));
            put(cx, out, "value", get(cx, src, &i.to_string()));
            out
        }),
    );
    put(cx, it, "next", n);
    it
}
unsafe extern "C" fn retain(user: *mut c_void, v: Value) {
    if v.heap() {
        *unsafe { &mut *(user as *mut TestEngine) }
            .refs
            .entry(v.payload)
            .or_default() += 1;
    }
}
unsafe extern "C" fn release(user: *mut c_void, v: Value) {
    if v.heap() {
        *unsafe { &mut *(user as *mut TestEngine) }
            .refs
            .entry(v.payload)
            .or_default() -= 1;
    }
}
unsafe extern "C" fn operate(
    _: *mut c_void,
    cx: Ctx,
    op: u32,
    r: Value,
    args: *const Value,
    len: usize,
    bytes: Bytes,
    aux: i64,
) -> Value {
    let a = if len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(args, len) }
    };
    let b = unsafe { bytes.slice() };
    let k = String::from_utf8_lossy(b);
    let mut owned = true;
    let out = match op {
        1 => {
            owned = false;
            unsafe { h(cx) }.global
        }
        2 => string(cx, &k),
        4 => Value::number(match r.tag {
            0 => f64::NAN,
            1 => 0.,
            2 => r.payload as f64,
            3 => r.as_number(),
            4 => text(cx, r).parse().unwrap_or(f64::NAN),
            _ => f64::NAN,
        }),
        5 => Value::boolean(match r.tag {
            0 | 1 => false,
            2 => r.payload != 0,
            3 => r.as_number() != 0. && !r.as_number().is_nan(),
            4 => !text(cx, r).is_empty(),
            _ => true,
        }),
        6 => Value::boolean(entry(cx, r).call.is_some()),
        7 => {
            owned = false;
            get(cx, r, &k)
        }
        8 => {
            owned = false;
            get(cx, r, &format!("@@{aux}"))
        }
        9 => {
            put(cx, r, &k, a[0]);
            U
        }
        10 => call(cx, r, a[0], &a[1..]),
        11 => construct(cx, r, a),
        12 => object(cx),
        13 => array(cx, a),
        14 => {
            let e = entry(cx, r);
            let mut keys: Vec<_> = e
                .props
                .keys()
                .filter(|k| !k.starts_with("@@"))
                .map(|k| string(cx, k))
                .collect();
            keys.extend(e.raw_props.keys().map(|k| {
                allocate(
                    cx,
                    4,
                    Entry {
                        text: Some(k.clone()),
                        ..Default::default()
                    },
                )
            }));
            array(cx, &keys)
        }
        16 => {
            let (p, ok, err) = promise(cx);
            array(cx, &[p, ok, err])
        }
        17 => react(cx, r, a[0], a[1]),
        18 => U,
        19 | 20 => {
            unsafe { h(cx) }.reserved_ops += 1;
            Value::EXCEPTION
        }
        21 => {
            unsafe { h(cx) }.exception = r;
            Value::EXCEPTION
        }
        22 => {
            owned = false;
            let e = unsafe { h(cx) }.exception;
            unsafe { h(cx) }.exception = U;
            e
        }
        23 => {
            let e = string(cx, &k);
            unsafe { h(cx) }.exception = e;
            Value::EXCEPTION
        }
        24 => U,
        26 => allocate(
            cx,
            5,
            Entry {
                bytes: Some(b.to_vec()),
                ..Default::default()
            },
        ),
        27 => {
            let buffer = allocate(
                cx,
                5,
                Entry {
                    bytes: Some(b.to_vec()),
                    ..Default::default()
                },
            );
            allocate(
                cx,
                5,
                Entry {
                    view: Some((buffer, 0, b.len(), 1, 1)),
                    ..Default::default()
                },
            )
        }
        28 => {
            let off = a[0].as_number() as usize;
            let n = a[1].as_number() as usize;
            let element = match aux {
                3 | 4 => 2,
                5 | 6 | 7 => 4,
                8 | 9 | 10 => 8,
                _ => 1,
            };
            allocate(
                cx,
                5,
                Entry {
                    view: Some((r, off, n * element, element, aux as i32)),
                    ..Default::default()
                },
            )
        }
        29 => {
            unsafe { h(cx) }.heap.get_mut(&r.payload).unwrap().detached = true;
            U
        }
        30 => Value::boolean(entry(cx, r).detached),
        31 => {
            unsafe { h(cx) }.heap.get_mut(&r.payload).unwrap().proto = a[0];
            U
        }
        32 => {
            owned = false;
            entry(cx, r).proto
        }
        33 => Value::boolean(false),
        34 => {
            // Primitive-only test engine support. Integration acceptance uses
            // actual Hermes backing pointers in NativeWebReceiveBuffer_tests.
            let source = unsafe { h(cx) }.heap.get_mut(&r.payload).unwrap();
            if source.detached || source.bytes.is_none() || source.view.is_some() {
                let error = string(cx, "Expected a live ArrayBuffer");
                unsafe { h(cx) }.exception = error;
                Value::EXCEPTION
            } else {
                let bytes = source.bytes.replace(Vec::new()).unwrap();
                source.detached = true;
                allocate(cx, 5, Entry { bytes: Some(bytes), ..Default::default() })
            }
        }
        _ => {
            let e = string(cx, &format!("unsupported test engine op {op}"));
            unsafe { h(cx) }.exception = e;
            Value::EXCEPTION
        }
    };
    if !owned && out.heap() {
        unsafe { retain((*cx).engine.user, out) }
    }
    out
}
unsafe extern "C" fn bytes(_: *mut c_void, cx: Ctx, v: Value, mode: u32, out: *mut Bytes) -> i32 {
    if mode == 1 {
        let s = if v.tag == 4 {
            entry(cx, v).text.unwrap()
        } else {
            text(cx, v).into_bytes()
        };
        unsafe { h(cx) }.scratch = s;
        let b = &unsafe { h(cx) }.scratch;
        unsafe { *out = b.as_slice().into() };
        return 1;
    }
    let e = entry(cx, v);
    let (buffer, offset, length) = if let Some((b, o, l, _, _)) = e.view {
        (b, o, l)
    } else if let Some(b) = e.bytes {
        (v, 0, b.len())
    } else {
        return 0;
    };
    let h = unsafe { h(cx) };
    let e = h.heap.get_mut(&buffer.payload).unwrap();
    let b = e.bytes.as_mut().unwrap();
    if e.detached {
        unsafe { *out = Bytes::default() };
        return 1;
    }
    unsafe {
        *out = Bytes {
            data: b.as_ptr().add(offset),
            len: length,
        }
    };
    1
}
unsafe extern "C" fn view(_: *mut c_void, cx: Ctx, v: Value, out: *mut View) -> i32 {
    let Some((buffer, offset, length, element, kind)) = entry(cx, v).view else {
        return 0;
    };
    unsafe {
        retain((*cx).engine.user, buffer);
        *out = View {
            buffer,
            offset,
            length,
            element,
            kind,
        };
    }
    1
}
unsafe extern "C" fn register(
    _: *mut c_void,
    cx: Ctx,
    spec: *const ClassSpec,
    pp: Value,
    pc: Value,
    p: *mut Value,
    c: *mut Value,
) -> i32 {
    let s = unsafe { &*spec };
    let proto = object(cx);
    if pp.tag == 5 {
        unsafe { h(cx) }.heap.get_mut(&proto.payload).unwrap().proto = pp;
    }
    let ctor = function(cx, F::Ctor(s.constructor));
    if pc.tag == 5 {
        unsafe { h(cx) }.heap.get_mut(&ctor.payload).unwrap().proto = pc;
    }
    put(cx, ctor, "prototype", proto);
    put(cx, proto, "constructor", ctor);
    for i in 0..s.methods_len {
        let m = unsafe { &*s.methods.add(i) };
        let key = if m.symbol != 0 {
            format!("@@{}", m.symbol)
        } else {
            unsafe { CStr::from_ptr(m.name) }.to_str().unwrap().into()
        };
        let f = function(cx, F::Native(m.call));
        put(cx, proto, &key, f);
    }
    for i in 0..s.accessors_len {
        let m = unsafe { &*s.accessors.add(i) };
        let key = unsafe { CStr::from_ptr(m.name) }
            .to_str()
            .unwrap()
            .to_owned();
        let e = unsafe { h(cx) }.heap.get_mut(&proto.payload).unwrap();
        e.getters.insert(key.clone(), m.get);
        if let Some(set) = m.set {
            e.setters.insert(key, set);
        }
    }
    for i in 0..s.statics_len {
        let m = unsafe { &*s.statics.add(i) };
        let key = unsafe { CStr::from_ptr(m.name) }.to_str().unwrap();
        let f = function(cx, F::Native(m.call));
        put(cx, ctor, key, f);
    }
    for i in 0..s.constants_len {
        let m = unsafe { &*s.constants.add(i) };
        let key = unsafe { CStr::from_ptr(m.name) }.to_str().unwrap();
        put(cx, ctor, key, Value::number(m.value));
        put(cx, proto, key, Value::number(m.value));
    }
    unsafe {
        *p = proto;
        *c = ctor
    };
    0
}
unsafe extern "C" fn native_new(
    _: *mut c_void,
    cx: Ctx,
    proto: Value,
    kind: u32,
    data: *mut c_void,
    finalize: Finalize,
    trace: Trace,
    call: Option<Native>,
) -> Value {
    allocate(
        cx,
        5,
        Entry {
            proto,
            attachment: Some(Attachment {
                data: data as usize,
                kind,
                finalize,
                trace,
            }),
            call: call.map(F::Attached),
            ..Default::default()
        },
    )
}
unsafe extern "C" fn native_get(_: *mut c_void, cx: Ctx, v: Value, kind: u32) -> *mut c_void {
    entry(cx, v)
        .attachment
        .filter(|a| a.kind == kind)
        .map(|a| a.data as *mut c_void)
        .unwrap_or(std::ptr::null_mut())
}
struct Host {
    cx: Ctx,
    engine: Box<TestEngine>,
}
impl Host {
    fn new() -> Self {
        let mut engine = Box::<TestEngine>::default();
        let vtable = Engine {
            abi_version: 1,
            struct_size: std::mem::size_of::<Engine>(),
            user: (&mut *engine as *mut TestEngine).cast(),
            operate,
            retain,
            release,
            bytes,
            view,
            register_class: register,
            native_new,
            native_get,
        };
        let cx = unsafe { nsh_web_create(&vtable) };
        assert!(!cx.is_null());
        engine.object_proto = object(cx);
        engine.function_proto = object(cx);
        engine.array_proto = object(cx);
        engine.global = object(cx);
        let f = function(cx, F::Builtin(|_, _, _| U));
        put(cx, f, "prototype", engine.function_proto);
        put(cx, engine.global, "Function", f);
        let reflect = object(cx);
        let reflect_get = function(
            cx,
            F::Builtin(|cx, _, args| {
                let key = entry(cx, args[1]).text.expect("Reflect.get string key");
                let v = entry(cx, args[0])
                    .raw_props
                    .get(&key)
                    .copied()
                    .unwrap_or_else(|| get(cx, args[0], std::str::from_utf8(&key).unwrap_or("")));
                unsafe { retain((*cx).engine.user, v) };
                v
            }),
        );
        put(cx, reflect, "get", reflect_get);
        put(cx, engine.global, "Reflect", reflect);
        let values = function(cx, F::Builtin(array_values));
        put(cx, engine.array_proto, "values", values);
        put(cx, engine.array_proto, "@@1", values);
        let weak = function(
            cx,
            F::Builtin(|cx, _, args| {
                let o = object(cx);
                put(cx, o, "target", args[0]);
                let d = function(
                    cx,
                    F::Builtin(|cx, this, _| {
                        let v = get(cx, this, "target");
                        unsafe { retain((*cx).engine.user, v) };
                        v
                    }),
                );
                put(cx, o, "deref", d);
                o
            }),
        );
        put(cx, engine.global, "WeakRef", weak);
        let dom = function(
            cx,
            F::Builtin(|cx, _, args| {
                let o = object(cx);
                put(cx, o, "message", args[0]);
                put(cx, o, "name", args[1]);
                o
            }),
        );
        put(cx, engine.global, "DOMException", dom);
        let result = unsafe { nsh_web_install(cx) };
        assert_eq!(result, 0, "install failed: {}", text(cx, engine.exception));
        Self { cx, engine }
    }
    fn new_class(&self, name: &str, args: &[Value]) -> Value {
        let f = get(self.cx, self.engine.global, name);
        let v = construct(self.cx, f, args);
        self.check(v)
    }
    fn check(&self, v: Value) -> Value {
        assert_ne!(
            v.tag,
            8,
            "JS exception: {}",
            text(self.cx, self.engine.exception)
        );
        v
    }
    fn method(&self, o: Value, name: &str, args: &[Value]) -> Value {
        self.check(call(self.cx, get(self.cx, o, name), o, args))
    }
    fn str(&self, s: &str) -> Value {
        string(self.cx, s)
    }
    fn get(&self, o: Value, k: &str) -> Value {
        self.check(get(self.cx, o, k))
    }
    fn settled(&self, p: Value) -> (u8, Value) {
        pump(self.cx);
        let (s, v, _) = entry(self.cx, p).promise.expect("promise result");
        (s, v)
    }
    fn collect(&self, extra: &[Value]) -> usize {
        unsafe extern "C" fn visit(p: *mut c_void, v: Value) {
            unsafe { &mut *(p as *mut Vec<Value>) }.push(v);
        }
        let mut work = vec![self.engine.global];
        work.extend_from_slice(extra);
        assert_eq!(
            unsafe { nsh_web_trace_roots(self.cx, visit, (&mut work as *mut Vec<Value>).cast()) },
            0
        );
        let mut seen = BTreeSet::new();
        while let Some(v) = work.pop() {
            if !v.heap() || !seen.insert(v.payload) {
                continue;
            }
            let e = entry(self.cx, v);
            work.push(e.proto);
            work.extend(e.props.values());
            work.extend(e.raw_props.values());
            if let Some((b, _, _, _, _)) = e.view {
                work.push(b);
            }
            if let Some((_, v, reactions)) = e.promise {
                work.push(v);
                for (a, b, c) in reactions {
                    work.extend([a, b, c]);
                }
            }
            if let Some(F::Resolve(p, _)) = e.call {
                work.push(p);
            }
            if let Some(a) = e.attachment {
                assert_eq!(
                    unsafe {
                        (a.trace)(
                            a.data as *mut c_void,
                            visit,
                            (&mut work as *mut Vec<Value>).cast(),
                        )
                    },
                    0
                );
            }
        }
        let dead: Vec<_> = self
            .engine
            .heap
            .iter()
            .filter(|(id, e)| !seen.contains(id) && e.attachment.is_some())
            .map(|(id, e)| (*id, e.attachment.clone().unwrap()))
            .collect();
        let count = dead.len();
        for (id, a) in dead {
            unsafe { h(self.cx) }.heap.get_mut(&id).unwrap().attachment = None;
            unsafe { (a.finalize)(self.cx, a.data as *mut c_void) };
        }
        count
    }
}
impl Drop for Host {
    fn drop(&mut self) {
        let attachments: Vec<_> = self
            .engine
            .heap
            .values_mut()
            .filter_map(|e| e.attachment.take())
            .collect();
        for a in attachments {
            unsafe { (a.finalize)(self.cx, a.data as *mut c_void) };
        }
        unsafe { nsh_web_destroy(self.cx) };
        assert_eq!(
            self.engine.reserved_ops, 0,
            "must not inspect engine Promise brand/state"
        );
        if !std::thread::panicking() {
            assert!(
                self.engine.refs.values().all(|n| *n >= 0),
                "over-released host handles: {:?}",
                self.engine
                    .refs
                    .iter()
                    .filter(|(_, n)| **n < 0)
                    .collect::<Vec<_>>()
            );
        }
    }
}

#[test]
fn registers_real_headers_and_rejects_invalid_header_names() {
    let h = Host::new();
    let o = h.new_class("Headers", &[]);
    h.method(o, "append", &[h.str("X-Test"), h.str(" one ")]);
    h.method(o, "append", &[h.str("x-test"), h.str("two")]);
    assert_eq!(
        text(h.cx, h.method(o, "get", &[h.str("X-TEST")])),
        "one, two"
    );
    let invalid = call(
        h.cx,
        get(h.cx, o, "append"),
        o,
        &[h.str("bad header"), h.str("x")],
    );
    assert_eq!(invalid.tag, 8);
}
#[test]
fn request_normalizes_method_and_relative_url_and_response_clones_bytes() {
    let h = Host::new();
    assert_eq!(
        unsafe { nsh_web_set_base_url(h.cx, b"https://example.test/base/".as_slice().into()) },
        0
    );
    let init = object(h.cx);
    put(h.cx, init, "method", h.str("post"));
    put(h.cx, init, "body", h.str("body"));
    let req = h.new_class("Request", &[h.str("../submit"), init]);
    assert_eq!(text(h.cx, h.get(req, "url")), "https://example.test/submit");
    assert_eq!(text(h.cx, h.get(req, "method")), "POST");
    let res = h.new_class("Response", &[h.str("hello")]);
    let clone = h.method(res, "clone", &[]);
    assert_eq!(text(h.cx, h.settled(h.method(res, "text", &[])).1), "hello");
    assert_eq!(
        text(h.cx, h.settled(h.method(clone, "text", &[])).1),
        "hello"
    );
    assert_eq!(h.get(res, "bodyUsed"), Value::boolean(true));
    assert_eq!(h.settled(h.method(res, "text", &[])).0, 2);
}
#[test]
fn readable_stream_enqueues_reads_closes_and_rejects_second_reader() {
    let h = Host::new();
    let source = object(h.cx);
    let start = function(
        h.cx,
        F::Builtin(|cx, _, args| {
            let controller = args[0];
            let value = string(cx, "chunk");
            call(cx, get(cx, controller, "enqueue"), controller, &[value]);
            call(cx, get(cx, controller, "close"), controller, &[]);
            U
        }),
    );
    put(h.cx, source, "start", start);
    let stream = h.new_class("ReadableStream", &[source]);
    let reader = h.method(stream, "getReader", &[]);
    let bad = call(h.cx, get(h.cx, stream, "getReader"), stream, &[]);
    assert_eq!(bad.tag, 8);
    let (state, result) = h.settled(h.method(reader, "read", &[]));
    assert_eq!(state, 1);
    assert_eq!(text(h.cx, h.get(result, "value")), "chunk");
    assert_eq!(h.get(result, "done"), Value::boolean(false));
    let (_, result) = h.settled(h.method(reader, "read", &[]));
    assert_eq!(h.get(result, "done"), Value::boolean(true));
}
#[test]
fn deferred_writer_settlement_uses_no_engine_state_queries() {
    let h = Host::new();
    let stream = h.new_class("WritableStream", &[]);
    let writer = h.method(stream, "getWriter", &[]);
    assert_eq!(h.settled(h.get(writer, "ready")).0, 1);
    assert_eq!(h.settled(h.method(writer, "write", &[h.str("x")])).0, 1);
    assert_eq!(h.settled(h.method(writer, "close", &[])).0, 1);
    assert_eq!(h.settled(h.get(writer, "closed")).0, 1);
}
#[test]
fn rust_native_js_cycles_are_edges_not_permanent_roots() {
    let h = Host::new();
    let target = h.new_class("EventTarget", &[]);
    let listener = js::closure(h.cx, vec![V::dup(h.cx, target)], |_, _, _| {
        Ok(V::undefined())
    })
    .unwrap();
    h.method(
        target,
        "addEventListener",
        &[h.str("cycle"), listener.raw()],
    );
    drop(listener);
    let before = unsafe { (*h.cx).natives.borrow().len() };
    assert!(h.collect(&[]) >= 2);
    let after = unsafe { (*h.cx).natives.borrow().len() };
    assert!(after + 2 <= before);
}
#[test]
fn websocket_binary_transport_and_close_release_callback_root() {
    let h = Host::new();
    let ws = h.new_class("WebSocket", &[h.str("wss://example.test/socket")]);
    let ops = std::mem::take(&mut unsafe { (*h.cx).sockets.borrow_mut() }.ops);
    assert_eq!(ops.len(), 1);
    let id = ops[0].id;
    assert_eq!(ops[0].op, 1);
    assert_eq!(
        unsafe { nsh_web_ws_event(h.cx, id, 1, b"".as_slice().into(), 0, 0) },
        0
    );
    assert_eq!(h.get(ws, "readyState").as_number(), 1.);
    let binary = js::array_buffer(h.cx, &[0, 255, 128]).unwrap();
    h.method(ws, "send", &[binary.raw()]);
    let ops = std::mem::take(&mut unsafe { (*h.cx).sockets.borrow_mut() }.ops);
    assert_eq!(ops[0].op, 3);
    assert_eq!(ops[0].bytes, [0, 255, 128]);
    h.method(ws, "close", &[Value::number(1000.), h.str("done")]);
    assert_eq!(
        unsafe { nsh_web_ws_event(h.cx, id, 4, b"done".as_slice().into(), 1000, 1) },
        0
    );
    assert_eq!(h.get(ws, "readyState").as_number(), 3.);
    assert!(unsafe { (*h.cx).sockets.borrow() }.active.is_empty());
}

fn http_event(h: &Host, id: u32, event: u32, status: u16, body: &[u8]) -> i32 {
    let e = HttpEvent {
        id,
        event,
        status,
        status_text: b"OK".as_slice().into(),
        url: b"https://example.test/final".as_slice().into(),
        headers: std::ptr::null(),
        headers_len: 0,
        bytes: body.into(),
        redirected: 0,
    };
    unsafe { nsh_web_http_event(h.cx, &e) }
}
#[derive(Debug)]
struct CopiedOp {
    id: u32,
    op: u32,
    bytes: Vec<u8>,
    body_kind: u32,
    url: Vec<u8>,
}
fn drain_http(h: &Host) -> Vec<CopiedOp> {
    unsafe extern "C" fn visit(user: *mut c_void, op: *const HttpOp) {
        let o = unsafe { &*op };
        unsafe { &mut *(user as *mut Vec<CopiedOp>) }.push(CopiedOp {
            id: o.id,
            op: o.op,
            bytes: unsafe { o.bytes.slice() }.to_vec(),
            body_kind: o.body_kind,
            url: unsafe { o.url.slice() }.to_vec(),
        });
    }
    let mut out = Vec::new();
    unsafe { nsh_web_drain_http(h.cx, visit, (&mut out as *mut Vec<CopiedOp>).cast()) };
    out
}
fn fetch(h: &Host, url: &str, init: Value) -> Value {
    let f = get(h.cx, h.engine.global, "fetch");
    h.check(call(h.cx, f, U, &[h.str(url), init]))
}
#[test]
fn streaming_upload_waits_for_bodywanted_then_emits_binary_and_finish() {
    let h = Host::new();
    let source = object(h.cx);
    let start = function(
        h.cx,
        F::Builtin(|cx, _, args| {
            let c = args[0];
            let bytes = js::uint8_array(cx, &[0, 255, 128]).unwrap();
            call(cx, get(cx, c, "enqueue"), c, &[bytes.raw()]);
            call(cx, get(cx, c, "close"), c, &[]);
            U
        }),
    );
    put(h.cx, source, "start", start);
    let stream = h.new_class("ReadableStream", &[source]);
    let init = object(h.cx);
    put(h.cx, init, "method", h.str("POST"));
    put(h.cx, init, "body", stream);
    put(h.cx, init, "duplex", h.str("half"));
    let p = fetch(&h, "https://example.test/upload", init);
    pump(h.cx);
    let ops = drain_http(&h);
    assert_eq!(ops.len(), 1);
    assert_eq!((ops[0].op, ops[0].body_kind), (1, 2));
    let id = ops[0].id;
    assert_eq!(entry(h.cx, p).promise.unwrap().0, 0);
    assert!(drain_http(&h).is_empty());
    assert_eq!(http_event(&h, id, 5, 0, &[]), 0);
    pump(h.cx);
    let ops = drain_http(&h);
    assert_eq!(ops.len(), 1);
    assert_eq!(ops[0].op, 2);
    assert_eq!(ops[0].bytes, [0, 255, 128]);
    assert_eq!(http_event(&h, id, 5, 0, &[]), 0);
    pump(h.cx);
    let ops = drain_http(&h);
    assert_eq!(ops.len(), 1);
    assert_eq!(ops[0].op, 3);
    assert_eq!(http_event(&h, id, 1, 200, &[]), 0);
    let (s, response) = h.settled(p);
    assert_eq!(s, 1);
    let body = h.method(response, "bytes", &[]);
    assert_eq!(http_event(&h, id, 2, 0, &[0, 255, 128]), 0);
    assert_eq!(http_event(&h, id, 3, 0, &[]), 0);
    let (s, value) = h.settled(body);
    assert_eq!(s, 1);
    assert_eq!(js::buffer_source(h.cx, value), Some(vec![0, 255, 128]));
}
#[test]
fn queued_fetch_abort_removes_start_and_rejects_without_transport() {
    let h = Host::new();
    let controller = h.new_class("AbortController", &[]);
    let init = object(h.cx);
    put(h.cx, init, "signal", h.get(controller, "signal"));
    let p = fetch(&h, "https://example.test/cancel", init);
    h.method(controller, "abort", &[h.str("stop")]);
    assert!(drain_http(&h).is_empty());
    let (s, reason) = h.settled(p);
    assert_eq!(s, 2);
    assert_eq!(text(h.cx, reason), "stop");
}
#[test]
fn response_body_backpressure_emits_pause_and_reader_pull_resumes() {
    let h = Host::new();
    let p = fetch(&h, "https://example.test/stream", U);
    let ops = drain_http(&h);
    let id = ops[0].id;
    assert_eq!(ops[0].url, b"https://example.test/stream");
    assert_eq!(http_event(&h, id, 1, 200, &[]), 0);
    let (_, response) = h.settled(p);
    let bytes = vec![42; (1 << 20) + 1];
    assert_eq!(http_event(&h, id, 2, 0, &bytes), 0);
    let ops = drain_http(&h);
    assert!(ops.iter().any(|o| o.op == 5));
    let stream = h.get(response, "body");
    let reader = h.method(stream, "getReader", &[]);
    let (_, result) = h.settled(h.method(reader, "read", &[]));
    assert_eq!(
        js::buffer_source(h.cx, h.get(result, "value")).unwrap(),
        bytes
    );
    let pending = h.method(reader, "read", &[]);
    pump(h.cx);
    let ops = drain_http(&h);
    assert!(ops.iter().any(|o| o.op == 6));
    assert_eq!(http_event(&h, id, 3, 0, &[]), 0);
    let (_, end) = h.settled(pending);
    assert_eq!(h.get(end, "done"), Value::boolean(true));
}
#[test]
fn network_response_headers_are_immutable_and_transport_errors_reject() {
    let h = Host::new();
    let p = fetch(&h, "https://example.test/headers", U);
    let id = drain_http(&h)[0].id;
    let name = b"X-From-Server";
    let value = b"abc";
    let header = Header {
        name: name.as_slice().into(),
        value: value.as_slice().into(),
    };
    let e = HttpEvent {
        id,
        event: 1,
        status: 200,
        status_text: b"OK".as_slice().into(),
        url: b"https://example.test/headers".as_slice().into(),
        headers: &header,
        headers_len: 1,
        bytes: Bytes::default(),
        redirected: 0,
    };
    assert_eq!(unsafe { nsh_web_http_event(h.cx, &e) }, 0);
    let (_, response) = h.settled(p);
    let headers = h.get(response, "headers");
    assert_eq!(
        text(h.cx, h.method(headers, "get", &[h.str("x-from-server")])),
        "abc"
    );
    assert_eq!(
        call(
            h.cx,
            get(h.cx, headers, "set"),
            headers,
            &[h.str("x-from-server"), h.str("changed")]
        )
        .tag,
        8
    );
    assert_eq!(http_event(&h, id, 3, 0, &[]), 0);
    let p = fetch(&h, "https://example.test/error", U);
    let id = drain_http(&h)[0].id;
    assert_eq!(http_event(&h, id, 4, 0, b"connection lost"), 0);
    assert_eq!(h.settled(p).0, 2);
}
#[test]
fn deferred_pending_flag_is_shared_and_settles_with_capability() {
    let h = Host::new();
    let d = js::Deferred::new(h.cx).unwrap();
    let clone = d.clone_ref();
    assert!(d.is_pending());
    assert!(clone.is_pending());
    d.resolve(h.cx, U);
    assert!(!d.is_pending());
    assert!(!clone.is_pending());
    let rejected = js::Deferred::new(h.cx).unwrap();
    rejected.reject(h.cx, h.str("failed"));
    assert!(!rejected.is_pending());
}
#[test]
fn byte_stream_byob_transfers_buffer_and_reports_partial_read() {
    let h = Host::new();
    let source = object(h.cx);
    put(h.cx, source, "type", h.str("bytes"));
    let start = function(
        h.cx,
        F::Builtin(|cx, _, args| {
            let c = args[0];
            let chunk = js::uint8_array(cx, &[10, 20, 30]).unwrap();
            call(cx, get(cx, c, "enqueue"), c, &[chunk.raw()]);
            call(cx, get(cx, c, "close"), c, &[]);
            U
        }),
    );
    put(h.cx, source, "start", start);
    let stream = h.new_class("ReadableStream", &[source]);
    let opts = object(h.cx);
    put(h.cx, opts, "mode", h.str("byob"));
    let reader = h.method(stream, "getReader", &[opts]);
    let dest = js::uint8_array(h.cx, &[0; 8]).unwrap();
    let p = h.method(reader, "read", &[dest.raw()]);
    let (s, result) = h.settled(p);
    assert_eq!(s, 1);
    assert_eq!(
        js::buffer_source(h.cx, h.get(result, "value")),
        Some(vec![10, 20, 30])
    );
    let view = js::view(h.cx, dest.raw()).unwrap();
    assert!(js::is_detached(h.cx, view.buffer.raw()));
}
#[test]
fn websocket_transport_adapter_preserves_upstream_url_and_send_validation() {
    let h = Host::new();
    let ctor = get(h.cx, h.engine.global, "WebSocket");
    for url in [
        "https://example.test/",
        "wss://example.test/#fragment",
        "wss://user@example.test/",
    ] {
        assert_eq!(
            construct(h.cx, ctor, &[h.str(url)]).tag,
            8,
            "URL must throw synchronously: {url}"
        );
    }
    let ws = h.new_class("WebSocket", &[h.str("wss://example.test/")]);
    let id = unsafe { (*h.cx).sockets.borrow() }.ops.last().unwrap().id;
    assert_eq!(
        unsafe { nsh_web_ws_event(h.cx, id, 1, Bytes::default(), 0, 0) },
        0
    );
    assert_eq!(
        call(h.cx, get(h.cx, ws, "send"), ws, &[Value::number(42.)]).tag,
        8
    );
    let large = js::array_buffer(h.cx, &vec![0; 8 * 1024 * 1024 + 1]).unwrap();
    assert_eq!(call(h.cx, get(h.cx, ws, "send"), ws, &[large.raw()]).tag, 8);
}
#[test]
fn rust_closure_prototype_uses_captured_intrinsic_after_global_function_override() {
    let h = Host::new();
    let replacement = object(h.cx);
    let other = object(h.cx);
    put(h.cx, replacement, "prototype", other);
    put(h.cx, h.engine.global, "Function", replacement);
    let f = js::closure(h.cx, Vec::new(), |_, _, _| Ok(V::undefined())).unwrap();
    assert_eq!(entry(h.cx, f.raw()).proto, h.engine.function_proto);
}

#[test]
fn context_teardown_with_open_websocket_releases_callback_before_census() {
    let h = Host::new();
    let _socket = h.new_class("WebSocket", &[h.str("wss://example.test/open-at-teardown")]);
    assert_eq!(unsafe { (*h.cx).sockets.borrow() }.active.len(), 1);
    // Host::drop finalizes native attachments, then destroys context. The active
    // callback must untrack while the ownership census is still allocated.
}

#[test]
fn review_r1_writer_release_replaces_closed_after_close_settles() {
    let h = Host::new();
    let stream = h.new_class("WritableStream", &[]);
    let w = h.method(stream, "getWriter", &[]);
    assert_eq!(h.settled(h.method(w, "close", &[])).0, 1);
    let old = h.get(w, "closed");
    assert_eq!(h.settled(old).0, 1);
    h.method(w, "releaseLock", &[]);
    let new = h.get(w, "closed");
    assert_ne!(
        new, old,
        "release must replace a fulfilled closed capability"
    );
    assert_eq!(h.settled(new).0, 2);
    assert_eq!(h.settled(old).0, 1);
}
#[test]
fn review_r1_writer_release_replaces_ready_after_backpressure_drains() {
    let h = Host::new();
    let stream = h.new_class("WritableStream", &[]);
    let w = h.method(stream, "getWriter", &[]);
    let write = h.method(w, "write", &[h.str("chunk")]);
    let old = h.get(w, "ready");
    assert_eq!(h.settled(write).0, 1);
    assert_eq!(h.settled(old).0, 1);
    h.method(w, "releaseLock", &[]);
    let new = h.get(w, "ready");
    assert_ne!(
        new, old,
        "release must replace a ready capability fulfilled by queue drain"
    );
    assert_eq!(h.settled(new).0, 2);
}
#[test]
fn review_r1_writer_release_replaces_ready_resolved_by_close() {
    let h = Host::new();
    let strategy = object(h.cx);
    put(h.cx, strategy, "highWaterMark", Value::number(0.));
    let stream = h.new_class("WritableStream", &[U, strategy]);
    let w = h.method(stream, "getWriter", &[]);
    let old = h.get(w, "ready");
    assert_eq!(h.settled(h.method(w, "close", &[])).0, 1);
    assert_eq!(h.settled(old).0, 1);
    h.method(w, "releaseLock", &[]);
    let new = h.get(w, "ready");
    assert_ne!(new, old);
    assert_eq!(h.settled(new).0, 2);
}
#[test]
fn review_r1_writer_release_replaces_prior_closed_rejection_reason() {
    let h = Host::new();
    let sink = object(h.cx);
    let write = function(
        h.cx,
        F::Builtin(|cx, _, _| {
            let reason = string(cx, "sink failed");
            unsafe { self::h(cx) }.exception = reason;
            Value::EXCEPTION
        }),
    );
    put(h.cx, sink, "write", write);
    let stream = h.new_class("WritableStream", &[sink]);
    let w = h.method(stream, "getWriter", &[]);
    assert_eq!(h.settled(h.method(w, "write", &[h.str("chunk")])).0, 2);
    let old = h.get(w, "closed");
    assert_eq!(h.settled(old).0, 2);
    h.method(w, "releaseLock", &[]);
    let new = h.get(w, "closed");
    assert_ne!(new, old);
    let (s, reason) = h.settled(new);
    assert_eq!(s, 2);
    assert_eq!(text(h.cx, reason), "the writer was released");
}
#[test]
fn review_r1_record_keeps_lone_surrogate_key_for_property_lookup() {
    let h = Host::new();
    let input = object(h.cx);
    let value = h.str("x");
    unsafe { self::h(h.cx) }
        .heap
        .get_mut(&input.payload)
        .unwrap()
        .raw_props
        .insert(vec![0xed, 0xa0, 0x80], value);
    let params = h.new_class("URLSearchParams", &[input]);
    assert_eq!(text(h.cx, h.method(params, "toString", &[])), "%EF%BF%BD=x");
}
#[test]
fn review_r1_record_does_not_alias_surrogate_and_replacement_character_properties() {
    let h = Host::new();
    let input = object(h.cx);
    let value = h.str("surrogate");
    unsafe { self::h(h.cx) }
        .heap
        .get_mut(&input.payload)
        .unwrap()
        .raw_props
        .insert(vec![0xed, 0xa0, 0x80], value);
    put(h.cx, input, "\u{fffd}", h.str("replacement"));
    let record = js::record(h.cx, input).unwrap();
    let mut values: Vec<_> = record
        .iter()
        .map(|(k, v)| {
            assert_eq!(k, "\u{fffd}");
            text(h.cx, v.raw())
        })
        .collect();
    values.sort();
    assert_eq!(values, ["replacement", "surrogate"]);
}

#[test]
fn review_r1_record_lookup_uses_captured_reflect_after_app_override() {
    let h = Host::new();
    let input = object(h.cx);
    let value = h.str("x");
    unsafe { self::h(h.cx) }
        .heap
        .get_mut(&input.payload)
        .unwrap()
        .raw_props
        .insert(vec![0xed, 0xa0, 0x80], value);
    let replaced = object(h.cx);
    let wrong = function(h.cx, F::Builtin(|cx, _, _| string(cx, "wrong")));
    put(h.cx, replaced, "get", wrong);
    put(h.cx, h.engine.global, "Reflect", replaced);
    let params = h.new_class("URLSearchParams", &[input]);
    assert_eq!(text(h.cx, h.method(params, "toString", &[])), "%EF%BF%BD=x");
}
