#!/usr/bin/env python3
"""Firmware-free, real-socket checks of the optional MMX/RCC sideband."""
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

from probe_rcc import QEMUMachine
from qemu.machine.qtest import QEMUQtestProtocol


MAGIC = 0x5348324D


def frame(hz=100000000, reset_n=1, magic=MAGIC, version=1):
    return struct.pack("<4I", magic, version, hz, reset_n)


def read(qtest, address):
    return int(qtest.cmd(f"readl {address:#x}").split()[1], 0)


def write(qtest, address, value):
    response = qtest.cmd(f"writel {address:#x} {value:#x}").strip()
    if response != "OK":
        raise RuntimeError(response)


def wait_value(qtest, address, expected):
    deadline = time.monotonic() + 5
    while True:
        value = read(qtest, address)
        if value == expected:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"{address:#x}: {value:#x} != {expected:#x}")
        time.sleep(0.005)


def configure_source(qtest):
    for address, value in (
        (0x7000e430, 0x103), (0x7000e434, 0x10),
        (0x60006330, 0x140), (0x60006314, 0x140),
        (0x600060e8, 0x4d00c801), (0x600060ec, 0x433c),
        (0x7009f040, 2), (0x7009f044, 0x70),
        (0x7009f134, 0x38), (0x7009f01c, 0),
        (0x010030c8, 0xd7acd8ac), (0x010030d0, 0x30),
        (0x01003960, 2), (0x01003910, 0x1b),
    ):
        write(qtest, address, value)


def configure_receiver(qtest):
    write(qtest, 0x4a0093b0, 0x702)
    write(qtest, 0x4a003c14, 0x20000)
    write(qtest, 0x4a00821c, 0x180)


@contextmanager
def machine(binary, directory, name, machine_type, backend):
    directory = Path(directory)
    image = directory / f"{name}.bin"
    payload = bytearray(0x110)
    struct.pack_into("<I", payload, 8, 0x00ff7eeb)
    struct.pack_into("<H", payload, 12, 1)
    struct.pack_into("<HH", payload, 16, 0x100, 40)
    struct.pack_into("<I", payload, 20, 0x80100100)
    struct.pack_into("<I", payload, 28, 0x80100008)
    struct.pack_into("<II", payload, 40, len(payload) - 8, len(payload) - 8)
    struct.pack_into("<I", payload, 0x100, 0xeafffffe)
    image.write_bytes(payload)
    path = str(directory / f"{name}-qtest.sock")
    qtest = QEMUQtestProtocol(path, server=True)
    args = [
        "-M", machine_type, "-S", "-kernel", str(image),
        "-display", "none", "-serial", "null", "-serial", "null",
        "-serial", "null", "-nic", "none",
        "-qtest", f"unix:{path}", "-qtest-log", "/dev/null",
        "-chardev", backend,
    ]
    try:
        qtest.settimeout(5)
        with QEMUMachine(binary, args=args) as vm:
            vm.launch()
            vm._qmp.settimeout(5)
            listener = qtest._sock
            qtest.accept()
            listener.close()
            qtest.settimeout(5)
            yield vm, qtest
    finally:
        qtest.close()


@unittest.skipUnless(os.environ.get("QTEST_QEMU_BINARY"),
                     "Set QTEST_QEMU_BINARY to run these hardware tests")
class SidebandTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mh2p-wire-test-")
        self.addCleanup(self.directory.cleanup)
        self.binary = str(Path(os.environ["QTEST_QEMU_BINARY"]).resolve())
        self.path = str(Path(self.directory.name) / "wire.sock")
        self.backend = (
            f"socket,id=mh2p-pcie-sideband,path={self.path},server=on,wait=off")

    def vm(self, name, kind, backend=None):
        return machine(self.binary, self.directory.name, name, kind,
                       backend or self.backend)

    def connect(self):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(peer.close)
        peer.settimeout(5)
        peer.connect(self.path)
        return peer

    def receive_frame(self, peer):
        data = bytearray()
        while len(data) < 16:
            part = peer.recv(16 - len(data))
            self.assertTrue(part, "sideband disconnected mid-frame")
            data.extend(part)
        return bytes(data)

    def test_fragmentation_reference_loss_and_receiver_reset(self):
        with self.vm("rcc", "mh2p-rcc") as (vm, qtest):
            peer = self.connect()
            configure_receiver(qtest)
            self.assertEqual(read(qtest, 0x4a008220), 0)
            peer.sendall(frame()[:7])
            self.assertEqual(read(qtest, 0x48051138), 0)
            self.assertEqual(read(qtest, 0x4a008220), 0)
            peer.sendall(frame()[7:])
            wait_value(qtest, 0x48051138, 1 << 27)
            write(qtest, 0x4a00821c, 0x181)
            wait_value(qtest, 0x4a008220, 1)
            write(qtest, 0x4a003c14, 0)
            self.assertEqual(read(qtest, 0x4a008220), 0)
            write(qtest, 0x4a003c14, 0x20000)
            self.assertEqual(read(qtest, 0x4a008220), 1)
            vm.cmd("system_reset")
            self.assertEqual(read(qtest, 0x48051138), 1 << 27)
            configure_receiver(qtest)
            write(qtest, 0x4a00821c, 0x181)
            self.assertEqual(read(qtest, 0x4a008220), 1)
            peer.sendall(frame(hz=0))
            wait_value(qtest, 0x4a008220, 0)
            self.assertEqual(read(qtest, 0x48051138), 1 << 27)
            peer.close()
            wait_value(qtest, 0x48051138, 0)
            peer = self.connect()
            peer.sendall(frame()[:8])
            peer.close()
            peer = self.connect()
            self.assertEqual(read(qtest, 0x4a008220), 0)
            peer.sendall(frame())
            wait_value(qtest, 0x48051138, 1 << 27)
            write(qtest, 0x4a00821c, 0x181)
            wait_value(qtest, 0x4a008220, 1)

    def test_invalid_frames_disconnect_and_clear_inputs(self):
        with self.vm("rcc", "mh2p-rcc") as (_, qtest):
            configure_receiver(qtest)
            for invalid in (frame(magic=0), frame(version=2),
                            frame(hz=99999999), frame(reset_n=2)):
                with self.subTest(invalid=invalid):
                    peer = self.connect()
                    peer.sendall(frame())
                    wait_value(qtest, 0x48051138, 1 << 27)
                    write(qtest, 0x4a00821c, 0x181)
                    wait_value(qtest, 0x4a008220, 1)
                    peer.sendall(invalid)
                    wait_value(qtest, 0x48051138, 0)
                    self.assertEqual(read(qtest, 0x4a008220), 0)
                    self.assertEqual(peer.recv(1), b"")
                    peer.close()

    def test_source_edges_prerequisites_and_reconnect_snapshot(self):
        with self.vm("mmx", "mh2p") as (vm, qtest):
            peer = self.connect()
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            configure_source(qtest)
            self.assertEqual(self.receive_frame(peer), frame())
            write(qtest, 0x01003910, 0x1a)
            write(qtest, 0x01003910, 0x1b)
            self.assertEqual(self.receive_frame(peer), frame(reset_n=0))
            self.assertEqual(self.receive_frame(peer), frame())
            for address, disabled, enabled in (
                (0x7009f044, 0, 0x70),
                (0x7009f134, 0, 0x38),
                (0x7009f01c, 0x07000000, 0),
                (0x7009f040, 9, 2),
                (0x600060ec, 0x633c, 0x433c),
                (0x600060e8, 0x4d00c802, 0x4d00c801),
                (0x600060e8, 0x4c00c801, 0x4d00c801),
                (0x6000648c, 0x10000000, 0),
                (0x6000648c, 4, 0),
                (0x01003960, 0x200, 2),
                (0x01003910, 0x13, 0x1b),
            ):
                write(qtest, address, disabled)
                self.assertEqual(self.receive_frame(peer), frame(0))
                write(qtest, address, enabled)
                self.assertEqual(self.receive_frame(peer), frame())
            write(qtest, 0x7000e5b8, 0x80000010)
            self.assertEqual(self.receive_frame(peer), frame(0))
            write(qtest, 0x7000e5b8, 0x40000010)
            self.assertEqual(self.receive_frame(peer), frame())
            write(qtest, 0x60006334, 0x100)
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            write(qtest, 0x60006330, 0x100)
            self.assertEqual(self.receive_frame(peer), frame())
            peer.close()
            peer = self.connect()
            self.assertEqual(self.receive_frame(peer), frame())
            write(qtest, 0x60006310, 0x100)
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            write(qtest, 0x60006314, 0x100)
            write(qtest, 0x01003960, 2)
            write(qtest, 0x01003910, 0x1b)
            self.assertEqual(self.receive_frame(peer), frame())
            write(qtest, 0x7000e430, 0x103)
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            write(qtest, 0x7000e430, 0x103)
            write(qtest, 0x7000e434, 0x10)
            write(qtest, 0x01003960, 2)
            write(qtest, 0x01003910, 0x1b)
            self.assertEqual(self.receive_frame(peer), frame())
            vm.cmd("system_reset")
            self.assertEqual(self.receive_frame(peer), frame(0, 0))

    def test_backpressure_overflow_disconnects_without_silent_edge_loss(self):
        with self.vm("mmx", "mh2p") as (vm, qtest):
            peer = self.connect()
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            configure_source(qtest)
            self.assertEqual(self.receive_frame(peer), frame())
            for edge in range(2048):
                write(qtest, 0x01003910, 0x1a | (edge & 1))
            data = bytearray()
            while True:
                part = peer.recv(65536)
                if not part:
                    break
                data.extend(part)
            self.assertGreater(len(data), 0)
            peer.close()
            peer = self.connect()
            self.assertEqual(self.receive_frame(peer), frame())
        self.assertIn("reset-edge queue exhausted", vm.get_log())

    def test_transmitter_rejects_reverse_data(self):
        with self.vm("mmx", "mh2p"):
            peer = self.connect()
            self.assertEqual(self.receive_frame(peer), frame(0, 0))
            peer.sendall(frame())
            self.assertEqual(peer.recv(1), b"")

    def test_two_machines_clock_reset_and_disconnect_without_link_up(self):
        with self.vm("mmx", "mh2p") as (mmx, source):
            client = f"socket,id=mh2p-pcie-sideband,path={self.path}"
            with self.vm("rcc", "mh2p-rcc", client) as (_, target):
                configure_receiver(target)
                self.assertEqual(read(target, 0x48051138), 0)
                self.assertEqual(read(target, 0x4a008220), 0)
                configure_source(source)
                wait_value(target, 0x48051138, 1 << 27)
                write(target, 0x4a00821c, 0x181)
                wait_value(target, 0x4a008220, 1)
                write(source, 0x01003910, 0x1a)
                wait_value(target, 0x48051138, 0)
                self.assertEqual(read(target, 0x4a008220), 1)
                write(source, 0x01003910, 0x1b)
                wait_value(target, 0x48051138, 1 << 27)
                self.assertEqual(read(source, 0x01001f00), 0)
                write(target, 0x4ae07310, 2)
                write(target, 0x51002100, 0)
                self.assertEqual(read(target, 0x5100210c), 0)
                mmx.cmd("quit")
                wait_value(target, 0x48051138, 0)
                wait_value(target, 0x4a008220, 0)


if __name__ == "__main__":
    unittest.main()
