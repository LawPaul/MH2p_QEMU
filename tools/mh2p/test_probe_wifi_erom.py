# SPDX-License-Identifier: GPL-2.0-or-later
import struct
from pathlib import Path
import tempfile
import unittest

import probe_wifi_erom as probe


class WifiEROMTests(unittest.TestCase):
    def test_identity_and_output_guards(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            driver, output = root / "driver", root / "result"
            driver.write_bytes(b"private original")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                probe.probe(root / "qemu", driver, output)
            self.assertFalse(output.exists())
            output.mkdir()
            with self.assertRaises(FileExistsError):
                probe.probe(root / "qemu", driver, output)
            self.assertEqual(driver.read_bytes(), b"private original")

    def test_original_filter_semantics(self):
        self.assertEqual(probe.expected_entry((2, 1), 0, 0), (2, 4))
        self.assertEqual(probe.expected_entry((0, 2, 5, 15), 0xe, 4), (5, 12))
        self.assertEqual(probe.expected_entry((7, 15, 5), 0xe, 4), (15, 8))
        self.assertEqual(probe.expected_entry((0xffffffff, 15), 0xe, 0),
                         (15, 8))
        self.assertEqual(probe.expected_entry((7,) * 1023 + (1, 15), 0xe, 0),
                         (1, 4096))
        self.assertEqual(probe.expected_entry((7,) * 1024 + (1, 15), 0xe, 0),
                         (7, 4096))
        self.assertEqual(probe.expected_entry((0,) * 1025 + (1, 15), 0xe, 0),
                         (1, 4104))
        with self.assertRaises(ValueError):
            probe.expected_entry((0, 7), 0xe, 0)

    def test_exact_native_return_and_pointer(self):
        rows = probe.cases()
        starts = [0x280000 + i * 8192 for i in range(len(rows))]
        data = bytearray()
        for (_, _, _, (word, consumed)), start in zip(rows, starts):
            data.extend(struct.pack("<II", word, start + consumed))
        probe.validate_results(rows, data, starts)
        for index in (0, 4, len(data) - 8, len(data) - 4):
            wrong = bytearray(data)
            wrong[index] ^= 1
            with self.assertRaisesRegex(RuntimeError, "mismatch"):
                probe.validate_results(rows, wrong, starts)
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            probe.validate_results(rows, data[:-1], starts)
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            probe.validate_results(rows, data, starts[:-1])

    def test_exact_original_address_decoder_results(self):
        rows = probe.address_cases()
        starts = [0x280000 + i * 64 for i in range(len(rows))]
        data = bytearray()
        for (_, _, _, expected), start in zip(rows, starts):
            word, consumed, *fields = expected
            data.extend(struct.pack("<6I", word, start + consumed, *fields))
        probe.validate_addresses(rows, data, starts)
        for field in range(6):
            wrong = bytearray(data)
            wrong[field * 4] ^= 1
            with self.assertRaisesRegex(RuntimeError, "mismatch"):
                probe.validate_addresses(rows, wrong, starts)
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            probe.validate_addresses(rows, data[:-1], starts)
        self.assertTrue(any(expected[-1] == 0x87654321
                            for _, _, _, expected in rows))
        self.assertTrue(any(expected[1] == 8 and expected[0] == 0
                            for _, _, _, expected in rows))


if __name__ == "__main__":
    unittest.main()
