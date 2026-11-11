# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import prepare_rcc_nor


class PrepareRccNorTests(unittest.TestCase):
    def test_layout_source_preservation_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.bin"
            payload = b"\x5a" * prepare_rcc_nor.APP_SIZE
            source.write_bytes(payload)
            output = root / "media"
            with patch.object(prepare_rcc_nor, "APP_SHA256",
                              hashlib.sha256(payload).hexdigest()):
                metadata = prepare_rcc_nor.prepare(source, output)
                with self.assertRaises(FileExistsError):
                    prepare_rcc_nor.prepare(source, output)
            image = (output / "nor.bin").read_bytes()
            start = prepare_rcc_nor.APP_OFFSET
            end = start + len(payload)
            self.assertEqual(len(image), prepare_rcc_nor.NOR_SIZE)
            self.assertEqual(image[:start], b"\xff" * start)
            self.assertEqual(image[start:end], payload)
            self.assertEqual(image[end:], b"\xff" * (len(image) - end))
            self.assertEqual(source.read_bytes(), payload)
            self.assertEqual(metadata["nor_sha256"],
                             hashlib.sha256(image).hexdigest())
            self.assertEqual(metadata["app_offset"], 32 * 1024 * 1024)
            self.assertTrue(metadata["incomplete_flash"])
            self.assertEqual(
                json.loads((output / "manifest.json").read_text()), metadata)

    def test_rejects_wrong_size_and_hash_without_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.bin"
            output = root / "media"
            source.write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "30 MiB"):
                prepare_rcc_nor.prepare(source, output)
            with source.open("wb") as stream:
                stream.truncate(prepare_rcc_nor.APP_SIZE)
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare_rcc_nor.prepare(source, output)
            self.assertFalse(output.exists())

    def test_original_default_volumes_layout_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.bin"
            payload = b"\x5a" * prepare_rcc_nor.APP_SIZE
            source.write_bytes(payload)
            persistence = root / "defaults.bin"
            defaults = b"\x31" * 1048576 + b"\x32" * 1048576
            persistence.write_bytes(defaults)
            output = root / "media"
            with patch.object(prepare_rcc_nor, "APP_SHA256",
                              hashlib.sha256(payload).hexdigest()), \
                    patch.object(prepare_rcc_nor, "PERSISTENCE_SHA256",
                                 hashlib.sha256(defaults).hexdigest()):
                metadata = prepare_rcc_nor.prepare(source, output, persistence)
            image = (output / "nor.bin").read_bytes()
            start = prepare_rcc_nor.APP_OFFSET
            end = start + len(payload)
            self.assertEqual(len(image), prepare_rcc_nor.NOR_SIZE)
            self.assertEqual(image[:start], b"\xff" * start)
            self.assertEqual(image[start:end], payload)
            self.assertEqual(image[end:], defaults)
            self.assertEqual(source.read_bytes(), payload)
            self.assertEqual(persistence.read_bytes(), defaults)
            self.assertEqual(metadata["nor_sha256"],
                             hashlib.sha256(image).hexdigest())
            self.assertEqual(metadata["persistence_defaults"]["offset"], end)
            self.assertEqual(metadata["persistence_defaults"]["sha256"],
                             hashlib.sha256(defaults).hexdigest())
            self.assertTrue(metadata["incomplete_flash"])
            self.assertEqual(
                json.loads((output / "manifest.json").read_text()), metadata)

    def test_rejects_invalid_default_volumes_before_creating_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "app.bin"
            payload = b"\x5a" * prepare_rcc_nor.APP_SIZE
            source.write_bytes(payload)
            persistence = root / "defaults.bin"
            output = root / "media"
            with patch.object(prepare_rcc_nor, "APP_SHA256",
                              hashlib.sha256(payload).hexdigest()):
                persistence.write_bytes(b"bad")
                with self.assertRaisesRegex(ValueError, "Per1 and Per2"):
                    prepare_rcc_nor.prepare(source, output, persistence)
                persistence.write_bytes(b"\0" * prepare_rcc_nor.PERSISTENCE_SIZE)
                with self.assertRaisesRegex(ValueError, "defaults SHA-256"):
                    prepare_rcc_nor.prepare(source, output, persistence)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
