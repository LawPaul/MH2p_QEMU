#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send a bounded native two-contact sequence; does not assert HMI/phone delivery."""
import argparse
import json
from pathlib import Path
import re
import time

from check_hmi_native_input import qmp_connection


class TransportTracePending(ValueError):
    """The matching native sequence or its guest acknowledgments are incomplete."""


def contact_event(kind, slot, tracking, axis="x", value=0):
    return {"type": "mtt", "data": {
        "type": kind, "slot": slot, "tracking-id": tracking,
        "axis": axis, "value": value,
    }}


def coordinates(slot, tracking, x, y):
    if (type(x) is not int or type(y) is not int or
            not 0 <= x < 1920 or not 0 <= y < 720):
        raise ValueError("Contact coordinates must be integer pixels inside 1920x720")
    return [
        contact_event("data", slot, tracking, "x", round(x * 32767 / 1919)),
        contact_event("data", slot, tracking, "y", round(y * 32767 / 719)),
    ]


def exercise(command, *, pause=time.sleep):
    batches = []
    active = {0: 1001, 1: 1002}

    def send(label, events):
        command("input-send-event", events=events)
        batches.append({"label": label, "events": events})

    try:
        send("two-contact-press", [
            contact_event("begin", 0, 1001),
            *coordinates(0, 1001, 700, 360),
            contact_event("begin", 1, 1002),
            *coordinates(1, 1002, 1200, 360),
        ])
        pause(0.15)
        send("two-contact-move", [
            *coordinates(0, 1001, 650, 340),
            *coordinates(1, 1002, 1250, 380),
        ])
        pause(0.15)
        send("first-contact-lift", [contact_event("end", 0, 1001)])
        del active[0]
        send("survivor-move", coordinates(1, 1002, 1280, 400))
        pause(0.15)
    finally:
        send("cancel-held-contacts", [
            contact_event("cancel", slot, tracking)
            for slot, tracking in active.items()
        ])
    return {
        "scope": "QMP native contact injection acknowledgments only",
        "batches": batches,
        "diagnosis_touch_used": False,
        "hmi_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def verify_transport_trace(text):
    expected_contacts = [
        [(0, 1, 373, 269), (1, 1, 639, 269)],
        [(0, 1, 346, 254), (1, 1, 666, 284)],
        [(0, 0, 346, 254), (1, 1, 666, 284)],
        [(1, 1, 682, 299)],
        [(1, 0, 682, 299)],
    ]
    expected_counts = [(2, 2), (2, 2), (2, 1), (1, 1), (1, 0)]
    contacts, reports, acknowledgments = [], [], set()
    for line in text.splitlines(keepends=True):
        if not line.endswith("\n"):
            continue
        line = line.rstrip("\r\n")
        contact = re.fullmatch(
            r"mh2p_rstp_touch_contact id (\d+) active ([01]) raw-x (\d+) raw-y (\d+)",
            line)
        report = re.fullmatch(
            r"mh2p_rstp_multitouch updates (\d+) fingers (\d+) dt \d+", line)
        packet = re.fullmatch(r"mh2p_rstp_keypanel fid 0x0f len (\d+)", line)
        transmit = re.fullmatch(
            r"mh2p_rstp_tx flag 0x10 chn 0x40 sqn (\d+) len (\d+)", line)
        ack = re.fullmatch(
            r"mh2p_rstp_rx flag 0x11 chn 0x40 sqn (\d+) len 6", line)
        if contact:
            contacts.append(tuple(map(int, contact.groups())))
        elif report:
            updates, fingers = map(int, report.groups())
            index = len(reports)
            if (index >= len(expected_counts) or
                    (updates, fingers) != expected_counts[index] or
                    contacts != expected_contacts[index]):
                raise ValueError("Native contact lifecycle, coordinates or survivor slot differ")
            reports.append({"updates": updates, "fingers": fingers, "contacts": contacts})
            contacts = []
        elif packet and reports:
            current = reports[-1]
            if int(packet[1]) != 6 + 6 * current["updates"]:
                raise ValueError("Native contact payload length differs")
            if "keypanel_length" in current:
                raise ValueError("Unexpected additional native contact packet")
            current["keypanel_length"] = int(packet[1])
        elif transmit and reports:
            current = reports[-1]
            if "keypanel_length" in current and "sequence" not in current:
                if int(transmit[2]) != current["keypanel_length"] + 4:
                    raise ValueError("Native contact transport frame length differs")
                current["sequence"] = int(transmit[1])
        elif ack:
            sequence = int(ack[1])
            if any(report.get("sequence") == sequence for report in reports):
                acknowledgments.add(sequence)
    if contacts or len(reports) != len(expected_counts):
        raise TransportTracePending("Waiting for all five native contact reports")
    if any("sequence" not in report or report["sequence"] not in acknowledgments
           for report in reports):
        raise TransportTracePending("Waiting for matching guest transport acknowledgments")
    if len({report["sequence"] for report in reports}) != len(reports):
        raise ValueError("Native contact sequence numbers were reused")
    return {
        "scope": "exact native contact lifecycle and guest RSTP transport acknowledgments",
        "reports": reports,
        "native_transport_guest_acknowledged": True,
        "hmi_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def assess_hmi_routing(text):
    """Observe original TCP log records; gesture delivery is not raw CarPlay delivery."""
    prefix = r"^\d{2}:\d{2}:\d{2}\.\d{3} DBG2\s+<Fw\.Kbd\.Input>\s+"
    header = re.compile(prefix + r"HIDController#updateGesture keyboardID=(\d+), "
                        r"gestureID=(\d+), stateID=(\d+)$")
    points = re.compile(prefix + r"HIDController#updateGesture fingerCount=(\d+), "
                        r"x=(\d+), y=(\d+)$")
    region = re.compile(prefix + r"HIDController#updateGesture regionID=(\d+), "
                        r"hbValue=(\d+)$")
    widget = re.compile(
        r"^\d{2}:\d{2}:\d{2}\.\d{3} DEBUG\s+<Fw\.Widgets\.VirtualButton>\s+"
        r"#onConnectAfterChildren\(\): setup virtualButtonModel = "
        r"\{de\.audi\.atip\.hmi\.model\.VirtualButtonModel@[0-9a-fA-F]+"
        r"_ID:3200001_HT:-?\d+\}$")
    frames, current = [], None
    connected = False
    incomplete = 0
    for line in text.splitlines():
        if widget.fullmatch(line):
            connected = True
        match = header.fullmatch(line)
        if match:
            incomplete += current is not None
            current = dict(zip(("keyboard", "gesture", "state"), map(int, match.groups())))
        elif match := points.fullmatch(line):
            if current is not None and "fingers" not in current:
                current.update(zip(("fingers", "x", "y"), map(int, match.groups())))
        elif match := region.fullmatch(line):
            if current is not None and "fingers" in current:
                current.update(zip(("region", "haptic"), map(int, match.groups())))
                frames.append(current)
                current = None
    incomplete += current is not None
    return {
        "scope": "original Java HMI gesture and virtual-button connection observations",
        "original_virtual_button_model_3200001_connected": connected,
        "original_front_touchscreen_two_contact_gesture_observed": any(
            frame["keyboard"] == 13 and frame["fingers"] == 2 for frame in frames),
        "gesture_frames": frames,
        "incomplete_gesture_records": incomplete,
        "raw_carplay_contact_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def verify_hmi_raw_touch_sequence(text):
    """Verify the bounded exercise at the original HID-to-Java notification boundary."""
    prefix = re.compile(
        r"^\d{2}:\d{2}:\d{2}\.\d{3}\s+hmi\s+\S+\s+hmi_Fw_Kbd_Input\s+"
        r"debug\s+\d+\s+~Dispatcher-dsi\.HIDController\.0~"
        r"HIDController#updateTouchEventRaw ")
    payload = re.compile(
        r"touchEvents = (null|\[.*\]), fingetCount = (\d+), validFlag = (\d+)$")
    point = re.compile(
        r"TouchEvent\(x=(\d+),y=(\d+),fingerID=(\d+),gestureID=(\d+)\)")
    expected = [
        ([(699, 359, 0, 4), (1198, 359, 1, 4)], 2),
        ([(649, 339, 0, 5), (1249, 379, 1, 5)], 2),
        ([(649, 339, 0, 3), (1249, 379, 1, 5)], 1),
        ([(1279, 399, 1, 5)], 1),
        ([(1279, 399, 1, 3)], 0),
    ]
    frames = []
    for line in text.splitlines():
        header = prefix.match(line)
        if header is None:
            continue
        record = payload.fullmatch(line[header.end():])
        if record is None:
            raise ValueError("Malformed original HID raw touch notification")
        events, fingers, valid = record.groups()
        if events == "null" and (fingers, valid) == ("0", "2") and not frames:
            continue
        entries = events[1:-1].split(", ") if events.startswith("[") else []
        contacts = []
        for entry in entries:
            match = point.fullmatch(entry)
            if match is None:
                raise ValueError("Malformed original HID raw touch contact")
            contacts.append(tuple(map(int, match.groups())))
        index = len(frames)
        if (index >= len(expected) or valid != "1" or
                (contacts, int(fingers)) != expected[index]):
            raise ValueError("Original HID raw touch lifecycle or coordinates differ")
        frames.append({"contacts": contacts, "fingers": int(fingers)})
    if len(frames) != len(expected):
        raise ValueError("Expected all five original HID raw touch notifications")
    return {
        "scope": "original HID-to-Java raw notification boundary, not CarPlay output",
        "original_hid_raw_touch_lifecycle_verified": True,
        "frames": frames,
        "raw_carplay_contact_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def assess_terminalmode_touch_dispatch(text):
    """Observe original controller selection without inferring a successful DSI call."""
    prefix = re.compile(
        r"^\d{2}:\d{2}:\d{2}\.\d{3}\s+hmi\s+\S+\s+"
        r"hmi_App_TerminalMode_(?:HMI|DSI)\s+"
        r"(?:debug2|debug|trace|info)\s+\d+\s+(?:~[^~]*~)?")
    inactive, conversion = [], []
    for line in text.splitlines():
        header = prefix.match(line)
        if header is None:
            continue
        message = line[header.end():]
        if message == "[NullTerminalModeDSIKeyEventsController.updateTouchEvents]":
            inactive.append(line)
        elif message.startswith("[TerminalModeDSIKeyEventsController.updateTouchEvent] "):
            conversion.append(line)
    return {
        "scope": "original TerminalMode controller logs, not backend/phone delivery",
        "inactive_controller_touch_records": inactive,
        "carplay_controller_conversion_records": conversion,
        "original_inactive_controller_observed": bool(inactive),
        "original_carplay_controller_log_observed": bool(conversion),
        "controller_dispatch_evidence_present": bool(inactive or conversion),
        "live_carplay_dsi_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def verify_terminalmode_inactive_touch_sequence(text):
    """Verify all exercise frames reach the original inactive controller, unrejected."""
    raw = verify_hmi_raw_touch_sequence(text)
    relevant = re.compile(
        r"^\d{2}:\d{2}:\d{2}\.\d{3}\s+hmi\s+\S+\s+"
        r"(?P<channel>hmi_Fw_Kbd_Input|hmi_App_TerminalMode_HMI|"
        r"hmi_App_TerminalMode_DSI)\s+"
        r"(?:debug2|debug|trace|info)\s+\d+\s+(?:~[^~]*~)?(?P<message>.*)$")
    stage, completed = 0, 0
    for line in text.splitlines():
        match = relevant.fullmatch(line)
        if match is None:
            continue
        channel, message = match.group("channel", "message")
        if channel == "hmi_Fw_Kbd_Input":
            if not message.startswith("HIDController#updateTouchEventRaw "):
                continue
            if "touchEvents = null, fingetCount = 0, validFlag = 2" in message:
                continue
            if stage:
                raise ValueError("Original raw frame did not complete inactive dispatch")
            stage = 1
        elif message.startswith("[TouchPointFilter.filter] filtered: "):
            # The original log prints filteredFingers.isEmpty(), not isFiltered().
            if (channel != "hmi_App_TerminalMode_HMI" or stage != 1 or
                    message != "[TouchPointFilter.filter] filtered: true"):
                raise ValueError("Original raw filter rejected contacts or differs")
            stage = 2
        elif message == "[NullTerminalModeDSIKeyEventsController.updateTouchEvents]":
            if channel != "hmi_App_TerminalMode_HMI" or stage != 2:
                raise ValueError("Original inactive callback is out of sequence")
            completed += 1
            stage = 0
        elif message.startswith("[TerminalModeDSIKeyEventsController.updateTouchEvent]"):
            raise ValueError("CarPlay controller dispatched instead of inactive controller")
    if stage or completed != 5:
        raise ValueError("Expected all five unrejected original inactive touch callbacks")
    return {
        "scope": "original native raw exercise through filter to inactive controller",
        "original_hid_raw_touch_lifecycle_verified": raw[
            "original_hid_raw_touch_lifecycle_verified"],
        "original_filter_rejected_any_contact": False,
        "original_inactive_controller_callbacks_verified": completed,
        "live_carplay_dsi_delivery_verified": False,
        "carplay_backend_delivery_verified": False,
        "iphone_session_verified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New evidence file; must not exist")
    parser.add_argument("--trace", type=Path,
                        help="Existing mh2p_rstp_* trace log; verify only newly appended reports")
    args = parser.parse_args()
    trace_offset = args.trace.stat().st_size if args.trace else None
    with args.output.open("x") as stream:
        with qmp_connection(args.qmp, 10) as command:
            result = exercise(command)
        if args.trace:
            deadline = time.monotonic() + 30
            while True:
                with args.trace.open("rb") as trace:
                    trace.seek(trace_offset)
                    text = trace.read().decode("utf-8")
                try:
                    result["native_transport"] = verify_transport_trace(text)
                    break
                except TransportTracePending:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(
                            "Native contact sequence did not receive all guest transport ACKs")
                    time.sleep(0.1)
            result["scope"] = "QMP injection with independently verified native guest transport ACKs"
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print("Native contact sequence acknowledged" +
          (" by guest transport" if args.trace else " by QMP") +
          "; HMI/CarPlay delivery is not asserted")


if __name__ == "__main__":
    main()
