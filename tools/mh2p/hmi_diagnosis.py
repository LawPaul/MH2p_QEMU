#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in client for the original HMI's development diagnosis TCP service."""
import argparse
import json
import re
import socket


UNIT_GETTERS = {
    "distance": "getDistance",
    "pressure": "getPressure",
    "speed": "getSpeed",
    "temperature": "getTemperature",
    "volume": "getVolume",
    "consumption_petrol": "getConsumptionPetrol",
    "consumption_electric": "getConsumptionElectric",
    "date_format": "getDateFormat",
    "time_format": "getTimeFormat",
}


class DiagnosisError(RuntimeError):
    pass


def is_read_only_command(command):
    return isinstance(command, str) and re.fullmatch(
        r"getapps|getbundles|getcommands,[0-9]{1,8}|getkeys,[0-9]{1,8}|"
        r"getbundleinfo,[0-9]{1,3}|getvalue,[0-9]{1,8},[A-Za-z0-9_]+",
        command) is not None


def listener_wait_command():
    pattern = (
        r"^[[:space:]]*tcp[46]?[[:space:]]+[0-9]+[[:space:]]+[0-9]+"
        r"[[:space:]]+[^[:space:]]+[.:]1501[[:space:]]+[^[:space:]]+"
        r"[[:space:]]+LISTEN[[:space:]]*$")
    return (
        "DIAG_LISTENER_N=0; DIAG_LISTENER_READY=0; "
        'while [ "$DIAG_LISTENER_N" -lt 120 ]; do '
        "netstat -an >/tmp/diagnosis-listeners 2>&1 || "
        "{ cat /tmp/diagnosis-listeners; exit 1; }; "
        f"if grep -E '{pattern}' /tmp/diagnosis-listeners; then "
        "DIAG_LISTENER_READY=1; break; fi; "
        "DIAG_LISTENER_N=$((DIAG_LISTENER_N + 1)); sleep 1 || exit 1; done; "
        'if [ "$DIAG_LISTENER_READY" = 1 ]; then echo ORIGINAL_DIAG_LISTENER_READY; '
        "else cat /tmp/diagnosis-listeners; echo ORIGINAL_DIAG_LISTENER_TIMEOUT; "
        "exit 1; fi")


def original_getter_failed(reply):
    prefix = "response_getvalue"
    if not reply.startswith(prefix):
        return False
    value = reply[len(prefix):]
    return value.startswith("Exception occured in ") or re.fullmatch(
        r"(?:[A-Za-z_$][\w$]*\.)+[A-Za-z_$][\w$]*(?:Exception|Error)(?::.*)?",
        value, re.DOTALL) is not None


class HmiDiagnosis:
    def __init__(self, host, port=1501, timeout=5):
        self.socket = socket.create_connection((host, port), timeout)
        self.reader = self.socket.makefile("rb")
        try:
            self.request("diag", "response_diag")
        except BaseException:
            self.close()
            raise

    def close(self):
        self.reader.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, command, expected, *, appended_value=False):
        self.socket.sendall((command + "\n").encode("utf-8"))
        reply = self.reader.readline(65537)
        if len(reply) > 65536 or not reply.endswith(b"\n"):
            raise DiagnosisError("Missing, truncated or oversized diagnosis reply")
        line = reply.decode("utf-8").rstrip("\r\n")
        valid = (line.startswith(expected) if appended_value else
                 line == expected or line.startswith(expected + " "))
        if not valid:
            raise DiagnosisError(f"Unexpected diagnosis reply: {line!r}")
        if original_getter_failed(line):
            raise DiagnosisError(f"Original diagnosis method failed: {line!r}")
        return line

    def screen(self):
        return self.request("getvalue,102,getCurrentScreenData",
                            "response_getvalue", appended_value=True)

    def commands(self):
        return self.request("getcommands,102", "response_getcommands",
                            appended_value=True)

    def units(self):
        return {
            name: self.request(f"getvalue,10003,{method}", "response_getvalue",
                               appended_value=True)
            for name, method in UNIT_GETTERS.items()
        }

    def touch(self, action, x, y):
        methods = {
            "tap": "cmdTouchScreenTyped",
            "press": "cmdGestureScreenPressed",
            "release": "cmdGestureScreenReleased",
            "move": "cmdGestureScreenMoved",
        }
        if action not in methods:
            raise ValueError(f"Unknown touch action: {action}")
        if (type(x) is not int or type(y) is not int or
                not 0 <= x < 1920 or not 0 <= y < 720):
            raise ValueError("Touch coordinates must be within 1920x720")
        reply = self.request(
            f"processcommand,102,{methods[action]},{x},{y}",
            "response_processcommand")
        if reply[len("response_processcommand"):].strip() != (
                "Call executed successfully."):
            raise DiagnosisError(f"Touch command was not executed: {reply!r}")
        return reply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True,
                        help="Reachable guest or explicitly configured TCP relay")
    parser.add_argument("--port", type=int, default=1501)
    parser.add_argument("--timeout", type=float, default=5)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("screen")
    actions.add_parser("commands")
    actions.add_parser(
        "units", help="Read original unit settings and view options; no setters")
    for action in ("tap", "press", "release", "move"):
        touch = actions.add_parser(action)
        touch.add_argument("x", type=int)
        touch.add_argument("y", type=int)
    args = parser.parse_args()
    if args.timeout <= 0 or not 1 <= args.port <= 65535:
        parser.error("Timeout must be positive and port must be in 1..65535")
    try:
        with HmiDiagnosis(args.host, args.port, args.timeout) as diagnosis:
            if args.action == "screen":
                reply = diagnosis.screen()
            elif args.action == "commands":
                reply = diagnosis.commands()
            elif args.action == "units":
                reply = diagnosis.units()
            else:
                reply = diagnosis.touch(args.action, args.x, args.y)
    except (OSError, UnicodeError, ValueError, DiagnosisError) as exc:
        parser.exit(1, f"HMI diagnosis failed: {exc}\n")
    print(json.dumps({"action": args.action, "reply": reply,
                      "native_touch_verified": False}))


if __name__ == "__main__":
    main()
