# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

from boot_status import assess, record_command


class BootStatusTests(unittest.TestCase):
    def test_progress_is_persisted_before_execution_and_completed_afterwards(self):
        with tempfile.TemporaryDirectory() as temporary:
            with record_command(temporary, "waitfor /dev/name/local/ipte-dm", 18):
                report = assess(temporary)
                self.assertEqual(report["state"], "starting")
                self.assertEqual(report["command"]["state"], "running")
                self.assertEqual(report["command"]["timeout_seconds"], 18)
            self.assertEqual(assess(temporary)["command"]["state"], "completed")

    def test_timeout_retains_exact_command_and_propagates_original_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(TimeoutError, "Missing guest response"):
                with record_command(temporary, "sleep 10; HMI_LSD_SCRIPT=start", 600):
                    raise TimeoutError("Missing guest response")
            report = assess(temporary)
            self.assertEqual(report["state"], "failed")
            self.assertEqual(report["command"]["error_type"], "TimeoutError")
            self.assertEqual(report["command"]["command"], "sleep 10; HMI_LSD_SCRIPT=start")
            self.assertGreaterEqual(report["command"]["elapsed_seconds"], 0)

    def test_control_is_not_full_readiness_or_verified_liveness(self):
        for state in ("control-ready", "control-closed", "failed"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temporary:
                (Path(temporary) / "live-session.json").write_text(json.dumps({
                    "state": state, "full_hmi_readiness_verified": True}))
                report = assess(temporary)
                self.assertEqual(report["state"], state)
                self.assertFalse(report["full_hmi_readiness_verified"])
                self.assertFalse(report["liveness_verified"])

    def test_failed_command_overrides_stale_ready_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / "live-session.json").write_text('{"state":"control-ready"}')
            with self.assertRaises(RuntimeError):
                with record_command(temporary, "pidin ar", 120):
                    raise RuntimeError("Console closed")
            self.assertEqual(assess(temporary)["state"], "failed")

    def test_missing_malformed_and_invalid_evidence_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                assess(temporary)
            path = Path(temporary) / "boot-command.json"
            for text in ("{", "[]", '{"state":"healthy"}'):
                path.write_text(text)
                with self.subTest(text=text), self.assertRaises(ValueError):
                    assess(temporary)

    def test_invalid_timeout_does_not_create_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            for timeout in (0, -1, float("inf"), float("nan")):
                with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                    with record_command(temporary, "bad", timeout):
                        self.fail("Invalid command executed")
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_cli_failed_run_is_machine_readable_and_nonzero(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(TimeoutError):
                with record_command(temporary, "original command", 600):
                    raise TimeoutError("Timed out")
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("boot_status.py")),
                 "--directory", temporary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["state"], "failed")
