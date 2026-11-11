# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free tests of the explicitly isolated original-code call harness."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import prepare_subcpu_elf as prepare
import probe_subcpu as reset
import probe_subcpu_checksum as probe


SIMULATOR = os.environ.get("MH2P_RL78_SIM")


def frame(length=10):
    body = length.to_bytes(2, "little") + bytes(length - 2)
    return b"\xf7\xc1\x01" + body + bytes([sum(body) & 0xff])


class ChecksumCallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "synthetic.bin"
        raw = bytearray(b"\xff" * prepare.IMAGE_SIZE)
        raw[:2] = b"\xd8\x00"
        raw[0xd8:0xda] = b"\x61\xcf"
        raw[probe.CHECKSUM_ADDRESS:probe.CHECKSUM_ADDRESS + 4] = (
            b"\x32\x01\x00\xd7")  # movw bc,#1; ret
        self.source.write_bytes(raw)
        self.raw = bytes(raw)
        self.digest = hashlib.sha256(raw).hexdigest()
        self.frame_path = self.root / "frame.bin"
        self.frame_path.write_bytes(frame())
        self.simulator = self.root / "runner"
        self.simulator.write_bytes(b"test runner")
        self.output = self.root / "observation"

    def run_call(self, *, signature=True, timeout=False, marker=0xf0002,
                 exit_code=1, ram_read=False, pc_offset=0):
        def run(command, **kwargs):
            self.assertEqual(command[:3],
                             [str(self.simulator), "-v", "-d"])
            self.assertTrue(all(variable not in kwargs["env"]
                                for variable in reset.DIAGNOSTICS.values()))
            elf = Path(command[-1]).read_bytes()
            size = struct.unpack_from("<I", elf, 100)[0]
            if signature:
                kwargs["stdout"].write((reset.SIGNATURE + "\n").encode())
            if ram_read:
                kwargs["stdout"].write(
                    b"Uninitialized MH2P SubCPU RAM read at 0xfe000, "
                    b"PC 0x0bb25\n")
            kwargs["stdout"].write(
                f"Unmodeled MH2P SubCPU peripheral write at {marker:#07x}, "
                f"PC {probe.CALL_ADDRESS + size + pc_offset:#07x}\n".encode())
            if timeout:
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            return subprocess.CompletedProcess(command, exit_code)

        with patch.object(prepare, "IMAGE_SHA256", self.digest), \
                patch.object(probe.subprocess, "run", side_effect=run), \
                patch.dict(os.environ,
                           dict.fromkeys(reset.DIAGNOSTICS.values(), "1")):
            return probe.observe(self.source, self.frame_path, self.simulator,
                                 self.output)

    def test_preserves_source_and_separates_call_from_reset_evidence(self):
        report = self.run_call()
        original = (self.output / "subcpu.elf").read_bytes()
        call = (self.output / "checksum-call.elf").read_bytes()
        for elf in (original, call):
            self.assertEqual(
                elf[prepare.PAYLOAD_OFFSET:
                    prepare.PAYLOAD_OFFSET + len(self.raw)], self.raw)
        self.assertEqual(struct.unpack_from("<I", original, 24)[0], 0xd8)
        self.assertEqual(struct.unpack_from("<I", call, 24)[0],
                         probe.CALL_ADDRESS)
        self.assertEqual(struct.unpack_from("<H", call, 44)[0], 2)
        self.assertEqual(struct.unpack_from("<H", call, 48)[0], 4)
        offset, address = struct.unpack_from("<II", call, 88)
        self.assertEqual(address, probe.CALL_ADDRESS)
        self.assertGreaterEqual(offset, len(original))
        self.assertEqual(call[offset:offset + 6], b"\x61\xcf\xcb\xf8\x20\xfe")
        self.assertEqual((self.output / "frame.bin").read_bytes(), frame())
        self.assertEqual(self.source.read_bytes(), self.raw)
        self.assertEqual(json.loads(
            (self.output / "observation.json").read_text()), report)
        self.assertTrue(report["original_checksum_returned_accept"])
        self.assertTrue(report["host_checksum_matches"])
        self.assertTrue(report["all_source_bytes_preserved"])
        for name in ("original_reset_executed", "subcpu_board_boot_verified",
                     "subcpu_exchange_verified", "rstp_verified", "hmi_ready"):
            self.assertFalse(report[name])
        self.assertFalse((self.output / "result.json").exists())

    def test_wrong_marker_exit_status_or_ram_fault_prevents_acceptance(self):
        for options in ({"marker": 0xf0001}, {"exit_code": 0},
                        {"ram_read": True}, {"pc_offset": -4}):
            with self.subTest(options=options):
                report = self.run_call(**options)
                self.assertFalse(report["original_checksum_returned_accept"])
                self.output = self.root / (self.output.name + "-next")

    def test_diagnostic_code_size_does_not_overlap_elf_metadata(self):
        with patch.object(prepare, "IMAGE_SHA256", self.digest):
            prepare.prepare(self.source, self.output)
        original = (self.output / "subcpu.elf").read_bytes()
        for length in (1, 0xfff, 0x1001):
            with self.subTest(length=length):
                code = bytes(index & 0xff for index in range(length))
                elf = probe.prepare_diagnostic_call(original, code)
                offset, address, physical, size, memory_size = (
                    struct.unpack_from("<5I", elf, 88))
                self.assertEqual(address, probe.CALL_ADDRESS)
                self.assertEqual(physical, address)
                self.assertEqual(size, length)
                self.assertEqual(memory_size, length)
                self.assertEqual(elf[offset:offset + size], code)
                self.assertEqual(
                    elf[prepare.PAYLOAD_OFFSET:
                        prepare.PAYLOAD_OFFSET + len(self.raw)], self.raw)
                sections = struct.unpack_from("<I", elf, 32)[0]
                names_offset, names_size = struct.unpack_from(
                    "<II", elf, sections + 2 * 40 + 16)
                self.assertGreaterEqual(names_offset, offset + size)
                self.assertGreaterEqual(sections, names_offset + names_size)
                self.assertEqual(
                    elf[names_offset:names_offset + names_size],
                    b"\0.text\0.shstrtab\0.diagnostic_call\0")
                self.assertEqual(
                    struct.unpack_from("<II", elf, sections + 3 * 40 + 16),
                    (offset, length))

    def test_timeout_and_missing_signature_retain_evidence(self):
        for options, error in (({"timeout": True}, TimeoutError),
                               ({"signature": False}, ValueError)):
            with self.subTest(options=options), self.assertRaises(error):
                self.run_call(**options)
            self.assertTrue((self.output / "observation.json").exists())
            self.assertTrue((self.output / "checksum-trace.log").exists())
            self.output = self.root / (self.output.name + "-next")

    def test_invalid_frames_are_rejected_before_output_creation(self):
        for data in (b"", b"x" * 14, frame()[:-1], frame() + b"\0",
                     b"\xf7\xc1\0\x09\0" + bytes(9),
                     b"\xf7\xc1\0\x0b\x01" + bytes(265)):
            with self.subTest(data=data[:5]):
                self.frame_path.write_bytes(data)
                with self.assertRaises(ValueError):
                    probe.observe(self.source, self.frame_path,
                                  self.simulator, self.output)
                self.assertFalse(self.output.exists())

    def test_invalid_timeout_does_not_create_output(self):
        for timeout in (0, -1, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                probe.observe(self.source, self.frame_path, self.simulator,
                              self.output, timeout=timeout)
        self.assertFalse(self.output.exists())

    def test_host_checksum_mismatch_is_reported_not_silently_repaired(self):
        corrupt = frame()[:-1] + b"\xff"
        self.frame_path.write_bytes(corrupt)
        report = self.run_call(marker=0xf0001)
        self.assertFalse(report["host_checksum_matches"])
        self.assertFalse(report["original_checksum_returned_accept"])
        self.assertEqual((self.output / "frame.bin").read_bytes(), corrupt)

    @unittest.skipUnless(
        SIMULATOR, "Set MH2P_RL78_SIM to the strict GNU runner")
    def test_generated_call_executes_and_checks_return_register(self):
        for length in (10, 266):
            self.frame_path.write_bytes(frame(length))
            with patch.object(prepare, "IMAGE_SHA256", self.digest):
                report = probe.observe(self.source, self.frame_path,
                                       Path(SIMULATOR), self.output)
            self.assertTrue(report["original_checksum_returned_accept"], report)
            self.output = self.root / (self.output.name + "-next")
        raw = bytearray(self.raw)
        raw[probe.CHECKSUM_ADDRESS + 1] = 0
        self.source.write_bytes(raw)
        with patch.object(prepare, "IMAGE_SHA256",
                          hashlib.sha256(raw).hexdigest()):
            report = probe.observe(self.source, self.frame_path,
                                   Path(SIMULATOR), self.output)
        self.assertFalse(report["original_checksum_returned_accept"])
        self.assertEqual(report["first_unsupported_access"]["address"],
                         "0xf0001")


if __name__ == "__main__":
    unittest.main()
