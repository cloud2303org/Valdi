#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'upstream/web/js.rs').read_text()
a=s.index('use std::{'); b=s.index('/// What values an object owns')
s=s[:a]+'''use std::{ffi::{CStr, c_int}, ptr};
use crate::engine::{self, Op};
pub(crate) type Ctx = *mut crate::NshWebContext;
pub(crate) type Raw = engine::Value;
pub(crate) type Native = unsafe extern "C" fn(Ctx, Raw, c_int, *mut Raw) -> Raw;
pub(crate) const UNDEFINED: Raw = Raw::UNDEFINED;
pub(crate) const NULL: Raw = Raw::NULL;
#[derive(Debug, Clone, Copy)] pub(crate) struct Throw;
pub(crate) type R<T> = Result<T, Throw>;

/// Owns one host handle and one entry in the Rust ownership census. Handle
/// references preserve ID lifetime; the census and Trace provide GC roots/edges.
pub(crate) struct V { cx: Ctx, raw: Raw }
impl Drop for V {
    fn drop(&mut self) {
        if self.raw.heap() {
            unsafe { (*self.cx).untrack(self.raw); }
            engine::release(self.cx, self.raw);
        }
    }
}
impl V {
    pub(crate) fn own(cx: Ctx, raw: Raw) -> V {
        if raw.heap() { unsafe { (*cx).track(raw); } }
        V { cx, raw }
    }
    pub(crate) fn dup(cx: Ctx, raw: Raw) -> V { engine::retain(cx,raw); Self::own(cx,raw) }
    pub(crate) fn checked(cx: Ctx, raw: Raw) -> R<V> {
        if raw.tag == 8 { Err(Throw) } else { Ok(Self::own(cx,raw)) }
    }
    pub(crate) const fn undefined() -> V { V { cx:ptr::null_mut(),raw:UNDEFINED } }
    pub(crate) const fn null() -> V { V { cx:ptr::null_mut(),raw:NULL } }
    pub(crate) fn bool(b:bool) -> V { V {cx:ptr::null_mut(),raw:Raw::boolean(b)} }
    pub(crate) fn num(n:f64) -> V { V {cx:ptr::null_mut(),raw:Raw::number(n)} }
    pub(crate) fn raw(&self) -> Raw { self.raw }
    pub(crate) fn take(self) -> Raw {
        if self.raw.heap() { unsafe { (*self.cx).untrack(self.raw); } }
        let raw=self.raw; std::mem::forget(self); raw
    }
    pub(crate) fn clone_ref(&self) -> V { Self::dup(self.cx,self.raw) }
    pub(crate) fn take_out(&mut self) -> V { std::mem::take(self) }
    pub(crate) fn is_undefined(&self)->bool { is_undefined(self.raw) }
    pub(crate) fn is_nullish(&self)->bool { is_nullish(self.raw) }
}
impl Default for V { fn default()->V { V::undefined() } }
impl std::fmt::Debug for V {
    fn fmt(&self,f:&mut std::fmt::Formatter<'_>)->std::fmt::Result { self.raw.fmt(f) }
}
fn operation(cx:Ctx, op:Op, receiver:Raw, args:&[Raw], text:&[u8], aux:i64)->R<V> {
    V::checked(cx,engine::op(cx,op,receiver,args,text,aux))
}
''' + s[b:]
def replace_fn(name,new):
    global s
    import re
    match=re.search(r'(?m)^(?:pub\(crate\) )?(?:unsafe )?(?:extern "C" )?fn '+name+r"(?:<'[^>]*>)?\(",s)
    assert match, name
    start=match.start(); brace=s.index('{',match.end()); depth=1; end=brace+1
    while depth:
        if s[end]=='{':depth+=1
        elif s[end]=='}':depth-=1
        end+=1
    s=s[:start]+new+s[end:]
replace_fn('is_undefined','pub(crate) fn is_undefined(v: Raw)->bool { v.tag == 0 }')
replace_fn('is_nullish','pub(crate) fn is_nullish(v: Raw)->bool { v.tag <= 1 }')
replace_fn('is_object','pub(crate) fn is_object(v: Raw)->bool { v.tag == 5 }')
replace_fn('is_function','pub(crate) fn is_function(cx: Ctx,v:Raw)->bool { engine::op(cx,Op::IsFunction,v,&[],&[],0).payload == 1 }')
replace_fn('same', '''pub(crate) fn same(a:Raw,b:Raw)->bool {
    if a.tag == 3 && b.tag == 3 { let (a,b)=(a.as_number(),b.as_number());
        return (a.is_nan() && b.is_nan()) || (a == b && (a != 0.0 || a.to_bits() == b.to_bits())); }
    a == b
}''')
replace_fn('string','pub(crate) fn string(cx:Ctx,s:&str)->R<V> { operation(cx,Op::StringNew,UNDEFINED,&[],s.as_bytes(),0) }')
replace_fn('to_string','pub(crate) fn to_string(cx:Ctx,v:Raw)->R<String> { Ok(from_wtf8(&to_wtf8(cx,v)?)) }')
replace_fn('to_wtf8','''pub(crate) fn to_wtf8(cx:Ctx,v:Raw)->R<Vec<u8>> {
    let e=unsafe{(*cx).engine}; let mut b=engine::Bytes::default();
    if unsafe{(e.bytes)(e.user,cx,v,1,&mut b)} < 1 { return Err(Throw); }
    Ok(unsafe{b.slice()}.to_vec())
}''')
replace_fn('to_number','pub(crate) fn to_number(cx:Ctx,v:Raw)->R<f64> { operation(cx,Op::ToNumber,v,&[],&[],0).map(|n| n.raw().as_number()) }')
replace_fn('to_bool','pub(crate) fn to_bool(cx:Ctx,v:Raw)->bool { engine::op(cx,Op::ToBool,v,&[],&[],0).payload == 1 }')
replace_fn('new_object','pub(crate) fn new_object(cx:Ctx)->R<V> { operation(cx,Op::ObjectNew,UNDEFINED,&[],&[],0) }')
replace_fn('new_array','''pub(crate) fn new_array(cx:Ctx,items:Vec<V>)->R<V> {
    let raws:Vec<_>=items.iter().map(V::raw).collect();
    operation(cx,Op::ArrayNew,UNDEFINED,&raws,&[],0)
}''')
replace_fn('global','pub(crate) fn global(cx:Ctx)->V { V::own(cx,engine::op(cx,Op::Global,UNDEFINED,&[],&[],0)) }')
replace_fn('cstr','')
replace_fn('get','pub(crate) fn get(cx:Ctx,obj:Raw,key:&str)->R<V> { operation(cx,Op::Get,obj,&[],key.as_bytes(),0) }')
replace_fn('get_atom','pub(crate) fn get_atom(cx:Ctx,obj:Raw,atom:u32)->R<V> { operation(cx,Op::GetSymbol,obj,&[],&[],atom as i64) }')
replace_fn('define','''pub(crate) fn define(cx:Ctx,obj:Raw,key:&str,value:V,enumerable:bool)->R<()> {
    define_flags(cx,obj,key,value,5 | if enumerable {2} else {0})
}
pub(crate) fn define_flags(cx:Ctx,obj:Raw,key:&str,value:V,flags:i64)->R<()> {
    operation(cx,Op::Define,obj,&[value.raw()],key.as_bytes(),flags).map(drop)
}''')
replace_fn('call','''pub(crate) fn call(cx:Ctx,f:Raw,this:Raw,args:&[Raw])->R<V> {
    let mut a=Vec::with_capacity(args.len()+1); a.push(this); a.extend_from_slice(args);
    operation(cx,Op::Call,f,&a,&[],0)
}''')
replace_fn('construct','pub(crate) fn construct(cx:Ctx,f:Raw,args:&[Raw])->R<V> { operation(cx,Op::Construct,f,args,&[],0) }')
for name,kind in [('type_error',0),('range_error',1),('syntax_error',2)]:
    replace_fn(name, f'pub(crate) fn {name}(cx:Ctx,message:&str)->Throw {{ engine::op(cx,Op::Error,UNDEFINED,&[],message.as_bytes(),{kind}); Throw }}')
replace_fn('throw','pub(crate) fn throw(cx:Ctx,value:V)->Throw { engine::op(cx,Op::Throw,value.raw(),&[],&[],0); Throw }')
replace_fn('take_exception','pub(crate) fn take_exception(cx:Ctx)->V { V::own(cx,engine::op(cx,Op::TakeException,UNDEFINED,&[],&[],0)) }')
replace_fn('rethrow','')
replace_fn('report','pub(crate) fn report(cx:Ctx,error:V) { let _=operation(cx,Op::Report,error.raw(),&[],&[],0); }')
s=s.replace('Err(_) => rquickjs::qjs::JS_EXCEPTION,','Err(_) => $crate::engine::Value::EXCEPTION,')
# Native functions must contain panic at the FFI boundary.
s=s.replace('match body(cx, this, args) {\n                Ok(v) => v.take(),\n                Err(_) => $crate::engine::Value::EXCEPTION,\n            }','''match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(cx,this,args))) {
                Ok(Ok(v)) => v.take(),
                Ok(Err(_)) => $crate::engine::Value::EXCEPTION,
                Err(_) => { $crate::web::js::type_error(cx,"Rust native panicked"); $crate::engine::Value::EXCEPTION }
            }''')
replace_fn('function','''pub(crate) fn function(cx:Ctx,_name:&CStr,f:Native,_length:c_int)->R<V> {
    closure(cx,Vec::new(),move |cx,_,args| {
        V::checked(cx,unsafe{f(cx,UNDEFINED,args.len() as c_int,args.0.as_ptr().cast_mut())})
    })
}''')
# Above plain functions must preserve this; closures' callback API excludes this,
# so use a dedicated native attachment in registration layer.
replace_fn('function','pub(crate) fn function(cx:Ctx,name:&CStr,f:Native,length:c_int)->R<V> { super::new_function(cx,name,f,length) }')
# Replace the method in Deferred's impl separately.
a=s.index('        let mut funcs = [UNDEFINED; 2];'); b=s.index('\n    /// Resolves with',a)
s=s[:a]+'''        let parts=operation(cx,Op::PromiseNew,UNDEFINED,&[],&[],0)?;
        Ok(Deferred {
            promise:get(cx,parts.raw(),"0")?, resolve:get(cx,parts.raw(),"1")?, reject:get(cx,parts.raw(),"2")?
        })
    }
''' + s[b:]
replace_fn('rejecting','''pub(crate) fn rejecting(cx:Ctx,r:Raw)->Raw {
    if r.tag != 8 { return r; }
    let e=take_exception(cx);
    if engine::op(cx,Op::IsUncatchable,e.raw(),&[],&[],0).payload == 1 { throw(cx,e); return Raw::EXCEPTION; }
    match rejected(cx,e.raw()) { Ok(p)=>p.take(), Err(_)=>Raw::EXCEPTION }
}''')
replace_fn('mark_handled','pub(crate) fn mark_handled(cx:Ctx,promise:Raw) { let _=operation(cx,Op::PromiseHandled,promise,&[],&[],0); }')
replace_fn('then','pub(crate) fn then(cx:Ctx,promise:Raw,on_ok:Raw,on_err:Raw)->R<V> { operation(cx,Op::PromiseThen,promise,&[on_ok,on_err],&[],0) }')
s=s.replace('qjs::JS_ATOM_Symbol_iterator as _','engine::ITERATOR')
replace_fn('record','''pub(crate) fn record(cx:Ctx,obj:Raw)->R<Vec<(String,V)>> {
    let keys=operation(cx,Op::Keys,obj,&[],&[],0)?;
    let len=to_number(cx,get(cx,keys.raw(),"length")?.raw())? as usize;
    let mut out=Vec::with_capacity(len);
    let getter=super::intrinsic_get(cx)?;
    for i in 0..len {
        // Lookup must use the original JS key: converting to USVString first
        // changes lone surrogates and can select a different property/getter.
        let key=get(cx,keys.raw(),&i.to_string())?;
        let value=call(cx,getter.raw(),UNDEFINED,&[obj,key.raw()])?;
        let name=to_string(cx,key.raw())?;
        out.push((name,value));
    }
    Ok(out)
}''')
replace_fn('bytes_of','''pub(crate) unsafe fn bytes_of<'a>(cx:Ctx,v:Raw)->Option<&'a [u8]> {
    let e=unsafe{(*cx).engine}; let mut b=engine::Bytes::default();
    if unsafe{(e.bytes)(e.user,cx,v,0,&mut b)} != 1 { return None; }
    Some(unsafe{b.slice()})
}''')
replace_fn('view','''pub(crate) fn view(cx:Ctx,v:Raw)->Option<View> {
    let e=unsafe{(*cx).engine}; let mut info=engine::View {buffer:UNDEFINED,offset:0,length:0,element:0,kind:-1};
    if unsafe{(e.view)(e.user,cx,v,&mut info)} != 1 { return None; }
    Some(View {buffer:V::own(cx,info.buffer),offset:info.offset,length:info.length,element:info.element,kind:if info.kind<0 {None} else {Some(info.kind)}})
}''')
replace_fn('array_buffer','pub(crate) fn array_buffer(cx:Ctx,bytes:&[u8])->R<V> { operation(cx,Op::ArrayBufferNew,UNDEFINED,&[],bytes,0) }')
replace_fn('uint8_array','pub(crate) fn uint8_array(cx:Ctx,bytes:&[u8])->R<V> { operation(cx,Op::Uint8ArrayNew,UNDEFINED,&[],bytes,0) }')
replace_fn('new_view','''pub(crate) fn new_view(cx:Ctx,kind:Option<c_int>,buffer:Raw,offset:usize,length:usize)->R<V> {
    operation(cx,Op::ViewNew,buffer,&[Raw::number(offset as f64),Raw::number(length as f64)],&[],kind.unwrap_or(-1) as i64)
}''')
s=s.replace('unsafe { qjs::JS_DetachArrayBuffer(cx, buffer) };','operation(cx,Op::Detach,buffer,&[],&[],0)?;')
replace_fn('is_detached','pub(crate) fn is_detached(cx:Ctx,buffer:Raw)->bool { engine::op(cx,Op::IsDetached,buffer,&[],&[],0).payload == 1 }')
replace_fn('transfer','''pub(crate) fn transfer(cx:Ctx,buffer:Raw)->R<V> {
    // The engine moves the backing store and detaches every old view. Never
    // borrow payload bytes across this operation or snapshot through a Vec.
    operation(cx,Op::ArrayBufferTransfer,buffer,&[],&[],0)
}''')
replace_fn('opaque','')
s=s.replace("/// Raw pointer to an object's opaque data, of either web class.",'')
s=s.replace('    pub(crate) reject: V,', '    pub(crate) reject: V,\n    pending: std::rc::Rc<std::cell::Cell<bool>>,')
s=s.replace('promise:get(cx,parts.raw(),"0")?, resolve:get(cx,parts.raw(),"1")?, reject:get(cx,parts.raw(),"2")?', 'promise:get(cx,parts.raw(),"0")?, resolve:get(cx,parts.raw(),"1")?, reject:get(cx,parts.raw(),"2")?, pending:std::rc::Rc::new(std::cell::Cell::new(true))')
s=s.replace('        let _ = call(cx, self.resolve.raw()', '        self.pending.set(false);\n        let _ = call(cx, self.resolve.raw()')
s=s.replace('        let _ = call(cx, self.reject.raw()', '        self.pending.set(false);\n        let _ = call(cx, self.reject.raw()')
s=s.replace('    /// Resolves with `value`', '    pub(crate) fn from_parts(promise:V,resolve:V,reject:V)->Self { Self {promise,resolve,reject,pending:std::rc::Rc::new(std::cell::Cell::new(true))} }\n    pub(crate) fn clone_ref(&self)->Self { Self {promise:self.promise.clone_ref(),resolve:self.resolve.clone_ref(),reject:self.reject.clone_ref(),pending:self.pending.clone()} }\n\n    /// Resolves with `value`')
s=s.replace('    /// Resolves with `value`', '    /// Used only for ready/closed capabilities settled with undefined or rejection.\n    pub(crate) fn is_pending(&self)->bool { self.pending.get() }\n\n    /// Resolves with `value`')
s=s.replace('over QuickJS\'s C\n//! API.', 'over the Valdi engine-neutral C ABI.').replace('QuickJS passes','The engine passes')
assert 'qjs::' not in s
(root/'src/web/js.rs').write_text(s)

import subprocess
subprocess.run(["rustfmt", "--edition", "2024", "--config", "skip_children=true", str(root/"src/web/js.rs")], check=True)
