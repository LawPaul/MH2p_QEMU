# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free Cortex-A15 AXI write-error execution checks."""
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT
from qemu.machine.qtest import QEMUQtestProtocol
from test_rcc_ipu import ArmProgram


class RccAXIErrorTests(unittest.TestCase):
    def test_unwired_a15_keeps_synchronous_store_abort(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        program.load(0, 0x80020000)
        program.words.append(0xee0c0f10)
        program.write(0x48140a84, 0x40020)
        program.words.append(0xee391f70)
        program.load(0, 0x80010308)
        program.words.append(0xe5801000)
        program.write(0x80010200, 0x5a17c0de)
        handler = ArmProgram()
        handler.words += [0xee151f10, 0xee162f10]
        handler.load(0, 0x80010300)
        handler.words += [0xe5801000, 0xe5802004, 0xe25ef004]
        vectors = bytearray(0x100)
        struct.pack_into("<I", vectors, 0x10, 0xea00003a)
        vectors.extend(handler.image()[0x100:])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(program.image()[0x100:])
            (root / "vectors").write_bytes(vectors)
            args = [
                "-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
                "-S", "-display", "none", "-serial", "null", "-nic", "none",
                "-device", f"loader,file={root / 'vectors'},"
                           "addr=0x80020000,force-raw=on",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)

                def word(address):
                    raw = vm.cmd("human-monitor-command",
                                 command_line=f"xp /1wx {address:#x}")
                    return int(raw.split(":")[1].strip(), 16)

                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while word(0x80010200) != 0x5a17c0de:
                    if time.monotonic() >= deadline:
                        self.fail(vm.cmd("human-monitor-command",
                                         command_line="info registers"))
                    time.sleep(.01)
                vm.cmd("stop")
                self.assertEqual(word(0x80010300) & 0xc0f, 0x808)
                self.assertEqual(word(0x80010304), 0x48140a84)
                self.assertEqual(word(0x80010308), 0)

    def test_write_error_status_irq_clear_and_read_abort(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        expected = []
        result = 0x80010000

        def capture_l2(value):
            nonlocal result
            program.words.append(0xee391f70)  # MRC p15,1,r1,c9,c0,3
            program.load(0, result)
            program.words.append(0xe5801000)
            expected.append((result, value))
            result += 4

        program.write(0x48211000, 1)
        program.write(0x48211104, 8)  # Enable SPI 35.
        capture_l2(0)
        program.write(0x80010400, 1)
        program.wait(0x80010404, 1)
        program.load(1, 0x20000000)
        program.words.append(0xee291f70)  # Software cannot assert L2ECTLR.
        capture_l2(0)
        program.write(0x48140a84, 0x40020)
        capture_l2(0x20000000)
        program.copy(0x48211204, result)  # GICD_ISPENDR1, SPI 35.
        expected.append((result, 8))
        result += 4
        program.load(1, 0xffffffff)
        program.words.append(0xee291f70)
        capture_l2(0x20000000)
        program.load(1, 0)
        program.words.append(0xee291f70)
        capture_l2(0x20000000)  # NSACR.NS_L2ERR initially denies clearing.
        program.words.append(0xe1600070)  # SMC: grant NS_L2ERR in test monitor.
        program.load(1, 0)
        program.words.append(0xee291f70)
        capture_l2(0)
        program.write(0x48211284, 8)  # Clear the GIC's latched pending bit.
        program.copy(0x48211204, result)
        expected.append((result, 0))
        result += 4
        program.write(0x48140a88, 0x40020)
        capture_l2(0x20000000)
        program.load(1, 0)
        program.words.append(0xee291f70)
        capture_l2(0)

        program.load(0, 0x80020000)
        program.words.append(0xee0c0f10)  # VBAR
        program.copy(0x48140a84, result)  # Failed reads must still abort.
        program.write(0x48140a84, 0x40020)
        program.write(0x80010200, 0x5a17c0de)

        handler = ArmProgram()
        handler.words += [0xee151f10, 0xee162f10]  # DFSR, DFAR
        handler.load(0, 0x80010300)
        handler.words += [0xe5801000, 0xe5802004, 0xe25ef004]
        vectors = bytearray(0x100)
        struct.pack_into("<I", vectors, 0x10, 0xea00003a)
        vectors.extend(handler.image()[0x100:])
        monitor = ArmProgram()
        monitor.load(0, 1 << 17)
        monitor.words += [0xee010f51, 0xe1b0f00e]  # NSACR; exception return.
        monitor_vectors = bytearray(0x100)
        struct.pack_into("<I", monitor_vectors, 8, 0xea00003c)
        monitor_vectors.extend(monitor.image()[0x100:])

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(program.image())
            (root / "vectors").write_bytes(vectors)
            (root / "monitor").write_bytes(monitor_vectors)
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "kernel"),
                "-S", "-display", "none", "-serial", "null",
                "-serial", "null", "-serial", "null",
                "-device", f"loader,file={root / 'vectors'},"
                           "addr=0x80020000,force-raw=on",
                "-device", f"loader,file={root / 'monitor'},"
                           "addr=0x40000000,force-raw=on",
                "-qtest", f"unix:{root / 'qtest'},server=on,wait=off",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)

                def word(address):
                    raw = vm.cmd("human-monitor-command",
                                 command_line=f"xp /1wx {address:#x}")
                    return int(raw.split(":")[1].strip(), 16)

                def wait_word(address, value):
                    deadline = time.monotonic() + 5
                    while word(address) != value:
                        if time.monotonic() >= deadline:
                            self.fail(vm.cmd("human-monitor-command",
                                             command_line="info registers"))
                        time.sleep(.01)

                qtest = QEMUQtestProtocol(str(root / "qtest"))
                qtest.connect()
                qtest.settimeout(5)
                try:
                    vm.cmd("cont")
                    wait_word(0x80010400, 1)
                    qtest.cmd("writel 0x80010404 1")
                    wait_word(0x80010200, 0x5a17c0de)
                    vm.cmd("stop")
                    for address, value in expected:
                        self.assertEqual(word(address), value, hex(address))
                    self.assertEqual(word(0x80010300) & 0xc0f, 8)
                    self.assertEqual(word(0x80010304), 0x48140a84)
                    # Leave an error asserted, then verify CPU and IRQ reset.
                    qtest.cmd("writel 0x80010400 0")
                    qtest.cmd("writel 0x80010404 0")
                    vm.cmd("system_reset")
                    vm.cmd("cont")
                    wait_word(0x80010400, 1)
                    vm.cmd("stop")
                    self.assertEqual(word(0x80010000), 0)
                    self.assertEqual(word(0x48211204) & 8, 0)
                finally:
                    qtest.close()


if __name__ == "__main__":
    unittest.main()
