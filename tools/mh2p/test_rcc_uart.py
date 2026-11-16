# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free RCC UART banking and FIFO checks."""
import os
from pathlib import Path
import socket
import tempfile
import unittest

from probe_rcc import QEMUMachine, ROOT
from qemu.machine.qtest import QEMUQtestProtocol
from test_rcc_ipu import a15_image


class RccUARTTests(unittest.TestCase):
    def test_receive_fifo_depth_and_trigger_levels(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        for enhanced, levels, capacity in (
            (False, (1, 4, 8, 14), 16),
            (True, (8, 16, 56, 60), 64),
        ):
            with self.subTest(enhanced=enhanced), \
                    tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "kernel").write_bytes(a15_image())
                args = [
                    "-M", "mh2p-rcc" + (
                        ",experimental-uart-efr=on" if enhanced else ""),
                    "-kernel", str(root / "kernel"),
                    "-S", "-display", "none",
                    "-serial", "null", "-serial", "null", "-serial", "null",
                    "-qtest", f"unix:{root / 'qtest'},server=on,wait=off",
                ]
                with QEMUMachine(str(qemu), args=args) as vm:
                    vm.launch()
                    qtest = QEMUQtestProtocol(str(root / "qtest"))
                    qtest.connect()
                    qtest.settimeout(3)
                    try:
                        def write(offset, value):
                            qtest.cmd(f"writel {0x48020000 + offset:#x} {value}")

                        def read(offset):
                            return int(qtest.cmd(
                                f"readl {0x48020000 + offset:#x}").split()[1], 0)

                        for reset in ("initial", "soft", "system"):
                            if reset == "soft":
                                write(0x54, 2)
                            elif reset == "system":
                                vm.cmd("system_reset")
                            write(0x20, 0)
                            write(0x0c, 3)
                            write(0x10, 0x10)
                            write(4, 1)
                            for index, threshold in enumerate(levels):
                                with self.subTest(reset=reset, threshold=threshold):
                                    write(8, (index << 6) | 7)
                                    for value in range(threshold - 1):
                                        write(0, value)
                                    self.assertEqual(read(8), 0xc1)
                                    write(0, threshold - 1)
                                    self.assertEqual(read(8), 0xc4)
                                    for value in range(threshold, capacity):
                                        write(0, value)
                                    self.assertFalse(read(0x14) & 2)
                                    write(0, 0xaa)
                                    self.assertTrue(read(0x14) & 2)
                                    self.assertEqual(
                                        [read(0) for _ in range(capacity)],
                                        list(range(capacity)))
                                    self.assertFalse(read(0x14) & 1)
                                    self.assertEqual(read(8), 0xc1)
                    finally:
                        qtest.close()

    def test_enhanced_feature_bank_does_not_alias_fifo_control(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(a15_image())
            args = [
                "-M", "mh2p-rcc,experimental-uart-efr=on",
                "-kernel", str(root / "kernel"),
                "-S", "-display", "none",
                "-serial", "null", "-serial", "null", "-serial", "null",
                "-qtest", f"unix:{root / 'qtest'},server=on,wait=off",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                self.assertTrue(vm.cmd(
                    "qom-get", path="/machine", property="experimental-uart-efr"))
                qtest = QEMUQtestProtocol(str(root / "qtest"))
                qtest.connect()
                qtest.settimeout(3)
                try:
                    def write(offset, value):
                        qtest.cmd(f"writel {0x48020000 + offset:#x} {value}")

                    def read(offset):
                        return int(qtest.cmd(
                            f"readl {0x48020000 + offset:#x}").split()[1], 0)

                    for reset in ("initial", "soft", "system"):
                        with self.subTest(reset=reset):
                            if reset == "soft":
                                write(0x54, 2)
                            elif reset == "system":
                                vm.cmd("system_reset")
                            write(0x0c, 3)
                            write(8, 1)
                            self.assertEqual(read(8), 0xc1)
                            write(0x0c, 0xbf)
                            self.assertEqual(read(8), 0)
                            write(8, 0x10)
                            self.assertEqual(read(8), 0x10)
                            write(0x0c, 3)
                            self.assertEqual(read(8), 0xc1)
                            write(8, 0)
                            write(0x0c, 0xbf)
                            self.assertEqual(read(8), 0x10)
                            qtest.cmd("writeb 0x48020008 0")
                            self.assertEqual(int(qtest.cmd(
                                "readb 0x48020008").split()[1], 0), 0)
                            write(8, 0x10)
                finally:
                    qtest.close()

    def test_transmit_fifo_holds_64_bytes_under_backpressure(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel").write_bytes(a15_image())
            args = [
                "-M", "mh2p-rcc", "-kernel", str(root / "kernel"),
                "-S", "-display", "none",
                "-serial", "null", "-serial", "null",
                "-chardev", f"socket,id=console,path={root / 'uart'},"
                            "server=on,wait=off",
                "-serial", "chardev:console",
                "-qtest", f"unix:{root / 'qtest'},server=on,wait=off",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                self.assertFalse(vm.cmd(
                    "qom-get", path="/machine", property="experimental-uart-efr"))
                qtest = QEMUQtestProtocol(str(root / "qtest"))
                qtest.connect()
                qtest.settimeout(3)
                try:
                    with socket.socket(socket.AF_UNIX) as console:
                        console.settimeout(3)
                        console.connect(str(root / "uart"))

                        def write(offset, value):
                            qtest.cmd(f"writel {0x48020000 + offset:#x} {value}")

                        def read(offset):
                            return int(qtest.cmd(
                                f"readl {0x48020000 + offset:#x}").split()[1], 0)

                        for after_reset in (False, True):
                            with self.subTest(after_reset=after_reset):
                                if after_reset:
                                    vm.cmd("system_reset")
                                write(0x20, 0)
                                write(0x0c, 3)
                                write(0x08, 7)
                                prefix = bytearray()
                                for _ in range(4096):
                                    write(0, 0x55)
                                    prefix.append(0x55)
                                    if not read(0x14) & 0x20:
                                        break
                                else:
                                    self.fail("UART backend never applied backpressure")
                                self.assertFalse(read(0x44) & 1)
                                payload = bytes(range(1, 64))
                                for value in payload[:-1]:
                                    write(0, value)
                                self.assertFalse(
                                    read(0x44) & 1,
                                    "TX FIFO became full before 64 bytes")
                                write(0, payload[-1])
                                self.assertTrue(read(0x44) & 1)
                                expected = bytes(prefix) + payload
                                actual = bytearray()
                                while len(actual) < len(expected):
                                    chunk = console.recv(65536)
                                    self.assertTrue(chunk, "Console closed during drain")
                                    actual.extend(chunk)
                                self.assertEqual(bytes(actual), expected)
                                self.assertFalse(read(0x44) & 1)
                                self.assertEqual(read(0x14) & 0x60, 0x60)
                finally:
                    qtest.close()
