# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import prepare_subcpu_elf as prepare
import probe_subcpu as probe


class ProbeSubcpuTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "synthetic.bin"
        raw = bytearray(b"\xff" * prepare.IMAGE_SIZE)
        raw[:2] = b"\xd8\x00"
        raw[0xd8:0xda] = b"\x61\xcf"
        self.source.write_bytes(raw)
        self.digest = hashlib.sha256(raw).hexdigest()
        self.simulator = self.root / "runner"
        self.simulator.write_bytes(b"synthetic test runner")
        self.output = self.root / "observation"

    def run_observation(self, *, signature=True, timeout=False, **options):
        def run(command, **kwargs):
            self.assertEqual(command[:3],
                             [str(self.simulator), "-v", "-d"])
            environment = kwargs["env"]
            for name, variable in probe.DIAGNOSTICS.items():
                self.assertEqual(variable in environment,
                                 options.get(name, False))
            if signature:
                kwargs["stdout"].write((probe.SIGNATURE + "\n").encode())
            kwargs["stdout"].write(
                b"Unmodeled MH2P SubCPU peripheral write at 0xf00f3, "
                b"PC 0x05f35\n")
            if timeout:
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            return subprocess.CompletedProcess(command, 1)

        with patch.object(prepare, "IMAGE_SHA256", self.digest), \
                patch.object(probe.subprocess, "run", side_effect=run), \
                patch.dict(os.environ,
                           dict.fromkeys(probe.DIAGNOSTICS.values(), "1")):
            return probe.observe(self.source, self.simulator, self.output,
                                 **options)

    def test_strict_observation_strips_inherited_fixtures(self):
        report = self.run_observation()
        self.assertEqual(report["source_sha256"], self.digest)
        expected_simulator_hash = hashlib.sha256(
            self.simulator.read_bytes()).hexdigest()
        self.assertEqual(report["simulator_sha256"], expected_simulator_hash)
        self.assertEqual(report["exit_code"], 1)
        self.assertTrue(report["strict_diagnostic_signature_present"])
        self.assertFalse(report["timed_out"])
        self.assertEqual(report["first_unsupported_access"], {
            "operation": "write", "address": "0xf00f3",
            "pc_after_instruction": "0x05f35",
        })
        self.assertEqual(
            json.loads((self.output / "observation.json").read_text()), report)
        for field in ("exact_mcu_variant_verified",
                      "original_internal_mcu_rom_execution_verified",
                      "subcpu_board_boot_verified", "rstp_verified",
                      "hmi_ready"):
            self.assertFalse(report[field])
        self.assertFalse((self.output / "result.json").exists())

    def test_explicit_diagnostics_are_recorded_separately(self):
        report = self.run_observation(
            erased_data_flash=True, pfdl_api=True, f13_pior=True,
            f13_clock=True, f13_tau=True, f13_irq=True, f13_ports=True,
            f13_trd=True)
        self.assertTrue(all(report["diagnostics_requested"].values()))
        self.assertFalse(report["subcpu_board_boot_verified"])

    def test_missing_signature_retains_failure_evidence(self):
        with self.assertRaisesRegex(ValueError, "lacks the strict"):
            self.run_observation(signature=False)
        report = json.loads((self.output / "observation.json").read_text())
        self.assertFalse(report["strict_diagnostic_signature_present"])
        self.assertFalse(report["subcpu_board_boot_verified"])

    def test_timeout_retains_failure_evidence(self):
        with self.assertRaisesRegex(TimeoutError, "timed out"):
            self.run_observation(timeout=True)
        report = json.loads((self.output / "observation.json").read_text())
        self.assertTrue(report["timed_out"])
        self.assertIsNone(report["exit_code"])
        self.assertTrue((self.output / "reset-trace.log").exists())

    def test_rejects_api_without_explicit_flash(self):
        with self.assertRaisesRegex(ValueError, "requires explicit"):
            probe.observe(self.source, self.simulator, self.output,
                          pfdl_api=True)
        self.assertFalse(self.output.exists())

    def test_rejects_tau_without_explicit_clock(self):
        with self.assertRaisesRegex(ValueError, "requires explicit clock"):
            probe.observe(self.source, self.simulator, self.output,
                          f13_tau=True)
        self.assertFalse(self.output.exists())

    def test_rejects_trd_without_explicit_clock(self):
        with self.assertRaisesRegex(ValueError, "requires explicit clock"):
            probe.observe(self.source, self.simulator, self.output,
                          f13_trd=True)
        self.assertFalse(self.output.exists())

    def test_rejects_invalid_timeout(self):
        for timeout in (0, -1, float("nan"), float("inf")):
            with self.subTest(timeout=timeout):
                with self.assertRaisesRegex(ValueError, "positive"):
                    probe.observe(self.source, self.simulator, self.output,
                                  timeout=timeout)
        self.assertFalse(self.output.exists())

    def test_trace_parser_handles_chunk_boundaries_and_keeps_first_fault(self):
        log = self.root / "trace.log"
        prefix = b"x" * (4096 - 10)
        log.write_bytes(
            prefix + probe.SIGNATURE.encode() + b"\n"
            + b"x" * (4096 - len(probe.SIGNATURE) - 1)
            + b"Unmodeled MH2P SubCPU peripheral read at 0xf1000, "
            + b"PC 0x00c5e\n"
            + b"Unmodeled MH2P SubCPU peripheral write at 0xf0804, "
            + b"PC 0x011c9\n")
        signature, access, ram_read = probe.inspect_trace(log)
        self.assertTrue(signature)
        self.assertIsNone(ram_read)
        self.assertEqual(access, {
            "operation": "read", "address": "0xf1000",
            "pc_after_instruction": "0x00c5e",
        })

    def test_trace_without_unsupported_access_does_not_invent_one(self):
        log = self.root / "trace.log"
        log.write_text(probe.SIGNATURE)
        self.assertEqual(probe.inspect_trace(log), (True, None, None))

    def test_trace_parser_does_not_truncate_pc_at_chunk_boundary(self):
        log = self.root / "trace.log"
        partial = (b"Unmodeled MH2P SubCPU peripheral read at 0xf1000, "
                   b"PC 0x0")
        log.write_bytes(
            b"x" * (4096 - len(partial)) + partial + b"0c5e\n")
        signature, access, ram_read = probe.inspect_trace(log)
        self.assertFalse(signature)
        self.assertIsNone(ram_read)
        self.assertEqual(access["pc_after_instruction"], "0x00c5e")

    def test_uninitialized_ram_read_is_recorded_not_a_peripheral_fault(self):
        log = self.root / "trace.log"
        log.write_text(
            probe.SIGNATURE + "\n"
            "Uninitialized MH2P SubCPU RAM read at 0xfe8cd, PC 0x08d29\n")
        self.assertEqual(probe.inspect_trace(log), (
            True, None,
            {"address": "0xfe8cd", "pc_after_instruction": "0x08d29"}))


if __name__ == "__main__":
    unittest.main()
