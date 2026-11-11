# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_carplay_country_policy as policy


def fixture():
    data = bytearray(0x70000)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", data, 18, 40)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HH", data, 42, 32, 1)
    struct.pack_into("<8I", data, 52, 1, 0x100, 0, 0,
                     len(data) - 0x100, len(data) - 0x100, 5, 0x1000)
    for address, before, _ in policy.PATCHES:
        data[0x100 + address:0x100 + address + len(before)] = before
    return bytes(data)


class CountryPolicyTests(unittest.TestCase):
    def test_only_exact_sites_change(self):
        source = fixture()
        with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
            output = policy.patch_country_policy(source)
        expected = bytearray(source)
        for address, before, after in policy.PATCHES:
            expected[0x100 + address:0x100 + address + len(before)] = after
        self.assertEqual(output, expected)
        self.assertEqual(len(output), len(source))

    def test_rejects_other_identity(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            policy.patch_country_policy(fixture())

    def test_rejects_changed_preimage_even_with_matching_identity(self):
        for address, _, _ in policy.PATCHES:
            with self.subTest(address=hex(address)):
                source = bytearray(fixture())
                source[0x100 + address] ^= 1
                with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
                    with self.assertRaisesRegex(ValueError, "instructions"):
                        policy.patch_country_policy(source)

    def test_rejects_bad_elf_and_missing_mapping(self):
        for source in (b"", b"\x7fELF", b"x" * 100):
            with self.assertRaises(ValueError):
                policy.file_offset(source, 0, 4)
        with self.assertRaisesRegex(ValueError, "mapping"):
            policy.file_offset(fixture(), 0x80000, 4)

    def test_output_isolated_and_not_overwritten(self):
        source = fixture()
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            original, output = root / "original", root / "mod"
            original.write_bytes(source)
            with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
                metadata = policy.prepare(original, output)
                self.assertEqual(original.read_bytes(), source)
                self.assertEqual(json.loads((output / "manifest.json").read_text()), metadata)
                self.assertFalse(metadata["installed"])
                self.assertFalse(metadata["wireless_carplay_verified"])
                self.assertEqual(metadata["output_sha256"], hashlib.sha256(
                    (output / "connectionmanager").read_bytes()).hexdigest())
                with self.assertRaises(FileExistsError):
                    policy.prepare(original, output)

    def test_invalid_input_creates_no_output(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            original, output = root / "original", root / "mod"
            original.write_bytes(b"not original")
            with self.assertRaises(ValueError):
                policy.prepare(original, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
