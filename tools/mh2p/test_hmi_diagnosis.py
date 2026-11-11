# SPDX-License-Identifier: GPL-2.0-or-later
from contextlib import redirect_stdout
from io import StringIO
import json
import socket
import subprocess
import re
import unittest
from unittest.mock import patch

from hmi_diagnosis import (
    DiagnosisError, HmiDiagnosis, UNIT_GETTERS, is_read_only_command,
    listener_wait_command, main)


class HmiDiagnosisTests(unittest.TestCase):
    def test_listener_wait_is_bounded_and_matches_local_tcp_listener_only(self):
        command = listener_wait_command()
        self.assertIn('"$DIAG_LISTENER_N" -lt 120', command)
        self.assertIn("sleep 1 || exit 1", command)
        self.assertIn("netstat -an", command)
        self.assertLess(len(command), 800)
        parsed = subprocess.run(["sh", "-n"], input=command, text=True, capture_output=True)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        pattern = re.search(r"grep -E '([^']+)'", command)[1]
        for line, expected in (
                ("tcp 0 0 127.0.0.1.1501 *.* LISTEN", True),
                ("tcp4 0 0 *.1501 *.* LISTEN", True),
                ("tcp6 0 0 ::1:1501 *:* LISTEN", True),
                ("tcp 0 0 *.15010 *.* LISTEN", False),
                ("tcp 0 0 *.1 127.0.0.1.1501 LISTEN", False),
                ("tcp 0 0 *.1501 *.* ESTABLISHED", False),
                ("udp 0 0 *.1501 *.* LISTEN", False),
                ("class load: TcpDiagnosisServer", False)):
            result = subprocess.run(["grep", "-E", pattern], input=line + "\n",
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode == 0, expected, line)

    def test_read_only_surface_includes_bundle_inventory_but_not_commands(self):
        for command in ("getapps", "getbundles", "getcommands,101", "getkeys,32",
                        "getbundleinfo,123", "getvalue,102,getCurrentScreenData"):
            self.assertTrue(is_read_only_command(command), command)
        for command in ("getbundles\n", "getapps;anything", "getbundleinfo,1000",
                        "getvalue,32,Configuration\n", "", None,
                        "processcommand,101,cmdStartApp,TerminalMode",
                        "processcommand,101,cmdChangeToApp,32"):
            self.assertFalse(is_read_only_command(command), command)

    def setUp(self):
        self.client, self.server = socket.socketpair()
        self.client.settimeout(0.1)
        self.addCleanup(self.server.close)
        self.addCleanup(self.client.close)
        self.connector = patch("hmi_diagnosis.socket.create_connection",
                               return_value=self.client)
        self.connector.start()
        self.addCleanup(self.connector.stop)

    def connect(self):
        self.server.sendall(b"response_diag\n")
        diagnosis = HmiDiagnosis("example", timeout=0.1)
        self.addCleanup(diagnosis.close)
        self.assertEqual(self.server.recv(4096), b"diag\n")
        return diagnosis

    def test_handshake_and_screen(self):
        diagnosis = self.connect()
        self.server.sendall(b"response_getvaluecurrent-screen-data\n")
        self.assertEqual(diagnosis.screen(),
                         "response_getvaluecurrent-screen-data")
        self.assertEqual(self.server.recv(4096),
                         b"getvalue,102,getCurrentScreenData\n")

    def test_touch_methods_and_exact_success(self):
        diagnosis = self.connect()
        for action, method in (
                ("tap", "cmdTouchScreenTyped"),
                ("press", "cmdGestureScreenPressed"),
                ("release", "cmdGestureScreenReleased"),
                ("move", "cmdGestureScreenMoved")):
            with self.subTest(action=action):
                self.server.sendall(
                    b"response_processcommand   Call executed successfully.\r\n")
                diagnosis.touch(action, 1919, 719)
                self.assertEqual(
                    self.server.recv(4096),
                    f"processcommand,102,{method},1919,719\n".encode())

    def test_units_reads_all_original_settings_and_preserves_view_options(self):
        diagnosis = self.connect()
        expected = {
            name: f"response_getvalue{index} Disabled"
            for index, name in enumerate(UNIT_GETTERS)
        }
        self.server.sendall(
            ("\r\n".join(expected.values()) + "\r\n").encode())
        self.assertEqual(diagnosis.units(), expected)
        sent = self.server.recv(4096).decode().splitlines()
        self.assertEqual(sent, [
            f"getvalue,10003,{method}" for method in UNIT_GETTERS.values()
        ])
        self.assertFalse(any("processcommand" in command for command in sent))

    def test_units_does_not_replace_missing_original_data_with_defaults(self):
        diagnosis = self.connect()
        self.server.sendall(
            b"response_getvalueException occured in getDistance() : "
            b"java.lang.reflect.InvocationTargetException\n")
        with self.assertRaises(DiagnosisError):
            diagnosis.units()
        self.assertEqual(self.server.recv(4096), b"getvalue,10003,getDistance\n")

    def test_units_preserves_specific_original_disabling_reason(self):
        diagnosis = self.connect()
        reasons = [
            "DisabledClamp15", "DisabledSpeed", "DisabledDefect",
            "DisabledStandstill", "Visible", "Invisible", "Disabled",
            "Disabled", "Disabled",
        ]
        replies = [f"response_getvalue{index} {reason}"
                   for index, reason in enumerate(reasons)]
        self.server.sendall(("\n".join(replies) + "\n").encode())
        self.assertEqual(list(diagnosis.units().values()), replies)

    def test_units_cli_emits_original_replies_without_claiming_native_touch(self):
        expected = {name: f"response_getvalue{name} Disabled"
                    for name in UNIT_GETTERS}
        self.server.sendall(
            ("response_diag\n" + "\n".join(expected.values()) + "\n").encode())
        output = StringIO()
        with (patch("sys.argv", ["hmi_diagnosis.py", "--host", "example", "units"]),
              redirect_stdout(output)):
            main()
        result = json.loads(output.getvalue())
        self.assertEqual(result["action"], "units")
        self.assertEqual(result["reply"], expected)
        self.assertFalse(result["native_touch_verified"])
        commands = self.server.recv(4096).decode().splitlines()
        self.assertEqual(commands, ["diag", *[
            f"getvalue,10003,{method}" for method in UNIT_GETTERS.values()
        ]])

    def test_rejects_invalid_input_before_sending(self):
        diagnosis = self.connect()
        for action, x, y in (("tap", -1, 0), ("tap", 1920, 0),
                             ("tap", 0, 720), ("tap", True, 0),
                             ("tap", 0.5, 0), ("other", 0, 0)):
            with self.subTest(action=action, x=x, y=y):
                with self.assertRaises(ValueError):
                    diagnosis.touch(action, x, y)
        self.server.settimeout(0.01)
        with self.assertRaises(TimeoutError):
            self.server.recv(1)

    def test_rejects_unknown_handshake_and_closes_connection(self):
        self.server.sendall(b"unknown\n")
        with self.assertRaises(DiagnosisError):
            HmiDiagnosis("example")
        self.assertEqual(self.client.fileno(), -1)

    def test_rejects_failed_touch(self):
        diagnosis = self.connect()
        self.server.sendall(b"response_processcommand   No such method\n")
        with self.assertRaises(DiagnosisError):
            diagnosis.touch("tap", 0, 0)

    def test_rejects_original_reflection_exception(self):
        diagnosis = self.connect()
        self.server.sendall(
            b"response_getvalueException occured in getCurrentScreenData() : "
            b"java.lang.reflect.InvocationTargetException\n")
        with self.assertRaises(DiagnosisError):
            diagnosis.screen()

    def test_rejects_raw_original_java_getter_failure(self):
        diagnosis = self.connect()
        self.server.sendall(b"response_getvaluejava.lang.NullPointerException\n")
        with self.assertRaisesRegex(DiagnosisError, "Original diagnosis method failed"):
            diagnosis.units()
        self.assertEqual(self.server.recv(4096), b"getvalue,10003,getDistance\n")

    def test_multiple_replies_stay_framed(self):
        diagnosis = self.connect()
        self.server.sendall(b"response_getcommandscmdTouchScreenTyped\n"
                            b"response_getvaluescreen\n")
        self.assertIn("cmdTouchScreenTyped", diagnosis.commands())
        self.assertEqual(diagnosis.screen(), "response_getvaluescreen")

    def test_truncated_oversized_and_timeout(self):
        for payload in (b"response_getvalue", b"x" * 65537, None):
            with self.subTest(payload_length=(
                    len(payload) if payload is not None else None)):
                client, server = socket.socketpair()
                client.settimeout(0.01)
                diagnosis = HmiDiagnosis.__new__(HmiDiagnosis)
                diagnosis.socket = client
                diagnosis.reader = client.makefile("rb")
                try:
                    if payload is not None:
                        server.sendall(payload)
                        server.shutdown(socket.SHUT_WR)
                    with self.assertRaises((DiagnosisError, TimeoutError)):
                        diagnosis.screen()
                finally:
                    diagnosis.close()
                    server.close()


if __name__ == "__main__":
    unittest.main()
