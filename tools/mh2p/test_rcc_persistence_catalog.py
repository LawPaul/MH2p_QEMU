#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import tempfile
import unittest
from pathlib import Path

import rcc_persistence_catalog as catalog

BASE = 0x8000000


def build_elf(blob, symbols):
    """Return an ELF32 with @blob loaded at BASE and @symbols {name: (addr, size)}."""
    strtab = b"\0"
    symtab = bytes(16)
    for name, (value, size) in symbols.items():
        symtab += struct.pack("<IIIBBH", len(strtab), value, size, 0x11, 0, 1)
        strtab += name.encode() + b"\0"
    blob_off = 0x34 + 32
    symtab_off = blob_off + len(blob)
    strtab_off = symtab_off + len(symtab)
    shoff = strtab_off + len(strtab)
    header = b"\x7fELF\x01\x01\x01" + bytes(9) + struct.pack(
        "<HHIIIIIHHHHHH", 2, 40, 1, BASE, 0x34, shoff, 0, 0x34, 32, 1, 40, 3,
        0)
    phdr = struct.pack("<IIIIIIII", 1, blob_off, BASE, BASE, len(blob),
                       len(blob) + 0x100, 5, 4)
    sections = bytes(40) + struct.pack(
        "<IIIIIIIIII", 0, 2, 0, 0, symtab_off, len(symtab), 2, 1, 4, 16) + \
        struct.pack("<IIIIIIIIII", 0, 3, 0, 0, strtab_off, len(strtab), 0, 0,
                    1, 0)
    return header + phdr + blob + symtab + strtab + sections


def sample_elf():
    blob = bytearray(0x200)
    keys_5f22 = BASE + 0x40
    keys_5f34 = BASE + 0xa0
    default = BASE + 0x100
    struct.pack_into("<IIII", blob, 0, 0x5f22, keys_5f22, 0x5f34, keys_5f34)
    for i, (key, forwarded, size) in enumerate(((0xb34, 0, 4),
                                                (0x180b, 0, 4),
                                                (0xf187, 1, 11))):
        catalog.KEY_ENTRY.pack_into(blob, 0x40 + 20 * i, key, 0x8056345, 0, 0,
                                    forwarded, 0, size, 0)
    catalog.KEY_ENTRY.pack_into(blob, 0xa0, 0x8000, 0x8056345, 0, 0, 0, 0,
                                6, 0)
    struct.pack_into("<IIHHI", blob, 0x120, 0x5f22, 0xb34, 4, 5, default)
    struct.pack_into("<IIHHI", blob, 0x130, 0x5f22, 0xf187, 11, 5, default)
    blob[0x100:0x104] = b"\x0a" * 4
    struct.pack_into("<IHH", blob, 0x140, 0x8000, 5, 6)
    return build_elf(bytes(blob), {
        catalog.NS_TABLE: (BASE, 16),
        catalog.ADAPTATION_TABLE: (BASE + 0x120, 32),
        catalog.ADAPTATION_5F34_TABLE: (BASE + 0x140, 8)})


def sample_m4():
    """An M4 image with two NvM blocks and a key map for 0x5f22:0xf187."""
    blob = bytearray(0x200)
    m4_base = 0x4000
    blob[0x100:0x10b] = b"4N0035063  "
    blob[0x110:0x114] = b"X020"
    blob[0x40:0x40 + catalog.NVM_BLOCK.size] = bytes(catalog.NVM_BLOCK.size)
    for ident, (length, rom) in enumerate(((11, 0x100), (4, 0x110)) * 4, 1):
        catalog.NVM_BLOCK.pack_into(blob, 0x40 + 20 * ident, ident, 0, length,
                                    0x80000000, m4_base + rom, 0x10101)
    for i, (block, key, off, size) in enumerate(((1, 0xf187, 0, 11),
                                                 (2, 0x409, 0, 100),
                                                 (2, 0x5ff, 0, 4))):
        catalog.NVM_KEY.pack_into(blob, 0x180 + 16 * i, block, 0, 0x5f22, key,
                                  off, size)
    header = b"\x7fELF\x01\x01\x01" + bytes(9) + struct.pack(
        "<HHIIIIIHHHHHH", 2, 40, 1, m4_base, 0x34, 0, 0, 0x34, 32, 1, 40, 0,
        0)
    phdr = struct.pack("<IIIIIIII", 1, 0x54, m4_base, m4_base, len(blob),
                       len(blob), 5, 4)
    return header + phdr + bytes(blob)


class CatalogTest(unittest.TestCase):
    def test_m4_image_supplies_defaults_for_forwarded_keys(self):
        text = catalog.catalog(catalog.Elf(sample_elf()), m4=catalog.Elf(
            sample_m4()))
        lines = text.splitlines()
        self.assertIn("0x5f22 0xf187 m4 11 344e303033353036332020", lines)
        self.assertIn("0x5f22 0x180b rcc 4", lines)
        self.assertIn("# --m4-image: 1 M4 keys read as the M4's "
                      "blank-EEPROM NvM ROM defaults", lines)

    def test_m4_image_without_nvm_tables_is_rejected(self):
        with self.assertRaises(ValueError):
            catalog.catalog(catalog.Elf(sample_elf()),
                            m4=catalog.Elf(sample_elf()))

    def test_catalog_lists_owners_and_builtin_defaults(self):
        text = catalog.catalog(catalog.Elf(sample_elf()))
        self.assertEqual(text.splitlines()[0], catalog.MAGIC)
        self.assertEqual([l for l in text.splitlines()
                          if not l.startswith("#")], [
            "0x5f22 0xb34 rcc 4 0a0a0a0a",
            "0x5f22 0x180b rcc 4",
            "0x5f22 0xf187 m4 11",
            "0x5f34 0x8000 rcc 6 000000000000"])

    def test_provisioning_zero_fills_only_rcc_keys(self):
        text = catalog.catalog(catalog.Elf(sample_elf()), provision_rcc=True)
        lines = text.splitlines()
        self.assertIn("0x5f22 0x180b rcc 4 00000000", lines)
        self.assertIn("0x5f22 0xb34 rcc 4 0a0a0a0a", lines)
        self.assertIn("0x5f22 0xf187 m4 11", lines)

    def test_main_writes_catalog_and_rejects_non_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "PERSISTENCE").write_bytes(sample_elf())
            self.assertEqual(catalog.main([str(root / "PERSISTENCE"),
                                           str(root / "catalog.txt")]), 0)
            self.assertTrue((root / "catalog.txt").read_text()
                            .startswith(catalog.MAGIC))
            (root / "bad").write_bytes(b"nope")
            with self.assertRaises(SystemExit):
                catalog.main([str(root / "bad"), str(root / "out")])


if __name__ == "__main__":
    unittest.main()
