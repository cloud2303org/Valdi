#!/usr/bin/env python3
"""Verify retained originals and produce a reviewable, deterministic port patch."""
import difflib, hashlib, json, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[1]
manifest=json.loads((root/'upstream/SHA256.json').read_text())
for rel,expected in manifest['files'].items():
    path=root/'upstream/web'/rel
    assert hashlib.sha256(path.read_bytes()).hexdigest()==expected,rel
patch=[]
for original in sorted((root/'upstream/web').rglob('*.rs')):
    rel=original.relative_to(root/'upstream/web')
    adapted=root/'src/web'/rel
    patch.extend(difflib.unified_diff(original.read_text().splitlines(True),adapted.read_text().splitlines(True),fromfile='a/upstream/web/'+str(rel),tofile='b/src/web/'+str(rel)))
patch=''.join(patch)
if '--write-patch' in sys.argv:
    (root/'upstream/port.patch').write_text(patch)
else:
    assert (root/'upstream/port.patch').read_text()==patch,'port.patch stale; review and regenerate'
metadata=json.loads(subprocess.check_output(['cargo','metadata','--locked','--format-version','1','--manifest-path',str(root/'Cargo.toml')]))
for package in metadata['packages']:
    name=package['name'].lower()
    assert not any(s in name for s in ('quickjs','rquickjs')),name
    version=package.get('rust_version')
    if version:
        assert tuple(map(int,version.split('.')[:2])) <= (1,87),(name,version)
print('PASS:',len(manifest['files']),'pinned originals; adaptation patch matches; no QuickJS package; declared dependency MSRVs <= 1.87')
