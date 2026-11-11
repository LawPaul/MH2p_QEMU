import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_mnand


class PrepareMnandTests(unittest.TestCase):
    def media_fixture(self, root):
        source = root / "diagnostic.bin"
        size = 32 * 1024**2
        crash = size // 512 - 2048
        with source.open("wb") as stream:
            stream.truncate(size)
            mbr = bytearray(512)
            mbr[446:462] = prepare_mnand.partition(177, crash, 2048)
            mbr[462:478] = prepare_mnand.partition(
                5, prepare_mnand.EXT_SECTOR, crash - prepare_mnand.EXT_SECTOR)
            mbr[510:] = b"\x55\xaa"
            stream.write(mbr)
            for index in range(1, 4):
                at = index * 2048
                ebr = bytearray(512)
                ebr[446:462] = prepare_mnand.partition(177, 1, 2)
                if index < 3:
                    next_ebr = at + 2048
                    ebr[462:478] = prepare_mnand.partition(
                        5, next_ebr - prepare_mnand.EXT_SECTOR, crash - next_ebr)
                ebr[510:] = b"\x55\xaa"
                stream.seek(at * 512)
                stream.write(ebr)
                stream.write(bytes([index]) * 1024)
            stream.seek(crash * 512)
            stream.write(b"primary persistence contents")
        return source, {"DISK_SIZE": size, "CRASH_SECTOR": crash,
                        "MEDIA_VOLUME_SECTORS": 2048}

    def test_extend_media_preserves_payloads_and_creates_exact_slots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, constants = self.media_fixture(root)
            before = source.read_bytes()
            with patch.multiple(prepare_mnand, **constants):
                result = prepare_mnand.extend_media(source, root / "extended")
                with self.assertRaises(FileExistsError):
                    prepare_mnand.extend_media(source, root / "extended")
            after = (root / "extended/mnand.bin").read_bytes()
            expected = bytearray(before)
            at = 6144
            self.assertEqual([v["logical"] for v in result["empty_diagnostic_volumes"]],
                             list(range(4, 12)))
            for volume in result["empty_diagnostic_volumes"]:
                next_ebr = volume["ebr_sector"]
                expected[at * 512 + 462:at * 512 + 478] = prepare_mnand.partition(
                    5, next_ebr - prepare_mnand.EXT_SECTOR,
                    constants["CRASH_SECTOR"] - next_ebr)
                ebr = bytearray(512)
                ebr[446:462] = prepare_mnand.partition(177, 2048, 2048)
                ebr[510:] = b"\x55\xaa"
                expected[next_ebr * 512:(next_ebr + 1) * 512] = ebr
                self.assertEqual(volume["start_sector"], next_ebr + 2048)
                self.assertEqual(volume["sectors"], 2048)
                self.assertFalse(volume["formatted"])
                at = next_ebr
            self.assertEqual(after, expected)
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(result["source_sha256"], hashlib.sha256(before).hexdigest())
            self.assertEqual(result["mnand_sha256"], hashlib.sha256(after).hexdigest())
            self.assertFalse(result["original_physical_layout"])
            self.assertEqual(json.loads(
                (root / "extended/manifest.json").read_text()), result)

    def test_extend_media_rejects_unsafe_or_nonempty_layout(self):
        for corruption in ("size", "mbr", "ebr", "cycle", "overlap",
                           "fourth-volume", "occupied", "too-small"):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source, constants = self.media_fixture(root)
                with source.open("r+b") as stream:
                    if corruption == "size":
                        stream.truncate(1024)
                    elif corruption == "mbr":
                        stream.seek(510)
                        stream.write(b"\x00\x00")
                    elif corruption == "ebr":
                        stream.seek(2048 * 512 + 510)
                        stream.write(b"\x00\x00")
                    elif corruption in ("cycle", "overlap"):
                        stream.seek(2048 * 512 + 462)
                        stream.write(prepare_mnand.partition(
                            5, 0 if corruption == "cycle" else 1, 2048))
                    elif corruption == "fourth-volume":
                        stream.seek(6144 * 512 + 462)
                        stream.write(prepare_mnand.partition(5, 8192, 2048))
                    elif corruption == "occupied":
                        stream.seek(10000 * 512)
                        stream.write(b"existing unreferenced data")
                    elif corruption == "too-small":
                        constants["MEDIA_VOLUME_SECTORS"] *= 8
                before = prepare_mnand.digest(source)
                with patch.multiple(prepare_mnand, **constants):
                    with self.assertRaises(ValueError):
                        prepare_mnand.extend_media(source, root / "output")
                self.assertFalse((root / "output").exists())
                self.assertEqual(prepare_mnand.digest(source), before)

    def test_layout_and_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.img"
            payload = b"synthetic app!!\0\0" * 65536
            source.write_bytes(payload)
            checksum = hashlib.sha256(payload).hexdigest()
            output = root / "media"
            with patch.multiple(prepare_mnand, APP_SIZE=len(payload), APP_SHA256=checksum):
                result = prepare_mnand.prepare(source, output)
                with self.assertRaises(FileExistsError):
                    prepare_mnand.prepare(source, output)
            self.assertTrue(result["incomplete_disk"])
            self.assertFalse(result["crash_filesystem_present"])
            image = output / "mnand.bin"
            self.assertEqual(image.stat().st_size, 4 * 1024**3)
            with image.open("rb") as stream:
                mbr = stream.read(512)
                self.assertEqual(mbr[510:], b"\x55\xaa")
                self.assertEqual(mbr[450], 177)
                self.assertEqual(mbr[466], 5)
                crash, count = struct.unpack_from("<II", mbr, 454)
                self.assertEqual(crash, prepare_mnand.CRASH_SECTOR)
                self.assertEqual(count * 512, 64 * 1024**2)
                start, count = struct.unpack_from("<II", mbr, 470)
                self.assertEqual(start, prepare_mnand.EXT_SECTOR)
                self.assertEqual(start + count, crash)
                stream.seek(start * 512)
                ebr = stream.read(512)
                self.assertEqual(ebr[510:], b"\x55\xaa")
                self.assertEqual(ebr[450], 177)
                relative, count = struct.unpack_from("<II", ebr, 454)
                self.assertEqual(start + relative, prepare_mnand.APP_SECTOR)
                self.assertEqual(count * 512, len(payload))
                stream.seek((start + relative) * 512)
                self.assertEqual(stream.read(len(payload)), payload)
                self.assertEqual(stream.read(512), b"\0" * 512)
                stream.seek(crash * 512)
                self.assertEqual(stream.read(512), b"\0" * 512)
            self.assertEqual(prepare_mnand.digest(source), checksum)
            self.assertEqual(json.loads((output / "manifest.json").read_text()), result)

    def test_rejects_unknown_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.img"
            source.write_bytes(b"wrong")
            with self.assertRaises(ValueError):
                prepare_mnand.prepare(source, root / "output")
            with patch.object(prepare_mnand, "APP_SIZE", 5):
                with self.assertRaises(ValueError):
                    prepare_mnand.prepare(source, root / "output")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
