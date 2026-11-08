#!/usr/bin/env python3
"""Deterministic narrow algorithm adaptations; engine replacements are separate."""
from pathlib import Path
from writer_settlement import adapt_writer
root=Path(__file__).resolve().parents[1]
for src in (root/'upstream/web').rglob('*.rs'):
    rel=src.relative_to(root/'upstream/web')
    if str(rel) in ('js.rs','mod.rs'): continue
    s=src.read_text()
    s=s.replace('rquickjs::qjs::JSTypedArrayEnum_JS_TYPED_ARRAY_UINT8', 'crate::engine::UINT8')
    s=s.replace('rquickjs::qjs::JS_ATOM_Symbol_asyncIterator', 'crate::engine::ASYNC_ITERATOR')
    s=s.replace('rquickjs::qjs::JS_ATOM_Symbol_iterator', 'crate::engine::ITERATOR')
    if str(rel)=='encoding.rs':
        s=s.replace('unsafe { rquickjs::qjs::JS_GetTypedArrayType(dest) }', 'js::view(cx, dest).and_then(|v| v.kind).unwrap_or(-1)')
    if str(rel)=='streams/writable.rs':
        s=adapt_writer(s)
        s=s.replace('fn is_pending(cx: Ctx,', 'fn is_pending(_cx: Ctx,')
        s=s.replace('unsafe {\n        rquickjs::qjs::JS_PromiseState(cx, d.promise.raw())\n            == rquickjs::qjs::JSPromiseStateEnum_JS_PROMISE_PENDING\n    }', 'd.is_pending()')
    if str(rel)=='fetch.rs':
        s=s.replace('V::checked(cx, unsafe {\n                        rquickjs::qjs::JS_ParseJSON(cx, text.as_ptr().cast(), len as _, c\"<json>\".as_ptr())\n                    })', 'V::checked(cx, crate::engine::parse_json(cx, &text[..len]))')
        s=s.replace('rquickjs::qjs::JS_ParseJSON(cx, text.as_ptr().cast(), len as _, c"<json>".as_ptr())', 'crate::engine::parse_json(cx, &text[..len])')
        s=s.replace('nsh_core::element::codes::ACCESS_DENIED','"ERR_ACCESS_DENIED"')
        s=s.replace('nsh_core::decl!(', 'crate::decl!(')
    if str(rel)=='websocket.rs':
        s=s.replace('use rquickjs::qjs;\n', '')
        s=s.replace('let r = unsafe { qjs::JS_DefinePropertyValueStr(cx, e, key.as_ptr(), value.take(), 0) };\n    if r < 0 { Err(js::Throw) } else { Ok(()) }', 'js::define_flags(cx, e, &key.to_string_lossy(), value, 0)')
        old='''            let value = match js::buffer_source(cx, data) {
                Some(bytes) if !array_buffers => {
                    super::blob::new_blob(cx, super::blob::Blob::new(bytes, String::new()))?
                }
                _ => V::dup(cx, data),
            };'''
        new='''            let value = if array_buffers {
                V::dup(cx, data)
            } else {
                // Blob owns a snapshot; ArrayBuffer delivery keeps the original
                // engine backing without creating an unused Rust Vec.
                match js::buffer_source(cx, data) {
                    Some(bytes) => super::blob::new_blob(
                        cx, super::blob::Blob::new(bytes, String::new()))?,
                    None => V::dup(cx, data),
                }
            };'''
        assert old in s
        s=s.replace(old,new,1)
    if str(rel)=='blob.rs':
        s=s.replace('nsh_core::ImageData','crate::support::SharedBytes')
        s=s.replace('nsh_core::time::unix_ms()', 'crate::support::unix_ms()')
        # NSH UI image props are not Web APIs. Blob URL storage/fetch stays intact.
        a=s.index('/// Gives the image source of `prop`')
        b=s.index('native!(\n    fn create_object_url', a)
        s=s[:a]+s[b:]
        a=s.index('    /// The key and bytes an image source of this URL shows')
        b=s.index('\n}',a)
        s=s[:a]+s[b:]
        s=s.replace('            image: None,\n','')
        s=s.replace('    sync::Arc,\n','')
    if str(rel)=='streams/readable.rs':
        s=s.replace('let d = Deferred {\n                promise: c[1].clone_ref(),\n                resolve: c[2].clone_ref(),\n                reject: c[3].clone_ref(),\n            };', 'let d = Deferred::from_parts(c[1].clone_ref(),c[2].clone_ref(),c[3].clone_ref());')
    if str(rel)=='streams/transform.rs':
        s=s.replace('Some(Deferred {\n            promise: d.promise.clone_ref(),\n            resolve: d.resolve.clone_ref(),\n            reject: d.reject.clone_ref(),\n        })', 'Some(d.clone_ref())')
    # Rust 1.87 portability: preserve the short-circuit order of let chains.
    if str(rel)=='encoding.rs':
        s=s.replace('if let Err(e) = std::str::from_utf8(bytes)\n                && e.error_len().is_none()\n            {\n                end = e.valid_up_to();\n                self.pending = bytes[end..].to_vec();\n            }', 'if let Err(e) = std::str::from_utf8(bytes) {\n                if e.error_len().is_none() {\n                    end = e.valid_up_to();\n                    self.pending = bytes[end..].to_vec();\n                }\n            }')
        s=s.replace('if stream\n            && let Some(&last) = codes.last()\n            && (0xD800..0xDC00).contains(&last)\n        {\n            codes.pop();\n            cut += 2;\n        }', 'if stream {\n            if let Some(&last) = codes.last() {\n                if (0xD800..0xDC00).contains(&last) { codes.pop(); cut += 2; }\n            }\n        }')
    if str(rel)=='streams/text.rs':
        s=s.replace('if bytes.len() >= 3\n        && let Some(last) = surrogate(&bytes[bytes.len() - 3..])\n        && (0xD800..0xDC00).contains(&last)\n    {\n        *pending = Some(last);\n        bytes = &bytes[..bytes.len() - 3];\n    }', 'if bytes.len() >= 3 {\n        if let Some(last) = surrogate(&bytes[bytes.len() - 3..]) {\n            if (0xD800..0xDC00).contains(&last) {\n                *pending = Some(last); bytes = &bytes[..bytes.len() - 3];\n            }\n        }\n    }')
    if str(rel)=='streams/transform.rs':
        s=s.replace('if backpressure == Some(true)\n        && let Some(change) = change', 'if let Some(change) = change.filter(|_|backpressure == Some(true))')
    if str(rel)=='fetch.rs':
        old='''fn on_data(cx: Ctx, id: RequestId, bytes: &[u8]) -> R<()> {'''
        new='''fn on_data(cx: Ctx, id: RequestId, bytes: &[u8]) -> R<()> {
    on_data_with(cx, id, |cx, c| readable::enqueue_bytes(cx, c, bytes))
}

/// A host-rooted Uint8Array, consumed by the ordinary byte-stream transfer and
/// queue algorithms. No borrowed Rust payload slice crosses JS callbacks.
pub(crate) fn data_buffer(cx: Ctx, id: RequestId, view: Raw) -> R<()> {
    forget_dropped(cx);
    on_data_with(cx, id, |cx, c| readable::bc_enqueue(cx, c, view))
}

fn on_data_with(
    cx: Ctx, id: RequestId, enqueue: impl FnOnce(Ctx, Raw) -> R<()>,
) -> R<()> {'''
        assert old in s
        s=s.replace(old,new,1)
        old='    readable::enqueue_bytes(cx, c.raw(), bytes)?;'
        assert s.count(old)==1
        s=s.replace(old,'    enqueue(cx, c.raw())?;',1)
        s=s.replace('else if init_body.is_undefined()\n        && let Some(b) = &base', 'else if let Some(b) = base.as_ref().filter(|_|init_body.is_undefined())')
        s=s.replace('if let Some(mime) = mime\n            && !headers(cx, h.raw())?.has("content-type")\n        {\n            headers(cx, h.raw())?.append(cx, "content-type", &mime)?;\n        }', 'if let Some(mime) = mime {\n            if !headers(cx, h.raw())?.has("content-type") {\n                headers(cx, h.raw())?.append(cx, "content-type", &mime)?;\n            }\n        }')
        s=s.replace('if !self.done.get()\n            && let Ok(mut dropped) = self.shared.dropped_fetches.try_borrow_mut()\n        {\n            dropped.push(self.id);\n        }', 'if !self.done.get() {\n            if let Ok(mut dropped) = self.shared.dropped_fetches.try_borrow_mut() {\n                dropped.push(self.id);\n            }\n        }')
        s=s.replace('if queued > QUEUE_LIMIT\n        && let Some(e) = web(cx).fetches.get_mut(&id)\n        && !e.paused\n    {\n        e.paused = true;\n        push_op(cx, id, FetchOp::Pause(true));\n    }', 'if queued > QUEUE_LIMIT {\n        if let Some(e) = web(cx).fetches.get_mut(&id) {\n            if !e.paused { e.paused = true; push_op(cx, id, FetchOp::Pause(true)); }\n        }\n    }')
    if str(rel)=='event.rs':
        s=s.replace('if let Some(s) = &signal\n            && aborted(cx, s.raw())?\n        {\n            return Ok(V::undefined());\n        }', 'if let Some(s) = &signal {\n            if aborted(cx, s.raw())? { return Ok(V::undefined()); }\n        }')
    if str(rel)=='streams/mod.rs':
        s=s.replace('if let Some(b) = bytes.borrow_mut().take()\n            && !b.is_empty()\n        {\n            readable::enqueue_bytes(cx, controller, &b)?;\n        }', 'if let Some(b) = bytes.borrow_mut().take() {\n            if !b.is_empty() { readable::enqueue_bytes(cx, controller, &b)?; }\n        }')
    if str(rel)=='streams/readable.rs':
        s=s.replace('if let Some((buffer, offset, filled)) = first\n        && filled > 0\n    {\n        bc_enqueue_cloned_chunk(cx, c, buffer.raw(), offset, filled)?;\n    }', 'if let Some((buffer, offset, filled)) = first {\n        if filled > 0 { bc_enqueue_cloned_chunk(cx, c, buffer.raw(), offset, filled)?; }\n    }')
        s=s.replace('if let Some(v) = js::view(cx, view.raw())\n            && js::is_detached(cx, v.buffer.raw())\n        {\n            return Err(js::type_error(cx, "the view\'s buffer is detached"));\n        }', 'if let Some(v) = js::view(cx, view.raw()) {\n            if js::is_detached(cx, v.buffer.raw()) {\n                return Err(js::type_error(cx, "the view\'s buffer is detached"));\n            }\n        }')
    if str(rel) in ('encoding.rs','blob.rs','url.rs','fetch.rs'):
        s+='\n#[cfg(test)] mod port_tests { use super::*; include!("../../tests/'+str(rel)+'"); }\n'
    dst=root/'src/web'/rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(s)
