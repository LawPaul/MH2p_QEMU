# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import probe_wifi_memory as probe


class WifiMemoryTests(unittest.TestCase):
    def test_case_shapes_and_boundaries(self):
        firmware = b"private firmware fixture"
        rows = probe.cases(firmware)
        self.assertEqual(rows[-1], (0x180000, firmware))
        sizes = set()
        offsets = set()
        for offset, payload in rows:
            self.assertLessEqual(offset + len(payload), probe.BAR_SIZE)
            self.assertEqual(offset % 4, 0)
            sizes.add(len(payload))
            offsets.add(offset)
        self.assertTrue({0, 1, 7, 8, 9, 15, 16, 17, 2047, 2048, 2049} <= sizes)
        self.assertIn(0x180004, offsets)
        self.assertIn((probe.BAR_SIZE - 64, bytes(
            (index * 37 + 64) & 255 for index in range(64))), rows)

    def test_exact_native_returns_payloads_and_guards(self):
        rows = ((0, b""), (0x180000, b"private firmware"))
        returned = bytes(16)
        captures = [b"\x5a" * 16 + payload + b"\x5a" * 16
                    for _, payload in rows]
        probe.verify_transfers(rows, returned, captures)
        for index in range(4):
            wrong = bytearray(returned)
            struct.pack_into("<I", wrong, index * 4, 1)
            with self.assertRaises(RuntimeError):
                probe.verify_transfers(rows, wrong, captures)
        for index in (0, 16, len(captures[1]) - 1):
            wrong = bytearray(captures[1])
            wrong[index] ^= 1
            with self.assertRaisesRegex(RuntimeError, "mismatch"):
                probe.verify_transfers(rows, returned, [captures[0], wrong])
        for wrong in (returned[:-1], returned + bytes(8)):
            with self.assertRaisesRegex(RuntimeError, "Incomplete"):
                probe.verify_transfers(rows, wrong, captures)
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            probe.verify_transfers(rows, returned, captures[:-1])

    def test_entire_bar_not_only_written_region(self):
        with patch.object(probe, "BAR_SIZE", 128):
            rows = ((0, b"abcd"), (2, b"XY"), (124, b"tail"))
            expected = bytearray(b"\xa5" * 160)
            expected[16:20] = b"abXY"
            expected[140:144] = b"tail"
            probe.verify_bar(rows, expected)
            for index in (0, 15, 16, 19, 20, 139, 143, 144, 159):
                with self.subTest(index=index):
                    wrong = bytearray(expected)
                    wrong[index] ^= 1
                    with self.assertRaises(RuntimeError):
                        probe.verify_bar(rows, wrong)
            with self.assertRaises(RuntimeError):
                probe.verify_bar(rows, expected[:-1])

    def test_inputs_preserved_before_output_creation(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            driver, image, output = root / "driver", root / "image", root / "result"
            driver.write_bytes(b"private driver")
            image.write_bytes(b"private image")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                probe.probe(root / "qemu", driver, image, output)
            self.assertFalse(output.exists())
            output.mkdir()
            with self.assertRaises(FileExistsError):
                probe.probe(root / "qemu", driver, image, output)
            self.assertEqual(driver.read_bytes(), b"private driver")
            self.assertEqual(image.read_bytes(), b"private image")

    def test_exact_original_dump_and_metadata(self):
        image, address = b"private firmware fixture", 0xe00010
        data = bytearray(b"\x5a" * 16 + image + b"\x5a" * 16)
        metadata = struct.pack("<II", address, len(image))
        probe.verify_dump(image, address, data, metadata)
        for index in (0, 15, 16, len(data) - 17, len(data) - 1):
            wrong = bytearray(data)
            wrong[index] ^= 1
            with self.assertRaisesRegex(RuntimeError, "payload"):
                probe.verify_dump(image, address, wrong, metadata)
        for wrong in (metadata[:-1], struct.pack("<II", address + 4, len(image)),
                      struct.pack("<II", address, len(image) + 1)):
            with self.assertRaisesRegex(RuntimeError, "metadata"):
                probe.verify_dump(image, address, data, wrong)


if __name__ == "__main__":
    unittest.main()
