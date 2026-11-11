# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the shipped checksum algorithm against the system POSIX implementation."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


class CarplayChecksumTests(unittest.TestCase):
    def test_posix_vectors_buffer_boundaries_and_errors(self):
        source = Path(__file__).with_name("carplay_modkit_cksum.c")
        with tempfile.TemporaryDirectory(prefix="mh2p-cksum-test-") as temporary:
            binary = Path(temporary) / "cksum"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-ffreestanding", "-fno-builtin", "-DMH2P_HOST_TEST",
                            str(source), "-o", str(binary)], check=True)
            for size in (0, 1, 9, 255, 256, 16383, 16384, 16385, 1048576):
                data = (bytes(range(256)) * ((size + 255) // 256))[:size]
                with self.subTest(size=size):
                    expected = subprocess.run(["cksum"], input=data, capture_output=True,
                                              check=True).stdout
                    observed = subprocess.run([str(binary)], input=data, capture_output=True,
                                              check=True).stdout
                    self.assertEqual(observed, expected)
            descriptor = os.open(source.parent, os.O_RDONLY)
            try:
                result = subprocess.run([str(binary)], stdin=descriptor,
                                        capture_output=True)
            finally:
                os.close(descriptor)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"checksum input read failed", result.stderr)
            with open("/dev/full", "wb") as full:
                result = subprocess.run([str(binary)], input=b"123456789",
                                        stdout=full, stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"checksum output write failed", result.stderr)
