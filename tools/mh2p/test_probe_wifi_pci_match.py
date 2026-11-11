# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import struct
import tempfile
import unittest

import probe_wifi_pci_match as probe


class WifiPCIMatchTests(unittest.TestCase):
    def test_exact_native_matrix_required(self):
        expected = bytearray(b"\x01\0\0\0" * (len(probe.VENDORS) * 65536))
        for device in probe.ACCEPTED_IDS:
            struct.pack_into("<I", expected, device * 4, 0)
        accepted = probe.validate_results(expected)
        self.assertEqual(set(accepted[0x14e4]), probe.ACCEPTED_IDS)
        self.assertIn(0x43ef, accepted[0x14e4])
        self.assertEqual(accepted[0], [])
        self.assertEqual(accepted[0xffff], [])
        for index, value in ((0x43ef, 1), (0, 0), (65536 + 0x43ef, 0),
                             (2 * 65536 + 0x43ef, 0), (65535, 2)):
            with self.subTest(index=index):
                original = expected[index * 4:index * 4 + 4]
                struct.pack_into("<I", expected, index * 4, value)
                with self.assertRaisesRegex(RuntimeError, "mismatch"):
                    probe.validate_results(expected)
                expected[index * 4:index * 4 + 4] = original
        for invalid in (expected[:-1], expected + b"\0\0\0\0"):
            with self.assertRaisesRegex(RuntimeError, "Incomplete"):
                probe.validate_results(invalid)

    def test_identity_and_output_guards(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            driver, output = root / "driver", root / "output"
            driver.write_bytes(b"private original")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                probe.probe(root / "qemu", driver, output)
            self.assertFalse(output.exists())
            output.mkdir()
            with self.assertRaises(FileExistsError):
                probe.probe(root / "qemu", driver, output)
            self.assertEqual(driver.read_bytes(), b"private original")


if __name__ == "__main__":
    unittest.main()
