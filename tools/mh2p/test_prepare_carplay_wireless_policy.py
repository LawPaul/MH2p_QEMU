# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_carplay_wireless_policy as policy


def fixture():
    data = bytearray(0xa0000)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", data, 18, 40)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HH", data, 42, 32, 1)
    struct.pack_into("<8I", data, 52, 1, 0x100, 0, 0,
                     len(data) - 0x100, len(data) - 0x100, 5, 0x1000)
    at = 0x100 + policy.PATCH_ADDRESS
    data[at:at + len(policy.PREIMAGE)] = policy.PREIMAGE
    return bytes(data)


class WirelessPolicyTests(unittest.TestCase):
    def test_only_exact_carplay_site_changes(self):
        source = fixture()
        with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
            modified = policy.patch_wireless_policy(source)
        expected = bytearray(source)
        at = 0x100 + policy.PATCH_ADDRESS
        expected[at:at + len(policy.PREIMAGE)] = policy.REPLACEMENT
        self.assertEqual(modified, expected)
        self.assertEqual(len(modified), len(source))

    def test_rejects_other_identity(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            policy.patch_wireless_policy(fixture())

    def test_rejects_wrong_preimage_even_when_identity_matches(self):
        source = bytearray(fixture())
        source[0x100 + policy.PATCH_ADDRESS] ^= 1
        with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
            with self.assertRaisesRegex(ValueError, "instructions"):
                policy.patch_wireless_policy(source)

    def test_rejects_nonexecutable_mapping(self):
        source = bytearray(fixture())
        struct.pack_into("<I", source, 52 + 24, 4)
        with patch.object(policy, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest()):
            with self.assertRaisesRegex(ValueError, "mapping"):
                policy.patch_wireless_policy(source)

    def test_preparation_preserves_original_and_does_not_overwrite(self):
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
                self.assertFalse(metadata["coding_instructions_changed"])
                self.assertFalse(metadata["cached_availability_storage_changed"])
                self.assertEqual(metadata["output_sha256"], hashlib.sha256(
                    (output / "smartphone_integrator").read_bytes()).hexdigest())
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
