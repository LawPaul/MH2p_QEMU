# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for guest Java evidence and binary transfer framing."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from probe_carplay_guest_java import (
    EXPECTED_RECONNECT_STABLE, EXPECTED_STABLE, GUEST_SCENARIOS, probe, verify_guest_result,
    verify_image_archive, verify_overlay_readback)


class CarplayGuestJavaTests(unittest.TestCase):
    def test_existing_output_rejected_before_sources(self):
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(FileExistsError):
                probe(SimpleNamespace(output=Path(name)))

    def test_binary_readback_inverts_serial_line_endings_exactly(self):
        payload = bytes(range(256)) + b"\r\n\n\r\0"
        response = (b"echo CARPLAY_JAR_BEGIN; cat overlay; echo CARPLAY_JAR_END\r\n"
                    b"CARPLAY_JAR_BEGIN\r\n" + payload.replace(b"\n", b"\r\n") +
                    b"\r\nCARPLAY_JAR_END\r\n")
        verify_overlay_readback(response, payload)
        with self.assertRaisesRegex(RuntimeError, "readback differs"):
            verify_overlay_readback(response, payload + b"changed")
        with self.assertRaisesRegex(RuntimeError, "readback differs"):
            verify_overlay_readback(b"no markers", payload)

    @staticmethod
    def result(frames, token=b"MH2P_CARPLAY_GUEST_LIFECYCLE_PASS"):
        return (b"\r\n" + "\r\n".join("GUEST_DSI " + " ".join(
            ",".join(str(value) for value in contact) for contact in frame)
            for frame in frames).encode("ascii") +
            b"\r\n" + token +
            b"\r\nMH2P_CARPLAY_GUEST_JAVA_CHECKS_PASS\r\n")

    def test_exact_guest_frame_sequence(self):
        expected = EXPECTED_STABLE + EXPECTED_RECONNECT_STABLE
        self.assertEqual(verify_guest_result(self.result(expected)), expected)

    def test_pass_tokens_alone_do_not_prove_delivery(self):
        with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
            verify_guest_result(self.result([]))
        with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
            verify_guest_result(self.result(EXPECTED_STABLE))

    def test_missing_pass_marker_rejected(self):
        result = self.result(EXPECTED_STABLE + EXPECTED_RECONNECT_STABLE)
        with self.assertRaisesRegex(RuntimeError, "check missing"):
            verify_guest_result(result.replace(b"MH2P_CARPLAY_GUEST_LIFECYCLE_PASS", b"failure"))

    def test_exact_filter_restart_and_sticky_sequences(self):
        for scenario in ("filter-restart", "filter-sticky"):
            frames, token = GUEST_SCENARIOS[scenario]
            with self.subTest(scenario=scenario):
                self.assertEqual(verify_guest_result(self.result(frames, token), scenario), frames)
                with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
                    verify_guest_result(self.result(frames[:-1], token), scenario)

    def test_filter_reset_does_not_replace_original_rejection_policy(self):
        frames, token = GUEST_SCENARIOS["filter-sticky"]
        wrongly_cleared = [list(frame) for frame in frames]
        wrongly_cleared[2] = [[100, 200, 1]]
        with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
            verify_guest_result(self.result(wrongly_cleared, token), "filter-sticky")

    def test_filter_restart_must_deliver_new_press_not_stale_rejection(self):
        frames, token = GUEST_SCENARIOS["filter-restart"]
        stale = [list(frame) for frame in frames]
        stale[2] = [[100, 200, 2]]
        with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
            verify_guest_result(self.result(stale, token), "filter-restart")

    def test_disabled_gate_must_not_deliver_any_frame(self):
        frames, token = GUEST_SCENARIOS["disabled"]
        self.assertEqual(verify_guest_result(self.result(frames, token), "disabled"), [])
        self.assertEqual(verify_guest_result(
            token + b"\nMH2P_CARPLAY_GUEST_JAVA_CHECKS_PASS\n", "disabled"), [])
        with self.assertRaisesRegex(RuntimeError, "DSI frames differ"):
            verify_guest_result(self.result([[[100, 200, 1]]], token), "disabled")

    def test_guest_archive_identity_checked_and_filesystem_closed(self):
        import hashlib
        from unittest.mock import Mock

        fs = Mock()
        fs.listdir.side_effect = [
            [("eso", 2)], [("hmi", 3)], [("lsd", 4)], [("lsd.jar", 5)]]
        fs.inode.return_value = {"mode": 0x8000}
        fs.read_file.return_value = b"original"
        expected = hashlib.sha256(b"original").hexdigest()
        with patch("probe_carplay_guest_java.Qnx6", return_value=fs):
            self.assertEqual(verify_image_archive(Path("image"), expected), expected)
        fs.f.close.assert_called_once()

        fs.reset_mock()
        fs.listdir.side_effect = [
            [("eso", 2)], [("hmi", 3)], [("lsd", 4)], [("lsd.jar", 5)]]
        with patch("probe_carplay_guest_java.Qnx6", return_value=fs):
            with self.assertRaisesRegex(ValueError, "pinned original"):
                verify_image_archive(Path("image"), "wrong identity")
        fs.f.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
