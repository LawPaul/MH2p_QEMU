# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import probe_wifi_reset as probe


class SocketFixture:
    def __init__(self, data):
        self.data = bytearray(data)
        self.sent = bytearray()

    def recv(self, count):
        result = bytes(self.data[:1])
        del self.data[:1]
        return result

    def sendall(self, data):
        self.sent.extend(data)


def packet(payload):
    return b"$" + payload + b"#" + f"{sum(payload) & 255:02x}".encode()


class WifiResetTests(unittest.TestCase):
    def test_fragmented_reply_and_request(self):
        sock = SocketFixture(b"+" + packet(b"OK"))
        self.assertEqual(probe.request(sock, "Hgp1.1"), "OK")
        self.assertEqual(sock.sent, packet(b"Hgp1.1") + b"+")

    def test_invalid_replies(self):
        for data in (b"", b"+", b"$OK", b"$OK#0", b"$OK#00",
                     b"-", b"!", b"$" + b"x" * 65537):
            with self.subTest(data=data[:16]):
                with self.assertRaises(RuntimeError):
                    probe.read_packet(SocketFixture(data))

    def test_rejected_command(self):
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            probe.require_ok(SocketFixture(packet(b"E01")), "Hgp1.1")

    def test_exact_native_state(self):
        source = bytes(range(64))
        registers = bytearray(64)
        registers[:4] = probe.CHIPCOMMON_BASE.to_bytes(4, "little")
        registers[60:64] = probe.FIRST_MMIO_PC.to_bytes(4, "little")
        vectors = source[32:64].hex()
        words = probe.verify_state(registers.hex(), vectors, source)
        self.assertEqual(words[0], probe.CHIPCOMMON_BASE)
        self.assertEqual(words[15], probe.FIRST_MMIO_PC)
        for offset in (0, 60):
            wrong = bytearray(registers)
            wrong[offset] ^= 1
            with self.assertRaises(RuntimeError):
                probe.verify_state(wrong.hex(), vectors, source)
        with self.assertRaisesRegex(RuntimeError, "vectors"):
            probe.verify_state(registers.hex(), bytes(32).hex(), source)
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            probe.verify_state(bytes(63).hex(), vectors, source)

    def test_identity_before_output_creation(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            image, output = root / "firmware", root / "result"
            source = bytes(range(64))
            image.write_bytes(source)
            with self.assertRaisesRegex(ValueError, "size"):
                probe.probe(root / "qemu", image, output)
            self.assertFalse(output.exists())
            with patch.object(probe, "FIRMWARE_SIZE", len(source)):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    probe.probe(root / "qemu", image, output)
                self.assertFalse(output.exists())
                with patch.object(probe, "FIRMWARE_SHA256",
                                  hashlib.sha256(source).hexdigest()):
                    self.assertEqual(probe.verify_firmware(image), source)
            self.assertEqual(image.read_bytes(), source)

    def test_output_preservation_and_timeout(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            missing = root / "missing"
            for timeout in (-1, 0, 31, float("nan")):
                with self.assertRaises(ValueError):
                    probe.probe(missing, missing, root / "new", timeout)
            with self.assertRaises(FileExistsError):
                probe.probe(missing, missing, root)
            self.assertFalse((root / "new").exists())


if __name__ == "__main__":
    unittest.main()
