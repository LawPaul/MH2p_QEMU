#!/usr/bin/env python3
"""Firmware-free RCC probe guards and synthetic board header checks."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import probe_rcc as rcc

QEMU = Path(os.environ.get(
    "QTEST_QEMU_BINARY", rcc.ROOT / "build/qemu-system-arm")).resolve()


class RCCProbeTests(unittest.TestCase):
    def test_invalid_observation_and_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for seconds in (0, -1, 31, float("nan"), float("inf")):
                with self.subTest(seconds=seconds), self.assertRaises(ValueError):
                    rcc.probe(root / "qemu", root / "missing", root / "output",
                              seconds)
            with self.assertRaises(FileExistsError):
                rcc.probe(root / "qemu", root / "missing", root)
            self.assertFalse((root / "output").exists())

    def test_original_image_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, output = root / "image", root / "output"
            image.write_bytes(b"synthetic")
            with self.assertRaisesRegex(ValueError, "size"):
                rcc.probe(root / "qemu", image, output)
            with patch.object(rcc, "RCC_IMAGE_SIZE", image.stat().st_size):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    rcc.probe(root / "qemu", image, output)
            self.assertFalse(output.exists())

    def test_bounded_command_and_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, output = root / "image", root / "output"
            payload = b"synthetic RCC fixture"
            image.write_bytes(payload)
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm.cmd.return_value = "synthetic stopped registers"

            def launch():
                for port in range(3):
                    (output / f"uart-{port}.log").write_bytes(b"x" * port)

            vm.launch.side_effect = launch
            with patch.object(rcc, "RCC_IMAGE_SIZE", len(payload)), \
                 patch.object(rcc, "RCC_SHA256",
                              hashlib.sha256(payload).hexdigest()), \
                 patch.object(rcc, "QEMUMachine", return_value=vm) as factory, \
                 patch.object(rcc.time, "sleep") as sleep:
                report = rcc.probe(root / "qemu", image, output, 0.25, True)
            sleep.assert_called_once_with(0.25)
            vm._qmp.settimeout.assert_called_once_with(5)
            self.assertEqual([call.args[0] for call in vm.cmd.call_args_list],
                             ["cont", "stop", "human-monitor-command"])
            vm.__exit__.assert_called_once()
            args = factory.call_args.kwargs["args"]
            self.assertEqual(args[:4], ["-M", "mh2p-rcc", "-kernel", str(image)])
            self.assertIn("shift=0,align=off,sleep=on", args)
            self.assertIn("in_asm,unimp,guest_errors", args)
            self.assertEqual(report["serial_bytes"], {"0": 0, "1": 1, "2": 2})
            self.assertIn("does not prove", report["note"])
            self.assertEqual(json.loads((output / "result.json").read_text()),
                             report)
            self.assertEqual((output / "registers.log").read_text(),
                             "synthetic stopped registers")
            self.assertEqual(image.read_bytes(), payload)

    def test_vm_failure_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, output = root / "image", root / "output"
            image.write_bytes(b"synthetic")
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm.launch.side_effect = RuntimeError("launch failed")
            with patch.object(rcc, "RCC_IMAGE_SIZE", 9), \
                 patch.object(rcc, "RCC_SHA256",
                              hashlib.sha256(b"synthetic").hexdigest()), \
                 patch.object(rcc, "QEMUMachine", return_value=vm), \
                 self.assertRaisesRegex(RuntimeError, "launch failed"):
                rcc.probe(root / "qemu", image, output)
            self.assertFalse((output / "result.json").exists())
            vm.__exit__.assert_called_once()

    def test_qmp_timeout_kills_vm_without_success_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, output = root / "image", root / "output"
            image.write_bytes(b"synthetic")
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm.cmd.side_effect = [None, TimeoutError("stop stalled")]
            with patch.object(rcc, "RCC_IMAGE_SIZE", 9), \
                 patch.object(rcc, "RCC_SHA256",
                              hashlib.sha256(b"synthetic").hexdigest()), \
                 patch.object(rcc, "QEMUMachine", return_value=vm), \
                 patch.object(rcc.time, "sleep"), \
                 self.assertRaisesRegex(TimeoutError, "stop stalled"):
                rcc.probe(root / "qemu", image, output)
            vm.kill.assert_called_once()
            self.assertFalse((output / "result.json").exists())


class RCCHeaderTests(unittest.TestCase):
    @unittest.skipUnless(QEMU.is_file(),
                         "requires an existing ARM QEMU build")
    def test_invalid_startup_headers(self):
        payload = bytearray(0x110)
        struct.pack_into("<IH", payload, 8, 0x00ff7eeb, 1)
        struct.pack_into("<HH", payload, 16, 0x100, 40)
        struct.pack_into("<I", payload, 20, 0x80100100)
        struct.pack_into("<I", payload, 28, 0x80100008)
        struct.pack_into("<II", payload, 40, 0x108, 0x108)
        cases = [(offset, value) for offset, value in
                 ((8, 0), (12, 2), (17, 0), (18, 3), (20, 1), (28, 0),
                  (40, 0xff), (44, 0))]
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "startup"
            for offset, value in cases:
                invalid = payload.copy()
                invalid[offset] = value
                image.write_bytes(invalid)
                with self.subTest(offset=offset):
                    result = subprocess.run(
                        [str(QEMU),
                         "-M", "mh2p-rcc", "-kernel", str(image),
                         "-display", "none", "-nic", "none", "-S"],
                        capture_output=True, text=True, timeout=5, check=False)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("unsupported QNX startup header", result.stderr)


@unittest.skipUnless(QEMU.is_file(),
                     "requires an existing ARM QEMU build")
class RCCMonitorTests(unittest.TestCase):
    def run_payload(self, code, extra=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "startup"
            payload = bytearray(0x300)
            struct.pack_into("<IH", payload, 8, 0x00ff7eeb, 1)
            struct.pack_into("<HH", payload, 16, 0x100, 40)
            struct.pack_into("<I", payload, 20, 0x80100100)
            struct.pack_into("<I", payload, 28, 0x80100008)
            struct.pack_into("<II", payload, 40, len(payload) - 8,
                             len(payload) - 8)
            struct.pack_into(f"<{len(code)}I", payload, 0x100, *code)
            for offset, words in extra:
                struct.pack_into(f"<{len(words)}I", payload, offset, *words)
            image.write_bytes(payload)
            args = ["-M", "mh2p-rcc", "-kernel", str(image),
                    "-display", "none", "-nic", "none", "-S",
                    "-icount", "shift=0,align=off,sleep=off",
                    "-serial", "null", "-serial", "null", "-serial", "null"]
            results = []
            with rcc.QEMUMachine(str(QEMU),
                                args=args) as vm:
                vm.launch()
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                    vm.cmd("cont")
                    rcc.time.sleep(0.1)
                    vm.cmd("stop")
                    dump = root / f"result-{boot}"
                    vm.cmd("pmemsave", val=0x80000000, size=24,
                           filename=str(dump))
                    results.append(struct.unpack("<6I", dump.read_bytes()))
            return results

    def test_nonsecure_virtual_timer_irq_and_reset(self):
        code = [
            0xe3001000, 0xe3481000,  # movw/movt r1, 0x80000000: results
            0xe3a00000,
            0xe5810000, 0xe5810004,  # Clear completion and interrupt ID.
            0xe3012000, 0xe3442821,  # r2 = 0x48211000: distributor
            0xe3024000, 0xe3444821,  # r4 = 0x48212000: CPU interface
            0xe3005200, 0xe3485010,  # r5 = 0x80100200: exception vectors
            0xee0c5f10,              # mcr p15,0,r5,c12,c0,0: VBAR
            0xe5940004, 0xe5810008,  # Record handed-off priority mask.
            0xe5920000, 0xe5810010,  # Record disabled distributor.
            0xe5940000, 0xe5810014,  # Record disabled CPU interface.
            0xe3a000ff, 0xe5840004,  # Allow all nonsecure priorities.
            0xe3a00001,
            0xe5820000, 0xe5840000,  # Enable nonsecure distributor/interface.
            0xe3a00000, 0xe5c2041b,  # Set PPI 27 nonsecure priority.
            0xe3a00302, 0xe5820100,  # Enable virtual timer PPI 27.
            0xe5920100, 0xe581000c,  # Record actual interrupt enable.
            0xe30003e8,              # movw r0,#1000
            0xee0e0f13,              # CNTV_TVAL = 1000 ticks
            0xe3a00001, 0xee0e0f33,  # CNTV_CTL = ENABLE
            0xf1080080,              # cpsie i
            0xeafffffe,
        ]
        handler = [
            0xe594000c, 0xe5810004,  # Acknowledge and record interrupt ID.
            0xe3a03000, 0xee0e3f33,  # Disable virtual timer.
            0xe5840010,              # End this interrupt.
            0xe3a03001, 0xe5813000,  # Completion marker.
            0xe25ef004,              # subs pc,lr,#4: exception return
        ]
        vectors = [(0x218, [0xea000008]), (0x240, handler)]
        for result in self.run_payload(code, vectors):
            done, irq, pmask, enabled, distributor, interface = result
            self.assertEqual(done, 1)
            self.assertEqual(irq, 27)
            self.assertEqual(pmask, 0xfe)
            self.assertEqual(enabled & (1 << 27), 1 << 27)
            self.assertEqual((distributor, interface), (0, 0))

    def test_secure_service_state_return_and_reset(self):
        code = [
            0xe59f1040,  # ldr r1, literal 0x80000000 at +0x48
            0xee3f2f10,  # mrc p15,1,r2,c15,c0,0
            0xe5812014,  # str r2,[r1,#20]: reset L2ACTLR
            0xe300c104,  # movw ip,#0x104
            0xe3000118,  # movw r0,#0x118
            0xe1530003,  # cmp r3,r3: set Z to test SPSR restoration
            0xe1600070,  # smc #0
            0xe5810000,  # str r0,[r1]: success status
            0xee3f2f10,  # mrc p15,1,r2,c15,c0,0
            0xe5812004,  # str r2,[r1,#4]: actual register value
            0xe10f2000,  # mrs r2,cpsr
            0xe5812008,  # str r2,[r1,#8]
            0xe3a0c000,  # mov ip,#0: unknown service
            0xe1600070,
            0xe581000c,  # str r0,[r1,#12]
            0xe3a02001,
            0xe5812010,  # str r2,[r1,#16]: completion marker
            0xeafffffe,
            0x80000000,
        ]
        for result in self.run_payload(code):
            status, value, cpsr, unknown, done, reset = result
            self.assertEqual(done, 1)
            self.assertEqual(status, 0)
            self.assertEqual(value, 0x118)
            self.assertEqual(cpsr & 0x1f, 0x13)  # Back in caller's SVC mode.
            self.assertEqual(cpsr & 0x40000000, 0x40000000)
            self.assertEqual(unknown, 0xffffffff)
            self.assertEqual(reset, 0)

    def test_nonsecure_direct_write_is_undefined(self):
        code = [
            0xe59f1024,  # ldr r1, literal 0x80000000 at +0x2c
            0xe59f2024,  # ldr r2, literal 0x80100200 at +0x30
            0xee0c2f10,  # mcr p15,0,r2,c12,c0,0: VBAR
            0xe3a00080,
            0xee2f0f10,  # Forbidden nonsecure L2ACTLR write.
            0xee3f2f10,
            0xe5812000,
            0xe5813004,  # Exception handler marker.
            0xe3a02001,
            0xe5812008,
            0xeafffffe,
            0x80000000,
            0x80100200,
        ]
        vectors = [(0x204, [0xea00000d]),
                   (0x240, [0xe3a03001, 0xe1b0f00e])]
        for result in self.run_payload(code, vectors):
            self.assertEqual(result[:3], (0, 1, 1))


if __name__ == "__main__":
    unittest.main()
