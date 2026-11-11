#!/usr/bin/env python3
"""Firmware-free checks of two live QEMUs' PCI configuration transport."""
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

from test_pcie_sideband import (
    configure_receiver, configure_source, machine, read, wait_value, write,
)


@unittest.skipUnless(os.environ.get("QTEST_QEMU_BINARY"),
                     "Set QTEST_QEMU_BINARY to run these hardware tests")
class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mh2p-config-test-")
        self.addCleanup(self.directory.cleanup)
        self.binary = str(Path(os.environ["QTEST_QEMU_BINARY"]).resolve())
        self.sideband = str(Path(self.directory.name) / "wire.sock")
        self.raw_count = 0

    @contextmanager
    def pair(self, outbound=False):
        root, endpoint = socket.socketpair()
        back_root, back_endpoint = (
            socket.socketpair() if outbound else (None, None))
        try:
            root.set_inheritable(True)
            root_type = f"mh2p,pcie-fd={root.fileno()}"
            if outbound:
                back_root.set_inheritable(True)
                root_type += f",pcie-outbound-fd={back_root.fileno()}"
            with machine(
                self.binary, self.directory.name, "mmx",
                root_type,
                f"socket,id=mh2p-pcie-sideband,path={self.sideband},"
                "server=on,wait=off",
            ) as mmx:
                root.close()
                endpoint.set_inheritable(True)
                endpoint_type = f"mh2p-rcc,pcie-fd={endpoint.fileno()}"
                if outbound:
                    back_root.close()
                    back_endpoint.set_inheritable(True)
                    endpoint_type += (
                        f",pcie-outbound-fd={back_endpoint.fileno()}")
                with machine(
                    self.binary, self.directory.name, "rcc",
                    endpoint_type,
                    f"socket,id=mh2p-pcie-sideband,path={self.sideband}",
                ) as rcc:
                    endpoint.close()
                    if outbound:
                        back_endpoint.close()
                    try:
                        yield mmx, rcc
                    except AssertionError:
                        for vm, _ in (mmx, rcc):
                            print(Path(vm._qemu_log_path).read_text())
                        raise
        finally:
            root.close()
            endpoint.close()
            for connection in (back_root, back_endpoint):
                if connection is not None:
                    connection.close()

    def setup_outbound(self, mmx, rcc):
        self.setup_controllers(mmx, rcc)
        write(rcc, 0x51000900, 0)
        write(rcc, 0x5100090c, 0x08000000)
        write(rcc, 0x51000914, 0x08000fff)
        write(rcc, 0x51000918, 0x90000000)
        write(rcc, 0x51000908, 0x80000000)
        write(rcc, 0x51002104, 1)
        self.assertEqual(read(mmx, 0x02000000), 0xb801104c)
        write(mmx, 0x02000004, 6)

    def test_reverse_memory_uses_live_outbound_region_and_real_mmx_ram(self):
        with self.pair(outbound=True) as ((mmx_vm, mmx), (_, rcc)):
            self.setup_outbound(mmx, rcc)
            write(mmx, 0x90000004, 0x12345678)
            self.assertEqual(read(rcc, 0x28000004), 0x12345678)
            write(rcc, 0x28000ffc, 0x89abcdef)
            self.assertEqual(read(mmx, 0x90000ffc), 0x89abcdef)
            rcc.cmd("writeb 0x28000005 0xaa")
            rcc.cmd("writew 0x28000006 0xbeef")
            self.assertEqual(read(mmx, 0x90000004), 0xbeefaa78)
            # Disabled bus mastering, disabled/overlapping regions and bounds.
            write(mmx, 0x02000004, 2)
            write(rcc, 0x28000004, 0)
            self.assertEqual(read(mmx, 0x90000004), 0xbeefaa78)
            write(mmx, 0x02000004, 6)
            write(rcc, 0x51000908, 0)
            write(rcc, 0x28000004, 0)
            self.assertEqual(read(mmx, 0x90000004), 0xbeefaa78)
            write(rcc, 0x51000908, 0x80000000)
            write(rcc, 0x28001000, 0xdeadbeef)
            self.assertEqual(read(mmx, 0x90001000), 0)
            mmx_vm.cmd("quit")
            start = time.monotonic()
            read(rcc, 0x28000004)
            self.assertLess(time.monotonic() - start, 2)

    def test_reverse_msi_uses_native_window_vector_enable_and_w1c(self):
        with self.pair(outbound=True) as ((_, mmx), (_, rcc)):
            self.setup_outbound(mmx, rcc)
            write(mmx, 0x01003860, 1)
            write(mmx, 0x01003864, 0x900000)
            write(mmx, 0x01003868, 0x90000000)
            write(mmx, 0x010038b4, 0x101)
            write(rcc, 0x28000000, 33)
            self.assertEqual(read(mmx, 0x01003870), 2)
            self.assertEqual(read(mmx, 0x90000000), 0)
            self.assertEqual(read(mmx, 0x50041210) & 8, 0)
            write(mmx, 0x01003890, 2)
            self.assertEqual(read(mmx, 0x50041210) & 8, 8)
            write(mmx, 0x01003870, 2)
            self.assertEqual(read(mmx, 0x01003870), 0)
            write(mmx, 0x50041290, 8)
            self.assertEqual(read(mmx, 0x50041210) & 8, 0)

    def test_qdma_moves_real_packet_memory_both_directions(self):
        with self.pair(outbound=True) as ((_, mmx), (_, rcc)):
            self.setup_outbound(mmx, rcc)
            source, target = 0x80011000, 0x28000040
            base, param = 0x43300000, 0x43307800
            write(rcc, source, 0x12345678)
            write(rcc, base + 0x218, 0x381c)
            write(rcc, base + 0x108c, 0x40)
            for offset, value in enumerate((
                0x0013e00c, source, 0x40001, target,
                0x10001, 0xffff, 0, 1,
            )):
                write(rcc, param + offset * 4, value)
            self.assertEqual(read(mmx, 0x90000040), 0x12345678)
            self.assertEqual(read(rcc, base + 0x106c), 1 << 30)
            write(rcc, base + 0x1074, 1 << 30)
            write(mmx, 0x90000040, 0x89abcdef)
            write(rcc, param + 4, target)
            write(rcc, param + 12, source + 4)
            write(rcc, param + 28, 1)
            self.assertEqual(read(rcc, source + 4), 0x89abcdef)
            self.assertEqual(read(rcc, base + 0x106c), 1 << 30)

    def setup_controllers(self, mmx, rcc):
        configure_source(mmx)
        write(mmx, 0x010038ac, 1)
        write(mmx, 0x01001018, 0x030302)
        write(mmx, 0x01003818, 0x02000000)
        write(mmx, 0x01003830, 0xfe100300)
        write(mmx, 0x01003800, 1)
        configure_receiver(rcc)
        wait_value(rcc, 0x48051138, 1 << 27)
        write(rcc, 0x4a00821c, 0x181)
        write(rcc, 0x4a003c40, 0x0500c000)
        write(rcc, 0x4ae07310, 2)
        write(rcc, 0x51002100, 0)
        write(rcc, 0x51000000, 0xb801104c)
        write(rcc, 0x51000008, 0x07800000)
        for i, mask in enumerate((0x3fff, 0xfff, 0xffff, 0, 0, 0)):
            write(rcc, 0x51000010 + i * 4, 0)
            write(rcc, 0x51001010 + i * 4, mask)

    @contextmanager
    def raw_peer(self, kind, server=True):
        host, peer = socket.socketpair()
        self.raw_count += 1
        try:
            host.set_inheritable(True)
            peer.settimeout(5)
            with machine(
                self.binary, self.directory.name, f"raw-{self.raw_count}",
                f"{kind},pcie-fd={host.fileno()}",
                f"socket,id=mh2p-pcie-sideband,path={self.sideband}" +
                (",server=on,wait=off" if server else ""),
            ) as vm:
                host.close()
                yield vm, peer
        finally:
            host.close()
            peer.close()

    def receive_frame(self, peer):
        frame = bytearray()
        while len(frame) < 32:
            chunk = peer.recv(32 - len(frame))
            self.assertTrue(chunk)
            frame.extend(chunk)
        return frame

    def test_mailbox_fifo_boundaries_complete_without_bus_errors(self):
        with machine(
            self.binary, self.directory.name, "mailbox-source", "mh2p",
            f"socket,id=mh2p-pcie-sideband,path={self.sideband},"
            "server=on,wait=off",
        ) as (_, mmx), self.raw_peer("mh2p-rcc", server=False) as (
            (_, rcc), peer,
        ):
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51000900, 0x80000001)
            write(rcc, 0x51000908, 0xc0000100)
            write(rcc, 0x51000918, 0x4a0f4000)
            write(rcc, 0x51002104, 1)
            # BAR assignment uses the configuration interface, not local DBI.
            sequence = 0

            def access(op, address, value=0, status=0):
                nonlocal sequence
                sequence += 1
                peer.sendall(struct.pack(
                    "<8I", 0x4348324d, 1, op, sequence, 1,
                    address, 4, value))
                reply = struct.unpack("<8I", self.receive_frame(peer))
                self.assertEqual(
                    reply[:7], (0x4348324d, 1, op | 0x80000000,
                                sequence, status, address, 4))
                return reply[7]

            access(2, 0x14, 0x14000)
            access(2, 4, 2)
            for fifo in range(8):
                message = 0x14040 + fifo * 4
                count = 0x140c0 + fifo * 4
                full = 0x14080 + fifo * 4
                self.assertEqual(access(3, count), 0)
                for value in range(4):
                    access(4, message, (fifo << 16) | value)
                self.assertEqual(access(3, full), 1)
                access(4, message, 0xdeadbeef)
                self.assertEqual(access(3, count), 4)
                self.assertEqual(access(3, full), 1)
                for value in range(4):
                    self.assertEqual(
                        access(3, message), (fifo << 16) | value)
                self.assertEqual(access(3, message), 0)
                self.assertEqual(access(3, count), 0)
                access(4, message, 0x12345678)
                self.assertEqual(access(3, message), 0x12345678)
            access(3, 0x14014, status=2)
            access(4, 0x14014, status=2)
            for control in (0, 0x700, 2, 0x102, 0x202, 0x302, 0x402, 0x502):
                with self.subTest(clock_control=hex(control)):
                    access(4, 0x14040, 0x12345678)
                    write(rcc, 0x4a0093b0, control)
                    access(3, 0x14040, status=1)
                    access(4, 0x14040, 0xdeadbeef, status=1)
                    self.assertEqual(read(rcc, 0x4a0f40c0), 1)
                    write(rcc, 0x4a0093b0, 0x702)
                    self.assertEqual(access(3, 0x14040), 0x12345678)
                    self.assertEqual(access(3, 0x140c0), 0)
            write(rcc, 0x4a0093b0, 0x602)
            access(4, 0x14040, 0x87654321)
            self.assertEqual(access(3, 0x14040), 0x87654321)

    def test_live_identity_bar_sizing_assignment_and_readonly_fields(self):
        with self.pair() as ((_, mmx), (_, rcc)):
            self.setup_controllers(mmx, rcc)
            self.assertEqual(read(mmx, 0x02000000), 0xffffffff)
            self.assertEqual(read(mmx, 0x01001f00), 0)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            self.assertEqual(read(mmx, 0x01001090), 0x20110000)
            write(mmx, 0x01001f00, read(mmx, 0x01001f00) | 0x1bfc0000)
            self.assertEqual(read(mmx, 0x01001f00), 0x5bfc0000)
            write(mmx, 0x01003910, 0x1a)
            self.assertEqual(read(mmx, 0x01001f00), 0x1bfc0000)
            write(mmx, 0x01003910, 0x1b)
            write(mmx, 0x01001f00, 1 << 30)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            self.assertEqual(read(rcc, 0x5100210c), 1 << 16)
            self.assertEqual(read(mmx, 0x01000f00), 0)
            self.assertEqual(read(mmx, 0x02000000), 0xb801104c)
            self.assertEqual(read(mmx, 0x02000008), 0x07800000)
            self.assertEqual(
                int(mmx.cmd("readw 0x02000000").split()[1], 0), 0x104c)
            write(mmx, 0x02000000, 0)
            write(mmx, 0x02000008, 0)
            self.assertEqual(read(rcc, 0x51000000), 0xb801104c)
            self.assertEqual(read(rcc, 0x51000008), 0x07800000)
            for bar, mask in enumerate((0x3fff, 0xfff, 0xffff, 0, 0, 0)):
                address = 0x02000010 + bar * 4
                write(mmx, address, 0xffffffff)
                expected = (~mask & 0xfffffff0) if mask else 0
                self.assertEqual(read(mmx, address), expected)
                value = (0x20012345 & ~mask & 0xfffffff0) if mask else 0
                write(mmx, address, 0x20012345)
                self.assertEqual(read(mmx, address), value)
                self.assertEqual(read(rcc, 0x51000010 + bar * 4), value)
                self.assertEqual(read(rcc, 0x51001010 + bar * 4), mask)
            write(mmx, 0x02000004, 0xf9100006)
            self.assertEqual(read(rcc, 0x51000004), 0x00100006)
            mmx.cmd("writeb 0x02000004 0x02")
            self.assertEqual(read(mmx, 0x02000004), 0x00100002)
            # No endpoint on another function or bus, and no DBI2 exposure.
            self.assertEqual(read(mmx, 0x02000100), 0xffffffff)
            write(mmx, 0x01003830, 0xfe100400)
            self.assertEqual(read(mmx, 0x02000000), 0xffffffff)

    def test_clock_reset_and_ltssm_gate_the_live_endpoint(self):
        with self.pair() as ((mmx_vm, mmx), (rcc_vm, rcc)):
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            for address, off, on in (
                (0x51002104, 0, 1),
                (0x4a0093b0, 0, 0x702),
                (0x4a0093b0, 0x302, 0x702),
                (0x4a0093b0, 0x502, 0x702),
                (0x4a003c14, 0, 0x20000),
                (0x4a003c40, 0, 0x0500c000),
            ):
                with self.subTest(address=address):
                    write(rcc, address, off)
                    self.assertEqual(read(mmx, 0x01001f00), 0)
                    self.assertEqual(read(mmx, 0x02000000), 0xffffffff)
                    write(rcc, address, on)
                    self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            write(mmx, 0x01003910, 0x1a)
            self.assertEqual(read(mmx, 0x01001f00), 0)
            self.assertEqual(read(rcc, 0x5100210c), 0)
            write(mmx, 0x01003910, 0x1b)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            write(rcc, 0x4ae07310, 3)
            write(rcc, 0x4ae07310, 2)
            self.assertEqual(read(mmx, 0x01001f00), 0)
            self.assertEqual(read(mmx, 0x02000000), 0xffffffff)
            rcc_vm.cmd("system_reset")
            self.assertEqual(read(mmx, 0x01001f00), 0)
            mmx_vm.cmd("system_reset")
            configure_source(mmx)
            write(mmx, 0x010038ac, 1)
            self.assertEqual(read(mmx, 0x01001f00), 0)

    def test_live_capability_chain_and_partial_control_writes(self):
        with self.pair() as ((mmx_vm, mmx), (rcc_vm, rcc)):
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            for offset, expected in (
                (0x40, 0x5a035001), (0x44, 0), (0x50, 0x00807005),
                (0x70, 0x00020010), (0x74, 0x8701), (0x78, 0x2010),
                (0x7c, 0x00433c22), (0x80, 0x30110008),
                (0x94, 0x11), (0x98, 0), (0x9c, 6), (0xa0, 0x10001),
            ):
                self.assertEqual(read(mmx, 0x02000000 + offset), expected)
                self.assertEqual(read(rcc, 0x51000000 + offset), expected)
            for offset in (0x40, 0x70, 0x74, 0x7c, 0x94, 0x9c):
                before = read(mmx, 0x02000000 + offset)
                write(mmx, 0x02000000 + offset, 0xffffffff)
                self.assertEqual(read(mmx, 0x02000000 + offset), before)
            mmx.cmd("writeb 0x02000045 0x81")
            self.assertEqual(read(rcc, 0x51000044), 0x100)
            mmx.cmd("writew 0x02000052 0x81")
            self.assertEqual(read(rcc, 0x51000050), 0x00817005)
            write(mmx, 0x02000054, 0xfee00123)
            write(mmx, 0x02000058, 0x12345678)
            write(mmx, 0x0200005c, 0xabcdef01)
            self.assertEqual(read(rcc, 0x51000054), 0xfee00120)
            self.assertEqual(read(rcc, 0x51000058), 0x12345678)
            self.assertEqual(read(rcc, 0x5100005c), 0xef01)
            mmx.cmd("writew 0x02000078 0x3010")
            mmx.cmd("writew 0x0200007a 0xffff")
            self.assertEqual(read(rcc, 0x51000078), 0x3010)
            rcc_vm.cmd("system_reset")
            mmx_vm.cmd("system_reset")
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x02000050), 0x00807005)
            self.assertEqual(read(rcc, 0x51000054), 0)

    def test_peer_exit_does_not_leave_a_link_or_block_the_root(self):
        with self.pair() as ((mmx_vm, mmx), (rcc_vm, rcc)):
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            rcc_vm.cmd("quit")
            start = time.monotonic()
            self.assertEqual(read(mmx, 0x01001f00), 0)
            self.assertLess(time.monotonic() - start, 2)
            self.assertEqual(read(mmx, 0x02000000), 0)
            self.assertEqual(mmx_vm.cmd("query-status")["status"], "prelaunch")

    def test_live_bar_memory_uses_programmed_inbound_translation(self):
        with self.pair() as ((_, mmx), (_, rcc)):
            self.setup_controllers(mmx, rcc)
            write(rcc, 0x51000004, 0x00100002)
            write(rcc, 0x51000900, 0x80000003)
            write(rcc, 0x51000904, 0)
            write(rcc, 0x51000908, 0xc0000200)
            write(rcc, 0x51000918, 0x80010000)
            write(rcc, 0x51000900, 0x80000000)
            write(rcc, 0x51000904, 0)
            write(rcc, 0x51000908, 0xc0000000)
            write(rcc, 0x51000918, 0x51000000)
            write(rcc, 0x51000900, 0x80000001)
            write(rcc, 0x51000904, 0)
            write(rcc, 0x51000908, 0xc0000100)
            write(rcc, 0x51000918, 0x4a0f4000)
            write(rcc, 0x51002104, 1)
            self.assertEqual(read(mmx, 0x01001f00), 1 << 30)
            write(mmx, 0x02000010, 0x10000)
            write(mmx, 0x02000014, 0x14000)
            write(mmx, 0x02000018, 0)
            write(mmx, 0x01003820, 0x11200000)
            write(mmx, 0x01003838, 1)
            write(mmx, 0x01003808, 0x20)
            write(rcc, 0x80010040, 0xdeadbeef)
            self.assertEqual(read(mmx, 0x11200040), 0xdeadbeef)
            write(mmx, 0x11200044, 0x12345678)
            self.assertEqual(read(rcc, 0x80010044), 0x12345678)
            mmx.cmd("writeb 0x11200045 0xaa")
            mmx.cmd("writew 0x11200046 0xbeef")
            self.assertEqual(read(rcc, 0x80010044), 0xbeefaa78)
            self.assertEqual(
                int(mmx.cmd("readb 0x11200045").split()[1], 0), 0xaa)
            self.assertEqual(
                int(mmx.cmd("readw 0x11200046").split()[1], 0), 0xbeef)
            write(mmx, 0x1120fffc, 0x11223344)
            self.assertEqual(read(rcc, 0x8001fffc), 0x11223344)
            self.assertEqual(read(mmx, 0x11210000), 0xb801104c)
            write(mmx, 0x11210900, 0)
            write(mmx, 0x1121090c, 0x88000000)
            write(mmx, 0x11210914, 0x8fffffff)
            write(mmx, 0x11210918, 0x80000000)
            write(mmx, 0x11210908, 0x80000000)
            self.assertEqual(read(rcc, 0x5100090c), 0x88000000)
            self.assertEqual(read(rcc, 0x51000914), 0x8fffffff)
            self.assertEqual(read(rcc, 0x51000918), 0x80000000)
            self.assertEqual(read(rcc, 0x51000908), 0x80000000)
            write(mmx, 0x1121002c, 0xdeadbeef)
            self.assertEqual(read(rcc, 0x5100002c), 0xdeadbeef)
            self.assertEqual(read(mmx, 0x0200002c), 0xdeadbeef)
            write(mmx, 0x0200002c, 0)
            self.assertEqual(read(rcc, 0x5100002c), 0xdeadbeef)
            write(mmx, 0x11214040, 0x87654321)
            self.assertEqual(read(rcc, 0x4a0f40c0), 1)
            self.assertEqual(read(rcc, 0x4a0f4040), 0x87654321)
            write(rcc, 0x4a0f4044, 0x12345678)
            self.assertEqual(read(mmx, 0x11214044), 0x12345678)
            # No BAR2 memory beyond its mask, despite a larger AXI window.
            self.assertEqual(read(mmx, 0x11220000), 0)
            write(mmx, 0x02000004, 0)
            self.assertEqual(read(mmx, 0x11200040), 0)
            write(mmx, 0x02000004, 2)
            self.assertEqual(read(mmx, 0x11200040), 0xdeadbeef)

    def test_fragmented_requests_and_invalid_endpoint_frames(self):
        for invalid in (
            (0, 0), (4, 2), (8, 99), (16, 2),
            (20, 0x1000), (20, 3), (24, 3), (28, 1),
        ):
            with self.subTest(invalid=invalid), \
                 self.raw_peer("mh2p-rcc") as ((vm, qtest), peer):
                request = struct.pack("<8I", 0x4348324d, 1, 1, 7, 1, 0, 4, 0)
                peer.sendall(request[:3])
                self.assertEqual(read(qtest, 0x5100210c), 0)
                peer.sendall(request[3:])
                reply = self.receive_frame(peer)
                self.assertEqual(struct.unpack("<8I", reply),
                                 (0x4348324d, 1, 0x80000001,
                                  7, 1, 0, 4, 0xffffffff))
                request = bytearray(request)
                struct.pack_into("<I", request, *invalid)
                peer.sendall(request)
                self.assertEqual(peer.recv(1), b"")
                self.assertEqual(vm.cmd("query-status")["status"], "prelaunch")

    def test_root_rejects_malformed_and_stale_completions(self):
        for invalid in ((0, 0), (4, 2), (8, 0), (12, 99),
                        (16, 1), (16, 2), (16, 3),
                        (20, 4), (24, 4), (28, 2)):
            with self.subTest(invalid=invalid), \
                 self.raw_peer("mh2p") as ((vm, qtest), peer), \
                 ThreadPoolExecutor(max_workers=1) as executor:
                configure_source(qtest)
                write(qtest, 0x010038ac, 1)
                pending = executor.submit(read, qtest, 0x01001f00)
                reply = self.receive_frame(peer)
                struct.pack_into("<I", reply, 8, 0x80000000)
                struct.pack_into("<I", reply, 16, 0)
                struct.pack_into("<I", reply, 28, 1)
                struct.pack_into("<I", reply, *invalid)
                peer.sendall(reply[:5])
                peer.sendall(reply[5:])
                self.assertEqual(pending.result(5), 0)
                self.assertEqual(peer.recv(1), b"")
                self.assertEqual(vm.cmd("query-status")["status"], "prelaunch")

    def test_vcpu_fragmented_completion_keeps_qmp_responsive(self):
        with self.raw_peer("mh2p") as ((vm, qtest), peer):
            configure_source(qtest)
            write(qtest, 0x010038ac, 1)
            # ARM: load link status, save it to RAM, then spin.
            for offset, instruction in enumerate((
                0xe59f000c, 0xe5901000, 0xe59f2008, 0xe5821000,
                0xeafffffe, 0x01001f00, 0x80100200,
            )):
                write(qtest, 0x80008000 + offset * 4, instruction)
            vm.cmd("cont")
            reply = self.receive_frame(peer)
            struct.pack_into("<I", reply, 8, 0x80000000)
            struct.pack_into("<I", reply, 16, 0)
            struct.pack_into("<I", reply, 28, 1)
            peer.sendall(reply[:5])
            start = time.monotonic()
            self.assertEqual(vm.cmd("query-status")["status"], "running")
            self.assertLess(time.monotonic() - start, 0.5)
            peer.sendall(reply[5:])
            wait_value(qtest, 0x80100200, 1 << 30)
            vm.cmd("stop")

    def test_unresponsive_peer_has_bounded_timeout_and_no_false_link(self):
        with self.raw_peer("mh2p") as ((vm, qtest), peer):
            configure_source(qtest)
            write(qtest, 0x010038ac, 1)
            start = time.monotonic()
            self.assertEqual(read(qtest, 0x01001f00), 0)
            elapsed = time.monotonic() - start
            self.assertGreater(elapsed, 0.8)
            self.assertLess(elapsed, 2)
            self.assertEqual(read(qtest, 0x01001f00), 0)
            self.assertEqual(vm.cmd("query-status")["status"], "prelaunch")


if __name__ == "__main__":
    unittest.main()
