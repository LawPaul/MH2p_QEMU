# SPDX-License-Identifier: GPL-2.0-or-later
import socket
import unittest
from unittest.mock import patch

import hmi_qemu_input
from hmi_qemu_input import DiagnosticClicks, DiagnosticGestures
from hmi_diagnosis import DiagnosisError


class QemuInputTests(unittest.TestCase):
    def setUp(self):
        self.taps = []
        self.clicks = DiagnosticClicks(
            lambda x, y: self.taps.append((x, y)) or "original reply")

    def test_click_requires_complete_pair_and_preserves_press_coordinates(self):
        self.assertIsNone(self.clicks.feed("press 1919 719\n"))
        self.assertEqual(self.taps, [])
        self.assertEqual(self.clicks.feed("release 1918 718\n"), "original reply")
        self.assertEqual(self.taps, [(1919, 719)])
        self.clicks.feed("press 0 0\n")
        self.clicks.feed("release 0 0\n")
        self.assertEqual(self.taps[-1], (0, 0))

    def test_rejects_malformed_out_of_range_and_incomplete_records(self):
        for line in ("press 1920 0\n", "press 0 720\n", "press -1 0\n",
                     "move 1 1\n", "press 1 1", "press 1 1\nextra",
                     "press 1.0 1\n", "press 10000 1\n"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.clicks.feed(line)
        self.assertEqual(self.taps, [])

    def test_rejects_unpaired_releases_duplicate_presses_and_drags(self):
        with self.assertRaisesRegex(ValueError, "without pressing"):
            self.clicks.feed("release 0 0\n")
        self.clicks.feed("press 0 0\n")
        with self.assertRaisesRegex(ValueError, "without releasing"):
            self.clicks.feed("press 1 1\n")
        with self.assertRaisesRegex(ValueError, "dragging"):
            self.clicks.feed("release 100 100\n")
        self.assertIsNone(self.clicks.pressed)
        self.assertEqual(self.taps, [])

    def test_original_errors_propagate_and_release_clears_state(self):
        def fail(x, y):
            raise DiagnosisError("original service rejected touch")
        clicks = DiagnosticClicks(fail)
        clicks.feed("press 75 33\n")
        with self.assertRaisesRegex(DiagnosisError, "rejected"):
            clicks.feed("release 75 33\n")
        self.assertIsNone(clicks.pressed)

    def test_reset_cancels_held_click_without_sending_touch(self):
        self.clicks.feed("press 75 33\n")
        self.assertIsNone(self.clicks.feed("cancel 75 33\n"))
        self.assertIsNone(self.clicks.pressed)
        self.assertEqual(self.taps, [])
        self.clicks.feed("press 0 0\n")
        self.clicks.feed("release 0 0\n")
        self.assertEqual(self.taps, [(0, 0)])

    def test_eight_pixel_tolerance_is_inclusive(self):
        self.clicks.feed("press 10 10\n")
        self.clicks.feed("release 18 2\n")
        self.assertEqual(self.taps, [(10, 10)])
        self.clicks.feed("press 10 10\n")
        with self.assertRaisesRegex(ValueError, "dragging"):
            self.clicks.feed("release 19 10\n")
        self.assertEqual(self.taps, [(10, 10)])


class GestureInputTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.gestures = DiagnosticGestures(
            lambda action, x, y: self.events.append((action, x, y)) or "reply")

    def test_stream_preserves_actions_order_and_final_coordinates(self):
        for line in ("press 10 20\n", "move 200 100\n", "move 1919 719\n",
                     "release 1800 700\n"):
            self.assertEqual(self.gestures.feed(line), "reply")
        self.assertEqual(self.events, [
            ("press", 10, 20), ("move", 200, 100), ("move", 1919, 719),
            ("release", 1800, 700),
        ])
        self.assertIsNone(self.gestures.pressed)

    def test_tap_is_not_replaced_by_async_typed_helper(self):
        self.gestures.feed("press 71 33\n")
        self.gestures.feed("release 71 33\n")
        self.assertEqual(self.events, [("press", 71, 33), ("release", 71, 33)])

    def test_bad_records_and_unpaired_events_never_dispatch(self):
        for line in ("move 1 1\n", "release 1 1\n", "move 1920 1\n",
                     "move -1 1\n", "move 1 720\n", "move 1 1",
                     "move 1 1\nextra", "tap 1 1\n"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.gestures.feed(line)
        self.assertEqual(self.events, [])
        self.gestures.feed("press 0 0\n")
        with self.assertRaisesRegex(ValueError, "without releasing"):
            self.gestures.feed("press 10 10\n")
        self.assertEqual(self.events, [("press", 0, 0)])

    def test_reset_drops_held_state_without_activating_a_widget(self):
        self.gestures.feed("press 10 20\n")
        self.gestures.feed("move 30 40\n")
        self.assertIsNone(self.gestures.feed("cancel 30 40\n"))
        self.assertIsNone(self.gestures.pressed)
        self.assertEqual(self.events, [("press", 10, 20), ("move", 30, 40)])
        self.gestures.feed("press 0 0\n")
        self.gestures.feed("release 0 0\n")

    def test_original_errors_propagate_without_claiming_state_transition(self):
        def fail(action, x, y):
            raise DiagnosisError("original service rejected gesture")

        gestures = DiagnosticGestures(fail)
        with self.assertRaises(DiagnosisError):
            gestures.feed("press 10 20\n")
        self.assertIsNone(gestures.pressed)
        self.gestures.feed("press 10 20\n")
        self.gestures.touch = fail
        for line in ("move 30 40\n", "release 30 40\n"):
            with self.subTest(line=line), self.assertRaises(DiagnosisError):
                self.gestures.feed(line)
            self.assertEqual(self.gestures.pressed, (10, 20))

    def test_cli_interrupt_with_held_gesture_requires_reset(self):
        def interrupt(path, relay):
            relay.pressed = (10, 20)
            raise KeyboardInterrupt()

        with (patch("sys.argv", ["relay", "--events", "unused", "--host",
                                "127.0.0.1", "--gestures"]),
              patch.object(hmi_qemu_input, "follow", side_effect=interrupt),
              patch("sys.stderr") as stderr):
            with self.assertRaises(SystemExit) as exit_error:
                hmi_qemu_input.main()
            self.assertEqual(exit_error.exception.code, 1)
            self.assertIn("reset the guest", "".join(
                call.args[0] for call in stderr.write.call_args_list))

    def test_cli_failure_with_held_gesture_preserves_original_error(self):
        def fail(path, relay):
            relay.pressed = (10, 20)
            raise DiagnosisError("original service unavailable")

        with (patch("sys.argv", ["relay", "--events", "unused", "--host",
                                "127.0.0.1", "--gestures"]),
              patch.object(hmi_qemu_input, "follow", side_effect=fail),
              patch("sys.stderr") as stderr):
            with self.assertRaises(SystemExit) as exit_error:
                hmi_qemu_input.main()
            self.assertEqual(exit_error.exception.code, 1)
            output = "".join(call.args[0] for call in stderr.write.call_args_list)
            self.assertIn("original service unavailable", output)
            self.assertIn("reset before restarting", output)


class RelayConnectionTests(unittest.TestCase):
    def test_cli_only_reopens_connection_after_explicit_guest_reset(self):
        for gestures in (False, True):
            with self.subTest(gestures=gestures):
                def interact(path, relay):
                    relay.feed("press 10 20\n")
                    relay.feed("release 10 20\n")
                    relay.feed("press 30 40\n")
                    relay.feed("cancel 30 40\n")
                    relay.feed("press 50 60\n")
                    relay.feed("release 50 60\n")
                    raise KeyboardInterrupt()

                arguments = ["relay", "--events", "unused", "--host", "localhost"]
                if gestures:
                    arguments.append("--gestures")
                with (patch("sys.argv", arguments),
                      patch.object(hmi_qemu_input, "follow", side_effect=interact),
                      patch.object(hmi_qemu_input, "HmiDiagnosis") as connect):
                    client = connect.return_value.__enter__.return_value
                    hmi_qemu_input.main()
                    self.assertEqual(connect.call_count, 2)
                    self.assertEqual(connect.return_value.__exit__.call_count, 2)
                    events = [call.args for call in client.touch.call_args_list]
                    self.assertNotIn(("release", 30, 40), events)
                    self.assertNotIn(("tap", 30, 40), events)
                    self.assertEqual(events[-1],
                                     ("release" if gestures else "tap", 50, 60))

    def test_cli_uses_one_real_protocol_handshake_for_a_complete_drag(self):
        client, server = socket.socketpair()
        client.settimeout(0.1)
        server.settimeout(0.1)
        self.addCleanup(client.close)
        self.addCleanup(server.close)
        server.sendall(
            b"response_diag\n" +
            b"response_processcommand   Call executed successfully.\n" * 3)

        def interact(path, relay):
            for line in ("press 10 20\n", "move 30 40\n", "release 30 40\n"):
                relay.feed(line)
            raise KeyboardInterrupt()

        with (patch("sys.argv", ["relay", "--events", "unused", "--host",
                                "localhost", "--gestures"]),
              patch.object(hmi_qemu_input, "follow", side_effect=interact),
              patch("hmi_diagnosis.socket.create_connection",
                    return_value=client) as connect):
            hmi_qemu_input.main()
        connect.assert_called_once()
        self.assertEqual(client.fileno(), -1)
        received = b""
        while chunk := server.recv(4096):
            received += chunk
        self.assertEqual(received, (
            b"diag\n"
            b"processcommand,102,cmdGestureScreenPressed,10,20\n"
            b"processcommand,102,cmdGestureScreenMoved,30,40\n"
            b"processcommand,102,cmdGestureScreenReleased,30,40\n"))

    def test_cli_reuses_one_connection_across_complete_interactions(self):
        for gestures in (False, True):
            with self.subTest(gestures=gestures):
                def interact(path, relay):
                    for line in ("press 10 20\n", "release 10 20\n",
                                 "press 30 40\n", "release 30 40\n"):
                        relay.feed(line)
                    raise KeyboardInterrupt()

                arguments = ["relay", "--events", "unused", "--host", "localhost"]
                if gestures:
                    arguments.append("--gestures")
                with (patch("sys.argv", arguments),
                      patch.object(hmi_qemu_input, "follow", side_effect=interact),
                      patch.object(hmi_qemu_input, "HmiDiagnosis") as connect):
                    hmi_qemu_input.main()
                    connect.assert_called_once_with("localhost", 1501)
                    connect.return_value.__enter__.assert_called_once()
                    connect.return_value.__exit__.assert_called_once()
                    client = connect.return_value.__enter__.return_value
                    expected = (
                        [("press", 10, 20), ("release", 10, 20),
                         ("press", 30, 40), ("release", 30, 40)] if gestures else
                        [("tap", 10, 20), ("tap", 30, 40)])
                    self.assertEqual(
                        [call.args for call in client.touch.call_args_list],
                        expected)

    def test_cli_does_not_reconnect_or_replay_failed_gesture(self):
        def interact(path, relay):
            relay.feed("press 10 20\n")
            relay.feed("move 30 40\n")

        with (patch("sys.argv", ["relay", "--events", "unused", "--host",
                                "localhost", "--gestures"]),
              patch.object(hmi_qemu_input, "follow", side_effect=interact),
              patch.object(hmi_qemu_input, "HmiDiagnosis") as connect,
              patch("sys.stderr") as stderr):
            client = connect.return_value.__enter__.return_value
            client.touch.side_effect = ["reply", DiagnosisError("socket failed")]
            with self.assertRaises(SystemExit) as exit_error:
                hmi_qemu_input.main()
            self.assertEqual(exit_error.exception.code, 1)
            connect.assert_called_once()
            connect.return_value.__exit__.assert_called_once()
            self.assertEqual(client.touch.call_count, 2)
            output = "".join(call.args[0] for call in stderr.write.call_args_list)
            self.assertIn("socket failed", output)
            self.assertIn("reset before restarting", output)


if __name__ == "__main__":
    unittest.main()
