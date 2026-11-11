# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free execution checks for the explicitly diagnostic IPU1 fixture."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine, ROOT


def a15_image():
    image = bytearray(0x110)
    struct.pack_into("<I", image, 8, 0x00ff7eeb)
    struct.pack_into("<H", image, 12, 1)
    struct.pack_into("<HH", image, 16, 0x100, 40)
    struct.pack_into("<I", image, 20, 0x80100100)
    struct.pack_into("<I", image, 28, 0x80100008)
    struct.pack_into("<II", image, 40, len(image) - 8, len(image) - 8)
    struct.pack_into("<I", image, 0x100, 0xeafffffe)
    return image


class ThumbProgram:
    def __init__(self):
        self.data = bytearray(16)
        struct.pack_into("<II", self.data, 0, 0x80001000, 0x11)
        self.literals = []

    def emit(self, instruction):
        self.data += struct.pack("<H", instruction)

    def load(self, register, value):
        self.literals.append((len(self.data), register, value))
        self.emit(0)

    def write(self, address, value):
        self.load(0, address)
        self.load(1, value)
        self.emit(0x6001)  # str r1, [r0]

    def copy(self, address, destination, byte=False):
        self.load(0, address)
        self.emit(0x7801 if byte else 0x6801)  # ldrb/ldr r1, [r0]
        self.load(0, destination)
        self.emit(0x6001)

    def elf(self):
        self.emit(0xe7fe)
        self.data += bytes(-len(self.data) % 4)
        for offset, register, value in self.literals:
            displacement = len(self.data) - ((offset + 4) & ~3)
            if not 0 <= displacement <= 1020:
                raise ValueError("Thumb literal pool out of range")
            struct.pack_into("<H", self.data, offset,
                             0x4800 | register << 8 | displacement // 4)
            self.data += struct.pack("<I", value)
        header = bytearray(0x100)
        header[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HHIIIIIHHHHHH", header, 16,
                         2, 40, 1, 0x11, 52, 0, 0, 52, 32, 1, 0, 0, 0)
        struct.pack_into("<IIIIIIII", header, 52,
                         1, 0x100, 0, 0, len(self.data), len(self.data), 7, 4)
        return header + self.data


class ArmProgram:
    def __init__(self):
        self.words = []
        self.literals = []

    def load(self, register, value):
        self.literals.append((len(self.words), register, value))
        self.words.append(0)

    def write(self, address, value):
        self.load(0, address)
        self.load(1, value)
        self.words.append(0xe5801000)

    def copy(self, address, destination):
        self.load(0, address)
        self.words.append(0xe5901000)
        self.load(0, destination)
        self.words.append(0xe5801000)

    def wait(self, address, value):
        self.load(0, address)
        self.load(2, value)
        self.words += [0xe5901000, 0xe1510002, 0x1afffffc]

    def image(self):
        self.words.append(0xeafffffe)
        for index, register, value in self.literals:
            displacement = (len(self.words) - index - 2) * 4
            if not 0 <= displacement <= 4095:
                raise ValueError("ARM literal pool out of range")
            self.words[index] = 0xe59f0000 | register << 12 | displacement
            self.words.append(value)
        image = a15_image()
        image[0x100:] = struct.pack(f"<{len(self.words)}I", *self.words)
        struct.pack_into("<II", image, 40, len(image) - 8, len(image) - 8)
        return image


class RccIPUTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not cls.qemu.is_file():
            raise unittest.SkipTest("Build qemu-system-arm first")

    def test_unimplemented_write_reports_issuing_arm_instruction_and_irq(self):
        host = ArmProgram()
        host.write(0x80101010, 0xeafffffe)
        host.load(0, 0x80101000)
        host.words.append(0xee0c0f10)  # mcr p15,0,r0,c12,c0,0 (VBAR)
        fault_pc = 0x80100100 + 4 * (len(host.words) + 2)
        host.write(0x48140a84, 0x40020)
        host.words.append(0xee391f70)  # L2ECTLR
        host.load(0, 0x80103000)
        host.words.append(0xe5801000)
        done_pc = 0x80100100 + 4 * len(host.words)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(host.image())
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "a15"),
                "-accel", "tcg,thread=single",
                "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
                "-d", "unimp", "-D", str(root / "unimplemented.log"),
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while True:
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    if f"R15={done_pc:08x}" in registers:
                        break
                    if time.monotonic() >= deadline:
                        self.fail(f"Expected AXI error without data abort: {registers}")
                    time.sleep(.01)
                vm.cmd("stop")
                memory = vm.cmd("human-monitor-command",
                                command_line="xp /1wx 0x80103000")
                self.assertIn("0x20000000", memory)
            self.assertIn(
                "unsupported write at 0x48140a84 size=4 value=0x40020"
                f" cpu=0 pc={fault_pc:#x}\n",
                (root / "unimplemented.log").read_text())

    def test_mcasp_quiescing_and_unsupported_releases(self):
        addresses = [0x48460044 + i * 0x4000 for i in range(8)]
        for enabled, value, size, succeeds in (
                (False, 0, 4, False), (True, 0, 4, True),
                (True, 1, 4, False), (True, 0x1f1f, 4, False),
                (True, 0, 2, False)):
            with self.subTest(enabled=enabled, value=value, size=size):
                host = ArmProgram()
                host.write(0x80101010, 0xeafffffe)
                host.load(0, 0x80101000)
                host.words.append(0xee0c0f10)
                for address in addresses:
                    host.load(0, address)
                    host.load(1, value)
                    host.words.append(0xe5801000 if size == 4 else 0xe1c010b0)
                    host.words.append(0xee392f70)  # L2ECTLR
                    host.load(0, 0x80103008)
                    host.words.append(0xe5802000)
                    host.copy(address, 0x80103000)
                host.write(0x80103004, 0x12345678)
                done_pc = 0x80100100 + 4 * len(host.words)
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    (root / "a15").write_bytes(host.image())
                    args = [
                        "-M", "mh2p-rcc,experimental-power-clocks="
                        + ("on" if enabled else "off"),
                        "-kernel", str(root / "a15"),
                        "-accel", "tcg,thread=single", "-S",
                        "-display", "none", "-nic", "none",
                        "-serial", "null", "-serial", "null", "-serial", "null",
                        "-d", "unimp", "-D", str(root / "unimplemented.log"),
                    ]
                    with QEMUMachine(str(self.qemu), args=args) as vm:
                        vm.launch()
                        vm._qmp.settimeout(5)
                        vm.cmd("cont")
                        expected = done_pc if enabled else 0x80101010
                        deadline = time.monotonic() + 5
                        while True:
                            registers = vm.cmd("human-monitor-command",
                                               command_line="info registers")
                            if f"R15={expected:08x}" in registers:
                                break
                            if time.monotonic() >= deadline:
                                self.fail(f"Unexpected McASP result: {registers}")
                            time.sleep(.01)
                        vm.cmd("stop")
                        memory = vm.cmd("human-monitor-command",
                                        command_line="xp /3wx 0x80103000")
                    self.assertIn("0x00000000", memory)
                    words = memory.split(":")[-1].split()
                    self.assertEqual(words[1], "0x12345678" if enabled
                                     else "0x00000000")
                    self.assertEqual(words[2], "0x00000000" if succeeds
                                     else "0x20000000")
                    diagnostics = (root / "unimplemented.log").read_text()
                    if succeeds:
                        self.assertNotIn("unsupported", diagnostics)
                    else:
                        self.assertIn("unsupported write at 0x48460044",
                                      diagnostics)

    def test_original_most_thumb_clock_store_does_not_data_abort(self):
        host = ArmProgram()
        host.write(0x80101010, 0xeafffffe)
        host.load(0, 0x80101000)
        host.words.append(0xee0c0f10)  # VBAR: stop if the store data-aborts.
        host.load(3, 0x4a009358)
        host.load(2, 2)
        host.load(1, 0x80103000)
        host.load(0, 0x80102001)
        host.words.append(0xe12fff10)  # bx r0
        image = host.image()
        image.extend(bytes(0x2000 - len(image)))
        image.extend(struct.pack("<4H",
                                 0x601a,  # Original MOST: str r2, [r3].
                                 0x681a,  # ldr r2, [r3]
                                 0x600a,  # str r2, [r1]
                                 0xe7fe))
        struct.pack_into("<II", image, 40, len(image) - 8, len(image) - 8)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(image)
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "a15"),
                "-accel", "tcg,thread=single",
                "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
                "-d", "unimp", "-D", str(root / "unimplemented.log"),
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while True:
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    self.assertNotIn("R15=80101010", registers)
                    if "R15=80102006" in registers:
                        break
                    if time.monotonic() >= deadline:
                        self.fail(f"Expected completed Thumb store: {registers}")
                    time.sleep(.01)
                vm.cmd("stop")
                result = vm.cmd("human-monitor-command",
                                command_line="xp /1wx 0x80103000")
                self.assertIn("0x00040002", result)
            self.assertNotIn("unsupported", (root / "unimplemented.log").read_text())

    def check_ipl_entry(self, first_stage):
        destination = 0x8cbf0000
        base = 0x40300000 if first_stage else 0x40340000
        ipl = bytearray(128 * 1024)
        if first_stage:
            struct.pack_into("<III", ipl, 0,
                             0xe59f8078, 0xe5880000, 0xea00003c)
            struct.pack_into("<I", ipl, 0x80, destination + 8)
            program = ArmProgram()
            program.copy(0x4ae0c204, destination + 4)
            program.load(0, destination + 12)
            program.words += [0xe5901000, 0xe2811001, 0xe5801000]
            program.write(destination, 0x13579bdf)
            payload = program.image()[0x100:]
            ipl[0x100:0x100 + len(payload)] = payload
        else:
            struct.pack_into("<II", ipl, 0, 0xfa001061, 0xeafffffd)
            program = ThumbProgram()
            program.copy(0x4ae0c204, destination + 4)
            program.load(0, destination + 12)
            program.emit(0x6801)
            program.emit(0x3101)
            program.emit(0x6001)
            program.write(destination, 0x13579bdf)
            payload = program.elf()[0x110:]
            ipl[0x418c:0x418c + len(payload)] = payload
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(a15_image())
            (root / "ipl").write_bytes(ipl)
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "a15"),
                "-bios", str(root / "ipl"), "-S", "-display", "none",
                "-nic", "none", "-serial", "null", "-serial", "null",
                "-serial", "null",
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    self.assertIn(f"R15={base:08x}", registers)
                    vm.cmd("cont")
                    deadline = time.monotonic() + 5
                    while True:
                        vm.cmd("human-monitor-command",
                               command_line=f'pmemsave {destination:#x} 16 '
                               f'"{root / "result"}"')
                        result = (root / "result").read_bytes()
                        if (struct.unpack_from("<II", result) ==
                                (0x13579bdf, 0x0b9bc02f) and
                                struct.unpack_from("<I", result, 12)[0] ==
                                boot + 1):
                            break
                        if time.monotonic() >= deadline:
                            self.fail(f"IPL did not execute: {result.hex()}")
                        time.sleep(.01)
                    vm.cmd("stop")
                    vm.cmd("human-monitor-command",
                           command_line=f'pmemsave {base:#x} 8 '
                           f'"{root / "loaded"}"')
                    self.assertEqual((root / "loaded").read_bytes(), ipl[:8])

    def test_original_first_stage_ipl_entry(self):
        self.check_ipl_entry(True)

    def test_original_second_stage_ipl_entry(self):
        self.check_ipl_entry(False)

    def test_reject_invalid_ipl(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(a15_image())
            for payload in (b"", b"\x78\x80\x9f\xe5", bytes(128 * 1024)):
                (root / "ipl").write_bytes(payload)
                result = subprocess.run([
                    str(self.qemu), "-M", "mh2p-rcc",
                    "-kernel", str(root / "a15"), "-bios", str(root / "ipl"),
                    "-display", "none", "-nic", "none",
                ], capture_output=True, timeout=5, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"unsupported RCC IPL size or entry",
                              result.stderr)
            (root / "ipl").unlink()
            result = subprocess.run([
                str(self.qemu), "-M", "mh2p-rcc",
                "-kernel", str(root / "a15"), "-bios", str(root / "ipl"),
                "-display", "none", "-nic", "none",
            ], capture_output=True, timeout=5, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"cannot read IPL", result.stderr)

    def wait_for_store(self, vm, destination, count, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = vm.cmd(
                "human-monitor-command",
                command_line=f'pmemsave 0x8cbf0000 0x90 "{destination}"')
            self.assertTrue(destination.is_file(), status)
            data = destination.read_bytes()
            self.assertEqual(len(data), 0x90)
            if (struct.unpack_from("<I", data, 28)[0] == 0x13579bdf and
                    struct.unpack_from("<I", data, 40)[0] == count):
                return data
            time.sleep(.01)
        vm.cmd("stop")
        registers = vm.cmd("human-monitor-command",
                           command_line="info registers", **{"cpu-index": 1})
        host = vm.cmd("human-monitor-command",
                      command_line="info registers", **{"cpu-index": 0})
        self.fail(f"M4 did not complete boot {count}: {data.hex()}\n"
                  f"{registers}\nA15:\n{host}")

    @staticmethod
    def mmu_static_entry(program, index, virtual, physical,
                         page=0, preserved=True):
        base = 0x55082000
        program.write(base + 0x50, index << 4)
        program.write(base + 0x58, virtual | 4 | page | (8 if preserved else 0))
        program.write(base + 0x5c, physical)
        program.write(base + 0x54, 1)

    def test_mmu_native_translation_all_page_sizes_and_reset(self):
        for walking in (False, True):
            for page in range(4):
                with self.subTest(walking=walking, page=page):
                    self.check_mmu_translation(walking, page)

    def test_native_a15_loads_local_ram_and_releases_each_m4(self):
        self.check_native_subsystem(1)

    def test_native_ipu2_executes_independently_and_restarts(self):
        self.check_native_subsystem(2)

    def check_native_subsystem(self, subsystem):
        public = 0x58800000 if subsystem == 1 else 0x55000000
        reset = 0x4ae06510 if subsystem == 1 else 0x4ae06910
        domain = 0x4a005500 if subsystem == 1 else 0x4a008900
        clock = 0x4a005520 if subsystem == 1 else 0x4a008920
        clock_enable = 0x1000001 if subsystem == 1 else 1
        m4 = ThumbProgram()
        struct.pack_into("<I", m4.data, 0, 0x20001000)
        m4.load(0, 0xe00fffe0)
        m4.emit(0x6801)  # ldr r1, [r0]
        m4.emit(0x0089)  # lsls r1, #2
        m4.load(0, 0x8cbf0000)
        m4.emit(0x1840)  # adds r0, r0, r1
        m4.emit(0x6801)
        m4.emit(0x3101)
        m4.emit(0x6001)
        m4.elf()
        host = ArmProgram()
        result = 0x8cbf0000
        if subsystem == 2:
            host.write(0x4a005500, 2)
            host.write(0x4a005520, 0x1000001)
            host.write(0x4ae06510, 3)
            host.write(0x58820000, 0x12345678)
            host.write(0x5888204c, 0x8cc04000)
        host.write(result, 0)
        host.write(result + 4, 0)
        host.write(result + 28, 0)
        host.copy(reset, result + 8)
        host.copy(reset + 4, result + 12)
        host.write(reset, 3)  # Clock is off: shared reset release is pending.
        host.copy(reset + 4, result + 16)
        host.write(domain, 2)
        host.write(clock, clock_enable)
        host.copy(reset + 4, result + 20)
        for offset in range(0, len(m4.data), 4):
            host.write(public + 0x20000 + offset,
                       struct.unpack_from("<I", m4.data, offset)[0])
        host.write(public + 0x809a0, 0x55020000)
        host.write(reset, 2)
        host.wait(result, 1)
        host.write(domain, 1)
        host.copy(clock, result + 32)
        host.write(domain, 0)
        host.copy(domain, result + 36)
        host.write(domain, 2)
        host.write(reset, 0)
        host.wait(result + 4, 1)
        host.write(reset + 4, 7)
        host.write(reset, 1)
        host.write(reset, 0)
        host.wait(result, 2)
        host.copy(reset + 4, result + 24)
        host.write(reset + 4, 1)
        if subsystem == 2:
            host.copy(0x58820000, result + 44)
            host.copy(0x5888204c, result + 48)
            host.copy(0x4ae06510, result + 52)
        host.load(0, result + 40)
        host.words += [0xe5901000, 0xe2811001, 0xe5801000]
        host.write(result + 28, 0x13579bdf)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(host.image())
            args = [
                "-M", "mh2p-rcc,experimental-ipu1-native=on" +
                (",experimental-ipu2-native=on" if subsystem == 2 else ""),
                "-accel", "tcg,thread=single", "-kernel", str(root / "a15"),
                "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "result", boot + 1)
                    vm.cmd("stop")
                    self.assertEqual(struct.unpack_from("<8I", data), (
                        2, 1, 7, 0, 0, 4, 1, 0x13579bdf))
                    self.assertEqual(struct.unpack_from("<I", data, 32)[0],
                                     clock_enable | 0x70000)
                    self.assertEqual(struct.unpack_from("<I", data, 36)[0], 0)
                    vm.cmd("human-monitor-command",
                           command_line=f'pmemsave {public + 0x20000:#x} '
                           f'{len(m4.data)} "{root / "local"}"')
                    self.assertEqual((root / "local").read_bytes(), m4.data)
                    if subsystem == 2:
                        self.assertEqual(struct.unpack_from("<3I", data, 44),
                                         (0x12345678, 0x8cc04000, 3))

    def check_mmu_translation(self, walking, page):
        program = ThumbProgram()
        mmu = 0x55082000
        result = 0x9bcf0000
        for index, (virtual, physical) in enumerate((
                (0, 0x8d000000), (0x80000000, 0x8d100000),
                (0x9bc00000, 0x8cb00000), (0x60000000, 0x8cc00000))):
            self.mmu_static_entry(program, index, virtual, physical)
        table = bytearray(0x4400)
        if page in (0, 3):
            offset = (0x70000000 >> 20) * 4
            descriptor = 2 | (1 << 18 if page == 3 else 0)
        else:
            struct.pack_into("<I", table, (0x70000000 >> 20) * 4, 0x8cc04001)
            offset = 0x4000
            descriptor = 2 if page == 2 else 1
        struct.pack_into("<I", table, offset, 0x8e000000 | descriptor)
        if not walking:
            self.mmu_static_entry(program, 4, 0x70000000, 0x8e000000,
                                  page=page, preserved=False)
        program.write(mmu + 0x4c, 0x8cc00000)
        program.write(mmu + 0x50, (4 << 10) | (4 << 4))
        program.write(mmu + 0x44, 6 if walking else 2)
        program.write(0x70000100, 0x12345678)
        program.copy(0x70000100, result)
        last = (0x100000, 0x10000, 0x1000, 0x1000000)[page] - 4
        program.write(0x70000000 + last, 0x24681357)
        program.copy(0x70000000 + last, result + 24)
        program.copy(mmu + 0x4c, result + 4)
        program.write(mmu + 0x50, (4 << 10) | (4 << 4))
        program.copy(mmu + 0x68, result + 8)
        program.copy(mmu + 0x6c, result + 12)
        if walking:
            program.write(0x60000000 + offset, 0x8f000000 | descriptor)
        program.write(mmu + 0x60, 1)
        program.copy(mmu + 0x68, result + 16)
        if not walking:
            self.mmu_static_entry(program, 4, 0x70000000, 0x8f000000,
                                  page=page, preserved=False)
        program.write(0x70000100, 0xabcdef12)
        program.copy(0x70000100, result + 20)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        self.run_mmu_fixture(program.elf(), table, (
            0x12345678, 0x8cc00000, 0x70000004 | page,
            0x8e000000, 0x70000000 | page, 0xabcdef12, 0x24681357), (
                (0x8e000100, 0x12345678), (0x8f000100, 0xabcdef12),
                (0x8e000000 + last, 0x24681357)))

    def test_mmu_bus_faults_multihit_irq_acknowledgement_and_reset(self):
        for fault in (1, 2, 16):
            with self.subTest(fault=fault):
                program = ThumbProgram()
                mmu = 0x55082000
                result = 0x9bcf0000
                for index, (virtual, physical) in enumerate((
                        (0, 0x8d000000), (0x80000000, 0x8d100000),
                        (0x9bc00000, 0x8cb00000))):
                    self.mmu_static_entry(program, index, virtual, physical)
                if fault == 16:
                    self.mmu_static_entry(program, 3, 0x71000000, 0x8e000000)
                    self.mmu_static_entry(program, 4, 0x71000000, 0x8f000000)
                program.write(mmu + 0x4c, 0x8cc00000)
                program.write(mmu + 0x88, 1)
                program.write(mmu + 0x1c, 0x1f)
                program.write(mmu + 0x44, 6 if fault == 2 else 2)
                program.copy(0x71000100, result)
                firmware = program.elf()
                handler = ThumbProgram()
                handler.copy(mmu + 0x18, result)
                handler.copy(mmu + 0x48, result + 4)
                handler.copy(mmu + 0x84, result + 8)
                handler.copy(0xe000e200, result + 12)
                handler.write(mmu + 0x88, 0)
                handler.copy(0xe000e200, result + 16)
                handler.write(mmu + 0x18, fault)
                handler.copy(mmu + 0x18, result + 20)
                handler.copy(0xe000ed28, result + 24)
                handler.load(0, result + 40)
                handler.emit(0x6801)
                handler.emit(0x3101)
                handler.emit(0x6001)
                handler.write(result + 28, 0x13579bdf)
                handler.elf()
                firmware += bytes(0x900 - len(firmware))
                firmware += handler.data[16:]
                struct.pack_into("<I", firmware, 0x10c, 0x801)
                struct.pack_into("<II", firmware, 52 + 16,
                                 len(firmware) - 0x100, len(firmware) - 0x100)
                self.run_mmu_fixture(firmware, bytes(0x4000), (
                    fault, 0x71000100, 9, 0, 1, 0, 0x8200, 0x13579bdf))

    def test_mmu_a15_services_stalled_m4_transactions_and_reset(self):
        for writing in (False, True):
            with self.subTest(writing=writing):
                self.check_mmu_stalled_transaction(writing)

    def check_mmu_stalled_transaction(self, writing):
        program = ThumbProgram()
        mmu = 0x55082000
        result = 0x9bcf0000
        for index, (virtual, physical) in enumerate((
                (0, 0x8d000000), (0x80000000, 0x8d100000),
                (0x9bc00000, 0x8cb00000), (0x70000000, 0x8e000000))):
            self.mmu_static_entry(program, index, virtual, physical)
        program.write(mmu + 0x1c, 1)
        program.write(mmu + 0x44, 2)
        program.write(0x70000100, 0xa5a5a5a5)
        program.write(result, 0x11223344)
        if writing:
            program.write(0x71000100, 0xdeadbeef)
            program.copy(0x70000100, result + 4)
        else:
            program.copy(0x71000100, result + 4)
        program.load(0, mmu + 0x18)
        program.emit(0x6801)
        program.emit(0x2900)
        program.emit(0xd1fc)
        program.copy(mmu + 0x18, result + 8)
        program.copy(0xe000ed28, result + 12)
        program.copy(mmu + 0x48, result + 16)
        program.copy(mmu + 0x84, result + 20)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)

        words = []
        literals = []

        def load(register, value):
            literals.append((len(words), register, value))
            words.append(0)

        load(0, 0x58882000)
        words += [0xe5901018, 0xe3110001, 0x0afffffc]
        load(2, 0x8e000100)
        words.append(0xe5923000)
        load(2, 0x8cbf0020)
        words.append(0xe5823000)
        for offset, value in (
                (0x50, 4 << 4), (0x58, 0x7100000c),
                (0x5c, 0x8e000000), (0x54, 1), (0x18, 1)):
            load(1, value)
            words.append(0xe5801000 | offset)
        words.append(0xeafffffe)
        for index, register, value in literals:
            displacement = (len(words) - index - 2) * 4
            words[index] = 0xe59f0000 | register << 12 | displacement
            words.append(value)
        host = a15_image()
        host[0x100:] = struct.pack(f"<{len(words)}I", *words)
        struct.pack_into("<II", host, 40, len(host) - 8, len(host) - 8)
        value = 0xdeadbeef if writing else 0xa5a5a5a5
        self.run_mmu_fixture(program.elf(), bytes(0x4000), (
            0x11223344, value, 0, 0, 0x71000100, 1 if writing else 9,
            0, 0x13579bdf, 0xa5a5a5a5),
            memory=((0x8e000100, value),), host_image=host)

    def run_mmu_fixture(self, firmware, table, expected, memory=(),
                        host_image=None, power_clocks=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a15").write_bytes(
                a15_image() if host_image is None else host_image)
            (root / "m4").write_bytes(firmware)
            (root / "native-text").write_bytes(firmware[0x100:])
            (root / "table").write_bytes(table)
            args = [
                "-M", f"mh2p-rcc,experimental-ipu1-firmware={root / 'm4'}",
                "-accel", "tcg,thread=single", "-kernel", str(root / "a15"),
                "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
                "-device", f"loader,file={root / 'native-text'},"
                "addr=0x8d000000,force-raw=on",
                "-device", f"loader,file={root / 'table'},"
                "addr=0x8cc00000,force-raw=on",
            ]
            if power_clocks:
                args[1] += ",experimental-power-clocks=on"
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "result", boot + 1)
                    vm.cmd("stop")
                    self.assertEqual(
                        struct.unpack_from(f"<{len(expected)}I", data),
                        expected)
                    for physical, value in memory:
                        word = root / "word"
                        vm.cmd(
                            "human-monitor-command",
                            command_line=f'pmemsave {physical} 4 "{word}"')
                        self.assertEqual(
                            struct.unpack("<I", word.read_bytes())[0], value)

    def test_power_task_dormant_clocks(self):
        registers = (
            (0x4a005234, 5), (0x4a0052a8, 5), (0x4a0052d8, 5),
            (0x4a008180, 5), (0x4a0051a0, 5), (0x4a0051e0, 5),
            (0x4a005550, 0x30000), (0x4a009860, 0x30000),
            (0x4a009868, 0x30000), (0x4a009898, 0x30000),
            (0x4a009878, 0x30000), (0x4a009904, 0x30000),
            (0x4a009908, 0x30000), (0x4a009890, 0x30000),
            (0x4a009838, 0x30000), (0x4a009220, 0x70000),
            (0x4a008f20, 0x70000), (0x4a008f28, 0x30000),
            (0x4a009120, 0x70000), (0x4a009130, 0x70000),
            (0x4a009020, 0x70000), (0x4a005764, 0x70000),
            (0x4a009388, 0x70000), (0x4a0093b8, 0x70000),
            (0x4a0093d0, 0x08070000),
        )
        for address, reset in registers:
            with self.subTest(address=hex(address)):
                program = ThumbProgram()
                result = 0x9bcf0000
                program.write(0x40000800, 0x48000000)
                program.write(0x40000820, 0x68000000)
                program.write(0x40000840, 3)
                program.copy(address, result)
                program.write(address, 5 if reset == 5 else 0)
                program.copy(address, result + 4)
                program.write(address, reset)
                program.copy(address, result + 8)
                program.load(0, result + 40)
                program.emit(0x6801)
                program.emit(0x3101)
                program.emit(0x6001)
                program.write(result + 28, 0x13579bdf)
                self.run_mmu_fixture(program.elf(), bytes(4), (
                    reset, 5 if reset == 5 else reset & 0x70000, reset),
                    power_clocks=True)

    def test_pcie_phy_output_clock_gates(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        base = 0x4a008200
        module = 0x4a0093b0
        program.write(0x40000800, 0x48000000)
        program.write(0x40000820, 0x68000000)
        program.write(0x40000840, 3)
        program.copy(base + 0x28, result)
        program.write(base + 0xc, 0x0602ee09)
        program.write(base + 0x10, 15)
        program.write(base, 7)
        program.write(module, 2)
        program.write(base + 0x1c, 0x101)
        program.copy(base + 0x20, result + 4)
        for index, control in enumerate((0x702, 0x302, 0x502, 0, 0x702)):
            program.write(module, control)
            program.copy(base + 0x28, result + 8 + index * 4)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        self.run_mmu_fixture(program.elf(), bytes(4), (
            0, 1, 0x600, 0x200, 0x400, 0, 0x600))

    def test_mpu_dpll_bypass_and_relock(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        base = 0x4a005160
        program.write(0x40000800, 0x48000000)
        program.write(0x40000820, 0x68000000)
        program.write(0x40000840, 3)
        program.copy(base, result)
        program.copy(base + 4, result + 4)
        program.copy(base + 8, result + 8)
        program.write(base, 5)
        program.copy(base, result + 12)
        program.copy(base + 4, result + 16)
        program.write(base, 6)
        program.copy(base + 4, result + 20)
        program.write(base, 7)
        program.copy(base + 4, result + 24)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        self.run_mmu_fixture(program.elf(), bytes(4), (
            7, 0x11, 0, 5, 0x1a, 0x1c, 0x11))

    def test_reserved_pad_mux_store_does_not_bus_fault(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        pad = 0x4a003744
        program.write(0x40000800, 0x48000000)
        program.write(0x40000820, 0x68000000)
        program.write(0x40000840, 3)
        program.copy(pad, result)
        program.write(pad, 0x0004000e)
        program.copy(pad, result + 4)
        program.write(pad, 0x0004000f)
        program.copy(pad, result + 8)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        self.run_mmu_fixture(program.elf(), bytes(4), (
            0x0004000f, 0x0004000e, 0x0004000f))

    def test_ammu_maintenance_selection_range_completion_and_reset(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        cache = 0x40000000
        program.copy(cache + 0xca8, result)
        program.copy(cache + 0xcac, result + 4)
        program.copy(cache + 0xcb0, result + 8)
        program.write(cache + 0xcac, 0x80000100)
        program.write(cache + 0xcb0, 0x800001ff)
        program.write(cache + 0xca8, 0x288)
        program.copy(cache + 0xca8, result + 12)
        program.copy(cache + 0xcb4, result + 16)
        program.copy(cache + 0xcac, result + 20)
        program.copy(cache + 0xcb0, result + 24)
        memory = []
        for index in range(10):
            program.write(cache + 0xaa0 + index * 4, 0xc)
            program.copy(cache + 0xaa0 + index * 4, result + 64 + index * 4)
            memory.append((0x8cbf0040 + index * 4, 0))
        program.write(cache + 0xca8, 0x400)
        program.copy(cache + 0xca8, result + 104)
        memory.append((0x8cbf0068, 0))
        program.write(cache + 0xca8, 0x198)
        program.copy(cache + 0xca8, result + 108)
        memory.append((0x8cbf006c, 0x180))
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        self.run_mmu_fixture(program.elf(), bytes(4), (
            0, 0, 0, 0x280, 0, 0x80000100, 0x800001ff), memory)

    def test_execution_ammu_cache_shared_aliases_and_reset(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        program.copy(0x40000a20, result)  # Reset small page 0 enabled.
        program.copy(0x400009a4, result + 4)  # Reset cache translation.
        program.write(0x40000010, 3)
        program.copy(0x40000010, result + 8)
        program.write(0x9bcf1000, 0x12345678)
        program.copy(0x608f1000, result + 12)
        program.write(0x40000800, 0x48000000)
        program.write(0x40000820, 0x68000000)
        program.write(0x40000840, 1)
        program.copy(0x48020020, result + 16, byte=True)
        program.write(0x40000840, 3)
        program.copy(0x4a005520, result + 20)  # L4 through 512 MiB page.
        program.copy(0x4a005500, result + 48)
        program.write(0x4a005500, 2)
        program.copy(0x4a005500, result + 52)
        program.write(0x40000928, 0x50000000)
        program.write(0x400009a8, 0x9bcf0000)
        program.write(0x40000a28, 1)
        program.write(0x50000080, 0xabcdef12)
        program.copy(0x9bcf0080, result + 24)
        program.write(0x40000860, 0x51000000)
        program.write(0x400008a0, 0x9bc00000)
        program.write(0x400008e0, 1)
        program.write(0x51001234, 0x31415926)
        program.copy(0x9bc01234, result + 32)
        program.write(0x40000804, 0x80000000)
        program.write(0x40000824, 0xffffffff)
        program.write(0x40000844, 3)
        program.write(0x80000080, 0x87654321)
        program.copy(0x80000080, result + 44)
        program.copy(0x800, result + 36)
        program.write(0x800, 0x99999999)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)  # adds r1, #1
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        firmware = program.elf()
        firmware += bytes(0x904 - len(firmware))
        struct.pack_into("<I", firmware, 0x900, 0xfeed1234)
        struct.pack_into("<II", firmware, 52 + 16, 0x804, 0x804)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "a15.bin"
            ipu = root / "m4.elf"
            image.write_bytes(a15_image())
            ipu.write_bytes(firmware)
            args = [
                "-M", f"mh2p-rcc,experimental-ipu1-firmware={ipu}",
                "-accel", "tcg,thread=single",
                "-kernel", str(image), "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "result.bin", boot + 1)
                    vm.cmd("stop")
                    self.assertEqual(struct.unpack_from("<10I", data), (
                        1, 0x55080000, 0, 0x12345678, 7, 1, 0xabcdef12,
                        0x13579bdf, 0x31415926, 0xfeed1234))
                    self.assertEqual(struct.unpack_from("<I", data, 44)[0],
                                     0x87654321)
                    self.assertEqual(struct.unpack_from("<2I", data, 48),
                                     (0x103, 0x102))
                    response = vm.cmd(
                        "human-monitor-command",
                        command_line=f'pmemsave 0x8cbff008 1 "{root / "token"}"')
                    self.assertEqual((root / "token").read_bytes(), b"\0",
                                     response)

    def test_sctm_native_counters_chaining_shadow_and_reset(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        program.copy(0x40000400, result)
        program.copy(0x40000508, result + 4)
        program.copy(0x40000588, result + 8)
        program.write(0x40000400, 1)
        program.write(0x400004f0, 4)
        program.copy(0x40000588, result + 12)
        program.load(0, 0x40000588)
        program.load(2, 100000)
        program.emit(0x6801)
        program.emit(0x4291)  # cmp r1, r2
        program.emit(0xd3fc)  # blo to counter read
        program.copy(0x40000588, result + 16)
        program.write(0x400004f0, 0)
        program.copy(0x40000588, result + 20)
        program.copy(0x40000588, result + 24)
        program.write(0x40000508, 4)
        program.write(0x4000050c, 4)
        program.write(0x400004f8, 0x4c)
        program.copy(0x40000588, result + 32)
        program.write(0x400004f0, 0x44)
        program.copy(0x400004f0, result + 76)
        program.load(0, 0x4000058c)
        program.emit(0x6801)
        program.emit(0x2900)  # cmp r1, #0
        program.emit(0xd0fc)  # Wait for a real 32-bit low-counter rollover.
        program.copy(0x4000058c, result + 36)
        program.copy(0x40000588, result + 44)
        program.load(2, 1000)
        program.emit(0x3a01)
        program.emit(0xd1fd)
        program.copy(0x40000588, result + 48)
        program.copy(0x4000058c, result + 52)
        program.copy(0x40000588, result + 56)
        program.copy(0x40000508, result + 60)
        program.copy(0x4000050c, result + 64)
        program.copy(0x40000518, result + 84)
        program.copy(0x40000518, result + 88)
        program.write(0x400004f0, 0)
        program.write(0x400004f8, 4)
        program.copy(0x4000058c, result + 72)
        program.copy(0x40000588, result + 68)
        program.write(0x40000400, 4)
        program.copy(0x40000598, result + 92)
        program.copy(0x40000400, result + 96)
        program.write(0x40000400, 1)
        program.write(0x40000510, 6)
        program.write(0x40000514, 4)
        program.write(0x40000510, 5)
        program.copy(0x40000594, result + 100)
        program.copy(0x40000590, result + 104)
        program.write(0x40000510, 6)
        program.copy(0x40000594, result + 108)
        program.copy(0x40000590, result + 112)
        program.copy(0x40000510, result + 116)
        program.write(0x40000518, 9)
        program.copy(0x40000598, result + 120)
        program.write(0x40000518, 0)
        program.copy(0x40000598, result + 124)
        program.copy(0x40000598, result + 128)
        program.copy(0x40000518, result + 132)
        program.write(0x40000400, 4)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image, firmware = root / "a15.bin", root / "m4.elf"
            image.write_bytes(a15_image())
            firmware.write_bytes(program.elf())
            args = [
                "-M", f"mh2p-rcc,experimental-ipu1-firmware={firmware}",
                "-icount", "shift=10,sleep=off",
                "-kernel", str(image), "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "counters.bin",
                                               boot + 1, timeout=15)
                    vm.cmd("stop")
                    values = struct.unpack("<36I", data)
                    self.assertEqual(values[:3], (0x4404, 0x80, 0))
                    self.assertGreater(values[3], 0)
                    self.assertGreaterEqual(values[4], 100000)
                    self.assertEqual(values[5], values[6])
                    self.assertEqual(values[8:10], (0, 1))
                    self.assertEqual(values[11], values[12])
                    self.assertEqual(values[13], 1)
                    self.assertGreater(values[14], values[12])
                    self.assertEqual(values[15:20], (0x85, 4, 0, 0, 0x44))
                    self.assertEqual(values[21:25], (0x41, 1, 0, 0x4404))
                    self.assertEqual(values[25], 0)
                    self.assertGreater(values[26], 0)
                    self.assertEqual(values[27:30], (0, 0, 0x84))
                    self.assertGreater(values[30], 0)
                    self.assertEqual(values[31], values[32])
                    self.assertEqual(values[33], 0)

    def test_dual_core_identity_shared_memory_intercore_irq_and_reset(self):
        program = ThumbProgram()
        result = 0x9bcf0000
        program.load(0, 0xe00fffe0)
        program.emit(0x6801)
        program.emit(0x2900)
        branch = len(program.data)
        program.emit(0)

        def wait(address, value):
            program.load(0, address)
            program.load(2, value)
            program.emit(0x6801)
            program.emit(0x4291)
            program.emit(0xd1fc)

        program.copy(0xe00fffe0, result)
        program.copy(0x40001000, result + 20)
        program.copy(0x40001010, result + 24)
        program.write(result + 8, 0)
        program.write(result + 12, 0)
        program.write(result + 16, 0)
        program.write(result + 44, 0)
        program.write(result + 48, 0)
        program.write(0x20000100 + 19 * 4, 0x901)
        program.write(0x20000100 + 23 * 4, 0xb01)
        program.write(0xe000ed08, 0x20000100)
        program.write(0xe000e100, 0x88)
        program.write(0x40001010, 0xaabbccdd)
        program.write(0xc, 1)
        wait(result + 16, 1)
        program.load(0, 0x40001002)
        program.load(1, 1)
        program.emit(0x8001)  # strh r1, [r0]: interrupt the peer via upper half.
        wait(result + 8, 0xc0)
        program.write(0x6a0027e0, 90)
        program.write(0x6ae2002c, 2)
        program.write(0x6ae2003c, 0xfffffffc)
        program.write(0x6ae20038, 1)
        wait(result + 44, 1)
        wait(result + 48, 1)
        program.copy(0x40001000, result + 32)
        program.copy(0x40001010, result + 36)
        program.load(0, result + 40)
        program.emit(0x6801)
        program.emit(0x3101)
        program.emit(0x6001)
        program.write(result + 28, 0x13579bdf)
        program.emit(0xe7fe)

        displacement = (len(program.data) - branch - 4) // 2
        self.assertLessEqual(displacement, 127)
        struct.pack_into("<H", program.data, branch, 0xd100 | displacement)
        program.load(0, 0x80002000)
        program.emit(0x4685)  # mov sp, r0: per-core stack.
        wait(0xc, 1)
        program.copy(0xe00fffe0, result + 4)
        program.write(0x20000200 + 19 * 4, 0xa01)
        program.write(0x20000200 + 23 * 4, 0xb01)
        program.write(0xe000ed08, 0x20000200)
        program.write(0xe000e100, 0x88)
        program.write(result + 16, 1)
        program.emit(0xbf30)  # wfi
        program.emit(0xe7fd)
        firmware = program.elf()
        self.assertLess(len(firmware), 0xa00)
        firmware += bytes(0xd00 - len(firmware))
        for index in range(2):
            handler = ThumbProgram()
            handler.write(0x40001000, 0)
            handler.write(result + 8 + index * 4, 0xc0 + index)
            if index:
                handler.load(0, 0x40001000)
                handler.load(1, 1)
                handler.emit(0x7001)  # strb r1, [r0]: interrupt core zero.
            handler.emit(0x4770)
            payload = handler.elf()[0x110:]
            start = 0xa00 + index * 0x100
            firmware[start:start + len(payload)] = payload
        handler = ThumbProgram()
        handler.write(0x6ae20028, 2)
        handler.load(0, 0xe00fffe0)
        handler.emit(0x6800)
        handler.emit(0x0080)  # lsls r0, r0, #2
        handler.load(2, result + 44)
        handler.emit(0x1812)  # adds r2, r2, r0
        handler.emit(0x2101)
        handler.emit(0x6011)
        handler.emit(0x4770)
        payload = handler.elf()[0x110:]
        firmware[0xc00:0xc00 + len(payload)] = payload
        struct.pack_into("<II", firmware, 52 + 16, 0xc00, 0xc00)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image, ipu = root / "a15.bin", root / "m4.elf"
            image.write_bytes(a15_image())
            ipu.write_bytes(firmware)
            args = [
                "-M", f"mh2p-rcc,experimental-ipu1-firmware={ipu},"
                "experimental-ipu1-smp=on",
                "-icount", "shift=0",
                "-kernel", str(image), "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                self.assertEqual(len(vm.cmd("query-cpus-fast")), 3)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "smp.bin", boot + 1)
                    vm.cmd("stop")
                    self.assertEqual(struct.unpack_from("<11I", data), (
                        0, 1, 0xc0, 0xc1, 1, 0, 0, 0x13579bdf,
                        0, 0xaabbccdd, boot + 1))
                    self.assertEqual(struct.unpack_from("<2I", data, 44),
                                     (1, 1))

    def test_rejects_smp_without_ipu_firmware(self):
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "a15.bin"
            image.write_bytes(a15_image())
            result = subprocess.run([
                str(self.qemu), "-M", "mh2p-rcc,experimental-ipu1-smp=on",
                "-kernel", str(image), "-display", "none", "-nic", "none",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"requires experimental-ipu1-firmware", result.stderr)

    def test_rejects_invalid_elf_without_starting(self):
        valid = ThumbProgram().elf()
        cases = []
        for offset, value in ((4, 2), (5, 2)):
            changed = bytearray(valid)
            changed[offset] = value
            cases.append(changed)
        for offset, value in ((24, 0x20), (28, 0xfffffff0), (28, 0),
                              (52, 0), (52 + 8, 0x1000),
                              (52 + 12, 0x8cbff000),
                              (52 + 16, 0x1000000), (52 + 24, 6)):
            changed = bytearray(valid)
            struct.pack_into("<I", changed, offset, value)
            cases.append(changed)
        cases.append(valid[:60])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "a15.bin"
            firmware = root / "bad.elf"
            image.write_bytes(a15_image())
            for payload in cases:
                with self.subTest(payload=payload[:32].hex()):
                    firmware.write_bytes(payload)
                    result = subprocess.run([
                        str(self.qemu), "-M",
                        f"mh2p-rcc,experimental-ipu1-firmware={firmware}",
                        "-kernel", str(image), "-display", "none", "-nic",
                        "none", "-serial", "null", "-serial", "null",
                        "-serial", "null",
                    ], capture_output=True, timeout=5)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b"mh2p-ipu1:", result.stderr)
            firmware.write_bytes(valid)
            result = subprocess.run([
                str(self.qemu), "-M",
                f"mh2p-rcc,experimental-ipu1-firmware={firmware}",
                "-kernel", str(image), "-accel", "tcg,thread=multi",
                "-display", "none", "-nic", "none", "-serial", "null",
                "-serial", "null", "-serial", "null",
            ], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"diagnostic execution requires", result.stderr)

    def test_real_timer_interrupt_reaches_m4_after_crossbar_remap(self):
        self.run_timer_interrupt_fixture(34, 0x68034000, 0x6a009740)

    def test_native_ipu2_timer_interrupt_and_crossbar_reset(self):
        result = 0x9bcf0000
        program = ThumbProgram()
        program.copy(0x4a0028a4, result)
        program.write(0xe000ed08, 0x400)
        program.write(0x4a002854, 34)
        program.write(0x4a002890, 35 << 16)
        program.write(0xe000e100, 1 << 7)
        program.write(0x4a009740, 2)
        program.write(0x4803402c, 2)
        program.write(0x4803403c, 0xfffffffc)
        program.write(0x48034038, 1)
        self.run_interrupt_fixture(program, 0x48034028, native_ipu2=True)

    def test_fixed_timer12_interrupt_reaches_m4_after_crossbar_remap(self):
        self.run_timer_interrupt_fixture(90, 0x6ae20000)

    def test_real_i2c_nack_interrupt_reaches_m4_after_crossbar_remap(self):
        program = ThumbProgram()
        program.copy(0x6a00281c, 0x9bcf0000)
        program.write(0xe000ed08, 0x400)
        program.write(0x6a0027e0, 52)  # I2C2 -> exception 23.
        program.write(0xe000e100, 1 << 7)
        program.write(0x6a0097a8, 2)
        program.write(0x6807202c, 2)
        program.write(0x680720ac, 0x49)  # No slave is attached to this bus.
        program.write(0x68072098, 1)
        program.write(0x680720a4, 0x8603)
        self.run_interrupt_fixture(program, 0x68072028)

    def test_real_spi_receive_interrupt_reaches_m4_after_crossbar_remap(self):
        program = ThumbProgram()
        program.copy(0x6a00281c, 0x9bcf0000)
        program.write(0xe000ed08, 0x400)
        program.write(0x6a0027e0, 43)  # SPI4 -> exception 23.
        program.write(0xe000e100, 1 << 7)
        program.write(0x6a009808, 2)
        program.write(0x680ba128, 1)
        program.write(0x680ba12c, 0x1603c0)
        program.write(0x680ba134, 1)
        program.write(0x680ba11c, 4)
        program.write(0x680ba138, 0x9f)
        self.run_interrupt_fixture(
            program, 0x680ba118, acknowledge_value=4,
            devices=["-device", "w25q80,bus=spi4,cs=0"])

    def run_timer_interrupt_fixture(self, source, timer, clock=None):
        program = ThumbProgram()
        result = 0x9bcf0000
        program.copy(0x6a00281c, result)
        program.write(0xe000ed08, 0x400)
        program.write(0x6a0027e0, source)  # Timer -> exception 23.
        if source == 34:
            program.write(0x6a00281c, 35 << 16)  # Remove default TIMER3 route.
        program.write(0xe000e100, 1 << 7)
        if clock is not None:
            program.write(clock, 2)
        program.write(timer + 0x2c, 2)
        program.write(timer + 0x3c, 0xfffffffc)
        program.write(timer + 0x38, 1)
        self.run_interrupt_fixture(program, timer + 0x28)

    def run_interrupt_fixture(self, program, acknowledge, acknowledge_value=2,
                              devices=(), native_ipu2=False):
        result = 0x9bcf0000
        firmware = program.elf()
        firmware += bytes(0x1000 - len(firmware))
        struct.pack_into("<I", firmware, 0x100 + 0x400 + 23 * 4, 0x901)
        handler = ThumbProgram()
        handler.write(acknowledge, acknowledge_value)
        handler.load(0, result + 40)
        handler.emit(0x6801)
        handler.emit(0x3101)
        handler.emit(0x6001)
        handler.write(result + 28, 0x13579bdf)
        handler.emit(0x4770)  # bx lr: exception return, not a host signal.
        # Relocating the complete literal pool by 0x8f0 preserves PC offsets.
        handler_bytes = handler.elf()[0x110:]
        firmware[0xa00:0xa00 + len(handler_bytes)] = handler_bytes
        struct.pack_into("<II", firmware, 52 + 16, 0xf00, 0xf00)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            machine = f"mh2p-rcc,experimental-ipu1-firmware={root / 'm4.elf'}"
            image = a15_image()
            if native_ipu2:
                host = ArmProgram()
                host.write(0x8cbf001c, 0)
                host.write(0x4a008900, 2)
                host.write(0x4a008920, 1)
                host.write(0x4ae06910, 3)
                for index, (virtual, physical, page) in enumerate((
                        (0, 0x8d000000, 0),
                        (0x80000000, 0x8d100000, 0),
                        (0x9bc00000, 0x8cb00000, 0),
                        (0x48000000, 0x48000000, 3),
                        (0x4a000000, 0x4a000000, 3))):
                    self.mmu_static_entry(host, index, virtual, physical, page)
                host.write(0x55082044, 2)
                for offset in range(0, 0xf00, 4):
                    value = struct.unpack_from("<I", firmware, 0x100 + offset)[0]
                    if value:
                        host.write(0x8d000000 + offset, value)
                host.write(0x4ae06910, 2)
                image = host.image()
                machine = ("mh2p-rcc,experimental-ipu1-native=on,"
                           "experimental-ipu2-native=on")
            (root / "a15.bin").write_bytes(image)
            (root / "m4.elf").write_bytes(firmware)
            args = [
                "-M", machine,
                "-kernel", str(root / "a15.bin"), "-S",
                "-accel", "tcg,thread=single", "-icount", "shift=0",
                "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
                *devices,
            ]
            with QEMUMachine(str(self.qemu), args=args) as vm:
                vm.launch()
                vm._qmp.settimeout(5)
                for boot in range(2):
                    if boot:
                        vm.cmd("system_reset")
                        vm.event_wait("RESET", timeout=5)
                    vm.cmd("cont")
                    data = self.wait_for_store(vm, root / "irq.bin", boot + 1)
                    vm.cmd("stop")
                    self.assertEqual(struct.unpack_from("<I", data)[0],
                                     0x006c0053 if native_ipu2 else 0x00230022)


if __name__ == "__main__":
    unittest.main()
