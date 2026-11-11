import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_nor


class PrepareNorTests(unittest.TestCase):
    def test_layout_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "stage2.img"
            payload = b"\x5a" * prepare_nor.STAGE2_SIZE
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            output = root / "media"
            with patch.object(prepare_nor, "STAGE2_SHA256", digest):
                metadata = prepare_nor.prepare(source, output)
                with self.assertRaises(FileExistsError):
                    prepare_nor.prepare(source, output)
            image = (output / "nor.bin").read_bytes()
            start = prepare_nor.STAGE2_OFFSET
            end = start + len(payload)
            self.assertEqual(len(image), 64 * 1024 * 1024)
            self.assertEqual(image[start:end], payload)
            self.assertEqual(image[:start], b"\xff" * start)
            self.assertEqual(image[end:], b"\xff" * (len(image) - end))
            self.assertEqual(source.read_bytes(), payload)
            self.assertEqual(metadata["nor_sha256"], hashlib.sha256(image).hexdigest())
            self.assertTrue(metadata["incomplete_flash"])
            self.assertEqual(json.loads((output / "manifest.json").read_text()), metadata)

    def test_original_flash_filesystems(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "stage2.img"
            stage2 = b"\x5a" * prepare_nor.STAGE2_SIZE
            source.write_bytes(stage2)
            images, partitions = {}, {}
            for value, name in enumerate(("system", "persist"), 1):
                offset, size, _ = prepare_nor.PARTITIONS[name]
                images[name] = bytes([value]) * size
                (root / name).write_bytes(images[name])
                partitions[name] = (offset, size,
                                    hashlib.sha256(images[name]).hexdigest())
            with patch.object(prepare_nor, "STAGE2_SHA256",
                              hashlib.sha256(stage2).hexdigest()), \
                    patch.dict(prepare_nor.PARTITIONS, partitions):
                bad = root / "bad"
                bad.write_bytes(b"\0" * partitions["system"][1])
                with self.assertRaisesRegex(ValueError, "system SHA-256"):
                    prepare_nor.prepare(source, root / "bad-media",
                                        {"system": bad})
                self.assertFalse((root / "bad-media").exists())
                metadata = prepare_nor.prepare(
                    source, root / "media",
                    {name: root / name for name in images})
            image = (root / "media/nor.bin").read_bytes()
            covered = [(prepare_nor.STAGE2_OFFSET, stage2)]
            for name, payload in images.items():
                offset = partitions[name][0]
                self.assertEqual(image[offset:offset + len(payload)], payload)
                self.assertEqual(metadata[name]["offset"], offset)
                covered.append((offset, payload))
            position = 0
            for offset, payload in sorted(covered, key=lambda item: item[0]):
                self.assertEqual(image[position:offset],
                                 b"\xff" * (offset - position))
                position = offset + len(payload)
            self.assertEqual(image[position:], b"\xff" * (len(image) - position))
            self.assertIn("system and persist are populated", metadata["note"])

    def test_bct_and_partition_table(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "stage2.img"
            stage2 = b"\x5a" * prepare_nor.STAGE2_SIZE
            source.write_bytes(stage2)
            bct = bytearray(b"\xa5" * prepare_nor.BCT_SIZE)
            struct.pack_into("<III", bct, prepare_nor.BCT_CUSTOMER_DATA,
                             2, 0x80000, 0x40000)
            (root / "bct").write_bytes(bct)
            with patch.object(prepare_nor, "STAGE2_SHA256",
                              hashlib.sha256(stage2).hexdigest()):
                with self.assertRaisesRegex(ValueError, "BCT SHA-256"):
                    prepare_nor.prepare(source, root / "bad", bct=root / "bct")
                self.assertFalse((root / "bad").exists())
                with patch.object(prepare_nor, "BCT_SHA256",
                                  hashlib.sha256(bct).hexdigest()):
                    metadata = prepare_nor.prepare(source, root / "media",
                                                   bct=root / "bct")
            image = (root / "media/nor.bin").read_bytes()
            self.assertEqual(image[:len(bct)], bct)
            self.assertEqual(metadata["partition_table"]["offset"], 0x80000)
            table = image[0x80000:0x80000 + 0x40000]
            magic, version, size = struct.unpack_from("<QII", table)
            self.assertEqual((magic, version), (0xFFFFFFFF8F9E8D8B, 0x100))
            self.assertEqual(size % 16, 0)
            self.assertEqual(table[size:], b"\xff" * (len(table) - size))
            self.assertEqual(table[16:32],
                             prepare_nor.cmac_zero_key(table[32:size]))
            count = struct.unpack_from("<I", table, 64)[0]
            entries = {}
            for index in range(count):
                entry = table[72 + index * 120:72 + (index + 1) * 120]
                pid = struct.unpack_from("<I", entry)[0]
                name = entry[4:28].rstrip(b"\0").decode()
                start, length = struct.unpack_from("<QQ", entry, 80)
                self.assertNotEqual(pid, 0)
                entries[name] = (start, length)
            persist = entries["EARLY_PERSIST_RAW"]
            self.assertEqual(persist, (prepare_nor.EARLY_PERSIST_OFFSET,
                                       prepare_nor.EARLY_PERSIST_SIZE))
            self.assertEqual(persist[1] % 0x40000, 0)
            self.assertLessEqual(sum(persist), prepare_nor.STAGE2_OFFSET)
            self.assertEqual(image[persist[0]:sum(persist)],
                             b"\xff" * persist[1])

    def test_zero_key_cmac_vector(self):
        # Independently derived with AES-ECB subkeys (RFC 4493) and a zero key.
        self.assertEqual(prepare_nor.cmac_zero_key(b"").hex(),
                         "4387c14b46ef7e176dceefa862d72ff9")

    def test_base_nor_preserves_every_unreplaced_byte_and_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "stage2.img"
            stage2 = b"\x5a" * prepare_nor.STAGE2_SIZE
            source.write_bytes(stage2)
            base = bytes(range(256)) * (prepare_nor.NOR_SIZE // 256)
            capture = root / "captured-nor.bin"
            capture.write_bytes(base)
            original_hash = hashlib.sha256(base).hexdigest()
            persist = root / "persist"
            offset, size, _ = prepare_nor.PARTITIONS["persist"]
            payload = b"\x33" * size
            persist.write_bytes(payload)
            output = root / "private"
            with patch.object(prepare_nor, "STAGE2_SHA256",
                              hashlib.sha256(stage2).hexdigest()), \
                    patch.dict(prepare_nor.PARTITIONS, {
                        "persist": (offset, size,
                                    hashlib.sha256(payload).hexdigest())}):
                result = prepare_nor.prepare(
                    source, output, {"persist": persist}, base_nor=capture)
            expected = bytearray(base)
            start = prepare_nor.STAGE2_OFFSET
            expected[start:start + len(stage2)] = stage2
            expected[offset:offset + size] = payload
            self.assertEqual((output / "nor.bin").read_bytes(), expected)
            self.assertEqual(hashlib.sha256(capture.read_bytes()).hexdigest(),
                             original_hash)
            self.assertEqual(source.read_bytes(), stage2)
            self.assertEqual(persist.read_bytes(), payload)
            self.assertEqual(output.stat().st_mode & 0o077, 0)
            self.assertEqual((output / "nor.bin").stat().st_mode & 0o077, 0)
            self.assertTrue(result["incomplete_flash"])
            self.assertTrue(result["unreplaced_base_bytes_preserved"])
            self.assertFalse(result["provisioning_verified"])
            self.assertEqual(result["base_nor"]["sha256"], original_hash)
            self.assertNotIn("partition_table", result)
            self.assertEqual(json.loads((output / "manifest.json").read_text()),
                             result)

    def test_base_nor_rejects_bad_size_and_bct_before_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture"
            capture.write_bytes(b"short")
            output = root / "output"
            with self.assertRaisesRegex(ValueError, "67,108,864"):
                prepare_nor.prepare(root / "unused", output, base_nor=capture)
            self.assertFalse(output.exists())
            with self.assertRaisesRegex(ValueError, "original BCT"):
                prepare_nor.prepare(root / "unused", output,
                                    bct=root / "unused-bct", base_nor=capture)
            self.assertFalse(output.exists())

    def test_rejects_wrong_size_and_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "stage2.img"
            output = root / "media"
            source.write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "32,505,856"):
                prepare_nor.prepare(source, output)
            with source.open("wb") as stream:
                stream.truncate(prepare_nor.STAGE2_SIZE)
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare_nor.prepare(source, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
