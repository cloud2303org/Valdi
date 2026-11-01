"""Compiler-free tests for the internal HBC build gate."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import check_internal_bytecode as checker


class InternalBytecodeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.first = self.root / '01-Promise.js'
        self.second = self.root / '02-Other.js'
        self.first.write_bytes(b'var first = 1;\r\n')
        self.second.write_bytes(b'var second = 2;\n')
        # Deliberately reversed; validation must apply upstream filename order.
        self.sources = [self.second, self.first]
        self.version = self.root / 'BytecodeVersion.h'
        self.version.write_text('const static uint32_t BYTECODE_VERSION = 99;\n')
        self.inc = self.root / 'InternalBytecode.inc'
        data = bytearray(160)
        struct.pack_into('<QI', data, 0, checker.MAGIC, 99)
        data[12:32] = hashlib.sha1(self.first.read_bytes() + self.second.read_bytes()).digest()
        struct.pack_into('<I', data, 32, len(data) + 20)
        self.data = bytes(data) + hashlib.sha1(data).digest()
        self.write_inc(self.data)

    def write_inc(self, data):
        # xxd.py has no trailing comma on the last line.
        self.inc.write_text(',\n'.join(', '.join(f'0x{byte:02x}' for byte in data[i:i+12])
                                      for i in range(0, len(data), 12)) + '\n')

    def validate(self):
        return checker.validate(self.inc, self.version, self.sources)

    def test_matches_sorted_raw_concatenation(self):
        self.assertEqual(self.validate(), len(self.data))

    def test_modified_promise_source(self):
        self.first.write_bytes(self.first.read_bytes() + b'// changed patch\n')
        with self.assertRaisesRegex(ValueError, 'sourceHash'):
            self.validate()

    def test_added_source(self):
        extra = self.root / '03-Added.js'
        extra.write_bytes(b'var added = 3;')
        self.sources.append(extra)
        with self.assertRaisesRegex(ValueError, 'sourceHash'):
            self.validate()

    def test_removed_source(self):
        self.sources.remove(self.second)
        with self.assertRaisesRegex(ValueError, 'sourceHash'):
            self.validate()

    def test_no_newlines_inserted_between_sources(self):
        source_hash = hashlib.sha1(self.first.read_bytes() + b'\n' + self.second.read_bytes()).digest()
        data = bytearray(self.data[:-20])
        data[12:32] = source_hash
        self.write_inc(bytes(data) + hashlib.sha1(data).digest())
        with self.assertRaisesRegex(ValueError, 'sourceHash'):
            self.validate()

    def test_raw_line_endings_are_preserved(self):
        self.first.write_bytes(self.first.read_bytes().replace(b'\r\n', b'\n'))
        with self.assertRaisesRegex(ValueError, 'sourceHash'):
            self.validate()

    def test_pinned_version_changed(self):
        self.version.write_text('const static uint32_t BYTECODE_VERSION = 100;\n')
        with self.assertRaisesRegex(ValueError, 'version 99.*pinned Hermes version 100'):
            self.validate()

    def test_actual_hbc_version_changed(self):
        data = bytearray(self.data)
        struct.pack_into('<I', data, 8, 98)
        self.write_inc(data)
        with self.assertRaisesRegex(ValueError, 'version 98.*pinned Hermes version 99'):
            self.validate()

    def test_missing_version(self):
        self.version.write_text('// no version\n')
        with self.assertRaisesRegex(ValueError, 'Missing BYTECODE_VERSION'):
            self.validate()

    def test_magic_mismatch(self):
        self.write_inc(b'\x00' + self.data[1:])
        with self.assertRaisesRegex(ValueError, 'magic mismatch'):
            self.validate()

    def test_truncated_header(self):
        self.write_inc(self.data[:12])
        with self.assertRaisesRegex(ValueError, 'Truncated HBC'):
            self.validate()

    def test_truncated_file(self):
        self.write_inc(self.data[:-1])
        with self.assertRaisesRegex(ValueError, 'fileLength'):
            self.validate()

    def test_extra_bytes(self):
        self.write_inc(self.data + b'\x00')
        with self.assertRaisesRegex(ValueError, 'fileLength'):
            self.validate()

    def test_payload_checksum_mismatch(self):
        data = bytearray(self.data)
        data[140] ^= 1
        self.write_inc(data)
        with self.assertRaisesRegex(ValueError, 'footer checksum'):
            self.validate()

    def test_footer_checksum_mismatch(self):
        self.write_inc(self.data[:-1] + bytes([self.data[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, 'footer checksum'):
            self.validate()

    def test_hex_parser_rejects_unparsed_text(self):
        self.inc.write_text(self.inc.read_text() + '// fake digest 0xff\n')
        with self.assertRaisesRegex(ValueError, 'Malformed.*hex include'):
            self.validate()

    def test_hex_parser_rejects_invalid_byte(self):
        self.inc.write_text(self.inc.read_text().replace('0xc6', '0x1c6', 1))
        with self.assertRaisesRegex(ValueError, 'Malformed.*hex include'):
            self.validate()

    def test_empty_sources_rejected(self):
        self.sources = []
        with self.assertRaisesRegex(ValueError, 'No InternalJavaScript'):
            self.validate()

    def test_duplicate_sources_rejected(self):
        self.sources.append(self.first)
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.validate()

    def test_cli_writes_stamp_only_on_success(self):
        stamp = self.root / 'checked.h'
        command = [sys.executable, str(Path(checker.__file__).resolve()), '--inc', str(self.inc),
                   '--bytecode-version-header', str(self.version), '--stamp', str(stamp)]
        for source in self.sources:
            command += ['--source', str(source)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#pragma once', stamp.read_text())
        stamp.unlink()
        self.first.write_bytes(b'changed Promise protocol')
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Stale InternalBytecode.inc', result.stderr)
        self.assertFalse(stamp.exists())


if __name__ == '__main__':
    unittest.main()
