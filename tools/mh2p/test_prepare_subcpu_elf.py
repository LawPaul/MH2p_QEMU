# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import prepare_subcpu_elf as prepare


class PrepareSubcpuElfTests(unittest.TestCase):
    def payload(self):
        raw = bytearray(b"\xff" * prepare.IMAGE_SIZE)
        struct.pack_into("<H", raw, 0, 0xd8)
        raw[0xd8:0xda] = b"\x61\xcf"
        raw[-16:] = bytes(range(16))
        return bytes(raw)

    def test_elf_layout_source_preservation_and_provenance(self):
        raw = self.payload()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            source.write_bytes(raw)
            output = root / "wrapped"
            with patch.object(prepare, "IMAGE_SHA256",
                              hashlib.sha256(raw).hexdigest()):
                metadata = prepare.prepare(source, output)
                with self.assertRaises(FileExistsError):
                    prepare.prepare(source, output)
            elf = (output / "subcpu.elf").read_bytes()
            header = struct.unpack_from("<16sHHIIIIIHHHHHH", elf)
            self.assertEqual(header[:5],
                             (b"\x7fELF\x01\x01\x01" + bytes(9),
                              2, 197, 1, 0xd8))
            self.assertEqual(header[5], 52)
            self.assertEqual(header[7:], (0, 52, 32, 1, 40, 3, 2))
            self.assertEqual(
                struct.unpack_from("<8I", elf, 52),
                (1, prepare.PAYLOAD_OFFSET, 0, 0,
                 len(raw), len(raw), 5, 0x1000))
            offset = prepare.PAYLOAD_OFFSET
            self.assertEqual(elf[offset:offset + len(raw)], raw)
            self.assertEqual(elf[offset + len(raw) - 16:offset + len(raw)],
                             bytes(range(16)))
            self.assertEqual(source.read_bytes(), raw)
            sections = header[6]
            self.assertEqual(sections % 4, 0)
            self.assertEqual(elf[sections:sections + 40], bytes(40))
            self.assertEqual(
                struct.unpack_from("<10I", elf, sections + 40),
                (1, 1, 6, 0, offset, len(raw), 0, 0, 1, 0))
            strings = struct.unpack_from("<10I", elf, sections + 80)
            self.assertEqual(strings,
                             (7, 3, 0, 0, offset + len(raw), 17, 0, 0, 1, 0))
            self.assertEqual(elf[strings[4]:strings[4] + strings[5]],
                             b"\0.text\0.shstrtab\0")
            self.assertEqual(len(elf), sections + 3 * 40)
            self.assertEqual(metadata["elf_sha256"],
                             hashlib.sha256(elf).hexdigest())
            self.assertEqual(
                json.loads((output / "manifest.json").read_text()), metadata)
            self.assertTrue(metadata["all_source_bytes_preserved"])
            for key in ("exact_mcu_variant_verified",
                        "board_memory_map_verified",
                        "subcpu_board_boot_verified", "rstp_verified"):
                self.assertFalse(metadata[key])

    def test_rejects_wrong_size_and_hash_before_creating_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            output = root / "wrapped"
            source.write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "131088-byte"):
                prepare.prepare(source, output)
            source.write_bytes(self.payload())
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare.prepare(source, output)
            self.assertFalse(output.exists())

    def test_rejects_bad_reset_vector_or_entry_instruction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            output = root / "wrapped"
            for offset in (0, 0xd8):
                raw = bytearray(self.payload())
                raw[offset] ^= 1
                source.write_bytes(raw)
                with patch.object(prepare, "IMAGE_SHA256",
                                  hashlib.sha256(raw).hexdigest()):
                    with self.assertRaisesRegex(ValueError, "reset vector"):
                        prepare.prepare(source, output)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
