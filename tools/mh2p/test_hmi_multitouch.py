# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from hmi_multitouch import (
    TransportTracePending, assess_hmi_routing, assess_terminalmode_touch_dispatch,
    coordinates, exercise, main,
    verify_hmi_raw_touch_sequence, verify_terminalmode_inactive_touch_sequence,
    verify_transport_trace,
)


def transport_trace():
    frames = [
        ([(0, 1, 373, 269), (1, 1, 639, 269)], 2),
        ([(0, 1, 346, 254), (1, 1, 666, 284)], 2),
        ([(0, 0, 346, 254), (1, 1, 666, 284)], 1),
        ([(1, 1, 682, 299)], 1),
        ([(1, 0, 682, 299)], 0),
    ]
    lines = []
    for sequence, (contacts, fingers) in enumerate(frames, 4):
        for slot, active, x, y in contacts:
            lines.append(f"mh2p_rstp_touch_contact id {slot} active {active} "
                         f"raw-x {x} raw-y {y}")
        lines += [
            f"mh2p_rstp_multitouch updates {len(contacts)} fingers {fingers} dt 1",
            f"mh2p_rstp_keypanel fid 0x0f len {6 + 6 * len(contacts)}",
            f"mh2p_rstp_tx flag 0x10 chn 0x40 sqn {sequence} len {10 + 6 * len(contacts)}",
        ]
    lines += [f"mh2p_rstp_rx flag 0x11 chn 0x40 sqn {sequence} len 6"
              for sequence in range(4, 9)]
    return "\n".join(lines) + "\n"


def raw_touch_trace():
    prefix = (
        "00:03:00.110 hmi              fw[DsiDefa.. hmi_Fw_Kbd_Input "
        "debug 18818 ~Dispatcher-dsi.HIDController.0~"
        "HIDController#updateTouchEventRaw ")
    frames = [
        ([(699, 359, 0, 4), (1198, 359, 1, 4)], 2),
        ([(649, 339, 0, 5), (1249, 379, 1, 5)], 2),
        ([(649, 339, 0, 3), (1249, 379, 1, 5)], 1),
        ([(1279, 399, 1, 5)], 1),
        ([(1279, 399, 1, 3)], 0),
    ]
    lines = [prefix + "touchEvents = null, fingetCount = 0, validFlag = 2"]
    for contacts, fingers in frames:
        events = ", ".join(
            f"TouchEvent(x={x},y={y},fingerID={finger},gestureID={gesture})"
            for x, y, finger, gesture in contacts)
        lines.append(prefix + f"touchEvents = [{events}], "
                     f"fingetCount = {fingers}, validFlag = 1")
        lines.append("unrelated interleaved original log")
    return "\r\n".join(lines) + "\r\n"


class NativeMultitouchTests(unittest.TestCase):
    def test_raw_frames_reach_inactive_controller_without_filter_rejection(self):
        raw_lines = raw_touch_trace().splitlines()
        prefix = ("00:03:00.110 hmi fw[HMIThre.. hmi_App_TerminalMode_HMI "
                  "debug 18830 ~Dispatcher-HMIEvent~")
        filtered = prefix + "[TouchPointFilter.filter] filtered: true"
        inactive = prefix.replace("debug 18830", "info 18831") + (
            "[NullTerminalModeDSIKeyEventsController.updateTouchEvents]")
        lines = [raw_lines[0]]
        for line in raw_lines[1:]:
            lines.append(line)
            if "HIDController#updateTouchEventRaw " in line:
                lines.extend((filtered, inactive))
        original = "\n".join(lines)
        result = verify_terminalmode_inactive_touch_sequence(original)
        self.assertEqual(result["original_inactive_controller_callbacks_verified"], 5)
        self.assertFalse(result["original_filter_rejected_any_contact"])
        self.assertFalse(result["live_carplay_dsi_delivery_verified"])
        for text in (
                original.replace(filtered, "", 1),
                original.replace(inactive, "", 1),
                original.replace(filtered, filtered.replace("true", "false"), 1),
                original.replace(inactive, inactive.replace("updateTouchEvents", "updateKey")),
                original.replace(filtered + "\n" + inactive, inactive + "\n" + filtered, 1),
                original + "\n" + inactive,
                original + "\n" + prefix + (
                    "[TerminalModeDSIKeyEventsController.updateTouchEvent] c=2"),
                original.replace("hmi_App_TerminalMode_HMI", "other"),
                original.replace("hmi_App_TerminalMode_HMI", "hmi_App_TerminalMode_DSI")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_terminalmode_inactive_touch_sequence(text)

    def test_terminalmode_controller_logs_do_not_assert_dsi_delivery(self):
        prefix = ("00:03:00.110 hmi fw[DsiDefa.. hmi_App_TerminalMode_HMI "
                  "debug2 18818 ~Dispatcher-dsi.HIDController.0~")
        inactive = prefix + "[NullTerminalModeDSIKeyEventsController.updateTouchEvents]"
        conversion = prefix.replace("_HMI", "_DSI") + (
            "[TerminalModeDSIKeyEventsController.updateTouchEvent] 549/289 1048/289 ")
        result = assess_terminalmode_touch_dispatch(inactive + "\n" + conversion)
        self.assertEqual(result["inactive_controller_touch_records"], [inactive])
        self.assertEqual(result["carplay_controller_conversion_records"], [conversion])
        self.assertTrue(result["controller_dispatch_evidence_present"])
        for field in ("live_carplay_dsi_delivery_verified",
                      "carplay_backend_delivery_verified", "iphone_session_verified"):
            self.assertFalse(result[field])
        for invalid in ("echo " + inactive, inactive.replace(" hmi ", " other "),
                        inactive.replace("hmi_App_TerminalMode_HMI", "other_channel"),
                        inactive.replace("updateTouchEvents]", "updateKey]"), ""):
            with self.subTest(invalid=invalid):
                self.assertFalse(assess_terminalmode_touch_dispatch(invalid)[
                    "controller_dispatch_evidence_present"])

    def test_original_raw_notification_lifecycle_and_delivery_boundary(self):
        text = raw_touch_trace()
        result = verify_hmi_raw_touch_sequence(text)
        self.assertTrue(result["original_hid_raw_touch_lifecycle_verified"])
        self.assertEqual([frame["fingers"] for frame in result["frames"]],
                         [2, 2, 1, 1, 0])
        self.assertEqual(result["frames"][3]["contacts"], [(1279, 399, 1, 5)])
        for field in ("raw_carplay_contact_delivery_verified",
                      "carplay_backend_delivery_verified", "iphone_session_verified"):
            self.assertFalse(result[field])
        self.assertEqual(verify_hmi_raw_touch_sequence(
            "echo " + text.splitlines()[1] + "\n" + text), result)

    def test_raw_notification_requires_exact_complete_original_sequence(self):
        original = raw_touch_trace()
        for text in (
                "", "\n".join(original.splitlines()[:-2]),
                original.replace("hmi_Fw_Kbd_Input", "other_channel"),
                original.replace("Dispatcher-dsi.HIDController.0", "Dispatcher-other"),
                original.replace("hmi              ", "other            "),
                original.replace("fingetCount = 1", "fingetCount = 2", 1),
                original.replace("validFlag = 1", "validFlag = 2", 1),
                original.replace("x=699", "x=700", 1),
                original.replace("fingerID=1", "fingerID=0", 1),
                original.replace("gestureID=3", "gestureID=5", 1),
                original.replace("TouchEvent(x=699", "Invalid(x=699", 1),
                original.replace("touchEvents =", "touchEvent =", 1),
                original + original.splitlines()[-2] + "\n",
                original.replace("null", "[]", 1),
                "\n".join(reversed(original.splitlines()))):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_hmi_raw_touch_sequence(text)

    def test_original_hmi_gesture_observation_preserves_raw_delivery_boundary(self):
        prefix = "00:05:19.020 DBG2  <Fw.Kbd.Input>       HIDController#updateGesture "
        text = (
            "00:05:15.910 DEBUG <Fw.Widgets.VirtualButton> "
            "#onConnectAfterChildren(): setup virtualButtonModel = "
            "{de.audi.atip.hmi.model.VirtualButtonModel@2fc92fc9_ID:3200001_HT:-1}\n"
            + prefix + "keyboardID=13, gestureID=8, stateID=1\n"
            + "unrelated interleaved original log\n"
            + prefix + "fingerCount=2, x=948, y=359\n"
            + prefix + "regionID=3, hbValue=15\n")
        result = assess_hmi_routing(text)
        self.assertTrue(result["original_virtual_button_model_3200001_connected"])
        self.assertTrue(result["original_front_touchscreen_two_contact_gesture_observed"])
        self.assertEqual(result["gesture_frames"], [{
            "keyboard": 13, "gesture": 8, "state": 1,
            "fingers": 2, "x": 948, "y": 359, "region": 3, "haptic": 15}])
        self.assertEqual(result["incomplete_gesture_records"], 0)
        for field in ("raw_carplay_contact_delivery_verified",
                      "carplay_backend_delivery_verified", "iphone_session_verified"):
            self.assertFalse(result[field])

    def test_hmi_observation_requires_complete_original_records(self):
        prefix = "00:05:19.020 DBG2 <Fw.Kbd.Input> HIDController#updateGesture "
        header = prefix + "keyboardID=13, gestureID=8, stateID=1\n"
        points = prefix + "fingerCount=2, x=948, y=359\n"
        region = prefix + "regionID=3, hbValue=15\n"
        for text in (header + points, points + region,
                     header + region, "echo " + header + points + region,
                     (header + points + region).replace("<Fw.Kbd.Input>", "<Other>")):
            with self.subTest(text=text):
                result = assess_hmi_routing(text)
                self.assertFalse(result["original_front_touchscreen_two_contact_gesture_observed"])
        self.assertEqual(assess_hmi_routing(header + header)["incomplete_gesture_records"], 2)
        self.assertFalse(assess_hmi_routing(
            (header + points + region).replace("keyboardID=13", "keyboardID=16")
        )["original_front_touchscreen_two_contact_gesture_observed"])

    def test_pixel_bounds_and_full_qmp_range(self):
        self.assertEqual([event["data"]["value"] for event in coordinates(0, 1, 1919, 719)],
                         [32767, 32767])
        self.assertEqual([event["data"]["value"] for event in coordinates(0, 1, 0, 0)],
                         [0, 0])
        for x, y in ((-1, 0), (1920, 0), (0, 720), (True, 1), (1.0, 0)):
            with self.assertRaises(ValueError):
                coordinates(0, 1, x, y)

    def test_synchronized_press_and_stable_survivor(self):
        calls = []
        result = exercise(lambda name, **args: calls.append((name, args)), pause=lambda _: None)
        self.assertEqual(len(calls), 5)
        events = calls[0][1]["events"]
        self.assertEqual([event["data"]["slot"] for event in events], [0, 0, 0, 1, 1, 1])
        self.assertEqual(calls[2][1]["events"][0]["data"]["type"], "end")
        self.assertEqual({event["data"]["slot"] for event in calls[3][1]["events"]}, {1})
        self.assertEqual(calls[4][1]["events"][0]["data"]["slot"], 1)
        self.assertEqual(calls[4][1]["events"][0]["data"]["type"], "cancel")
        self.assertFalse(result["hmi_delivery_verified"])
        self.assertFalse(result["iphone_session_verified"])

    def test_failure_cancels_both_held_contacts_and_propagates(self):
        calls = []

        def command(name, **args):
            calls.append(args["events"])
            if len(calls) == 2:
                raise RuntimeError("native input failure")

        with self.assertRaisesRegex(RuntimeError, "native input failure"):
            exercise(command, pause=lambda _: None)
        self.assertEqual([event["data"]["slot"] for event in calls[-1]], [0, 1])
        self.assertTrue(all(event["data"]["type"] == "cancel" for event in calls[-1]))

    def test_guest_trace_checks_exact_lifecycle_and_all_acks(self):
        evidence = verify_transport_trace(transport_trace())
        self.assertTrue(evidence["native_transport_guest_acknowledged"])
        self.assertEqual([frame["fingers"] for frame in evidence["reports"]], [2, 2, 1, 1, 0])
        self.assertEqual([frame["sequence"] for frame in evidence["reports"]], list(range(4, 9)))
        self.assertFalse(evidence["hmi_delivery_verified"])
        self.assertFalse(evidence["carplay_backend_delivery_verified"])

    def test_guest_trace_requires_all_reports(self):
        for text in ("", transport_trace().split("id 1 active 0")[0]):
            with self.subTest(text=text), self.assertRaises(TransportTracePending):
                verify_transport_trace(text)

    def test_guest_trace_rejects_slot_coordinate_count_and_length_changes(self):
        original = transport_trace()
        for before, after in (
                ("raw-x 373", "raw-x 374"),
                ("id 0 active 0", "id 1 active 0"),
                ("updates 2 fingers 1", "updates 2 fingers 2"),
                ("fid 0x0f len 18", "fid 0x0f len 12"),
                ("sqn 4 len 22", "sqn 4 len 21")):
            with self.subTest(before=before), self.assertRaises(ValueError):
                verify_transport_trace(original.replace(before, after, 1))

    def test_guest_trace_requires_matching_ack_after_transmission(self):
        original = transport_trace()
        ack = "mh2p_rstp_rx flag 0x11 chn 0x40 sqn 8 len 6\n"
        for text in (original.replace(ack, ""),
                     ack + original.replace(ack, ""),
                     original.replace(ack, ack.replace("sqn 8", "sqn 99"))):
            with self.subTest(text=text), self.assertRaises(TransportTracePending):
                verify_transport_trace(text)

    def test_guest_trace_partial_ack_is_pending(self):
        with self.assertRaises(TransportTracePending):
            verify_transport_trace(transport_trace().rstrip("\n"))

    def test_guest_trace_rejects_reused_sequence_numbers(self):
        with self.assertRaisesRegex(ValueError, "reused"):
            verify_transport_trace(transport_trace().replace("sqn 8 ", "sqn 7 "))

    def test_cli_verifies_new_trace_and_preserves_delivery_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            trace, output = (Path(directory) / name for name in ("trace.log", "result.json"))
            trace.write_text(transport_trace())
            calls = []

            def command(name, **args):
                calls.append((name, args))
                if len(calls) == 5:
                    with trace.open("a") as stream:
                        stream.write(transport_trace())

            with patch("sys.argv", ["hmi_multitouch", "--qmp", "unused.sock",
                                    "--trace", str(trace), "--output", str(output)]), \
                    patch("hmi_multitouch.qmp_connection") as connection, \
                    patch("hmi_multitouch.time.sleep"), patch("builtins.print"):
                connection.return_value.__enter__.return_value = command
                main()
            result = json.loads(output.read_text())
            self.assertEqual(len(calls), 5)
            self.assertEqual(len(result["native_transport"]["reports"]), 5)
            self.assertTrue(result["native_transport"]["native_transport_guest_acknowledged"])
            for name in ("hmi_delivery_verified", "carplay_backend_delivery_verified",
                         "iphone_session_verified"):
                self.assertFalse(result[name])
                self.assertFalse(result["native_transport"][name])

    def test_cli_cannot_accept_stale_guest_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            trace, output = (Path(directory) / name for name in ("trace.log", "result.json"))
            trace.write_text(transport_trace())
            with patch("sys.argv", ["hmi_multitouch", "--qmp", "unused.sock",
                                    "--trace", str(trace), "--output", str(output)]), \
                    patch("hmi_multitouch.qmp_connection"), \
                    patch("hmi_multitouch.time.sleep"), \
                    patch("hmi_multitouch.time.monotonic", side_effect=[0, 31]):
                with self.assertRaisesRegex(TimeoutError, "guest transport ACKs"):
                    main()
            self.assertEqual(output.read_text(), "")


if __name__ == "__main__":
    unittest.main()
