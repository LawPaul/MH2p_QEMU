#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Relay QEMU diagnostic pointer input to the original developer touch service."""
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import time

from hmi_diagnosis import HmiDiagnosis


def pointer_record(line, *, motion=False):
    actions = "press|release|cancel|move" if motion else "press|release|cancel"
    match = re.fullmatch(rf"({actions}) ([0-9]{{1,4}}) ([0-9]{{1,4}})\n",
                         line)
    if match is None:
        raise ValueError("Malformed QEMU diagnostic pointer record")
    action, x, y = match.groups()
    point = (int(x), int(y))
    if not 0 <= point[0] < 1920 or not 0 <= point[1] < 720:
        raise ValueError("QEMU pointer coordinates outside 1920x720")
    return action, point


class DiagnosticClicks:
    def __init__(self, tap, *, on_reset=None):
        self.tap = tap
        self.on_reset = on_reset
        self.pressed = None

    def feed(self, line):
        action, point = pointer_record(line)
        if action == "cancel":
            self.pressed = None
            if self.on_reset is not None:
                self.on_reset()
            return None
        if action == "press":
            if self.pressed is not None:
                raise ValueError("QEMU pointer pressed without releasing")
            self.pressed = point
            return None
        pressed, self.pressed = self.pressed, None
        if pressed is None:
            raise ValueError("QEMU pointer released without pressing")
        if max(abs(a - b) for a, b in zip(pressed, point)) > 8:
            raise ValueError("Diagnostic clicks do not support dragging")
        return self.tap(*pressed)


class DiagnosticGestures:
    def __init__(self, touch, *, on_reset=None):
        self.touch = touch
        self.on_reset = on_reset
        self.pressed = None

    def feed(self, line):
        action, point = pointer_record(line, motion=True)
        if action == "cancel":
            # QEMU reset also resets the guest's gesture state.
            self.pressed = None
            if self.on_reset is not None:
                self.on_reset()
            return None
        if action == "press":
            if self.pressed is not None:
                raise ValueError("QEMU pointer pressed without releasing")
        elif self.pressed is None:
            raise ValueError(f"QEMU pointer {action} without pressing")
        reply = self.touch(action, *point)
        self.pressed = None if action == "release" else point
        return reply


def follow(path, clicks, *, poll_interval=0.05):
    with path.open() as stream:
        while True:
            position = stream.tell()
            if os.fstat(stream.fileno()).st_size < position:
                raise ValueError("QEMU pointer file was truncated; restart the relay")
            line = stream.readline()
            if not line.endswith("\n"):
                stream.seek(position)
                time.sleep(poll_interval)
                continue
            reply = clicks.feed(line)
            if reply is not None:
                print(reply, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", type=Path, required=True,
                        help="Fresh chardev file written by mh2p-diagnostic-pointer")
    parser.add_argument("--host", required=True,
                        help="Reachable original guest diagnosis address")
    parser.add_argument("--diagnosis-port", type=int, default=1501)
    parser.add_argument("--gestures", action="store_true",
                        help="Forward press/move/release instead of typed clicks; "
                             "requires pointer motion=on and diagnostic release-zero override")
    args = parser.parse_args()

    with ExitStack() as connections:
        diagnosis = None

        def touch(action, x, y):
            nonlocal diagnosis
            if diagnosis is None:
                diagnosis = connections.enter_context(
                    HmiDiagnosis(args.host, args.diagnosis_port))
            return diagnosis.touch(action, x, y)

        def reset():
            nonlocal diagnosis
            connections.close()
            diagnosis = None

        relay = (DiagnosticGestures(touch, on_reset=reset) if args.gestures else
                 DiagnosticClicks(lambda x, y: touch("tap", x, y),
                                  on_reset=reset))
        try:
            follow(args.events, relay)
        except KeyboardInterrupt:
            if args.gestures and relay.pressed is not None:
                parser.exit(1, "Gesture relay stopped with a held touch; reset the "
                               "guest before restarting the relay\n")
            return
        except (OSError, ValueError, RuntimeError) as exc:
            held = ("; guest touch may remain held, reset before restarting"
                    if args.gestures and relay.pressed is not None else "")
            parser.exit(1, f"Original HMI QEMU input relay failed: {exc}{held}\n")


if __name__ == "__main__":
    main()
