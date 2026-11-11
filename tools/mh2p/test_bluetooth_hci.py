# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import bluetooth_hci as hci


class BluetoothHciTests(unittest.TestCase):
    def test_invalid_duration_precedes_socket_access(self):
        for seconds in (0, -1, 3601, float("nan"), float("inf")):
            with self.subTest(seconds=seconds), self.assertRaises(ValueError):
                hci.relay("/missing-uart", "/missing-controller", seconds)

    def test_relay_preserves_both_directions_without_payload_logging(self):
        with tempfile.TemporaryDirectory() as temporary:
            uart_path = Path(temporary) / "uart"
            controller_path = Path(temporary) / "hci"
            with socket.socket(socket.AF_UNIX) as uart, \
                    socket.socket(socket.AF_UNIX) as controller:
                for endpoint, path in ((uart, uart_path), (controller, controller_path)):
                    endpoint.bind(str(path))
                    endpoint.listen(1)
                    endpoint.settimeout(3)
                result, errors = [], []

                def run_relay():
                    try:
                        result.append(hci.relay(uart_path, controller_path, 0.3))
                    except Exception as exc:
                        errors.append(exc)

                thread = threading.Thread(target=run_relay)
                thread.start()
                try:
                    with uart.accept()[0] as guest, controller.accept()[0] as peer:
                        guest.settimeout(3)
                        peer.settimeout(3)
                        request = bytes.fromhex("01030c00")
                        response = bytes.fromhex("040e0401030c00")
                        for byte in request:
                            guest.sendall(bytes([byte]))
                        received = bytearray()
                        while len(received) < len(request):
                            received.extend(peer.recv(len(request) - len(received)))
                        self.assertEqual(bytes(received), request)
                        peer.sendall(response)
                        received = bytearray()
                        while len(received) < len(response):
                            received.extend(guest.recv(len(response) - len(received)))
                        self.assertEqual(bytes(received), response)
                        thread.join(3)
                        self.assertFalse(thread.is_alive())
                finally:
                    thread.join(4)
                self.assertEqual(errors, [])
                self.assertEqual(result[0]["bytes"], {
                    "guest_to_controller": 4, "controller_to_guest": 7})
                self.assertFalse(result[0]["payload_logged"])
                self.assertFalse(result[0]["hmi_readiness_verified"])

    def test_closed_endpoint_is_an_explicit_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            uart_path = Path(temporary) / "uart"
            controller_path = Path(temporary) / "hci"
            with socket.socket(socket.AF_UNIX) as uart, \
                    socket.socket(socket.AF_UNIX) as controller:
                for endpoint, path in ((uart, uart_path), (controller, controller_path)):
                    endpoint.bind(str(path))
                    endpoint.listen(1)
                    endpoint.settimeout(3)
                errors = []

                def run_relay():
                    try:
                        hci.relay(uart_path, controller_path, 1)
                    except ConnectionError as exc:
                        errors.append(str(exc))

                thread = threading.Thread(target=run_relay)
                thread.start()
                try:
                    guest = uart.accept()[0]
                    with controller.accept()[0]:
                        guest.close()
                        thread.join(3)
                        self.assertFalse(thread.is_alive())
                finally:
                    thread.join(4)
                self.assertEqual(len(errors), 1)
                self.assertIn("endpoint closed", errors[0])

    def test_build_rejects_changed_revision_or_tracked_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for commit, changes in (("unexpected", ""), (hci.BLUEZ_COMMIT, "emulator/btdev.c\n")):
                with self.subTest(commit=commit, changes=changes), patch.object(
                        hci.subprocess, "run", side_effect=[
                            subprocess.CompletedProcess([], 0, stdout=commit),
                            subprocess.CompletedProcess([], 0, stdout=changes),
                        ]), self.assertRaises(ValueError):
                    hci.build(root, root / "output")
                self.assertFalse((root / "output").exists())

    def test_build_never_replaces_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(hci.subprocess, "run") as run:
                with self.assertRaises(FileExistsError):
                    hci.build(root, root)
                run.assert_not_called()


@unittest.skipUnless(os.environ.get("MH2P_BLUEZ_SOURCE"),
                     "Set MH2P_BLUEZ_SOURCE for the real upstream controller test")
class BluezControllerTests(unittest.TestCase):
    def test_real_controller_protocol_and_socket_lifecycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = hci.build(Path(os.environ["MH2P_BLUEZ_SOURCE"]), root / "build")
            binary = root / "build/mh2p-hci"
            self.assertEqual(report["bluez_commit"], hci.BLUEZ_COMMIT)
            self.assertFalse(report["original_broadcom_firmware_executed"])
            self.assertEqual(json.loads((root / "build/build.json").read_text()), report)
            existing = root / "existing"
            existing.write_text("preserve")
            rejected = subprocess.run(
                [str(binary), str(existing)], capture_output=True, timeout=3)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(existing.read_text(), "preserve")
            rejected = subprocess.run(
                [str(binary), "/" + "a" * 108], capture_output=True, timeout=3)
            self.assertNotEqual(rejected.returncode, 0)
            path = root / "hci.sock"
            with subprocess.Popen(
                    [str(binary), str(path)], stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE) as controller:
                try:
                    deadline = time.monotonic() + 3
                    while not path.exists():
                        if controller.poll() is not None:
                            self.fail(controller.communicate()[1].decode())
                        if time.monotonic() >= deadline:
                            self.fail("Controller socket did not become ready")
                        time.sleep(0.01)
                    self.assertEqual(path.stat().st_mode & 0o777, 0o700)
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.settimeout(3)
                        peer.connect(str(path))

                        def exchange(command):
                            peer.sendall(bytes.fromhex(command))
                            header = receive(peer, 3)
                            return header + receive(peer, header[2])

                        def receive(endpoint, size):
                            result = bytearray()
                            while len(result) < size:
                                data = endpoint.recv(size - len(result))
                                self.assertTrue(data, "Unexpected H4 disconnect")
                                result.extend(data)
                            return bytes(result)

                        self.assertEqual(exchange("01030c00").hex(),
                                         "040e0401030c00")
                        version = exchange("01011000")
                        self.assertEqual(version[:7].hex(), "040e0c01011000")
                        self.assertEqual(len(version), 15)
                        features = exchange("01031000")
                        self.assertEqual(features[:7].hex(), "040e0c01031000")
                        self.assertEqual(len(features), 15)
                        self.assertEqual(exchange("0118fc060000c0c62d00").hex(),
                                         "040f04010118fc")
                        with self.subTest("fragmented command and ACL headers"):
                            for byte in bytes.fromhex("01030c00"):
                                peer.sendall(bytes([byte]))
                                time.sleep(0.01)
                            self.assertEqual(receive(peer, 7).hex(),
                                             "040e0401030c00")
                            # An ACL packet for an absent link is discarded by
                            # BlueZ, but must not consume the next H4 command.
                            for byte in bytes.fromhex("0201000300aabbcc"):
                                peer.sendall(bytes([byte]))
                                time.sleep(0.01)
                            self.assertEqual(exchange("01030c00").hex(),
                                             "040e0401030c00")
                        with self.subTest("coalesced packets"):
                            peer.sendall(bytes.fromhex("01030c0001011000"))
                            self.assertEqual(receive(peer, 22),
                                             bytes.fromhex("040e0401030c00") +
                                             version)
                        with self.subTest("interleaved clients"):
                            with socket.socket(socket.AF_UNIX) as second:
                                second.settimeout(3)
                                second.connect(str(path))
                                peer.sendall(b"\x01")
                                second.sendall(bytes.fromhex("01030c00"))
                                self.assertEqual(receive(second, 7).hex(),
                                                 "040e0401030c00")
                                peer.sendall(bytes.fromhex("030c00"))
                                self.assertEqual(receive(peer, 7).hex(),
                                                 "040e0401030c00")
                        with self.subTest("output backpressure preserves bytes"):
                            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                            peer.settimeout(20)
                            errors = []
                            repetitions = 50000
                            responses = version + features

                            def flood():
                                try:
                                    peer.sendall(bytes.fromhex("0101100001031000") *
                                                 repetitions)
                                    peer.shutdown(socket.SHUT_WR)
                                except OSError as exc:
                                    errors.append(exc)

                            writer = threading.Thread(target=flood, daemon=True)
                            writer.start()
                            try:
                                time.sleep(0.1)
                                self.assertEqual(
                                    receive(peer, len(responses) * repetitions),
                                    responses * repetitions)
                                writer.join(3)
                                self.assertFalse(writer.is_alive())
                                self.assertEqual(errors, [])
                                self.assertEqual(peer.recv(1), b"")
                            finally:
                                if writer.is_alive():
                                    peer.shutdown(socket.SHUT_RDWR)
                                    writer.join(3)
                    for payload, diagnostic in (
                            (b"\x7f", "Unsupported H4 packet type"),
                            (bytes.fromhex("020100ffff"),
                             "H4 packet exceeds controller frame capacity")):
                        with self.subTest(diagnostic=diagnostic):
                            with socket.socket(socket.AF_UNIX) as peer:
                                peer.settimeout(3)
                                peer.connect(str(path))
                                peer.sendall(payload)
                                self.assertEqual(peer.recv(1), b"")
                    with self.subTest("reconnect after malformed input"):
                        with socket.socket(socket.AF_UNIX) as peer:
                            peer.settimeout(3)
                            peer.connect(str(path))
                            peer.sendall(bytes.fromhex("01030c00"))
                            self.assertEqual(receive(peer, 7).hex(),
                                             "040e0401030c00")
                    with self.subTest("fragmented ACL payload reaches real peer"):
                        with socket.socket(socket.AF_UNIX) as initiator, \
                                socket.socket(socket.AF_UNIX) as target:
                            for endpoint in (initiator, target):
                                endpoint.settimeout(3)
                                endpoint.connect(str(path))

                            def event(endpoint):
                                header = receive(endpoint, 3)
                                self.assertEqual(header[0], 4)
                                return header + receive(endpoint, header[2])

                            def command(endpoint, opcode, parameters=b""):
                                endpoint.sendall(
                                    b"\x01" + struct.pack("<HB", opcode,
                                                         len(parameters)) +
                                    parameters)
                                return event(endpoint)

                            source_address = command(initiator, 0x1009)[7:13]
                            target_address = command(target, 0x1009)[7:13]
                            self.assertNotEqual(source_address, target_address)
                            self.assertEqual(command(target, 0x0c1a, b"\x03"),
                                             bytes.fromhex("040e04011a0c00"))
                            status = command(
                                initiator, 0x0405,
                                target_address + bytes.fromhex("18cc0100000001"))
                            self.assertEqual(status, bytes.fromhex("040f0400010504"))
                            request = event(target)
                            self.assertEqual(request[:3], bytes.fromhex("04040a"))
                            self.assertEqual(request[3:9], source_address)
                            self.assertEqual(command(target, 0x0409,
                                                     source_address + b"\x01"),
                                             bytes.fromhex("040f0400010904"))
                            connection = event(initiator)
                            self.assertEqual(connection[:4],
                                             bytes.fromhex("04030b00"))
                            self.assertEqual(event(target)[:4],
                                             bytes.fromhex("04030b00"))
                            handle = struct.unpack_from("<H", connection, 4)[0]
                            payload = bytes.fromhex("0400010001020304")
                            packet = (b"\x02" +
                                      struct.pack("<HH", handle | 0x2000,
                                                  len(payload)) + payload)
                            for byte in packet:
                                initiator.sendall(bytes([byte]))
                                time.sleep(0.01)
                            self.assertEqual(receive(target, len(packet)), packet)
                            completed = event(initiator)
                            self.assertEqual(completed[:3],
                                             bytes.fromhex("041305"))
                            initiator.sendall(packet * 2 +
                                              bytes.fromhex("01011000"))
                            self.assertEqual(receive(target, len(packet) * 2),
                                             packet * 2)
                            for _ in range(2):
                                self.assertEqual(event(initiator)[:3],
                                                 bytes.fromhex("041305"))
                            self.assertEqual(event(initiator), version)
                            payload = bytes(range(256)) * 255 + bytes(range(250))
                            self.assertEqual(len(payload), 65530)
                            packet = (b"\x02" +
                                      struct.pack("<HH", handle | 0x2000,
                                                  len(payload)) + payload)
                            initiator.sendall(packet)
                            self.assertEqual(receive(target, len(packet)), packet)
                            self.assertEqual(event(initiator)[:3],
                                             bytes.fromhex("041305"))
                    with self.subTest("incomplete frame disconnect"):
                        with socket.socket(socket.AF_UNIX) as peer:
                            peer.settimeout(3)
                            peer.connect(str(path))
                            peer.sendall(bytes.fromhex("01030c"))
                            peer.shutdown(socket.SHUT_WR)
                            self.assertEqual(peer.recv(1), b"")
                    with self.subTest("half-close drains queued responses"):
                        with socket.socket(socket.AF_UNIX) as peer:
                            peer.settimeout(3)
                            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                            4096)
                            peer.connect(str(path))
                            peer.sendall(bytes.fromhex("0101100001031000") * 500)
                            peer.shutdown(socket.SHUT_WR)
                            time.sleep(0.1)
                            self.assertEqual(receive(peer, 15000),
                                             (version + features) * 500)
                            self.assertEqual(peer.recv(1), b"")
                    with self.subTest("real P-256 keys and ECDH"):
                        with socket.socket(socket.AF_UNIX) as alice, \
                                socket.socket(socket.AF_UNIX) as bob:
                            for endpoint in (alice, bob):
                                endpoint.settimeout(3)
                                endpoint.connect(str(path))
                                command(endpoint, 0x0c01, bytes.fromhex(
                                    "ffffffffffffff3f"))
                                command(endpoint, 0x2001, bytes.fromhex(
                                    "ff01000000000000"))
                            supported = command(alice, 0x1002)
                            self.assertEqual(supported[7 + 34] & 6, 6)
                            generator = bytes.fromhex(
                                "96c298d84539a1f4a033eb2d817d0377"
                                "f240a463e5e6bcf847422ce1f2d1176b"
                                "f551bf376840b6cbce5e316b5733ce2b"
                                "169e0f7c4aebe78e9b7f1afee242e34f")

                            def status(opcode, code=0):
                                return (bytes.fromhex("040f04") +
                                        bytes([code, 1]) +
                                        struct.pack("<H", opcode))

                            self.assertEqual(command(alice, 0x2026, generator),
                                             status(0x2026, 0x0c))
                            self.assertEqual(command(alice, 0x2025, b"\x00"),
                                             status(0x2025, 0x12))
                            keys = []
                            for endpoint in (alice, bob):
                                self.assertEqual(command(endpoint, 0x2025),
                                                 status(0x2025))
                                response = event(endpoint)
                                self.assertEqual(response[:5],
                                                 bytes.fromhex("043e420800"))
                                public = response[5:]
                                self.assertEqual(len(public), 64)
                                x = int.from_bytes(public[:32], "little")
                                y = int.from_bytes(public[32:], "little")
                                prime = 2 ** 256 - 2 ** 224 + 2 ** 192 + 2 ** 96 - 1
                                b = int("5ac635d8aa3a93e7b3ebbd55769886bc"
                                        "651d06b0cc53b0f63bce3c3e27d2604b", 16)
                                self.assertTrue(0 < x < prime and 0 < y < prime)
                                self.assertEqual((y * y - x ** 3 + 3 * x - b) %
                                                 prime, 0)
                                keys.append(public)
                            self.assertNotEqual(keys[0], keys[1])
                            self.assertEqual(command(alice, 0x0c01, bytes(7)),
                                             bytes.fromhex("040e0401010c12"))
                            self.assertEqual(command(alice, 0x2001, bytes(9)),
                                             bytes.fromhex("040e0401012012"))
                            # The remote generator has private scalar 1:
                            # ECDH with it must equal our own public X.
                            self.assertEqual(command(alice, 0x2026, generator),
                                             status(0x2026))
                            self.assertEqual(event(alice),
                                             bytes.fromhex("043e220900") +
                                             keys[0][:32])
                            secrets = []
                            for endpoint, remote in ((alice, keys[1]),
                                                     (bob, keys[0])):
                                self.assertEqual(command(endpoint, 0x2026, remote),
                                                 status(0x2026))
                                response = event(endpoint)
                                self.assertEqual(response[:5],
                                                 bytes.fromhex("043e220900"))
                                secrets.append(response[5:])
                            self.assertEqual(secrets[0], secrets[1])
                            self.assertNotEqual(secrets[0], bytes(32))
                            self.assertEqual(command(alice, 0x0c03, b"\x00"),
                                             bytes.fromhex("040e0401030c12"))
                            self.assertEqual(command(alice, 0x2026, generator),
                                             status(0x2026))
                            self.assertEqual(event(alice),
                                             bytes.fromhex("043e220900") +
                                             keys[0][:32])
                            self.assertEqual(command(alice, 0x2025),
                                             status(0x2025))
                            regenerated = event(alice)
                            self.assertEqual(regenerated[:5],
                                             bytes.fromhex("043e420800"))
                            self.assertNotEqual(regenerated[5:], keys[0])
                            for invalid in (b"", bytes(63), bytes(64),
                                            b"\xff" * 64, generator + b"\x00"):
                                self.assertEqual(command(alice, 0x2026, invalid),
                                                 status(0x2026, 0x12))
                            command(alice, 0x2001, bytes(8))
                            self.assertEqual(command(alice, 0x2025),
                                             status(0x2025))
                            self.assertEqual(command(alice, 0x1001), version)
                            command(alice, 0x2001, bytes.fromhex(
                                "ff01000000000000"))
                            command(alice, 0x0c01, bytes(8))
                            self.assertEqual(command(alice, 0x2025),
                                             status(0x2025))
                            self.assertEqual(command(alice, 0x1001), version)
                            command(alice, 0x0c03)
                            self.assertEqual(command(alice, 0x2026, generator),
                                             status(0x2026, 0x0c))
                            command(alice, 0x2001, bytes.fromhex(
                                "ff01000000000000"))
                            self.assertEqual(command(alice, 0x2025),
                                             status(0x2025))
                            self.assertEqual(command(alice, 0x1001), version)
                finally:
                    controller.terminate()
                    _, errors = controller.communicate(timeout=3)
                self.assertEqual(controller.returncode, 0)
                self.assertIn(b"Unsupported H4 packet type", errors)
                self.assertIn(b"H4 packet exceeds controller frame capacity", errors)
                self.assertIn(b"H4 connection closed with incomplete packet", errors)
                self.assertNotIn(b"runtime error:", errors)
                self.assertNotIn(b"ERROR: AddressSanitizer", errors)
                self.assertNotIn(b"ERROR: LeakSanitizer", errors)
                self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
