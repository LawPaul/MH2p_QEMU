"""Firmware-free tests for the opt-in original HID endpoint startup gate."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from prepare_hmi_development import (
    HID_DISPLAY_GATE_PATH, HID_DISPLAY_GATE_SCRIPT, hid_display_gate,
    hid_display_startup,
)


class HidDisplayGateTests(unittest.TestCase):
    def test_only_hid_init_waits_before_run_with_original_deadlines(self):
        init = {"cmd": "init", "app": "hidcontroller", "apprunmode": 0,
                "ps_access": "PS_ACC_READ_WRITE", "timeout": 20000, "blocking": False}
        run = {"cmd": "run", "app": "hidcontroller", "timeout": 20000, "blocking": False}
        source = {"startup": [
            {"cmd": "run", "app": "other", "timeout": 123, "blocking": False},
            {"cmd": "fork", "commands": [
                {"cmd": "init", "app": "audioProc", "blocking": False},
                init, run]},
        ], "shutdown": [], "keep": True}
        expected = json.loads(json.dumps(source))
        expected["startup"][1]["commands"][1]["blocking"] = True
        self.assertEqual(hid_display_startup(json.dumps(source)), expected)
        self.assertFalse(init["blocking"])
        for field, value in (("blocking", True), ("timeout", 30000),
                             ("apprunmode", 1), ("ps_access", "other")):
            invalid = json.loads(json.dumps(source))
            invalid["startup"][1]["commands"][1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                hid_display_startup(json.dumps(invalid))
        for commands in ([run, init], [init], [init, run, init, run]):
            invalid = json.loads(json.dumps(source))
            invalid["startup"][1]["commands"] = commands
            with self.subTest(commands=commands), self.assertRaises(ValueError):
                hid_display_startup(json.dumps(invalid))
        for invalid in ({}, {"startup": None},
                        {"startup": [{"cmd": "fork", "commands": None}]}):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                hid_display_startup(json.dumps(invalid))

    def test_only_original_hid_executable_changes(self):
        original = {"procs": [
            {"name": "hidcontroller", "node": "mmx", "id": 366,
             "exec": "/run/enforcer/eso/bin/apps/hidcontroller",
             "environment": {"KEEP": "original"}},
            {"name": "other", "exec": "/original"},
        ], "keep": {"startup": True}}
        expected = json.loads(json.dumps(original))
        expected["procs"][0]["exec"] = HID_DISPLAY_GATE_PATH
        self.assertEqual(hid_display_gate(json.dumps(original)), expected)
        self.assertEqual(original["procs"][0]["exec"],
                         "/run/enforcer/eso/bin/apps/hidcontroller")
        for field, value in (("id", 367), ("node", "rcc"), ("exec", "/other")):
            invalid = json.loads(json.dumps(original))
            invalid["procs"][0][field] = value
            with self.assertRaises(ValueError):
                hid_display_gate(json.dumps(invalid))
        original["procs"].append(original["procs"][0].copy())
        with self.assertRaises(ValueError):
            hid_display_gate(json.dumps(original))

    def test_endpoint_gate_fails_closed_and_preserves_arguments(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-hid-gate-") as tmp:
            directory = Path(tmp)
            wait = directory / "wait"
            native = directory / "native"
            gate = directory / "gate"
            wait.write_text('test "$1" = /dev/name/local/ipte-dm && '
                            'test "$2" = 18 || exit 9\nexit "$WAIT_STATUS"\n')
            native.write_text('#!/bin/sh\nprintf "<%s>\\n" "$@"\n')
            native.chmod(0o700)
            script = HID_DISPLAY_GATE_SCRIPT.replace(
                "waitfor /dev/name/local/ipte-dm",
                f'sh "{wait}" /dev/name/local/ipte-dm').replace(
                '/run/enforcer/eso/bin/apps/hidcontroller', f'"{native}"')
            gate.write_text(script)
            for status in ("0", "1"):
                result = subprocess.run(
                    ["sh", str(gate), "-sm-name", "hidcontroller", "a b", ""],
                    env=dict(os.environ, WAIT_STATUS=status), capture_output=True,
                    text=True, timeout=5)
                if status == "0":
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout,
                                     "<-sm-name>\n<hidcontroller>\n<a b>\n<>\n")
                    self.assertIn("endpoint ready", result.stderr)
                else:
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual(result.stdout, "")
                    self.assertIn("endpoint unavailable", result.stderr)
