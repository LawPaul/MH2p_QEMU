# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import tempfile
import sys
import threading
import time
import unittest
from unittest.mock import patch

import hmi_live_control as control


class LiveControlTests(unittest.TestCase):
    def test_service_health_cli_uses_existing_serialized_actions(self):
        for action in ("health", "startup", "persistence-health"):
            with self.subTest(action=action), \
                    patch.object(sys, "argv", [
                        "hmi_live_control.py", "--control-directory", "/private/run",
                        "--timeout", "60", action]), \
                    patch.object(control, "request_control", return_value="native evidence") as request, \
                    patch("builtins.print") as output:
                control.main()
                request.assert_called_once_with(Path("/private/run"), action, "", 60)
                output.assert_called_once_with('"native evidence"')

    def test_lock_timeout_does_not_publish_a_request(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with control.control_transaction(root, 1):
                with self.assertRaisesRegex(TimeoutError, "control owner"):
                    control.request_control(root, "diagnosis", timeout=0.01)
            self.assertFalse((root / "control-request.json").exists())
            self.assertFalse((root / "control-inflight.json").exists())

    def test_publication_conflict_is_recorded_without_overwriting_request(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pending = root / "control-request.json"
            pending.write_text('{"id":"previous"}')
            with self.assertRaises(FileExistsError):
                control.request_control(root, "diagnosis")
            self.assertEqual(pending.read_text(), '{"id":"previous"}')
            self.assertFalse((root / "control-inflight.json").exists())
            record = json.loads(next((root / "control-checks").iterdir()).read_text())
            self.assertIn("error", record)

    def test_server_failure_is_explicit_and_malformed_reply_is_quarantined(self):
        for reply, quarantine in (({"error": "Native getter failed"}, False),
                                  ({}, True),
                                  ({"result": "value", "error": "bad"}, True)):
            with self.subTest(reply=reply), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)

                def respond(directory, request):
                    (directory / "control-response.json").write_text(json.dumps({
                        "id": request["id"], **reply,
                    }))

                with patch.object(control, "publish_control_request", side_effect=respond):
                    with self.assertRaises(RuntimeError):
                        control.request_control(root, "diagnosis")
                self.assertEqual((root / "control-inflight.json").exists(), quarantine)
                record = json.loads(
                    next((root / "control-checks").iterdir()).read_text())
                self.assertIn("error", record)

    def test_invalid_timeout_is_rejected_before_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            for timeout in (0, -1, float("nan"), float("inf")):
                with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                    control.request_control(directory, "diagnosis", timeout=timeout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_closed_session_is_not_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            with control.LiveSession(directory, []):
                pass
            with self.assertRaisesRegex(RuntimeError, "control is closed"):
                control.request_control(directory, "diagnosis")
            self.assertFalse((Path(directory) / "control-request.json").exists())

    def test_multiple_clients_are_serialized_and_evidence_retains_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            identity = root / "firmware-fixture"
            identity.write_bytes(b"fixture")
            errors = []
            results = []

            def client(command):
                try:
                    results.append(control.request_control(
                        root, "diagnosis", command, timeout=3))
                except Exception as exc:
                    errors.append(exc)

            with control.LiveSession(root, [identity]):
                threads = [threading.Thread(target=client, args=(str(i),))
                           for i in range(2)]
                for thread in threads:
                    thread.start()
                try:
                    for _ in range(2):
                        deadline = time.monotonic() + 3
                        pending = root / "control-request.json"
                        while not pending.exists():
                            if time.monotonic() >= deadline:
                                self.fail("Client did not publish request")
                            time.sleep(0.01)
                        request = json.loads(pending.read_text())
                        pending.unlink()
                        time.sleep(0.05)
                        self.assertFalse(pending.exists())
                        response = root / "response.tmp"
                        response.write_text(json.dumps({
                            "id": request["id"], "result": request["command"]}))
                        response.replace(root / "control-response.json")
                finally:
                    for thread in threads:
                        thread.join(4)
                self.assertFalse(errors)
                self.assertCountEqual(results, ["0", "1"])
                records = [json.loads(path.read_text())
                           for path in (root / "control-checks").iterdir()]
                self.assertEqual(len(records), 2)
                self.assertEqual(records[0]["session"]["boot_id"],
                                 records[1]["session"]["boot_id"])
                self.assertEqual(
                    records[0]["session"]["identity_sha256"][str(identity)],
                    hashlib.sha256(b"fixture").hexdigest())
                self.assertFalse((root / "control-inflight.json").exists())

    def test_timeout_keeps_uncertain_transaction_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(TimeoutError):
                control.request_control(root, "diagnosis", timeout=0.01)
            pending = (root / "control-request.json").read_bytes()
            with self.assertRaisesRegex(RuntimeError, "Unresolved"):
                control.request_control(root, "diagnosis", timeout=1)
            self.assertEqual((root / "control-request.json").read_bytes(), pending)
            records = list((root / "control-checks").iterdir())
            self.assertEqual(len(records), 1)
            self.assertIn("error", json.loads(records[0].read_text()))

    def test_idle_limit_activity_and_explicit_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(control.time, "monotonic", return_value=0) as clock:
                with control.LiveSession(directory, [], 5, 10) as live:
                    clock.return_value = 4
                    live.activity()
                    clock.return_value = 6
                    self.assertFalse(live.should_stop())
                    clock.return_value = 9
                    self.assertTrue(live.should_stop())
                    self.assertEqual(live.record["stop_reason"], "idle-timeout")
                    live.stop_requested = True
                    live.should_stop()
                    self.assertEqual(live.record["stop_reason"], "requested")
            self.assertEqual(json.loads(
                (Path(directory) / "live-session.json").read_text())["state"],
                "control-closed")

    def test_maximum_lifetime_cannot_be_extended_by_activity(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(control.time, "monotonic", return_value=0) as clock:
                with control.LiveSession(directory, [], 5, 10) as live:
                    clock.return_value = 10
                    live.activity()
                    self.assertTrue(live.should_stop())
                    self.assertEqual(live.record["stop_reason"], "maximum-lifetime")

    def test_session_records_failure_without_claiming_guest_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "failed check"):
                with control.LiveSession(directory, [], 5, 10):
                    raise RuntimeError("failed check")
            record = json.loads(
                (Path(directory) / "live-session.json").read_text())
            self.assertEqual(record["state"], "failed")
            self.assertFalse(record["full_hmi_readiness_verified"])


if __name__ == "__main__":
    unittest.main()
