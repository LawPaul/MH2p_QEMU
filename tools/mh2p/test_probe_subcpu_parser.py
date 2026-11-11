# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for the isolated original receive-parser observer."""
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
import probe_subcpu_parser as probe
from test_probe_subcpu_checksum import frame


SIMULATOR = os.environ.get("MH2P_RL78_SIM")


class ParserCallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "synthetic.bin"
        raw = bytearray(b"\xff" * prepare.IMAGE_SIZE)
        raw[:2] = b"\xd8\x00"
        raw[0xd8:0xda] = b"\x61\xcf"
        self.source.write_bytes(raw)
        self.raw = bytes(raw)
        self.digest = hashlib.sha256(raw).hexdigest()
        self.frame_path = self.root / "frame.bin"
        self.frame_path.write_bytes(frame())
        self.simulator = self.root / "runner"
        self.simulator.write_bytes(b"test runner")
        self.output = self.root / "observation"

    def run_call(self, *, signature=True, timeout=False, exit_code=1,
                 boundary=True, return_marker=False, wrong_pc=False,
                 runtime_init=False, dispatch=False):
        def run(command, **kwargs):
            self.assertEqual(command[:3],
                             [str(self.simulator), "-v", "-d"])
            for name, variable in reset.DIAGNOSTICS.items():
                if dispatch and name == "f13_irq":
                    self.assertEqual(kwargs["env"][variable], "1")
                else:
                    self.assertNotIn(variable, kwargs["env"])
            if signature:
                kwargs["stdout"].write((reset.SIGNATURE + "\n").encode())
            if boundary:
                kwargs["stdout"].write(
                    b"Uninitialized MH2P SubCPU RAM read at 0xfe720, "
                    + (b"PC 0x06226\n" if wrong_pc else b"PC 0x06225\n"))
            else:
                call = Path(command[-1]).read_bytes()
                size = struct.unpack_from("<I", call, 100)[0]
                marker = 0xf0002 if return_marker else 0xf0001
                kwargs["stdout"].write(
                    f"Unmodeled MH2P SubCPU peripheral write at {marker:#07x}, "
                    f"PC {probe.CALL_ADDRESS + size:#07x}\n".encode())
            if timeout:
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            return subprocess.CompletedProcess(command, exit_code)

        with patch.object(prepare, "IMAGE_SHA256", self.digest), \
                patch.object(probe.subprocess, "run", side_effect=run), \
                patch.dict("os.environ",
                           dict.fromkeys(reset.DIAGNOSTICS.values(), "1")):
            return probe.observe(self.source, self.frame_path, self.simulator,
                                 self.output, runtime_init=runtime_init,
                                 dispatch=dispatch)

    def test_preserves_source_and_only_calls_original_initializers(self):
        report = self.run_call()
        original = (self.output / "subcpu.elf").read_bytes()
        call = (self.output / "parser-call.elf").read_bytes()
        for elf in (original, call):
            self.assertEqual(
                elf[prepare.PAYLOAD_OFFSET:
                    prepare.PAYLOAD_OFFSET + len(self.raw)], self.raw)
        self.assertEqual(struct.unpack_from("<I", original, 24)[0], 0xd8)
        offset, address = struct.unpack_from("<II", call, 88)
        size = struct.unpack_from("<I", call, 100)[0]
        self.assertEqual(address, probe.CALL_ADDRESS)
        code = call[offset:offset + size]
        self.assertTrue(code.startswith(
            b"\x61\xcf\xcb\xf8\x20\xfe"
            b"\xfc\x2d\x66\x00\xfc\x00\xbf\x00"))
        self.assertEqual(code.count(b"\xfc\x41\xc0\x00"), len(frame()))
        checks_start = 14 + (len(frame()) - 1) * 7
        for index in range(550):
            check = code[checks_start + index * 11:
                         checks_start + (index + 1) * 11]
            self.assertEqual(check[:1], b"\x8f")  # Read, not a RAM store.
            self.assertEqual(check[3:4], b"\x4c")
            self.assertEqual(check[5:], b"\xdd\x04\xcf\x01\x00\x00")
        self.assertEqual(code.count(b"\xcf\x01\x00\x00"), 550)
        self.assertTrue(code.endswith(b"\xcf\x02\x00\x00"))
        self.assertEqual(self.source.read_bytes(), self.raw)
        self.assertEqual((self.output / "frame.bin").read_bytes(), frame())
        self.assertEqual(json.loads(
            (self.output / "observation.json").read_text()), report)
        self.assertEqual(report["pre_completion_exact_byte_checks"], 550)
        self.assertTrue(report["pre_completion_checks_passed"])
        self.assertTrue(
            report["original_queue_envelope_ram_boundary_observed"])
        self.assertFalse(report["diagnostic_return_marker_observed"])
        for name in ("original_reset_executed", "subcpu_board_boot_verified",
                     "subcpu_exchange_verified", "rstp_verified", "hmi_ready",
                     "original_frame_checksum_validation_verified"):
            self.assertFalse(report[name])
        self.assertFalse((self.output / "result.json").exists())

    def test_wrong_marker_ram_pc_or_exit_cannot_pass_checks(self):
        for options in ({"boundary": False}, {"wrong_pc": True},
                        {"exit_code": 0}, {"exit_code": -9}):
            with self.subTest(options=options):
                report = self.run_call(**options)
                self.assertFalse(report["pre_completion_checks_passed"])
                self.assertFalse(
                    report["original_queue_envelope_ram_boundary_observed"])
                self.output = self.root / (self.output.name + "-next")

    def test_return_marker_is_not_queue_or_exchange_evidence(self):
        report = self.run_call(boundary=False, return_marker=True)
        self.assertTrue(report["pre_completion_checks_passed"])
        self.assertTrue(report["diagnostic_return_marker_observed"])
        self.assertFalse(
            report["original_queue_envelope_ram_boundary_observed"])
        self.assertFalse(report["subcpu_exchange_verified"])
        self.assertFalse(report["original_parser_queue_bytes_verified"])

    def test_runtime_clear_is_explicit_and_adds_read_only_queue_checks(self):
        report = self.run_call(
            boundary=False, return_marker=True, runtime_init=True)
        call = (self.output / "parser-call.elf").read_bytes()
        offset = struct.unpack_from("<I", call, 88)[0]
        size = struct.unpack_from("<I", call, 100)[0]
        code = call[offset:offset + size]
        self.assertTrue(code.startswith(
            b"\x61\xcf\xcb\xf8\x20\xfe\x36\x0c\xe7"
            b"\xfc\x57\x12\x00\xfc\x2d\x66\x00\xfc\x00\xbf\x00"))
        self.assertTrue(report["diagnostic_runtime_ram_clear_call_added"])
        self.assertEqual(report["original_runtime_ram_clear_routine"],
                         "0x1257")
        self.assertEqual(report["original_runtime_ram_clear_hl_argument"],
                         "0xe70c")
        self.assertEqual(report["post_completion_exact_byte_checks"], 286)
        self.assertTrue(report["original_parser_queue_bytes_verified"])
        expected = probe.queue_expected(frame())
        self.assertEqual(len(expected), 286)
        self.assertEqual(expected[0xff3c3 + len(frame()) - 4], frame()[-1])
        self.assertTrue(all(
            expected[0xff3c3 + index] == 0
            for index in range(len(frame()) - 3, 267)))
        start = len(code) - 4 - len(expected) * 11
        for index, (address, byte) in enumerate(expected.items()):
            self.assertEqual(code[start + index * 11:start + (index + 1) * 11],
                             b"\x8f" + struct.pack("<H", address & 0xffff)
                             + b"\x4c" + bytes([byte])
                             + b"\xdd\x04\xcf\x01\x00\x00")
        self.assertFalse(report["original_reset_executed"])
        self.assertFalse(report["subcpu_exchange_verified"])

    def test_runtime_queue_verification_requires_exact_return(self):
        for options in ({}, {"boundary": False},
                        {"boundary": False, "return_marker": True,
                         "exit_code": 0}):
            with self.subTest(options=options):
                report = self.run_call(runtime_init=True, **options)
                self.assertFalse(report["original_parser_queue_bytes_verified"])
                self.output = self.root / (self.output.name + "-next")

    def test_dispatch_requires_explicit_runtime_initialization(self):
        with self.assertRaisesRegex(ValueError, "explicit runtime init"):
            probe.prepare_call(b"", frame(), dispatch=True)
        with self.assertRaisesRegex(ValueError, "explicit runtime init"):
            probe.observe(self.source, self.frame_path, self.simulator,
                          self.output, dispatch=True)
        self.assertFalse(self.output.exists())

    def test_dispatch_checks_exact_delivery_without_running_application(self):
        report = self.run_call(boundary=False, return_marker=True,
                               runtime_init=True, dispatch=True)
        call = (self.output / "parser-call.elf").read_bytes()
        offset = struct.unpack_from("<I", call, 88)[0]
        size = struct.unpack_from("<I", call, 100)[0]
        code = call[offset:offset + size]
        expected = probe.delivery_expected(frame())
        self.assertEqual(len(expected), 49)
        start = len(code) - 4 - len(expected) * 11
        self.assertEqual(code[start - 4:start], b"\xfc\x58\x65\x00")
        for index, (address, byte) in enumerate(expected.items()):
            self.assertEqual(code[start + index * 11:start + (index + 1) * 11],
                             b"\x8f" + struct.pack("<H", address & 0xffff)
                             + b"\x4c" + bytes([byte])
                             + b"\xdd\x04\xcf\x01\x00\x00")
        self.assertTrue(report["original_receive_event_delivery_verified"])
        self.assertTrue(report["diagnostic_f13_irq_configuration_enabled"])
        self.assertEqual(report["original_immediate_dispatcher"], "0x6558")
        self.assertEqual(report["post_dispatch_exact_byte_checks"], 49)
        self.assertFalse(report["original_frame_checksum_validation_verified"])
        self.assertFalse(report["original_reset_executed"])
        self.assertFalse(report["subcpu_exchange_verified"])

    def test_dispatch_delivery_requires_exact_success_marker(self):
        for options in ({}, {"boundary": False}, {"exit_code": 0}):
            with self.subTest(options=options):
                report = self.run_call(runtime_init=True, dispatch=True,
                                       **options)
                self.assertFalse(
                    report["original_receive_event_delivery_verified"])
                self.output = self.root / (self.output.name + "-next")

    def test_corrupt_checksum_is_reported_without_repair_or_acceptance(self):
        packet = frame()
        packet = packet[:-1] + bytes([packet[-1] ^ 1])
        self.frame_path.write_bytes(packet)
        report = self.run_call()
        self.assertFalse(report["host_checksum_matches"])
        self.assertFalse(report["original_frame_checksum_validation_verified"])
        self.assertEqual((self.output / "frame.bin").read_bytes(), packet)
        self.assertEqual(self.frame_path.read_bytes(), packet)

    def test_timeout_and_missing_signature_retain_evidence(self):
        for options, error in (({"timeout": True}, TimeoutError),
                               ({"signature": False}, ValueError)):
            with self.subTest(options=options), self.assertRaises(error):
                self.run_call(**options)
            self.assertTrue((self.output / "observation.json").exists())
            self.assertTrue((self.output / "parser-trace.log").exists())
            self.output = self.root / (self.output.name + "-next")

    def test_invalid_inputs_do_not_create_output(self):
        for data in (b"", b"x" * 14, frame()[:-1], frame() + b"\0",
                     b"\xf7\xc1\x01\x0b\x01" + bytes(267)):
            with self.subTest(data=data[:5]):
                self.frame_path.write_bytes(data)
                with self.assertRaises(ValueError):
                    probe.observe(self.source, self.frame_path,
                                  self.simulator, self.output)
                self.assertFalse(self.output.exists())
        self.frame_path.write_bytes(frame())
        for timeout in (0, -1, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                probe.observe(self.source, self.frame_path, self.simulator,
                              self.output, timeout=timeout)
            self.assertFalse(self.output.exists())

    def test_minimum_and_maximum_frames_use_same_read_only_checks(self):
        with patch.object(prepare, "IMAGE_SHA256", self.digest):
            prepare.prepare(self.source, self.output)
        original = (self.output / "subcpu.elf").read_bytes()
        for length in (10, 266):
            with self.subTest(length=length):
                call, count = probe.prepare_call(original, frame(length))
                offset, address = struct.unpack_from("<II", call, 88)
                size = struct.unpack_from("<I", call, 100)[0]
                self.assertEqual(address, probe.CALL_ADDRESS)
                self.assertEqual(count, 550)
                code = call[offset:offset + size]
                self.assertEqual(code.count(b"\xfc\x41\xc0\x00"), length + 4)

    @unittest.skipUnless(
        SIMULATOR, "Set MH2P_RL78_SIM to the strict GNU runner")
    def test_gnu_checks_and_bad_queue_slot(self):
        for length, bad_slot in ((10, False), (266, False), (10, True)):
            with self.subTest(length=length, bad_slot=bad_slot):
                packet = frame(length)
                initial = {
                    0xf4ce: length & 0xff, 0xf4cf: length >> 8,
                    0xf4d0: 5, 0xf4d2: length & 0xff,
                    0xf4d3: length >> 8, 0xe8ae: 0,
                }
                initial.update({0xe8a4 + index: 0 for index in range(10)})
                initial.update({
                    0xf2b8 + index: byte
                    for index, byte in enumerate(packet[3:-1])
                })
                initial.update({
                    0xf2b8 + index: 0 for index in range(length, 267)
                })
                initial.update({0xf3c3 + index: 0 for index in range(267)})
                if bad_slot:
                    initial[0xe8ad] = 1
                initializer = b"".join(
                    b"\xcf" + struct.pack("<HB", address, byte)
                    for address, byte in initial.items()) + b"\xd7"
                raw = bytearray(self.raw)
                start = probe.QUEUE_INITIALIZER
                raw[start:start + len(initializer)] = initializer
                raw[probe.PARSER_INITIALIZER] = 0xd7
                raw[probe.PARSER_ADDRESS] = 0xd7
                self.source.write_bytes(raw)
                self.frame_path.write_bytes(packet)
                self.output = self.root / f"gnu-{length}-{bad_slot}"
                with patch.object(
                        prepare, "IMAGE_SHA256",
                        hashlib.sha256(raw).hexdigest()):
                    report = probe.observe(
                        self.source, self.frame_path, Path(SIMULATOR),
                        self.output)
                self.assertEqual(report["pre_completion_checks_passed"],
                                 not bad_slot)
                self.assertEqual(report["diagnostic_return_marker_observed"],
                                 not bad_slot)
                self.assertEqual(
                    report["first_unsupported_access"]["address"],
                    "0xf0001" if bad_slot else "0xf0002")
                self.assertIsNone(report["first_uninitialized_ram_read"])

    @unittest.skipUnless(
        SIMULATOR, "Set MH2P_RL78_SIM to the strict GNU runner")
    def test_gnu_runtime_queue_checks_reject_corrupt_envelope(self):
        for length, corrupt, dispatch, bad_delivery in (
                (10, False, False, False), (266, False, False, False),
                (10, True, False, False), (10, False, True, False),
                (266, False, True, False), (10, False, True, True)):
            with self.subTest(length=length, corrupt=corrupt,
                              dispatch=dispatch, bad_delivery=bad_delivery):
                packet = frame(length)
                initial = {
                    0xf4ce: length & 0xff, 0xf4cf: length >> 8,
                    0xf4d0: 5, 0xf4d2: length & 0xff,
                    0xf4d3: length >> 8, 0xe8ae: 0,
                }
                initial.update({0xe8a4 + i: 0 for i in range(10)})
                initial.update({
                    0xf2b8 + i: byte for i, byte in enumerate(packet[3:-1])})
                initial.update({
                    0xf2b8 + i: 0 for i in range(length, 267)})
                initial.update({0xf3c3 + i: 0 for i in range(267)})

                def stores(values):
                    return b"".join(
                        b"\xcf" + struct.pack("<HB", address & 0xffff, byte)
                        for address, byte in values.items())

                raw = bytearray(self.raw)
                clear = stores({0xfe700: 0, 0xfe701: 0}) + b"\xd7"
                raw[probe.RUNTIME_RAM_CLEAR:
                    probe.RUNTIME_RAM_CLEAR + len(clear)] = clear
                initializer = stores(initial) + b"\xd7"
                raw[probe.QUEUE_INITIALIZER:
                    probe.QUEUE_INITIALIZER + len(initializer)] = initializer
                raw[probe.PARSER_INITIALIZER] = 0xd7
                # Count actual parser calls; only the last writes the queue.
                parser = (b"\xa2\x00\xe7\xaf\x00\xe7\x44"
                          + struct.pack("<H", len(packet)) + b"\xdd\x01\xd7")
                queued = probe.queue_expected(packet)
                if corrupt:
                    queued[0xfe85a] = 1
                parser += stores(queued) + b"\xd7"
                raw[probe.PARSER_ADDRESS:
                    probe.PARSER_ADDRESS + len(parser)] = parser
                if dispatch:
                    delivered = probe.delivery_expected(packet)
                    if bad_delivery:
                        delivered[0xfe758] ^= 1
                    handler = stores(delivered) + b"\xd7"
                    raw[probe.IMMEDIATE_DISPATCHER:
                        probe.IMMEDIATE_DISPATCHER + len(handler)] = handler
                self.source.write_bytes(raw)
                self.frame_path.write_bytes(packet)
                self.output = self.root / (
                    f"gnu-runtime-{length}-{corrupt}-{dispatch}-{bad_delivery}")
                with patch.object(
                        prepare, "IMAGE_SHA256",
                        hashlib.sha256(raw).hexdigest()):
                    report = probe.observe(
                        self.source, self.frame_path, Path(SIMULATOR),
                        self.output, runtime_init=True, dispatch=dispatch)
                self.assertEqual(report["original_parser_queue_bytes_verified"],
                                 not corrupt and not bad_delivery)
                self.assertEqual(
                    report["original_receive_event_delivery_verified"],
                    dispatch and not corrupt and not bad_delivery)
                self.assertEqual(report["first_unsupported_access"]["address"],
                                 "0xf0001" if corrupt or bad_delivery
                                 else "0xf0002")
                self.assertIsNone(report["first_uninitialized_ram_read"])


if __name__ == "__main__":
    unittest.main()
