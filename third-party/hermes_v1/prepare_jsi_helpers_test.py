"""Isolated validator tests; no compiler, network, or Bazel build required."""
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

import prepare_jsi_helpers as helpers


class ValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.source = self.root / 'helper.js'
        self.inc = self.root / 'helper.inc'
        self.source.write_text('globalThis.helper = 42;\n')
        data = bytearray(160)
        struct.pack_into('<QI', data, 0, helpers.MAGIC, 99)
        data[12:32] = hashlib.sha1(self.source.read_bytes()).digest()
        struct.pack_into('<I', data, 32, len(data) + 20)
        self.data = bytes(data) + hashlib.sha1(data).digest()
        self.inc.write_text(helpers.render(self.source, self.data, 'kHelper', '260318099.0.4', 99))

    def validate(self):
        return helpers.validate_embedded(self.source, self.inc, 'kHelper', '260318099.0.4', 99)

    def test_valid(self):
        self.assertEqual(self.validate(), self.data)

    def test_modified_source(self):
        self.source.write_text('globalThis.helper = 43;\n')
        with self.assertRaisesRegex(ValueError, 'Stale source digest'):
            self.validate()

    def test_updating_comment_digest_cannot_hide_stale_bytecode(self):
        self.source.write_text('globalThis.helper = 43;\n')
        self.inc.write_text(helpers.render(self.source, self.data, 'kHelper', '260318099.0.4', 99))
        with self.assertRaisesRegex(ValueError, 'HBC sourceHash mismatch'):
            self.validate()

    def test_stale_release_metadata(self):
        self.inc.write_text(self.inc.read_text().replace('Hermes 260318099.0.4', 'Hermes 1.0.0'))
        with self.assertRaisesRegex(ValueError, 'Stale Hermes/HBC metadata'):
            self.validate()

    def test_stale_hbc_metadata(self):
        self.inc.write_text(self.inc.read_text().replace('HBC 99.', 'HBC 98.'))
        with self.assertRaisesRegex(ValueError, 'Stale Hermes/HBC metadata'):
            self.validate()

    def test_actual_hbc_version_must_match_metadata(self):
        self.inc.write_text(self.inc.read_text().replace('0x63, 0x00, 0x00, 0x00', '0x62, 0x00, 0x00, 0x00', 1))
        with self.assertRaisesRegex(ValueError, 'HBC header mismatch'):
            self.validate()

    def test_bad_magic(self):
        data = b'\x00' + self.data[1:]
        with self.assertRaisesRegex(ValueError, 'HBC header mismatch'):
            helpers.validate_hbc(data, 99)

    def test_truncated_header(self):
        with self.assertRaisesRegex(ValueError, 'Truncated HBC'):
            helpers.validate_hbc(self.data[:12], 99)

    def test_truncated_payload(self):
        self.inc.write_text(helpers.render(self.source, self.data[:-1], 'kHelper', '260318099.0.4', 99))
        with self.assertRaisesRegex(ValueError, 'fileLength'):
            self.validate()

    def test_corrupted_payload(self):
        data = bytearray(self.data)
        data[100] ^= 1
        self.inc.write_text(helpers.render(self.source, data, 'kHelper', '260318099.0.4', 99))
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.validate()

    def test_malformed_array(self):
        self.inc.write_text(self.inc.read_text().replace('0xc6,', '0xnotbyte,', 1))
        with self.assertRaisesRegex(ValueError, 'Malformed byte array'):
            self.validate()

    def test_wrong_symbol(self):
        self.inc.write_text(self.inc.read_text().replace('kHelper[]', 'kOther[]'))
        with self.assertRaisesRegex(ValueError, 'byte array'):
            self.validate()

    def pins(self):
        module = '''http_archive(
    name = "hermes",
    strip_prefix = "hermes-hermes-v260318099.0.4",
    url = "https://github.com/facebook/hermes/archive/refs/tags/hermes-v260318099.0.4.tar.gz",
)
'''
        config = 'HERMES_RELEASE_VERSION=\\"260318099.0.4\\"'
        return module, config

    def test_archive_and_build_pin(self):
        self.assertEqual(helpers.pinned_release(*self.pins()), '260318099.0.4')

    def test_archive_and_build_pin_disagree(self):
        module, config = self.pins()
        with self.assertRaisesRegex(ValueError, 'pins disagree'):
            helpers.pinned_release(module, config.replace('260318099.0.4', '260318099.0.5'))

    def test_url_and_prefix_disagree(self):
        module, config = self.pins()
        module = module.replace('hermes-v260318099.0.4.tar', 'hermes-v260318099.0.5.tar')
        with self.assertRaisesRegex(ValueError, 'pins disagree'):
            helpers.pinned_release(module, config)

    def test_cli_stamp_and_failure_without_compiler(self):
        module, config = self.pins()
        (self.root / 'MODULE.bazel').write_text(module)
        (self.root / 'hermes.BUILD').write_text(config)
        version = self.root / 'BytecodeVersion.h'
        version.write_text('const static uint32_t BYTECODE_VERSION = 99;\n')
        stamp = self.root / 'checked.h'
        command = [sys.executable, str(pathlib.Path(helpers.__file__).resolve()),
                   '--validate-only', '--module', str(self.root / 'MODULE.bazel'),
                   '--config', str(self.root / 'hermes.BUILD'),
                   '--bytecode-version-header', str(version), '--source', str(self.source),
                   '--inc', str(self.inc), '--symbol', 'kHelper', '--stamp', str(stamp)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#pragma once', stamp.read_text())
        stamp.unlink()
        version.write_text('const static uint32_t BYTECODE_VERSION = 100;\n')
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Stale Hermes/HBC metadata', result.stderr)
        self.assertFalse(stamp.exists())


if __name__ == '__main__':
    unittest.main()
