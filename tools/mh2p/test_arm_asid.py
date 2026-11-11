# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute ASID switches and translation invalidations on an actual A15."""
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT
from test_rcc_ipu import ArmProgram


class ArmASIDTests(unittest.TestCase):
    def test_short_descriptor_code_switch_and_invalidation(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        expected = []

        def cp_write(instruction, value):
            program.load(0, value)
            program.words += [instruction, 0xf57ff06f]  # ISB

        def call(value, address=0x90000000):
            program.load(4, address)
            program.words.append(0xe12fff34)  # BLX r4
            program.load(1, 0x80010000 + len(expected) * 4)
            program.words.append(0xe5810000)
            expected.append(value)

        def select(asid):
            cp_write(0xee020f10, 0x80020000 if asid & 1 else 0x80024000)
            cp_write(0xee0d0f30, asid)  # CONTEXTIDR

        cp_write(0xee020f50, 0)  # TTBCR: short descriptors, TTBR0 only.
        cp_write(0xee030f10, 3)  # Domain 0 manager.
        select(1)
        cp_write(0xee080f17, 0)  # TLBIALL
        program.words += [0xee110f10, 0xe3800001, 0xee010f10, 0xf57ff06f]
        # Exceed the bank count, then revisit hot and evicted contexts.
        for asid in list(range(1, 21)) + [20, 19, 20, 1, 2, 1]:
            select(asid)
            call(0x11 if asid & 1 else 0x22)

        # Invalidate the shared virtual address in all ASID banks.
        program.write(0x80022400, 0x81220c02)
        program.words.append(0xf57ff04f)  # DSB
        cp_write(0xee080f77, 0x90000000)  # TLBIMVAA
        call(0x33)
        select(3)
        call(0x33)
        select(2)
        call(0x22)
        select(1)
        call(0x33)

        program.write(0x80022400, 0x81020c02)
        program.words.append(0xf57ff04f)
        cp_write(0xee080f17, 0)  # Full invalidation must expire inactive banks.
        call(0x11)
        select(3)
        call(0x11)
        # A Thumb-2 MOVW straddles the invalidated page in two cached banks.
        for asid in (1, 3, 1):
            select(asid)
            call(0x1111, 0x90000fff)
        program.write(0x80022400, 0x81220c02)
        program.words.append(0xf57ff04f)
        cp_write(0xee080f77, 0x90001000)
        call(0x2222, 0x90000fff)
        select(3)
        call(0x2222, 0x90000fff)
        program.write(0x80011000, 0x5a17c0de)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(program.image()[0x100:])
            args = [
                "-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
                "-S", "-display", "none", "-serial", "null", "-nic", "none",
            ]
            for name, physical in (("odd", 0x81000000), ("even", 0x81100000)):
                table = [i << 20 | 0xc02 for i in range(4096)]
                table[0x900] = physical | 0x20c02  # Non-global section.
                (root / name).write_bytes(struct.pack("<4096I", *table))
                address = 0x80020000 if name == "odd" else 0x80024000
                args += ["-device", f"loader,file={root / name},"
                         f"addr={address:#x},force-raw=on"]
            for value, address in ((0x11, 0x81000000), (0x22, 0x81100000),
                                   (0x33, 0x81200000)):
                path = root / f"code-{value}"
                path.write_bytes(struct.pack("<II", 0xe3a00000 | value,
                                             0xe12fff1e))
                args += ["-device", f"loader,file={path},"
                         f"addr={address:#x},force-raw=on"]
            for address, halfwords in (
                    (0x81000ffe, (0xf241, 0x1011, 0x4770)),
                    (0x81200ffe, (0xf242, 0x2022, 0x4770))):
                path = root / f"cross-page-{address:x}"
                path.write_bytes(struct.pack("<3H", *halfwords))
                args += ["-device", f"loader,file={path},"
                         f"addr={address:#x},force-raw=on"]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)

                def word(address):
                    result = vm.cmd("human-monitor-command",
                                    command_line=f"xp /1wx {address:#x}")
                    return int(result.split(":")[1].strip(), 16)

                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while word(0x80011000) != 0x5a17c0de:
                    if time.monotonic() >= deadline:
                        self.fail(vm.cmd("human-monitor-command",
                                         command_line="info registers"))
                    time.sleep(.01)
                vm.cmd("stop")
                actual = [word(0x80010000 + i * 4)
                          for i in range(len(expected))]
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
