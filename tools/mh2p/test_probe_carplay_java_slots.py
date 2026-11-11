# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free trace and native probe input checks."""
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from probe_carplay_hid import load_dsi_frames
from probe_carplay_java_slots import EXPECTED_ORIGINAL, EXPECTED_STABLE, parse_trace, run_java


class CarplayJavaTraceTests(unittest.TestCase):
    def test_exact_original_and_stable_trace_shape(self):
        for frames in (EXPECTED_ORIGINAL, EXPECTED_STABLE):
            trace = "".join("HMI diagnostic fixture\nDSI " + " ".join(
                ",".join(str(value) for value in contact) for contact in frame) + "\n"
                for frame in frames)
            self.assertEqual(parse_trace(trace), frames)

    def test_unexpected_trace_lines_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unexpected Java trace"):
            parse_trace("silently skipped a frame")

    def test_java_failure_exposes_captured_diagnostics(self):
        with patch("probe_carplay_java_slots.subprocess.run", return_value=SimpleNamespace(
                returncode=1, stdout="class verification failed", stderr="bad class")):
            with self.assertRaisesRegex(RuntimeError, "class verification failedbad class"):
                run_java(["java", "fixture"], 1)

    def test_valid_native_input_preserved_exactly(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "frames.json"
            path.write_text(json.dumps(EXPECTED_STABLE))
            self.assertEqual(load_dsi_frames(path), EXPECTED_STABLE)

    def test_malformed_native_frames_rejected(self):
        invalid = [None, {}, [], [[]], EXPECTED_STABLE * 2,
                   [[[0, 0, 1]] * 5], [[[0, 0]]], [[[0, 0, 3]]],
                   [[[0, 0, 0]]], [[[0, 0, True]]], [[[-1, 0, 1]]],
                   [[[0, 65536, 1]]], [[[1.0, 0, 1]]], [[None]]]
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "frames.json"
            for value in invalid:
                with self.subTest(value=value):
                    path.write_text(json.dumps(value))
                    with self.assertRaises(ValueError):
                        load_dsi_frames(path)


if __name__ == "__main__":
    unittest.main()
