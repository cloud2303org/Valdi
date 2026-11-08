"""Keep every writer ready/closed settlement and its Rust flag in sync."""
import re

def adapt_writer(source):
    for field,count in [('ready',2),('closed',1)]:
        pattern=(r'(?m)^(?P<i> *)let resolve = writer\(cx, w\.raw\(\)\)\?\.'+field+
                 r'\.resolve\.clone_ref\(\);\n(?P=i)js::call\(cx, resolve\.raw\(\), js::UNDEFINED, &\[js::UNDEFINED\]\)\?;')
        def replace(m):
            i=m['i']
            return (i+'let capability = writer(cx, w.raw())?.'+field+'.clone_ref();\n'+
                    i+'capability.resolve(cx, js::UNDEFINED);')
        source,n=re.subn(pattern,replace,source)
        assert n==count,(field,n,count)
    old='''        let closed = {
            let wr = writer(cx, w.raw())?;
            (wr.closed.reject.clone_ref(), wr.closed.promise.clone_ref())
        };
        js::call(cx, closed.0.raw(), js::UNDEFINED, &[err.raw()])?;
        js::mark_handled(cx, closed.1.raw());'''
    new='''        let closed = writer(cx, w.raw())?.closed.clone_ref();
        closed.reject(cx, err.raw());
        js::mark_handled(cx, closed.promise.raw());'''
    assert source.count(old)==1
    source=source.replace(old,new)
    for field in ('ready','closed'):
        old='''        let (reject, promise) = {
            let wr = writer(cx, w)?;
            (wr.FIELD.reject.clone_ref(), wr.FIELD.promise.clone_ref())
        };
        js::call(cx, reject.raw(), js::UNDEFINED, &[e])?;
        js::mark_handled(cx, promise.raw());'''.replace('FIELD',field)
        new='''        let capability = writer(cx, w)?.FIELD.clone_ref();
        capability.reject(cx, e);
        js::mark_handled(cx, capability.promise.raw());'''.replace('FIELD',field)
        assert source.count(old)==1,field
        source=source.replace(old,new)
    assert not re.search(r'\.(ready|closed)\.(resolve|reject)',source)
    return source
