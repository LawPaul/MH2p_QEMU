# SPDX-License-Identifier: GPL-2.0-or-later
import tempfile
from contextlib import nullcontext
import json
from pathlib import Path
import random
import unittest
from unittest.mock import Mock, patch

import check_hmi_native_input as check
import hmi_live_control as control
import hmi_development_ui as ui


HEADER = b"P6\n1920 720\n255\n"
BLACK = HEADER + bytes(1920 * 720 * 3)
WHITE = HEADER + b"\xff" * (1920 * 720 * 3)


class NativeInputCheckTests(unittest.TestCase):
    def test_control_request_is_complete_when_published(self):
        request = {"id": "test", "action": "diagnosis",
                   "command": "getbundleinfo,0"}
        link = control.os.link
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def observe_publication(source, destination):
                self.assertFalse(destination.exists())
                self.assertEqual(json.loads(source.read_text()), request)
                link(source, destination)
                self.assertEqual(json.loads(destination.read_text()), request)

            with patch.object(control.os, "link", side_effect=observe_publication):
                check.publish_control_request(root, request)
            self.assertEqual(list(root.iterdir()), [root / "control-request.json"])

    def test_control_request_never_overwrites_pending_request(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pending = root / "control-request.json"
            pending.write_text('{"id":"pending"}')
            with self.assertRaises(FileExistsError):
                check.publish_control_request(root, {"id": "replacement"})
            self.assertEqual(pending.read_text(), '{"id":"pending"}')
            self.assertEqual(list(root.iterdir()), [pending])

    def test_control_request_serialization_failure_does_not_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(TypeError):
                check.publish_control_request(root, {"id": object()})
            self.assertEqual(list(root.iterdir()), [])

    def test_screen_id_uses_main_screen_not_overlay(self):
        self.assertEqual(check.screen_id(
            "response_getvalue ScreenData[id:262 name:HOME] "
            "ScreenData[id:999 name:overlay]"), 262)
        for value in ("response_getvalue", "response_getvalueException",
                      "response_error id:262"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                check.screen_id(value)

    def test_pixel_comparison_excludes_chrome_and_requires_complete_frames(self):
        with patch.object(ui.zlib, "compress", side_effect=AssertionError("not a PNG request")):
            self.assertEqual(check.changed_pixels(BLACK, WHITE), 1700 * 620)
        chrome = bytearray(BLACK)
        chrome[len(HEADER):len(HEADER) + 1920 * 80 * 3] = b"\xff" * (1920 * 80 * 3)
        self.assertEqual(check.changed_pixels(BLACK, bytes(chrome)), 0)
        with self.assertRaises(ValueError):
            check.changed_pixels(BLACK, WHITE[:-1])

    def test_tap_releases_even_if_hold_is_interrupted(self):
        command = Mock()
        with patch.object(check.time, "sleep", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                check.tap(command, 1919, 719)
        events = command.call_args_list[0].kwargs["events"]
        self.assertEqual(events[0]["data"]["value"], 32767)
        self.assertEqual(events[1]["data"]["value"], 32767)
        self.assertEqual(command.call_args_list[-1].kwargs["events"],
                         [{"type": "btn", "data": {"button": "left", "down": False}}])
        command.reset_mock()
        with self.assertRaises(ValueError):
            check.tap(command, 1920, 0)
        command.assert_not_called()

    def test_pixel_comparison_counts_channels_once_and_chunk_edges_exactly(self):
        edited = bytearray(BLACK)
        points = [(200, 80), (263, 80), (264, 80), (1899, 699),
                  (199, 80), (1900, 699), (200, 79), (200, 700)]
        for x, y in points:
            at = len(HEADER) + (y * 1920 + x) * 3
            edited[at:at + 3] = b"\x01\x02\x03"
        self.assertEqual(check.changed_pixels(BLACK, bytes(edited)), 4)
        self.assertEqual(check.changed_pixels(bytes(edited), BLACK), 4)

    def test_sparse_pixel_comparison_matches_independent_coordinates(self):
        edited = bytearray(BLACK)
        rng = random.Random(42)
        changed = set()
        for _ in range(5000):
            x, y, channel = rng.randrange(1920), rng.randrange(720), rng.randrange(3)
            edited[len(HEADER) + (y * 1920 + x) * 3 + channel] = 1
            if 200 <= x < 1900 and 80 <= y < 700:
                changed.add((x, y))
        self.assertEqual(check.changed_pixels(BLACK, bytes(edited)), len(changed))

    def test_wrong_initial_screen_never_injects_input(self):
        command = Mock()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "initial screen"):
                check.transition(command, lambda: "response_getvalue id:262",
                                 Path(directory), "test", 100221, 262,
                                 [72, 34], 10, 100000)
        command.assert_not_called()

    @staticmethod
    def bundle_reply(state, *, name="HMISettings", bundle_id=0):
        return (f'response_getbundleinfo===== Bundle "{name}" =====#'
                f'   ID: {bundle_id}#   Version: null#   State: {state}#'
                '   Location: HMISettings#   Registered Services: NONE#')

    def test_waits_for_actual_original_bundle_activation(self):
        with (patch.object(check, "query_diagnosis", side_effect=[
                self.bundle_reply("RESOLVED"), self.bundle_reply("STARTING"),
                self.bundle_reply("ACTIVE")]) as query,
              patch.object(check.time, "sleep")):
            result = check.wait_for_bundle(Path("/unused"), 0, "HMISettings", 10)
        self.assertEqual([sample["state"] for sample in result["samples"]],
                         ["RESOLVED", "STARTING", "ACTIVE"])
        self.assertEqual(query.call_count, 3)
        self.assertEqual(query.call_args.args[1], "getbundleinfo,0")

    def test_rejects_wrong_bundle_identity_or_malformed_reply(self):
        for reply in (self.bundle_reply("ACTIVE", name="AppSystemBl"),
                      self.bundle_reply("ACTIVE", bundle_id=1),
                      "response_getbundleinfoException occured in getBundleInfo",
                      "response_getbundleinfo State: ACTIVE"):
            with (self.subTest(reply=reply),
                  patch.object(check, "query_diagnosis", return_value=reply),
                  self.assertRaises(ValueError)):
                check.wait_for_bundle(Path("/unused"), 0, "HMISettings", 10)

    def test_resolved_bundle_does_not_pass_on_timeout(self):
        with (patch.object(check, "query_diagnosis",
                           return_value=self.bundle_reply("RESOLVED")),
              patch.object(check.time, "sleep"),
              patch.object(check.time, "monotonic",
                           side_effect=[0, 0, 0, 0, 2, 2]),
              self.assertRaisesRegex(TimeoutError, "did not become ACTIVE")):
            check.wait_for_bundle(Path("/unused"), 0, "HMISettings", 1)

    def test_query_screen_still_validates_original_screen_reply(self):
        with patch.object(check, "query_diagnosis", return_value="response_getvaluebroken"):
            with self.assertRaisesRegex(ValueError, "No original main screen"):
                check.query_screen(Path("/unused"), 10)

    def test_query_rejects_raw_getter_failure_from_older_harness(self):
        with patch.object(check, "request_control",
                          return_value="response_getvaluejava.lang.NullPointerException"):
            with self.assertRaises(check.DiagnosisError):
                check.query_diagnosis(
                    Path("/unused"), "getvalue,6,ViewOptions", "response_getvalue", 10)

    def test_bundle_timeout_prevents_all_native_touch_checks(self):
        command = Mock(return_value=[{
            "name": "MH2P RCC keypanel", "absolute": True, "current": True,
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence"
            arguments = [
                "check_hmi_native_input.py", "--control-directory", directory,
                "--output", str(output), "--from-screen", "600176",
                "--to-screen", "1100087", "--tap", "895", "625",
                "--return-tap", "330", "33",
                "--wait-for-bundle", "0", "HMISettings",
            ]
            with (patch("sys.argv", arguments),
                  patch.object(check, "qmp_connection",
                               return_value=nullcontext(command)),
                  patch.object(check, "wait_for_bundle",
                               side_effect=TimeoutError("Settings is not ACTIVE")),
                  patch.object(check, "transition") as transition,
                  self.assertRaisesRegex(TimeoutError, "not ACTIVE")):
                check.main()
            transition.assert_not_called()
            result = json.loads((output / "result.json").read_text())
            self.assertFalse(result["native_rstp_touch_verified"])
            self.assertFalse(result["full_hmi_readiness_verified"])
            self.assertEqual(result["error"], "Settings is not ACTIVE")

    def test_animation_or_logical_change_alone_cannot_pass(self):
        for reply, changed in (("response_getvalue id:100221", 200000),
                               ("response_getvalue id:262", 1)):
            with self.subTest(reply=reply, changed=changed):
                with tempfile.TemporaryDirectory() as directory:
                    clock = [0.0]

                    def command(name, **arguments):
                        Path(arguments["filename"]).write_bytes(BLACK)

                    screen = Mock(side_effect=["response_getvalue id:100221", reply])
                    with (patch.object(check, "tap"),
                          patch.object(check, "changed_pixels", return_value=changed),
                          patch.object(check.time, "sleep",
                                       side_effect=lambda _: clock.__setitem__(0, 2)),
                          patch.object(check.time, "monotonic",
                                       side_effect=lambda: clock[0])):
                        with self.assertRaisesRegex(TimeoutError, "no logical screen"):
                            check.transition(command, screen, Path(directory),
                                             "test", 100221, 262, [72, 34],
                                             1, 100000)
                    self.assertEqual(screen.call_count, 2 if changed >= 100000 else 1)

    def test_logical_and_native_pixel_change_are_both_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            def command(name, **arguments):
                Path(arguments["filename"]).write_bytes(
                    BLACK if "before" in arguments["filename"] else WHITE)

            screen = Mock(side_effect=["response_getvalue id:100221",
                                       "response_getvalue id:262"])
            with patch.object(check, "tap") as tap:
                result = check.transition(command, screen, Path(directory),
                                          "test", 100221, 262, [72, 34],
                                          10, 100000)
            tap.assert_called_once_with(command, 72, 34)
            self.assertEqual(result["to"], 262)
            self.assertGreaterEqual(
                result["samples"][-1]["changed_content_pixels"], 100000)

    def test_pixel_observation_precedes_slow_logical_confirmation(self):
        clock = [0.0]
        events = []
        with tempfile.TemporaryDirectory() as directory:
            def command(name, **arguments):
                events.append("frame")
                Path(arguments["filename"]).write_bytes(
                    BLACK if "before" in arguments["filename"] else WHITE)

            def screen():
                events.append("screen")
                if events == ["screen"]:
                    return "response_getvalue id:100221"
                clock[0] += 8
                return "response_getvalue id:262"

            with (patch.object(check, "tap", side_effect=lambda *args: clock.__setitem__(0, 1)),
                  patch.object(check.time, "monotonic", side_effect=lambda: clock[0])):
                result = check.transition(command, screen, Path(directory),
                                          "test", 100221, 262, [72, 34], 20, 100000)
        self.assertEqual(events, ["screen", "frame", "frame", "screen", "frame"])
        self.assertEqual(result["first_content_change_host_seconds"], 1)
        self.assertEqual(result["samples"][-1]["screen_query_host_seconds"], 8)
        self.assertEqual(result["samples"][-1]["host_seconds"], 9)
        self.assertEqual(result["samples"][-1]["visible_host_seconds"], 1)
        self.assertEqual(result["visual_samples"][-1]["host_seconds"], 1)

    def test_visual_change_must_remain_after_logical_confirmation(self):
        clock = [0.0]
        with tempfile.TemporaryDirectory() as directory:
            frames = iter([BLACK, WHITE, BLACK])

            def command(name, **arguments):
                Path(arguments["filename"]).write_bytes(next(frames))

            def screen():
                if clock[0]:
                    clock[0] = 2
                    return "response_getvalue id:262"
                return "response_getvalue id:100221"

            with (patch.object(check, "tap", side_effect=lambda *args: clock.__setitem__(0, 0.1)),
                  patch.object(check.time, "monotonic", side_effect=lambda: clock[0]),
                  patch.object(check.time, "sleep"),
                  self.assertRaisesRegex(TimeoutError, "no logical screen")):
                check.transition(command, screen, Path(directory), "test",
                                 100221, 262, [72, 34], 1, 100000)

    def test_stage_timings_separate_capture_comparison_and_diagnosis(self):
        clock = [0.0]
        captures = [0]
        with tempfile.TemporaryDirectory() as directory:
            def command(name, **arguments):
                captures[0] += 1
                clock[0] += 0.02
                Path(arguments["filename"]).write_bytes(BLACK)

            def compare(before, after):
                clock[0] += 0.3
                return 0 if captures[0] == 2 else 200000

            def screen():
                if captures[0] == 0:
                    return "response_getvalue id:100221"
                clock[0] += 4
                return "response_getvalue id:262"

            with (patch.object(check, "tap",
                               side_effect=lambda *args: clock.__setitem__(0, clock[0] + 0.15)),
                  patch.object(check, "changed_pixels", side_effect=compare),
                  patch.object(check.time, "sleep",
                               side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)),
                  patch.object(check.time, "monotonic", side_effect=lambda: clock[0])):
                result = check.transition(command, screen, Path(directory),
                                          "test", 100221, 262, [72, 34], 10, 100000)
        self.assertAlmostEqual(result["input_host_seconds"], 0.15)
        first, second = result["visual_samples"]
        self.assertIsNone(first["previous_capture_started_host_seconds"])
        self.assertEqual(second["previous_capture_started_host_seconds"],
                         first["capture_started_host_seconds"])
        for sample in (first, second):
            self.assertAlmostEqual(sample["capture_host_seconds"], 0.02)
            self.assertAlmostEqual(sample["comparison_host_seconds"], 0.3)
        self.assertAlmostEqual(result["samples"][-1]["screen_query_host_seconds"], 4)
        self.assertLess(result["first_content_change_host_seconds"],
                        result["samples"][-1]["host_seconds"] - 4)


if __name__ == "__main__":
    unittest.main()
