# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks of the T124 secondary-core cold-start sequence."""
from contextlib import contextmanager
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT
from qemu.machine.qtest import QEMUQtestProtocol
from test_rcc_ipu import ArmProgram


class MMXSecondaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not cls.qemu.is_file():
            raise unittest.SkipTest("Build qemu-system-arm first")

    @contextmanager
    def machine(self, host, secondary, threads, psci=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            kernel = root / "startup"
            kernel.write_bytes(host.image()[0x100:])
            payload = root / "secondary"
            payload.write_bytes(secondary)
            args = [
                "-M", "mh2p,experimental-psci=on" if psci else "mh2p",
                "-smp", "4", "-m", "256M",
                "-kernel", str(kernel), "-S", "-display", "none",
                "-nic", "none", "-serial", "null",
                "-accel", f"tcg,thread={threads}",
                "-device",
                f"loader,file={payload},addr=0x80020000,force-raw=on",
                "-qtest", f"unix:{root / 'qtest'},server=on,wait=off",
                "-d", "unimp,guest_errors", "-D", str(root / "errors"),
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                qtest = QEMUQtestProtocol(str(root / "qtest"))
                qtest.connect()
                qtest.settimeout(5)
                try:
                    yield vm, qtest
                finally:
                    qtest.close()
            self.assertEqual((root / "errors").read_text(), "")

    def monitor(self, vm, command):
        return vm.cmd("human-monitor-command", command_line=command)

    def word(self, vm, address):
        reply = self.monitor(vm, f"xp /1wx {address:#x}")
        return int(reply.split(":")[1].strip(), 16)

    def wait_word(self, vm, address, expected):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.word(vm, address) == expected:
                return
            time.sleep(.01)
        self.fail(f"{address:#x}: expected {expected:#x}, "
                  f"got {self.word(vm, address):#x}")

    def exercise(self, threads, psci=False):
        program = ArmProgram()
        program.load(0, 0x80021000)
        program.words.append(0xee0c0f10)  # VBAR
        program.write(0x50042004, 0xff)  # GICC_PMR
        program.write(0x50042000, 1)  # GICC_CTLR
        program.words.append(0xf1080080)  # CPSIE i
        program.words += [0xee100fb0, 0xe2002003]  # MPIDR; core index
        program.load(1, 0x80010000)
        program.words.append(0xe7810102)  # STR r0, [r1, r2, LSL #2]
        secondary = bytearray(program.image()[0x100:])
        secondary.extend(bytes(0x1018 - len(secondary)))
        # IRQ handler: acknowledge/EOI, record the SGI on this core, return.
        secondary.extend(struct.pack(
            "<10I", 0xe59f0018, 0xe590200c, 0xe5802010,
            0xee103fb0, 0xe2033003, 0xe59f1008, 0xe7812103,
            0xe25ef004, 0x50042000, 0x80010600))
        host = ArmProgram()
        host.copy(0x7000f9f4, 0x80010800)
        host.words.append(0xee390f50)  # L2CTLR
        host.load(1, 0x80010804)
        host.words.append(0xe5810000)
        host.write(0x6000f100, 0x80020000)
        for cpu in (1, 2, 3):
            power = 1 << (cpu + 8)
            reset = (1 << cpu) | (1 << (cpu + 4)) | (1 << (cpu + 12))
            host.write(0x60007014, 0)
            if psci:
                host.load(0, 0x84000003)
                host.load(1, cpu)
                host.load(2, 0x80020000)
                host.load(3, 0)
                host.words.append(0xe1600070)  # SMC CPU_ON
                host.load(1, 0x80010700 + 4 * cpu)
                host.words.append(0xe5810000)  # Record actual PSCI result.
            else:
                host.write(0x7000e430, 0x100 | (cpu + 8))
            host.copy(0x7000e438, 0x80010100 + 4 * cpu)
            host.copy(0x7000e42c, 0x80010200 + 4 * cpu)
            host.write(0x7000e434, power)
            host.copy(0x7000e42c, 0x80010300 + 4 * cpu)
            host.write(0x6000604c, 0 if psci else
                       0xe00 & ~((1 << (cpu + 9)) - 1))
            host.write(0x60006344, reset)
            host.wait(0x80010000 + 4 * cpu, 0x80000000 | cpu)
        host.write(0x50041000, 1)  # GICD_CTLR
        host.write(0x50041f00, 0x000e0001)  # SGI 1 to cores 1, 2 and 3
        for cpu in (1, 2, 3):
            host.wait(0x80010600 + 4 * cpu, 1)
        host.write(0x80010400, 0x5a17c0de)
        with self.machine(host, secondary, threads, psci) as (vm, _qtest):
            self.assertEqual(len(vm.cmd("query-cpus-fast")), 4)
            vm.cmd("cont")
            self.wait_word(vm, 0x80010400, 0x5a17c0de)
            vm.cmd("stop")
            self.assertEqual(self.word(vm, 0x80010800), 0)
            self.assertEqual(self.word(vm, 0x80010804), 0x03800000)
            for cpu in (1, 2, 3):
                power = 1 << (cpu + 8)
                self.assertEqual(self.word(vm, 0x80010000 + 4 * cpu),
                                 0x80000000 | cpu)
                self.assertTrue(self.word(vm, 0x80010100 + 4 * cpu) & power)
                self.assertEqual(
                    bool(self.word(vm, 0x80010200 + 4 * cpu) & power),
                    not psci)
                self.assertFalse(self.word(vm, 0x80010300 + 4 * cpu) & power)
                self.assertEqual(self.word(vm, 0x80010600 + 4 * cpu), 1)
                if psci:
                    self.assertEqual(self.word(vm, 0x80010700 + 4 * cpu), 0)
            vm.cmd("system_reset")
            self.assertEqual(self.word(vm, 0x6000f100), 0)
            self.assertEqual(self.word(vm, 0x6000604c), 0 if psci else 0xe00)
            self.assertEqual(self.word(vm, 0x7000e438) & 0xe00,
                             0xe00 if psci else 0)
            self.assertEqual(self.word(vm, 0x7000e42c) & 0xe00,
                             0 if psci else 0xe00)

    def test_native_cold_start_single_tcg(self):
        self.exercise("single")

    def test_native_cold_start_multi_tcg(self):
        self.exercise("multi")

    def test_psci_cold_start_single_tcg(self):
        self.exercise("single", psci=True)

    def test_psci_cold_start_multi_tcg(self):
        self.exercise("multi", psci=True)

    def test_each_gate_inhibits_execution_and_reset_restarts_vector(self):
        secondary = struct.pack(
            "<6I", 0xe59f000c, 0xe3a01001, 0xe5801000,
            0xeafffffe, 0xe1a00000, 0x80010004)
        secondary += struct.pack(
            "<6I", 0xe59f000c, 0xe3a01002, 0xe5801000,
            0xeafffffe, 0xe1a00000, 0x80010004)
        power = 1 << 9
        reset = (1 << 1) | (1 << 5) | (1 << 13)
        for threads in ("single", "multi"):
            for gate in ("power", "clamp", "clock", "reset"):
                with self.subTest(threads=threads, gate=gate):
                    with self.machine(
                            ArmProgram(), secondary, threads) as (vm, qtest):
                        def write(address, value):
                            self.assertEqual(
                                qtest.cmd(f"writel {address:#x} {value:#x}"),
                                "OK\n")

                        write(0x6000f100, 0x80020000)
                        if gate != "power":
                            write(0x7000e430, 0x109)
                        if gate != "clamp":
                            write(0x7000e434, power)
                        if gate != "clock":
                            write(0x6000604c, 0xc00)
                        if gate != "reset":
                            write(0x60006344, reset)
                        vm.cmd("cont")
                        time.sleep(.05)
                        self.assertEqual(self.word(vm, 0x80010004), 0)
                        if gate == "power":
                            write(0x7000e430, 0x109)
                            # Removing a clamp while powered off cannot stick.
                            self.assertTrue(
                                self.word(vm, 0x7000e42c) & power)
                            write(0x7000e434, power)
                        elif gate == "clamp":
                            write(0x7000e434, power)
                        elif gate == "clock":
                            write(0x6000604c, 0xc00)
                        else:
                            write(0x60006344, reset)
                        self.wait_word(vm, 0x80010004, 1)
                        self.assertEqual(self.word(vm, 0x80010008), 0)
                        self.assertEqual(self.word(vm, 0x8001000c), 0)
                        write(0x60006340, reset)
                        time.sleep(.02)
                        write(0x80010004, 0)
                        time.sleep(.02)
                        self.assertEqual(self.word(vm, 0x80010004), 0)
                        write(0x6000f100, 0x80020018)
                        write(0x60006344, reset)
                        self.wait_word(vm, 0x80010004, 2)


if __name__ == "__main__":
    unittest.main()
