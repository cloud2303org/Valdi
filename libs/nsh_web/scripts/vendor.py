#!/usr/bin/env python3
"""Copy exact pinned upstream Web algorithms; adaptations live in port.patch."""
import hashlib, json, pathlib, shutil, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/valdi-nsh-integration-source-20261107')
pin = '701bdcee9512e0f6aed39042d1fe24564dcd0b43'
assert subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'], text=True).strip() == pin
files = ['../standard/websocket.rs','../standard/websocket_native.rs','event.rs','url.rs','encoding.rs','blob.rs','fetch.rs','websocket.rs','iterator.rs','js.rs','mod.rs', 'streams/mod.rs','streams/readable.rs','streams/writable.rs','streams/transform.rs','streams/text.rs']
manifest = {}
for name in files:
    src = source/'crates/nsh-js/src/web'/name
    dst = root/'upstream/web'/name
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src,dst)
    manifest[name] = hashlib.sha256(src.read_bytes()).hexdigest()
shutil.copyfile(source/'LICENSE-APACHE', root/'LICENSE-APACHE')
(root/'upstream/SHA256.json').write_text(json.dumps({'commit':pin, 'files':manifest},indent=2)+'\n')
print('Verified and copied',len(files),'files at',pin)
