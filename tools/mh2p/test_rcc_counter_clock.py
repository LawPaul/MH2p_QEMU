# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free RCC architectural counter and timer-expiry checks."""
import os
from pathlib import Path
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT
from test_rcc_ipu import ArmProgram


class RccCounterClockTests(unittest.TestCase):
    def test_native_counter_rate_and_virtual_timer_expiry(self):
        for shift in (2, 5):
            with self.subTest(shift=shift):
                self.check_counter_rate_and_expiry(shift)

    def check_counter_rate_and_expiry(self, shift, *, host_timeout=3):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        program.load(4, 0x4ae04030)
        program.words += [
            0xe5945000,  # ldr r5, [r4]: independent 32-kHz counter.
            0xec576f1e,  # mrrc p15, 1, r6, r7, c14: CNTVCT.
        ]
        program.load(0, 0x80010000)
        program.words += [
            0xe5806000, 0xe5805004,
            0xee1e1f10,  # mrc p15, 0, r1, c14, c0, 0: CNTFRQ.
            0xe5801008,
        ]
        program.load(8, 3277)
        program.words += [
            0xe5949000, 0xe0499005, 0xe1590008, 0x3afffffb,
            0xec576f1e, 0xe5949000, 0xe580600c, 0xe5809010,
            0xe5945000,
        ]
        program.load(1, 61440)
        program.words.append(0xee0e1f13)  # CNTV_TVAL: nominal 10 ms.
        program.load(1, 3)
        program.words += [
            0xee0e1f33,  # CNTV_CTL: enable, mask interrupt.
            0xee1e1f33, 0xe3110004, 0x0afffffc,  # Wait for ISTATUS.
            0xe5949000, 0xe0499005, 0xe5809018,
        ]
        program.load(1, 0)
        program.words.append(0xee0e1f33)  # Disable the expired timer.
        program.write(0x48211000, 1)
        program.write(0x48211100, 1 << 27)
        program.write(0x48212004, 0xff)
        program.write(0x48212000, 1)
        program.words.append(0xe5945000)
        program.load(1, 61440)
        program.words.append(0xee0e1f13)
        program.load(1, 1)
        program.words += [
            0xee0e1f33,  # Enable the unmasked virtual timer.
            0xf57ff04f, 0xe320f003,  # DSB; WFI, with CPSR.I still set.
        ]
        program.copy(0x4821200c, 0x80010020)  # GICC_IAR.
        program.load(1, 0)
        program.words.append(0xee0e1f33)
        program.write(0x48212010, 27)  # GICC_EOIR.
        program.load(0, 0x80010000)
        program.words += [0xe5949000, 0xe0499005, 0xe5809024]
        program.write(0x80010014, 0x5a17c0de)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(program.image())
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "kernel"),
                "-accel", "tcg,thread=single", "-S", "-display", "none",
                "-icount", f"shift={shift},align=off,sleep=off",
                "-serial", "null", "-serial", "null", "-serial", "null",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(3)

                def word(address):
                    raw = vm.cmd("human-monitor-command",
                                 command_line=f"xp /1wx {address:#x}")
                    return int(raw.split(":")[1].strip(), 16)

                vm.cmd("cont")
                deadline = time.monotonic() + host_timeout
                while word(0x80010014) != 0x5a17c0de:
                    if time.monotonic() >= deadline:
                        self.fail(vm.cmd("human-monitor-command",
                                         command_line="info registers"))
                    time.sleep(.005)
                vm.cmd("stop")
                values = [word(0x80010000 + index * 4) for index in range(10)]

        self.assertEqual(values[2], 6144000)
        counter_ticks = (values[3] - values[0]) & 0xffffffff
        slow_ticks = (values[4] - values[1]) & 0xffffffff
        self.assertGreaterEqual(slow_ticks, 3277)
        measured_hz = counter_ticks * 32768 / slow_ticks
        # Generic ARM timers use 162-ns ticks here, about 0.47% fast.
        self.assertAlmostEqual(measured_hz, 6144000, delta=6144000 * .01)
        self.assertAlmostEqual(values[6], 327.68, delta=4)
        self.assertEqual(values[8], 27, "WFI must wake on the virtual timer PPI")
        self.assertAlmostEqual(values[9], 327.68, delta=4)


if __name__ == "__main__":
    unittest.main()
