#!/usr/bin/env python3
"""Synthetic fixtures only; these tests do not need proprietary firmware."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

import prepare


def fixture():
    fs = bytearray(1024)
    fs[:8] = b"imagefs\x04"
    name = b"usr/sbin/main_stage1.sh\0"
    entry_size = (32 + len(name) + 3) & ~3
    directory_end = 92 + entry_size + 4
    struct.pack_into("<III", fs, 8, len(fs), directory_end, 92)
    struct.pack_into("<HHIIIII", fs, 92, entry_size, 0, 1, 0o100755, 0, 0, 0)
    struct.pack_into("<II", fs, 116, 256, 512)
    fs[124:124 + len(name)] = name
    fs[256:768] = b"#!/bin/ksh\necho stock\n".ljust(512, b"\n")
    total = sum(x[0] for x in struct.iter_unpack("<I", fs[:-4]))
    struct.pack_into("<I", fs, len(fs) - 4, -total & 0xFFFFFFFF)
    image = bytearray(264) + fs
    struct.pack_into("<IHBBHH", image, 8, prepare.STARTUP_MAGIC, 1, 1, 0, 256, 40)
    struct.pack_into("<9I", image, 20, prepare.LOAD_ADDRESS + 8, 0,
                     prepare.LOAD_ADDRESS + 8, prepare.LOAD_ADDRESS + 8,
                     len(image) - 8, 256, len(image) - 8, 0, len(fs))
    return bytes(image)


def wrap(image):
    compressed = zlib.compress(image)
    header = bytearray(2048)
    header[:8] = b"ANDROID!"
    struct.pack_into("<I", header, 8, len(compressed))
    struct.pack_into("<I", header, 36, 2048)
    return bytes(header) + compressed


class PrepareTests(unittest.TestCase):
    def test_decode(self):
        image = fixture()
        decoded, info = prepare.decode_loadimg(wrap(image))
        self.assertEqual(decoded, image)
        self.assertEqual(info["imagefs_offset"], 264)

    def test_invalid_wrapper_and_truncation(self):
        for raw in [b"", bytes(2048), wrap(fixture())[:-1]]:
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                prepare.decode_loadimg(raw)

    def test_bad_header_and_checksum(self):
        for offset in [8, 12, 14, 18, 28, 40, 44, 52, len(fixture()) - 1]:
            image = bytearray(fixture())
            image[offset] ^= 0xff
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                prepare.decode_loadimg(wrap(image))

    def test_expansion_limit(self):
        with patch.object(prepare, "MAX_IMAGE_SIZE", 128):
            with self.assertRaisesRegex(ValueError, "oversized"):
                prepare.decode_loadimg(wrap(fixture()))

    def test_shell_patch_is_bounded(self):
        image = fixture()
        modified, info = prepare.diagnostic_shell(image, 264)
        changed = {i for i, (a, b) in enumerate(zip(image, modified)) if a != b}
        permitted = set(range(info["offset"], info["offset"] + info["size"]))
        permitted.update(range(info["checksum_offset"], info["checksum_offset"] + 4))
        self.assertTrue(changed)
        self.assertTrue(changed <= permitted)
        self.assertEqual(len(image), len(modified))
        self.assertIn(b"exec /bin/ksh -i", modified)
        self.assertEqual(prepare.decode_loadimg(wrap(modified))[0], modified)

    def test_missing_script_and_invalid_directory(self):
        image = bytearray(fixture())
        image[264 + 124] = ord("x")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            prepare.diagnostic_shell(image, 264)
        struct.pack_into("<H", image, 264 + 92, 27)
        with self.assertRaisesRegex(ValueError, "directory entry"):
            prepare.diagnostic_shell(image, 264)

    def test_prepare_pins_inputs_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, dtb, output = root / "loadimg", root / "dtb", root / "out"
            source.write_bytes(wrap(fixture()))
            dtb.write_bytes(struct.pack(">II", 0xD00DFEED, 40) + bytes(32) + b"signature")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare.prepare(source, dtb, output)
            self.assertFalse(output.exists())
            with patch.object(prepare, "LOADIMG_SHA256", hashlib.sha256(source.read_bytes()).hexdigest()):
                with self.assertRaisesRegex(ValueError, "board"):
                    prepare.prepare(source, dtb, output)
                with patch.object(prepare, "DTB_SHA256", hashlib.sha256(dtb.read_bytes()).hexdigest()):
                    result = prepare.prepare(source, dtb, output, shell=True)
                    self.assertTrue(result["diagnostic_shell"])
                    self.assertFalse(result["native_executables_patched"])
                    self.assertEqual((output / "board.dtb").stat().st_size, 40)
                    with self.assertRaises(FileExistsError):
                        prepare.prepare(source, dtb, output)


if __name__ == "__main__":
    unittest.main()
