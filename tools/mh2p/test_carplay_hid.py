# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for touchscreen wire layouts and report validation."""
import struct
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from carplay_hid import decode_report, touch_layout, verify_report
from probe_carplay_hid import build_indexed_hid, expected_report


def descriptor(maximum=2, indexed=False):
    header = bytes.fromhex("050d0904a101")
    finger = bytes.fromhex("050d0922a102")
    index = bytes.fromhex("050d093815002503750895018102") if indexed else b""
    touch = bytes.fromhex("050d093315002501750195018102750795018101")
    axes = bytes.fromhex("0501093015012680077510950181020931150126d002751095018102c0")
    return header + (finger + index + touch + axes) * maximum + b"\xc0"


class CarplayHidTests(unittest.TestCase):
    def test_exact_field_offsets_and_report_sizes(self):
        for maximum in range(1, 5):
            for indexed in (False, True):
                with self.subTest(maximum=maximum, indexed=indexed):
                    layout = touch_layout(descriptor(maximum, indexed))
                    stride = 48 if indexed else 40
                    for slot in range(maximum):
                        fields = [field for field in layout if field.slot == slot]
                        self.assertEqual(fields[0].bit, slot * stride)
                        self.assertEqual(sum(field.size for field in fields), stride)
                        self.assertEqual([field.usage for field in fields],
                                         (["index"] if indexed else []) +
                                         ["touch", "padding", "x", "y"])
                        self.assertEqual(fields[-2].maximum, 1920)
                        self.assertEqual(fields[-1].maximum, 720)

    def test_roundtrip_full_range_and_release(self):
        contacts = [(0, 0, 1), (65535, 65535, 1), (1920, 720, 0), (1, 1, 1)]
        for maximum in range(1, 5):
            for indexed in (False, True):
                for release in (False, True):
                    for count in range(5):
                        with self.subTest(maximum=maximum, indexed=indexed,
                                          release=release, count=count):
                            report = expected_report(contacts[:count], maximum, release, indexed)
                            result = verify_report(descriptor(maximum, indexed), report,
                                                   contacts[:count], maximum, release, indexed)
                            self.assertEqual(len(result), maximum)
                            if indexed:
                                self.assertEqual([item["index"] for item in result],
                                                 list(range(maximum)))

    def test_survivor_slot_does_not_change_with_inactive_placeholder(self):
        frames = [[(100, 200, 1), (700, 500, 1)],
                  [(100, 200, 0), (710, 510, 1)],
                  [(100, 200, 0), (720, 520, 1)],
                  [(100, 200, 0), (720, 520, 0)]]
        for indexed in (False, True):
            decoded = [decode_report(descriptor(2, indexed),
                                     expected_report(frame, 2, False, indexed))
                       for frame in frames]
            self.assertEqual([row[1]["touch"] for row in decoded], [1, 1, 1, 0])
            self.assertEqual([row[0]["touch"] for row in decoded], [1, 0, 0, 0])
            self.assertEqual(decoded[2][1]["x"], 720)

    def test_explicit_index_does_not_fix_compacted_array(self):
        compacted = [(720, 520, 1)]
        report = expected_report(compacted, 2, False, True)
        decoded = decode_report(descriptor(2, True), report)
        self.assertEqual(decoded[0], {"index": 0, "touch": 1, "x": 720, "y": 520})
        self.assertEqual(decoded[1]["touch"], 0)
        with self.assertRaisesRegex(AssertionError, "Decoded HID"):
            verify_report(descriptor(2, True), report,
                          [(100, 200, 0), (720, 520, 1)], 2, indexed=True)

    def test_mismatched_report_format_rejected(self):
        for indexed in (False, True):
            report = expected_report([(1, 2, 1)], 2, False, not indexed)
            with self.assertRaisesRegex(ValueError, "length"):
                decode_report(descriptor(2, indexed), report)

    def test_inactive_indices_and_padding_checked(self):
        report = bytearray(expected_report([], 2, False, True))
        self.assertEqual(report, bytes.fromhex("000000000000010000000000"))
        report[6] = 0
        with self.assertRaises(AssertionError):
            verify_report(descriptor(2, True), report, [], 2, indexed=True)
        report = bytearray(expected_report([], 2, False, False))
        report[0] = 2
        with self.assertRaisesRegex(ValueError, "padding"):
            decode_report(descriptor(), report)

    def test_malformed_and_unsupported_descriptors_rejected(self):
        original = descriptor()
        malformed = [
            b"", b"\xfe", b"\x26\x01", b"\xc0", b"\xa1\x01",
            original[:-1], original + b"\xc0",
            original.replace(b"\x09\x33", b"\x09\x51"),
            original.replace(b"\x75\x10", b"\x75\x20"),
            original.replace(b"\x81\x02", b"\x81\x06"),
            original.replace(b"\x95\x01", b"\x95\x00"),
            original.replace(b"\x09\x04", b"\x09\x05"),
            original.replace(b"\x75\x10", b"\x85\x01"),
            descriptor(5),
            original.replace(b"\x75\x01", b"\x75\x02"),
        ]
        for data in malformed:
            with self.subTest(data=data), self.assertRaises(ValueError):
                touch_layout(data)

    def test_expected_legacy_bytes_are_unchanged(self):
        self.assertEqual(expected_report([(100, 200, 1), (700, 500, 1)], 2, False, False),
                         struct.pack("<BHHBHH", 1, 100, 200, 1, 700, 500))

    def test_compiler_failure_exposes_diagnostics(self):
        with patch("probe_carplay_hid.subprocess.run", return_value=SimpleNamespace(
                returncode=1, stdout="compiler output\n", stderr="link error")):
            with self.assertRaisesRegex(RuntimeError, "compiler output\nlink error"):
                build_indexed_hid("compiler-fixture")


if __name__ == "__main__":
    unittest.main()
