#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify reversible native RSTP touch on an already running paired HMI."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import socket
import time
import uuid

from hmi_development_ui import frame_pixels
from hmi_live_control import publish_control_request, request_control
from hmi_diagnosis import DiagnosisError, original_getter_failed


def screen_id(reply):
    if not reply.startswith("response_getvalue"):
        raise ValueError(f"Unexpected screen response: {reply!r}")
    # ScreenData can include overlays; the first ID is the main screen.
    match = re.search(r"\bid:(\d+)\b", reply)
    if match is None:
        raise ValueError(f"No original main screen ID: {reply!r}")
    return int(match[1])


def changed_pixels(before, after):
    frame_pixels(before)
    frame_pixels(after)
    before = before.split(b"\n", 3)[3]
    after = after.split(b"\n", 3)[3]
    # Exclude chrome and skip unchanged rows/spans before counting RGB pixels.
    changed = 0
    for row in range(80, 700):
        start = (row * 1920 + 200) * 3
        end = (row * 1920 + 1900) * 3
        left_row = before[start:end]
        right_row = after[start:end]
        if left_row == right_row:
            continue
        for at in range(0, len(left_row), 192):
            left = left_row[at:at + 192]
            right = right_row[at:at + 192]
            if left != right:
                changed += sum(left[p:p + 3] != right[p:p + 3]
                               for p in range(0, len(left), 3))
    return changed


@contextmanager
def qmp_connection(path, timeout):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(timeout)
        sock.connect(str(path))
        with sock.makefile("rwb", buffering=0) as wire:
            if "QMP" not in json.loads(wire.readline()):
                raise RuntimeError("Missing QMP greeting")

            def command(name, **arguments):
                token = uuid.uuid4().hex
                wire.write(json.dumps({
                    "execute": name, "arguments": arguments, "id": token,
                }).encode() + b"\n")
                while True:
                    line = wire.readline()
                    if not line:
                        raise RuntimeError("QMP connection closed")
                    response = json.loads(line)
                    if response.get("id") != token:
                        continue
                    if "error" in response:
                        raise RuntimeError(f"QMP {name}: {response['error']}")
                    return response["return"]

            command("qmp_capabilities")
            yield command


def query_diagnosis(directory, command, expected, timeout):
    reply = request_control(directory, "diagnosis", command, timeout)
    if not isinstance(reply, str) or not reply.startswith(expected):
        raise ValueError(f"Unexpected diagnosis response: {reply!r}")
    if original_getter_failed(reply):
        raise DiagnosisError(f"Original diagnosis method failed: {reply!r}")
    return reply


def query_screen(directory, timeout):
    reply = query_diagnosis(directory, "getvalue,102,getCurrentScreenData",
                            "response_getvalue", timeout)
    screen_id(reply)
    return reply


def wait_for_bundle(directory, bundle_id, name, timeout):
    if type(bundle_id) is not int or not 0 <= bundle_id <= 999 or not name or timeout <= 0:
        raise ValueError("Require a bundle ID in 0..999, name and positive timeout")
    started = time.monotonic()
    deadline = started + timeout
    samples = []
    while time.monotonic() < deadline:
        reply = query_diagnosis(
            directory, f"getbundleinfo,{bundle_id}", "response_getbundleinfo",
            min(120, max(0.1, deadline - time.monotonic())))
        match = re.fullmatch(
            r'response_getbundleinfo===== Bundle "([^"]+)" =====#'
            r'\s+ID: ([0-9]+)#.*?#\s+State: ([A-Z_]+)#.*',
            reply, re.DOTALL)
        if match is None:
            raise ValueError(f"Malformed original bundle information: {reply!r}")
        if match[1] != name or int(match[2]) != bundle_id:
            raise ValueError(
                f"Expected bundle {bundle_id} {name}, got {match[2]} {match[1]}")
        samples.append({"state": match[3],
                        "host_seconds": time.monotonic() - started})
        if match[3] == "ACTIVE":
            return {"id": bundle_id, "name": name, "samples": samples}
        time.sleep(min(1, max(0, deadline - time.monotonic())))
    raise TimeoutError(
        f"Bundle {bundle_id} {name} did not become ACTIVE within {timeout}s: {samples}")


def tap(command, x, y):
    if not (0 <= x < 1920 and 0 <= y < 720):
        raise ValueError("Touch must be inside the 1920x720 display")
    command("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / 1919)}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / 719)}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ])
    try:
        time.sleep(0.15)
    finally:
        command("input-send-event", events=[
            {"type": "btn", "data": {"button": "left", "down": False}},
        ])


def transition(command, screen, output, label, source, target, point,
               timeout, threshold):
    initial = screen()
    if screen_id(initial) != source:
        raise RuntimeError(f"Expected initial screen {source}, got {initial}")
    before_path = (output / f"{label}-before.ppm").resolve()
    after_path = (output / f"{label}-after.ppm").resolve()
    command("screendump", device="display0", filename=str(before_path))
    before = before_path.read_bytes()
    frame_pixels(before)
    started = time.monotonic()
    tap(command, *point)
    input_seconds = time.monotonic() - started
    deadline = started + timeout
    samples = []
    visual_samples = []
    first_visible = None
    previous_capture_started = None
    while time.monotonic() < deadline:
        capture_started = time.monotonic() - started
        command("screendump", device="display0", filename=str(after_path))
        observed = time.monotonic() - started
        compare_started = time.monotonic()
        changed = changed_pixels(before, after_path.read_bytes())
        comparison_seconds = time.monotonic() - compare_started
        visual_samples.append({"changed_content_pixels": changed,
                               "host_seconds": observed,
                               "capture_started_host_seconds": capture_started,
                               "capture_host_seconds": observed - capture_started,
                               "comparison_host_seconds": comparison_seconds,
                               "previous_capture_started_host_seconds":
                                   previous_capture_started})
        if changed >= threshold:
            if first_visible is None:
                first_visible = observed
            query_started = time.monotonic()
            reply = screen()
            query_seconds = time.monotonic() - query_started
            # Logical confirmation may block while the original HMI is busy.
            command("screendump", device="display0", filename=str(after_path))
            confirmed = changed_pixels(before, after_path.read_bytes())
            samples.append({
                "screen": reply, "changed_content_pixels": confirmed,
                "host_seconds": time.monotonic() - started,
                "visible_host_seconds": observed,
                "observed_changed_content_pixels": changed,
                "screen_query_host_seconds": query_seconds,
            })
            if screen_id(reply) == target and confirmed >= threshold:
                return {"from": source, "to": target, "touch": point,
                        "initial_screen": initial, "samples": samples,
                        "visual_samples": visual_samples,
                        "input_host_seconds": input_seconds,
                        "first_content_change_host_seconds": first_visible}
        previous_capture_started = capture_started
        time.sleep(0.1)
    raise TimeoutError(
        f"{label}: no logical screen {target} with {threshold} changed content "
        f"pixels within {timeout}s; observations: {samples}; "
        f"visual observations: {visual_samples}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-directory", required=True, type=Path,
                        help="Live paired harness directory; no other control client may run")
    parser.add_argument("--output", required=True, type=Path,
                        help="New evidence directory")
    parser.add_argument("--from-screen", type=int, required=True)
    parser.add_argument("--to-screen", type=int, required=True)
    parser.add_argument("--tap", nargs=2, type=int, required=True)
    parser.add_argument("--return-tap", nargs=2, type=int, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--wait-for-bundle", nargs=2, action="append", default=[],
                        metavar=("ID", "NAME"),
                        help="Wait for this original bundle to be ACTIVE before touching; repeatable")
    parser.add_argument("--readiness-timeout", type=float, default=600,
                        help="Maximum wait per required bundle, in host seconds")
    parser.add_argument("--minimum-changed-pixels", type=int, default=100000)
    args = parser.parse_args()
    if (args.timeout <= 0 or args.readiness_timeout <= 0 or
            any(not bundle_id.isascii() or not bundle_id.isdecimal() or
                not 0 <= int(bundle_id) <= 999 or not name
                for bundle_id, name in args.wait_for_bundle) or
            args.from_screen == args.to_screen or
            not 1 <= args.minimum_changed_pixels <= 1700 * 620 or
            any(not (0 <= x < 1920 and 0 <= y < 720)
                for x, y in (args.tap, args.return_tap))):
        parser.error("Use positive bounds, distinct screen IDs and in-display touches")
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"native_rstp_touch_verified": False,
              "full_hmi_readiness_verified": False, "transitions": []}
    try:
        with qmp_connection(args.control_directory / "control-qmp.sock", 30) as command:
            mice = command("query-mice")
            if not any(mouse["name"] == "MH2P RCC keypanel" and
                       mouse["absolute"] and mouse["current"] for mouse in mice):
                raise RuntimeError(f"Native RSTP pointer is not active: {mice}")
            result["input_devices"] = mice
            result["required_bundles"] = []
            for bundle_id, name in args.wait_for_bundle:
                result["required_bundles"].append(wait_for_bundle(
                    args.control_directory, int(bundle_id), name,
                    args.readiness_timeout))
            for label, source, target, point in (
                    ("outbound", args.from_screen, args.to_screen, args.tap),
                    ("return", args.to_screen, args.from_screen, args.return_tap)):
                result["transitions"].append(transition(
                    command, lambda: query_screen(args.control_directory, args.timeout),
                    args.output, label, source, target, point, args.timeout,
                    args.minimum_changed_pixels))
            result["native_rstp_touch_verified"] = True
    except (OSError, ValueError, RuntimeError) as exc:
        result["error"] = str(exc)
        raise
    finally:
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
