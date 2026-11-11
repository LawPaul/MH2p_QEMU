# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual A15 execution checks for CPSR writes and cached execution flags."""
import os
from pathlib import Path
import struct
import socket
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT
from test_rcc_ipu import ArmProgram


class ArmCPSRTests(unittest.TestCase):
    def test_cached_indirect_dispatch_breakpoint_and_resume(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        program.load(0, 0)
        program.load(4, 0x81000000)
        program.load(5, 0x80010000)
        loop = len(program.words)
        program.words += [0xe12fff34, 0xe5850000]
        program.words.append(
            0xea000000 | ((loop - len(program.words) - 2) & 0xffffff))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(program.image()[0x100:])
            (root / "target").write_bytes(struct.pack(
                "<2I", 0xe2800001, 0xe12fff1e))  # ADD r0, #1; BX lr.
            args = [
                "-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
                "-S", "-display", "none", "-serial", "null", "-nic", "none",
                "-gdb", f"unix:{root / 'gdb.sock'},server=on,wait=off",
                "-device", f"loader,file={root / 'target'},"
                           "addr=0x81000000,force-raw=on",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)

                def word():
                    raw = vm.cmd("human-monitor-command",
                                 command_line="xp /1wx 0x80010000")
                    return int(raw.split(":")[1].strip(), 16)

                def wait_for(predicate):
                    deadline = time.monotonic() + 5
                    while not predicate():
                        if time.monotonic() >= deadline:
                            self.fail(vm.cmd("human-monitor-command",
                                             command_line="info registers"))
                        time.sleep(.01)

                vm.cmd("cont")
                wait_for(lambda: word() >= 8)
                vm.cmd("stop")
                with socket.socket(socket.AF_UNIX) as debugger:
                    debugger.settimeout(5)
                    debugger.connect(str(root / "gdb.sock"))
                    with debugger.makefile("rwb", buffering=0) as wire:
                        def packet(command):
                            data = command.encode("ascii")
                            wire.write(b"$" + data + b"#" +
                                       f"{sum(data) & 255:02x}".encode("ascii"))
                            self.assertEqual(wire.read(1), b"+")
                            self.assertEqual(wire.read(1), b"$")
                            reply = bytearray()
                            while True:
                                byte = wire.read(1)
                                if byte == b"#":
                                    break
                                self.assertTrue(byte, "Debugger connection closed")
                                reply.extend(byte)
                            self.assertEqual(int(wire.read(2), 16), sum(reply) & 255)
                            wire.write(b"+")
                            return bytes(reply)

                        for _ in range(2):
                            self.assertEqual(packet("Z0,81000004,4"), b"OK")
                            vm.cmd("cont")
                            wait_for(lambda: vm.cmd("query-status")["status"] == "debug")
                            registers = vm.cmd("human-monitor-command",
                                               command_line="info registers")
                            self.assertIn("R15=81000004", registers)
                            before = word()
                            self.assertEqual(packet("z0,81000004,4"), b"OK")
                            vm.cmd("cont")
                            wait_for(lambda: word() > before + 8)
                            vm.cmd("stop")

    def test_instruction_masks_modes_endianness_and_user_trap(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")

        program = ArmProgram()
        address = 0x80010000
        expected = []

        def capture(register, value, mask=0xffffffff):
            nonlocal address
            program.load(1, address)
            program.words.append(0xe5810000 | register << 12)
            expected.append((address, value, mask))
            address += 4

        def cpsr(value, fields=1):
            program.load(0, value)
            program.words.append(0xe120f000 | fields << 16)

        def capture_cpsr(value, mask=0xf80f03df):
            program.words.append(0xe10f0000)
            capture(0, value, mask)

        program.load(0, 0x80020000)
        program.words.append(0xee0c0f10)  # VBAR
        cpsr(0xd2)
        program.load(13, 0x8001e000)
        cpsr(0xd3)
        program.write(0x80010604, 0)
        program.write(0x50041000, 1)  # GICD_CTLR
        program.write(0x50042004, 0xff)  # GICC_PMR
        program.write(0x50042000, 1)  # GICC_CTLR
        for count in (1, 2):
            program.words.append(0xf10c0080)  # CPSID i
            program.write(0x50041f00, 0x02000001)  # SGI 1 to this core.
            program.load(1, 0x80010604)
            program.words.append(0xe5910000)
            capture(0, count - 1)
            program.words.append(0xf1080080)  # CPSIE i must deliver pending IRQ.
            program.wait(0x80010604, count)
        program.load(1, 0x80010604)
        program.words.append(0xe5910000)
        capture(0, 2)
        cpsr(0x13, 15)
        program.words.append(0xf10c01c0)  # CPSID aif
        capture_cpsr(0x1d3)
        program.words.append(0xf10801c0)  # CPSIE aif
        capture_cpsr(0x13)
        program.words.append(0xf10c0080)  # CPSID i
        capture_cpsr(0x93)
        program.words.append(0xf1080080)  # CPSIE i
        capture_cpsr(0x13)
        for value in (0x13, 0xf80f0013, 0x500a00d3):
            cpsr(value, 15)
            capture_cpsr(value)

        program.write(0x80011000, 0x01020304)
        program.load(1, 0x80011000)
        cpsr(0x213, 3)  # MSR CPSR_xc: enable big-endian data accesses.
        # Do not read the little-endian literal pool until E is cleared.
        program.words += [0xe5912000, 0xe10f3000, 0xe3a00013, 0xe123f000]
        capture(2, 0x04030201)
        capture(3, 0x213, 0x3df)

        program.load(13, 0x8001c000)
        program.load(8, 0x12345678)
        cpsr(0xd1)
        program.load(13, 0x8001d000)
        program.load(8, 0x87654321)
        cpsr(0xd3)
        capture(13, 0x8001c000)
        capture(8, 0x12345678)
        cpsr(0xd1)
        capture(13, 0x8001d000)
        capture(8, 0x87654321)
        cpsr(0x13)

        program.load(0, 0x80020000)
        program.words.append(0xee0c0f10)  # VBAR
        program.write(0x80010600, 0)
        cpsr(0x10)
        capture_cpsr(0x10, 0x1f)
        program.words.append(0xf10c0080)  # CPSID is a NOP in user mode.
        capture_cpsr(0x10, 0x9f)
        # Privileged CP15 access must trap in user mode. SVC then returns SYS.
        program.words += [0xee112f10, 0xef000000]
        capture_cpsr(0x1f, 0x1f)
        program.words.append(0xee110f10)  # SCTLR must be accessible in SYS.
        capture(0, 0, 1)
        program.write(0x80010200, 0x5a17c0de)

        vectors = bytearray(0x200)
        struct.pack_into("<II", vectors, 4, 0xea00003d, 0xea00007c)
        struct.pack_into("<I", vectors, 0x18, 0xea0000b8)
        undefined = ArmProgram()
        undefined.load(1, 0x80010600)
        undefined.words += [0xe5910000, 0xe2800001, 0xe5810000]
        undefined.words.append(0xe1b0f00e)  # MOVS pc, lr
        payload = undefined.image()[0x100:]
        vectors[0x100:0x100 + len(payload)] = payload
        # MRS SPSR; replace mode with SYS; MSR SPSR_c; MOVS pc, lr.
        vectors.extend(struct.pack(
            "<5I", 0xe14f0000, 0xe3c0001f, 0xe380001f,
            0xe161f000, 0xe1b0f00e))
        irq = ArmProgram()
        irq.words.append(0xe92d000f)  # Preserve interrupted r0-r3.
        irq.load(1, 0x50042000)
        irq.words.append(0xe591200c)  # GICC_IAR
        irq.load(3, 0x80010604)
        irq.words += [0xe5930000, 0xe2800001, 0xe5830000, 0xe5812010]
        irq.words += [0xe8bd000f, 0xe25ef004]  # Restore and exception-return.
        vectors.extend(bytes(0x300 - len(vectors)))
        vectors.extend(irq.image()[0x100:])
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

                def word(at):
                    result = vm.cmd("human-monitor-command",
                                    command_line=f"xp /1wx {at:#x}")
                    return int(result.split(":")[1].strip(), 16)

                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while word(0x80010200) != 0x5a17c0de:
                    if time.monotonic() >= deadline:
                        self.fail(vm.cmd("human-monitor-command",
                                         command_line="info registers"))
                    time.sleep(.01)
                vm.cmd("stop")
                for at, value, mask in expected:
                    self.assertEqual(word(at) & mask, value & mask, hex(at))
                self.assertEqual(word(0x80010600), 1)


if __name__ == "__main__":
    unittest.main()
