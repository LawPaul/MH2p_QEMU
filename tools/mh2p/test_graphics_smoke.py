#!/usr/bin/env python3
"""Synthetic graphics-media fixtures; no firmware or QNX SDK is needed."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import build_graphics_smoke
import prepare_graphics_smoke as smoke
from test_prepare import fixture


def checksum(data, start=0, end=None):
    end = len(data) if end is None else end
    total = sum(x[0] for x in struct.iter_unpack("<I", data[start:end - 4]))
    struct.pack_into("<I", data, end - 4, -total & 0xffffffff)


def elf(kind=2, paddr=0x12340000):
    data = bytearray(256)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", data, 16, kind, 40, 1)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<I", data, 36, 0x05000200)
    struct.pack_into("<HHH", data, 40, 52, 32, 2)
    struct.pack_into("<8I", data, 52, 1, 128, 0x9000, paddr, 64, 64, 5, 4096)
    struct.pack_into("<8I", data, 84, 1, 192, 0xa000, 0xa000, 64, 64, 6, 4096)
    data[128:] = bytes(range(128))
    return bytes(data)


def startup():
    image = bytearray(fixture())
    fs = bytearray(image[264:])
    # A bootstrap inode must not transfer its high-bit flags to new files.
    struct.pack_into("<I", fs, 96, 0xe0000001)
    fs[256:512] = elf(paddr=smoke.LOAD_ADDRESS + 264 + 256 + 128)
    checksum(fs)
    image[264:] = fs
    checksum(image, 8, 264)
    return bytes(image)


class GraphicsSmokeTests(unittest.TestCase):
    def test_growth_checksums_xip_and_runtime_inode(self):
        original = startup()
        payload = elf()
        result, offset = smoke.add_resources(original, [("sbin/smoke", payload)])
        self.assertEqual(offset, 264)
        for chunk in (result[8:offset], result[offset:]):
            self.assertEqual(sum(x[0] for x in struct.iter_unpack("<I", chunk))
                             & 0xffffffff, 0)
        self.assertEqual(struct.unpack_from("<I", result, 36)[0], len(result) - 8)
        self.assertEqual(struct.unpack_from("<I", result, 44)[0], len(result) - 8)
        self.assertEqual(struct.unpack_from("<I", result, 52)[0], len(result) - offset)
        oldfs, fs = original[offset:], result[offset:]
        old, size = smoke.file_entries(oldfs)["usr/sbin/main_stage1.sh"]
        new, _ = smoke.file_entries(fs)["usr/sbin/main_stage1.sh"]
        self.assertEqual(new - old, 4096)
        expected = bytearray(oldfs[old:old + size])
        struct.pack_into("<I", expected, 64, smoke.LOAD_ADDRESS + offset + new + 128)
        self.assertEqual(fs[new:new + size], expected)
        added, length = smoke.file_entries(fs)["sbin/smoke"]
        self.assertEqual(fs[added:added + length], payload)
        next_record = 92 + struct.unpack_from("<H", fs, 92)[0]
        self.assertEqual(struct.unpack_from("<I", fs, next_record + 4)[0], 2)

    def test_no_resources_preserves_every_byte(self):
        original = startup()
        self.assertEqual(smoke.add_resources(original, [])[0], original)

    def test_duplicate_and_exhausted_inode(self):
        with self.assertRaisesRegex(ValueError, "already exists"):
            smoke.add_resources(startup(), [("usr/sbin/main_stage1.sh", elf())])
        image = bytearray(startup())
        struct.pack_into("<I", image, 264 + 96, 0xffffffff)
        fs = image[264:]
        checksum(fs)
        image[264:] = fs
        with self.assertRaisesRegex(ValueError, "inode"):
            smoke.add_resources(image, [("sbin/new", elf())])

    def test_corrupt_headers_checksums_and_directory(self):
        for position in (8, 36, 40, 44, 52, 260, 264 + 8, len(startup()) - 1):
            image = bytearray(startup())
            image[position] ^= 0xff
            with self.subTest(position=position), self.assertRaises(ValueError):
                smoke.add_resources(image, [("sbin/new", elf())])
        for position, value in ((12, 93), (16, 0), (92, 23), (116, 0),
                                (120, 0xffffffff), (256 + 28, 0xffffffff)):
            image = bytearray(startup())
            fs = image[264:]
            struct.pack_into("<I", fs, position, value)
            checksum(fs)
            image[264:] = fs
            with self.subTest(position=position), self.assertRaises(ValueError):
                smoke.add_resources(image, [("sbin/new", elf())])
        with patch.object(smoke, "MAX_IMAGE_SIZE", 512):
            with self.assertRaisesRegex(ValueError, "exceeds"):
                smoke.add_resources(startup(), [("sbin/new", elf())])

    def test_changed_original_payload_rejected(self):
        original = startup()[264:]
        expanded = bytearray(smoke.add_file(original, "sbin/new", elf()))
        offset, _ = smoke.file_entries(expanded)["usr/sbin/main_stage1.sh"]
        expanded[offset + 128] ^= 0xff
        with self.assertRaisesRegex(ValueError, "payload changed"):
            smoke.relocate_xip(original, expanded, smoke.LOAD_ADDRESS + 264)

    def test_prepare_provenance_and_input_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            media, build, output = root / "media", root / "build", root / "output"
            media.mkdir()
            build.mkdir()
            image, dtb = startup(), b"synthetic-dtb"
            manifest = {
                "diagnostic_shell": True,
                "startup_sha256": hashlib.sha256(image).hexdigest(),
                "dtb_sha256": hashlib.sha256(dtb).hexdigest(),
                "script_patches": [{"path": "usr/sbin/main_stage1.sh"}],
            }
            (media / "startup.bin").write_bytes(image)
            (media / "board.dtb").write_bytes(dtb)
            (media / "manifest.json").write_text(json.dumps(manifest))
            (build / "mh2p-egl-smoke").write_bytes(elf())
            (build / "libmh2p-egl-smoke.so").write_bytes(elf(3))
            for field, value in (("diagnostic_shell", False), ("graphics_smoke", True),
                                 ("native_display_probe", True),
                                 ("startup_sha256", "bad"), ("dtb_sha256", "bad")):
                (media / "manifest.json").write_text(json.dumps(manifest | {field: value}))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    smoke.prepare(media, build, output)
                self.assertFalse(output.exists())
            (media / "manifest.json").write_text(json.dumps(manifest))
            for position, value in ((0, 0), (4, 2), (5, 2), (16, 3), (18, 3),
                                    (39, 4), (37, 4)):
                payload = bytearray(elf())
                payload[position] = value
                (build / "mh2p-egl-smoke").write_bytes(payload)
                with self.subTest(position=position), self.assertRaises(ValueError):
                    smoke.prepare(media, build, output)
                self.assertFalse(output.exists())
            (build / "mh2p-egl-smoke").write_bytes(elf())
            result = smoke.prepare(media, build, output)
            self.assertTrue(result["native_executables_patched"])
            self.assertTrue(result["xip_program_headers_relocated"])
            self.assertFalse(result["native_executable_instructions_patched"])
            self.assertFalse(result["graphics_libraries_replaced"])
            self.assertEqual(result["parent_script_patches"], manifest["script_patches"])
            self.assertNotIn("script_patches", result)
            self.assertEqual((media / "startup.bin").read_bytes(), image)
            self.assertEqual((output / "board.dtb").read_bytes(), dtb)
            (build / "mh2p-native-display-probe").write_bytes(elf())
            native = root / "native"
            result = smoke.prepare(media, build, native, native_display_probe=True)
            self.assertTrue(result["native_display_probe"])
            self.assertNotIn("graphics_smoke", result)
            self.assertFalse(result["graphics_libraries_replaced"])
            self.assertEqual([r["path"] for r in result["added_resources"]],
                             ["sbin/mh2p-native-display-probe"])
            added = smoke.file_entries((native / "startup.bin").read_bytes()[264:])
            self.assertIn("sbin/mh2p-native-display-probe", added)
            self.assertNotIn("lib/libmh2p-egl-smoke.so", added)
            self.assertEqual((media / "startup.bin").read_bytes(), image)
            with self.assertRaises(FileExistsError):
                smoke.prepare(media, build, output)
            with self.assertRaises(FileExistsError):
                build_graphics_smoke.build("unused-compiler", build)


if __name__ == "__main__":
    unittest.main()
