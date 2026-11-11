#!/usr/bin/env python3
"""Compile and exercise Screen adapter publication without QNX or firmware."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ScreenWindowTests(unittest.TestCase):
    def test_host_bridge_surface_layout_transfers(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-host-surface-test-") as temporary:
            executable = Path(temporary) / "surface-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(Path(__file__).resolve().parents[2] / "include"),
                "-o", str(executable),
                str(Path(__file__).with_name("test_glbridge_surface.c")),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_cpu_fill_mapping_reference_and_fallback(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-cpu-fill-") as temporary:
            executable = Path(temporary) / "cpu-fill-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-fno-builtin",
                "-Wall", "-Wextra", "-Werror", "-o", str(executable),
                str(Path(__file__).with_name("test_screen_cpu_fill.c")), "-ldl",
            ], check=True)
            result = subprocess.run(
                [str(executable)], check=True, capture_output=True, text=True)
            samples = [
                line for line in result.stderr.splitlines()
                if line.startswith("mh2p-cpu: capture source ")
            ]
            self.assertEqual(len(samples), 4)
            for sample in samples:
                self.assertIn("memory=0 id=100 offset=0", sample)
                self.assertIn(
                    "bytes=256 nonzero_bytes=255 first_byte_region8=6 "
                    "fnv1a=18641b35", sample)
            self.assertIn("capture trace memory identity unavailable", result.stderr)

    def test_independent_context_lifecycle_and_state(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-egl-test-") as temporary:
            executable = Path(temporary) / "egl-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-o", str(executable),
                str(Path(__file__).with_name("test_screen_egl.c")),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_shared_bridge_setup_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-shared-bridge-") as temporary:
            executable = Path(temporary) / "shared-bridge-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-o", str(executable),
                str(Path(__file__).with_name("test_bridge_shared_setup.c")),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_bridge_connection_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-transport-test-") as temporary:
            executable = Path(temporary) / "transport-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-o", str(executable),
                str(Path(__file__).with_name("test_bridge_transport.c")),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_surface_layout_transfers(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-surface-test-") as temporary:
            executable = Path(temporary) / "surface-test"
            subprocess.run([
                "cc", "-std=gnu11", "-O2", "-ffreestanding", "-fno-builtin",
                "-Wall", "-Wextra", "-Werror",
                "-o", str(executable),
                str(Path(__file__).with_name("test_screen_surface.c")),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_native_image_ownership_and_transfers(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-image-test-") as temporary:
            executable = Path(temporary) / "image-test"
            subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-unused-function",
                "-o", str(executable),
                str(Path(__file__).with_name("test_screen_image.c")), "-ldl",
            ], check=True)
            subprocess.run([str(executable)], check=True)

    def test_publication_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="mh2p-screen-test-") as temporary:
            executable = Path(temporary) / "screen-test"
            subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-o", str(executable),
                str(Path(__file__).with_suffix(".c")), "-ldl",
            ], check=True)
            result = subprocess.run(
                [str(executable)], check=True, capture_output=True, text=True)
            start = "glbridge-screen: publication trace begin"
            end = "glbridge-screen: publication trace end"
            self.assertEqual(result.stderr.count(start), 2)
            self.assertEqual(result.stderr.count(end), 2)
            for block in result.stderr.split(start)[1:]:
                block = block.split(end)[0]
                self.assertIn("native rgba fnv1a=0xbf719f15", block)
                self.assertIn("memory id=0x00000077", block)
                self.assertEqual(block.count("nonzero pixels=0x00000004"), 4)
                for value in range(16):
                    count = int(value in (1, 5, 9, 13))
                    self.assertIn(
                        f"first-byte low nibble=0x{value:08x}\n"
                        f"glbridge-screen: pixels=0x{count:08x}", block)
            self.assertIn("publication trace native/Mesa mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
