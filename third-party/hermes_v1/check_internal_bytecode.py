#!/usr/bin/env python3
"""Check InternalBytecode.inc against the actual patched Hermes repository inputs.

Compiler-free build gate. Match upstream InternalJavaScript/CMakeLists.txt:
concatenate sorted *.js byte-for-byte, with no separators. The HBC sourceHash
must equal SHA-1 of those bytes; no generated comment/sidecar digest is trusted.
Regeneration remains an explicit regenerate_internal_bytecode.py action.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct


MAGIC = 0x1F1903C103BC1FC6
HEADER_SIZE = 128
FOOTER_SIZE = 20


def read_include(path):
    text = path.read_text()
    # Upstream xxd.py emits only comma-separated literals, without C++ wrappers.
    if not re.fullmatch(r'\s*0x[0-9a-fA-F]{2}(?:\s*,\s*0x[0-9a-fA-F]{2})*\s*,?\s*', text):
        raise ValueError(f'Malformed internal bytecode hex include: {path}')
    return bytes(int(byte, 16) for byte in re.findall(r'0x([0-9a-fA-F]{2})', text))


def validate(inc, version_header, sources):
    if not sources:
        raise ValueError('No InternalJavaScript sources supplied')
    if len({path.name for path in sources}) != len(sources):
        raise ValueError('Duplicate InternalJavaScript source names')
    if any(path.suffix != '.js' for path in sources):
        raise ValueError('InternalJavaScript sources must be .js files')
    version_match = re.search(r'\bBYTECODE_VERSION\s*=\s*(\d+)\s*;', version_header.read_text())
    if not version_match:
        raise ValueError(f'Missing BYTECODE_VERSION in pinned Hermes header: {version_header}')
    expected_version = int(version_match.group(1))
    data = read_include(inc)
    if len(data) < HEADER_SIZE + FOOTER_SIZE:
        raise ValueError('Truncated HBC header/footer')
    magic, version = struct.unpack_from('<QI', data)
    if magic != MAGIC:
        raise ValueError(f'HBC magic mismatch: {magic:#x}')
    if version != expected_version:
        raise ValueError(f'HBC version {version} does not match pinned Hermes version {expected_version}')
    # BytecodeFileHeader: magic:u64, version:u32, sourceHash:20, fileLength:u32.
    file_length = struct.unpack_from('<I', data, 32)[0]
    if file_length != len(data):
        raise ValueError(f'HBC fileLength {file_length} does not match embedded length {len(data)}')
    if hashlib.sha1(data[:-FOOTER_SIZE]).digest() != data[-FOOTER_SIZE:]:
        raise ValueError('HBC footer checksum mismatch')
    source_hash = hashlib.sha1()
    for source in sorted(sources, key=lambda path: path.name):
        source_hash.update(source.read_bytes())
    if data[12:32] != source_hash.digest():
        raise ValueError('Stale InternalBytecode.inc: HBC sourceHash does not match sorted patched '
                         'InternalJavaScript/*.js; run regenerate_internal_bytecode.py with matching hermesc')
    return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inc', type=Path, required=True)
    parser.add_argument('--bytecode-version-header', type=Path, required=True)
    parser.add_argument('--source', type=Path, action='append', required=True)
    parser.add_argument('--stamp', type=Path)
    args = parser.parse_args()
    size = validate(args.inc, args.bytecode_version_header, args.source)
    if args.stamp:
        args.stamp.write_text('// Internal HBC validated against patched Hermes sources and version.\n#pragma once\n')
    print(f'Checked {args.inc}: {size} HBC bytes, {len(args.source)} patched JS inputs')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
