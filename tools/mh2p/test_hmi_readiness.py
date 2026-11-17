# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
from contextlib import redirect_stderr, redirect_stdout
import io
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from hmi_readiness import (display_wait_command, main,
                           verify_native_m4_watchdogs,
                           verify_original_hmi_display)


VALID = (
    b"Jan 01 00:04:28    5 10203     1 [268210] 00:04:28.210 "
    b"DEBUG <Fw.Display>         waitForDSIDisplayManagement: succeeded!\r\n")
TIMEOUT = b"NATIVE_HMI_DISPLAY_WAIT_TIMEOUT\r\n"


class M4WatchdogTests(unittest.TestCase):
    def test_native_fatal_records_and_console_newlines(self):
        events = (
            b"[IPC WD] REBOOT ! ! ! ",
            b"[M4PW]J6 WD reset req,ID:7 1-4:0 0 238 82 5-8:0 0 0 0",
            b"[M4PW]J6 WD reset req Id:0x12.Audio Control",
            b"[MAWD] Enter infinity loop",
            b"[MAWD] M4 Application WatchDog Failed",
        )
        for event in events:
            for newline in (b"\n", b"\r\n", b"\r\r\n"):
                with self.subTest(event=event, newline=newline), \
                        self.assertRaisesRegex(RuntimeError, "tick 0000061010"):
                    verify_native_m4_watchdogs(
                        b"\r0000061010  " + event + newline)

    def test_normal_watchdog_setup_and_echoes_are_not_failures(self):
        verify_native_m4_watchdogs(
            b"\r0000000014  [MAWD] START_APP_WATCHDOG: 00 80595ae8\n"
            b"\r0000021978  [UARTP_C]UartP receive watchdog status, 1.\n"
            b"\r0000021978  [UARTP_C]Rcv watchdog status from M4 Power. "
            b"WatchdogStatus: 01\n"
            b"# grep '[IPC WD] REBOOT ! ! !' /tmp/log\n")
        verify_native_m4_watchdogs(b"")

    def test_first_native_failure_is_reported(self):
        with self.assertRaisesRegex(RuntimeError, r"tick 0000061010: \[IPC WD\]"):
            verify_native_m4_watchdogs(
                b"\r0000061010  [IPC WD] REBOOT ! ! ! \n"
                b"\r0000061063  [M4PW]J6 WD reset req,ID:14\n")

    def test_cli_m4_failure_overrides_display_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            display = root / "display.log"
            uart = root / "uart.log"
            display.write_bytes(VALID)
            uart.write_bytes(b"\r0000061010  [IPC WD] REBOOT ! ! ! \n")
            with patch.object(sys, "argv", [
                    "hmi_readiness", str(display), "--m4-uart", str(uart)]), \
                    redirect_stderr(io.StringIO()) as stderr, \
                    self.assertRaises(SystemExit) as error:
                main()
            self.assertEqual(error.exception.code, 1)
            self.assertIn("Native M4 watchdog failure", stderr.getvalue())


class DisplayReadinessTests(unittest.TestCase):
    def test_original_line_with_console_newline_translation(self):
        for line in (VALID, VALID.replace(b"\r\n", b"\n"),
                     VALID.replace(b"\r\n", b"\r\r\n")):
            with self.subTest(line=line):
                verify_original_hmi_display(line)

    def test_missing_or_malformed_original_line_is_rejected(self):
        for capture in (
            b"", b"waitForDSIDisplayManagement: succeeded!\r\n",
            b"mh2p# grep 'waitForDSIDisplayManagement: succeeded!' /tmp/hmi.log\r\n",
            VALID.replace(b"succeeded!", b"succeeded?"),
            VALID.rstrip() + b" unexpected suffix\n",
            VALID.replace(b"DEBUG ", b"DEBUG\n"),
            VALID.replace(b">         ", b">\r\n"),
            VALID.replace(b">         ", b">\n"),
        ):
            with self.subTest(capture=capture), self.assertRaises(RuntimeError):
                verify_original_hmi_display(capture)

    def test_timeout_takes_precedence_over_success(self):
        for capture in (TIMEOUT + VALID, VALID + TIMEOUT,
                        VALID + b"console-prefix " + TIMEOUT,
                        VALID + TIMEOUT.replace(b"\r\n", b"\r\r\n")):
            with self.subTest(capture=capture), \
                    self.assertRaisesRegex(RuntimeError, "timed out"):
                verify_original_hmi_display(capture)

    def test_log_capture_failure_takes_precedence_over_success(self):
        error = b"NATIVE_HMI_DISPLAY_LOG_ERROR\r\n"
        for capture in (error + VALID, VALID + error,
                        VALID + b"console-prefix " + error):
            with self.subTest(capture=capture), \
                    self.assertRaisesRegex(RuntimeError, "log capture failed"):
                verify_original_hmi_display(capture)

    def test_original_failure_takes_precedence_and_stops_native_poll(self):
        failed = VALID.replace(b"DEBUG", b"ERROR").replace(b"succeeded!", b"failed!")
        for capture in (failed, VALID + failed, failed + VALID):
            with self.subTest(capture=capture), \
                    self.assertRaisesRegex(RuntimeError, "initialization failed"):
                verify_original_hmi_display(capture)
        captured, ticks = self.observe(failed, slog=True)
        self.assertEqual(ticks, 0)
        self.assertNotIn(TIMEOUT.strip(), captured)
        with self.assertRaisesRegex(RuntimeError, "initialization failed"):
            verify_original_hmi_display(captured)

    def test_echoed_command_contains_no_success_literal(self):
        for slog in (False, True):
            command = display_wait_command(slog=slog)
            self.assertNotIn("waitForDSIDisplayManagement: succeeded!", command)
            self.assertNotIn("\r", command)
            self.assertNotIn("\n", command)
            with self.assertRaises(RuntimeError):
                verify_original_hmi_display(command.encode() + b"\r\n")

    def test_invalid_wait_inputs(self):
        for attempts in (0, -1, True, 1.5, "180"):
            with self.subTest(attempts=attempts), self.assertRaises(ValueError):
                display_wait_command(attempts=attempts)
        for path in ("", "bad\0path", None):
            with self.subTest(path=path), self.assertRaises(ValueError):
                display_wait_command(log=path)
        for slog in (None, 1, "true"):
            with self.subTest(slog=slog), self.assertRaises(ValueError):
                display_wait_command(slog=slog)

    def observe(self, initial, delayed=False, *, slog=False, slog_status=0):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "hmi 'quoted' log"
            path.write_bytes(initial)
            source = path
            if slog:
                source = Path(temporary) / "native slog"
                source.write_bytes(initial)
                path.write_bytes(b"JVM stdout without display initialization\n")
            ticks = Path(temporary) / "ticks"
            stubs = "netstat() { :; }; pidin() { :; }; ticks=0; "
            if slog:
                stubs += (
                    "sloginfo() { cat -- " + shlex.quote(str(source)) +
                    f"; return {slog_status}; }}; ")
            stubs += "sleep() { ticks=$((ticks+1)); "
            if delayed:
                stubs += (
                    'if [ "$ticks" = 3 ]; then printf "%s\\n" ' +
                    shlex.quote(VALID.decode().rstrip()) + " > " +
                    shlex.quote(str(source)) + "; fi; ")
            stubs += "}; "
            command = (
                stubs + display_wait_command(str(path), 3, slog=slog) +
                '; printf "%s" "$ticks" > ' + shlex.quote(str(ticks)))
            result = subprocess.run(
                ["/bin/sh", "-c", command], capture_output=True,
                check=True, timeout=10)
            self.assertIn(b"PRIORITY_HMI_OBSERVER_END", result.stdout)
            return result.stdout + result.stderr, int(ticks.read_text())

    def test_wait_accepts_actual_original_line_immediately(self):
        captured, ticks = self.observe(VALID)
        self.assertEqual(ticks, 0)
        verify_original_hmi_display(captured)

    def test_wait_accepts_success_at_final_attempt(self):
        captured, ticks = self.observe(b"", delayed=True)
        self.assertEqual(ticks, 3)
        verify_original_hmi_display(captured)

    def test_wait_rejects_bare_success_and_stops_at_exact_bound(self):
        captured, ticks = self.observe(
            b"waitForDSIDisplayManagement: succeeded!\n")
        self.assertEqual(ticks, 3)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            verify_original_hmi_display(captured)

    def test_native_slog_success_is_not_required_in_jvm_stdout(self):
        captured, ticks = self.observe(VALID, slog=True)
        self.assertEqual(ticks, 0)
        self.assertIn(b"JVM stdout without display initialization", captured)
        verify_original_hmi_display(captured)

    def test_native_slog_final_attempt_and_timeout(self):
        captured, ticks = self.observe(b"", delayed=True, slog=True)
        self.assertEqual(ticks, 3)
        verify_original_hmi_display(captured)
        captured, ticks = self.observe(b"", slog=True)
        self.assertEqual(ticks, 3)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            verify_original_hmi_display(captured)

    def test_failed_native_slog_cannot_supply_success(self):
        captured, ticks = self.observe(VALID, slog=True, slog_status=7)
        self.assertEqual(ticks, 0)
        with self.assertRaisesRegex(RuntimeError, "log capture failed"):
            verify_original_hmi_display(captured)

    def test_cli_reports_display_only_and_fails_for_invalid_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "capture.log"
            for content, accepted in ((VALID, True), (VALID + TIMEOUT, False),
                                      (b"", False)):
                path.write_bytes(content)
                out, err = io.StringIO(), io.StringIO()
                with self.subTest(content=content), \
                        patch.object(sys, "argv", ["hmi_readiness.py", str(path)]), \
                        redirect_stdout(out), redirect_stderr(err):
                    if accepted:
                        main()
                        self.assertIn("sustained health unverified", out.getvalue())
                        self.assertEqual(err.getvalue(), "")
                    else:
                        with self.assertRaises(SystemExit) as failure:
                            main()
                        self.assertEqual(failure.exception.code, 1)
                        self.assertIn("readiness failed", err.getvalue())
                        self.assertEqual(out.getvalue(), "")

    def test_cli_missing_capture_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(sys, "argv", [
                    "hmi_readiness.py", str(Path(temporary) / "missing")]), \
                redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit) as failure:
                main()
            self.assertEqual(failure.exception.code, 1)
            self.assertIn("readiness failed", error.getvalue())


if __name__ == "__main__":
    unittest.main()
