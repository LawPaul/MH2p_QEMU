# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for the direct native backend diagnostic."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from probe_carplay_backend import (
    BACKEND_STARTUP_CHANNELS, MAX_SNAPSHOTS, backend_debug_tracing,
    observation_script, observe, parse_observation,
)


class BackendObservationTests(unittest.TestCase):
    def test_tracing_changes_only_selected_channels(self):
        source = {
            "client": {"dio_manager": {
                "backends": {"original": {"enabled": True}},
                "levels": {"channel": {"*": "warning", "DIO": "info"}}},
                "other": {"levels": {"channel": {"*": "off"}}}},
            "server": {"default": {"backends": {
                "ConsoleBackend": {"levels": {
                    "channel": {"*": "off", "Clients": "info"}}}}}},
        }
        expected = copy.deepcopy(source)
        for channels in (
                expected["client"]["dio_manager"]["levels"]["channel"],
                expected["server"]["default"]["backends"]["ConsoleBackend"]["levels"]["channel"]):
            channels.update({name: "debug" for name in BACKEND_STARTUP_CHANNELS})
        observed = backend_debug_tracing(json.dumps(source))
        self.assertEqual(observed, expected)
        self.assertEqual(backend_debug_tracing(json.dumps(observed)), observed)
        for malformed in ({}, {"client": None}):
            with self.assertRaises(ValueError):
                backend_debug_tracing(json.dumps(malformed))

    def test_original_launch_has_no_readiness_or_configuration_overrides(self):
        config = {"children": {"carplay": {
            "exec": "dio_manager", "params": "", "envs": [
                "LD_LIBRARY_PATH=/eso/lib",
                "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production",
                "LIBIMG_CFGFILE=/etc/config/img.conf",
            ]}}}
        script = observation_script(config)
        reply = subprocess.run(["sh", "-n"], input=script, capture_output=True)
        self.assertEqual(reply.returncode, 0, reply.stderr)
        self.assertIn(b"env LD_PRELOAD=", script)
        self.assertIn(b"/eso/bin/apps/dio_manager", script)
        self.assertNotIn(b"/tmp/dio_manager.json", script)
        self.assertNotIn(b"sleep ", script)
        self.assertNotIn(b"persistence", script)
        self.assertNotIn(b"killall", script)
        self.assertNotIn(b"pkill", script)
        config["children"]["carplay"]["params"] = "--guess"
        with self.assertRaises(ValueError):
            observation_script(config)

    def test_surviving_process_is_not_claimed_as_ready(self):
        result = parse_observation(
            b"CARPLAY_DIRECT_PID:123\n" +
            b"".join(f"CARPLAY_DIRECT_SAMPLE:{sample}\n"
                     "CARPLAY_DIRECT_ALIVE:123\n".encode()
                     for sample in range(1, MAX_SNAPSHOTS + 1)) +
            b"CARPLAY_DIRECT_STOPPED\n")
        self.assertTrue(result["survived_all_samples"])
        self.assertTrue(result["owned_process_stopped"])
        self.assertFalse(result["airplay_server_ready_verified"])
        self.assertFalse(result["iphone_session_verified"])
        self.assertFalse(result["readiness_memory_written"])
        self.assertFalse(result["usb_record_injected"])
        self.assertFalse(result["native_service_threads_observed"])

    def test_native_thread_poll_stops_without_claiming_server_ready(self):
        result = parse_observation(
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_SAMPLE:1\n"
            b"CARPLAY_DIRECT_ALIVE:123\nCARPLAY_DIRECT_NATIVE_THREADS_OBSERVED\n"
            b" 123 n/apps/dio_manager DIOController CONDVAR\n"
            b" 123 n/apps/dio_manager HIDService CONDVAR\n"
            b" 123 n/apps/dio_manager IAP2Service CONDVAR\n"
            b"CARPLAY_DIRECT_STOPPED\n")
        self.assertTrue(result["native_service_threads_observed"])
        self.assertFalse(result["airplay_server_ready_verified"])
        self.assertEqual(result["alive_samples"], 1)

    def test_early_failure_is_retained(self):
        result = parse_observation(
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_SAMPLE:1\n"
            b"CARPLAY_DIRECT_EXIT:127\n"
            b"CARPLAY_DIRECT_STOPPED\n")
        self.assertEqual(result["early_exit_status"], 127)
        self.assertFalse(result["survived_all_samples"])

    def test_callbacks_are_not_borrowed_from_integrator(self):
        result = parse_observation(
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_SAMPLE:1\n"
            b"CARPLAY_DIRECT_EXIT:1\n"
            b"00:02:19.200 smartphone_int.. Main Channel info 1 "
            b"[PersistencyProvider] openResult : Opened partition=1 "
            b"(persistencyReadable=1, version='DSI-unversioned')\n"
            b"00:02:19.220 dio_manager DIOControl.. Channel info 2 "
            b"[PersistencyProvider] openResult : Opened partition=24354 "
            b"(persistencyReadable=0, version='DSI-unversioned')\n"
            b"00:02:19.230 dio_manager DIOControl.. Channel info 3 "
            b"[CSIService] onReady : <SI> accessMode: READY\n"
            b"00:02:19.240 dio_manager DIOControl.. Channel info 4 "
            b"[PersistencyProvider] valueChanged : No value available "
            b"for partition=24354, key=1322 (result=DOES_NOT_EXIST)\n"
            b"CARPLAY_DIRECT_STOPPED\n")
        self.assertEqual(result["persistence_partition_readability"], {"24354": False})
        self.assertEqual(len(result["integrator_callbacks"]), 1)
        self.assertEqual(len(result["configuration_read_results"]), 1)
        self.assertFalse(result["airplay_server_ready_verified"])

    def test_incomplete_or_conflicting_evidence_is_rejected(self):
        invalid = (
            b"",
            b"echo CARPLAY_DIRECT_PID:123; echo CARPLAY_DIRECT_STOPPED\n",
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_STOPPED\n",
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_ALIVE:124\n"
            b"CARPLAY_DIRECT_STOPPED\n",
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_EXIT:1\n",
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_PID:123\n"
            b"CARPLAY_DIRECT_EXIT:1\nCARPLAY_DIRECT_STOPPED\n",
            b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_SAMPLE:1\n"
            b"CARPLAY_DIRECT_ALIVE:123\nCARPLAY_DIRECT_NATIVE_THREADS_OBSERVED\n"
            b"CARPLAY_DIRECT_STOPPED\n",
        )
        for reply in invalid:
            with self.subTest(reply=reply), self.assertRaises(RuntimeError):
                parse_observation(reply)

    def test_unrecognized_private_input_never_reaches_guest(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            backend = root / "backend"
            launcher = root / "launcher"
            backend.write_bytes(b"not original")
            launcher.write_bytes(b"not original")
            output = root / "output"
            with self.assertRaisesRegex(ValueError, "Unrecognized"):
                observe(self.fail, backend, launcher, output)
            self.assertFalse(output.exists())
            with self.assertRaises(FileExistsError):
                observe(self.fail, backend, launcher, root)

    def test_guest_observation_keeps_fixture_source_explicit(self):
        config = {"children": {"carplay": {
            "exec": "dio_manager", "params": "", "envs": [
                "LD_LIBRARY_PATH=/eso/lib",
                "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production",
                "LIBIMG_CFGFILE=/etc/config/img.conf",
            ]}}}
        script = observation_script(config)

        def shell(command):
            if command.startswith("echo CARPLAY_SCRIPT_BEGIN;"):
                return b"CARPLAY_SCRIPT_BEGIN\n" + script + b"CARPLAY_SCRIPT_END\n"
            if command.startswith("ksh "):
                return (
                    b"CARPLAY_DIRECT_PID:123\nCARPLAY_DIRECT_SAMPLE:1\n"
                    b"CARPLAY_DIRECT_EXIT:127\nCARPLAY_DIRECT_STOPPED\n")
            return b""

        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            backend, launcher = root / "backend", root / "launcher"
            backend.write_bytes(b"synthetic executable identity")
            launcher.write_text(json.dumps(config))
            with patch("probe_carplay_backend.BACKEND_SHA256",
                       hashlib.sha256(backend.read_bytes()).hexdigest()), patch(
                    "probe_carplay_backend.LAUNCHER_SHA256",
                    hashlib.sha256(launcher.read_bytes()).hexdigest()):
                for source in ("none", "supplied-capture", "synthetic-empty"):
                    result = observe(shell, backend, launcher, root / source,
                                     usb_record_source=source)
                    self.assertEqual(result["usb_record_source"], source)
                    self.assertEqual(result["usb_record_injected"], source != "none")
                    self.assertEqual(result["early_exit_status"], 127)
                    self.assertFalse(result["iphone_session_verified"])


if __name__ == "__main__":
    unittest.main()
