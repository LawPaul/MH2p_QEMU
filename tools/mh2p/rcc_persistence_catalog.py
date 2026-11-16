#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the RCC persistence key catalog for the mh2p-rstp chardev.

The input is the RCC application binary /App/PERSISTENCE (an unstripped
32-bit ARM ELF) copied from the user's own RCC App image.  The output lists
every namespace/key the RCC serves, whether the RCC answers a missing value
itself or forwards the subscription to its M4, and the built-in defaults the
RCC returns for keys that have no stored row yet.  Load the result with
``qom-set /chardevs/<rstp> persistence-catalog <file>``.

A fresh RCC answers PE_IO_ERROR for its own keys without a row, which the
MMX rccpersistencebackend keeps re-subscribing and refuses to write.  Real
units are provisioned in production; --provision-rcc emulates that state by
giving every RCC-owned key without a built-in default a zero-filled value of
its catalogued size.  Keys the RCC forwards to its M4 are never provisioned.

Keys the RCC forwards are served by the RCC M4 (IPU1) from its AUTOSAR NvM
RAM blocks.  With --m4-image (the user's own app-ipu1.xem4) the catalog
records, as their defaults, the NvM ROM defaults the M4 loads when its
EEPROM is blank, which is the state of every emulated RCC.
"""
import argparse
from pathlib import Path
import struct
import sys

MAGIC = "# MH2P RCC persistence catalog v1"
NS_TABLE = "nstPersis_New_NS_Table"
ADAPTATION_TABLE = "nstPersis_Adaptation_NS_Key_Table"
ADAPTATION_5F34_TABLE = "nstPersis_Adap_Key_Table_5F34"
NS_ADAPTATION = 0x5F22
NS_5F34 = 0x5F34
KEY_ENTRY = struct.Struct("<IIBBBBII")
# Value types nvdPersis_QDB_Read_For_Uart can return for a missing row.
TYPE_COPY = (3, 5)
TYPE_ZERO = 5
# M4 NvM block descriptor: id, EEPROM offset, length, RAM, ROM default, flags.
NVM_BLOCK = struct.Struct("<HHIIII")
# M4 namespace/key to NvM block map: block, pad, namespace, key, offset, size.
NVM_KEY = struct.Struct("<HHIIHH")
NVM_MIN_BLOCKS = 8


class Elf:
    """Just enough ELF32 little-endian parsing for symbols and data."""

    def __init__(self, data):
        if data[:6] != b"\x7fELF\x01\x01":
            raise ValueError("Not a 32-bit little-endian ELF file")
        self.data = data
        (shoff,) = struct.unpack_from("<I", data, 0x20)
        phoff = struct.unpack_from("<I", data, 0x1C)[0]
        phentsize, phnum, shentsize, shnum = struct.unpack_from(
            "<HHHH", data, 0x2A)
        self.segments = []
        for i in range(phnum):
            kind, offset, vaddr, _, filesz, memsz = struct.unpack_from(
                "<IIIIII", data, phoff + i * phentsize)
            if kind == 1:
                self.segments.append((vaddr, offset, filesz, memsz))
        sections = [struct.unpack_from("<IIIIIIIIII", data,
                                       shoff + i * shentsize)
                    for i in range(shnum)]
        self.symbols = {}
        for section in sections:
            if section[1] != 2:
                continue
            strtab = sections[section[6]]
            for off in range(section[4], section[4] + section[5], 16):
                name, value, size = struct.unpack_from("<III", data, off)
                end = data.index(b"\0", strtab[4] + name)
                symbol = data[strtab[4] + name:end].decode("latin1")
                if symbol:
                    self.symbols[symbol] = (value, size)

    def symbol(self, name):
        if name not in self.symbols:
            raise ValueError(f"Symbol {name} not found; is this PERSISTENCE?")
        return self.symbols[name]

    def loaded(self, address, size):
        return any(vaddr <= address and address + size <= vaddr + filesz
                   for vaddr, _, filesz, _ in self.segments)

    def read(self, address, size):
        for vaddr, offset, filesz, memsz in self.segments:
            if vaddr <= address and address + size <= vaddr + memsz:
                start = address - vaddr
                data = self.data[offset + start:offset + min(start + size,
                                                             filesz)]
                return data + bytes(size - len(data))
        raise ValueError(f"Address {address:#x} is not mapped")


def key_tables(elf):
    """Yield (namespace, key, owner, size) from the RCC namespace table."""
    table, table_size = elf.symbol(NS_TABLE)
    for row in range(table_size // 8):
        namespace, keys = struct.unpack("<II", elf.read(table + 8 * row, 8))
        index = 0
        while keys:
            key, callback, _, _, forwarded, _, size, _ = KEY_ENTRY.unpack(
                elf.read(keys + KEY_ENTRY.size * index, KEY_ENTRY.size))
            if not callback:
                break
            yield namespace, key, "m4" if forwarded else "rcc", size
            index += 1


def builtin_defaults(elf):
    """Return {(namespace, key): bytes} for the RCC's missing-row answers."""
    defaults = {}
    table, size = elf.symbol(ADAPTATION_TABLE)
    for off in range(table, table + size, 16):
        namespace, key, length, kind, pointer = struct.unpack(
            "<IIHHI", elf.read(off, 16))
        if namespace == NS_ADAPTATION and kind in TYPE_COPY:
            defaults[(namespace, key)] = elf.read(pointer, length)
    table, size = elf.symbol(ADAPTATION_5F34_TABLE)
    for off in range(table, table + size, 8):
        key, kind, length = struct.unpack("<IHH", elf.read(off, 8))
        if kind == TYPE_ZERO:
            defaults[(NS_5F34, key)] = bytes(length)
    return defaults


def nvm_blocks(m4):
    """Return {block id: ROM default bytes} from the M4 NvM descriptors.

    The descriptor table is located by shape: consecutive 20-byte entries
    numbered from 1 whose ROM default lies in the loaded image.
    """
    best = {}
    for vaddr, offset, filesz, _ in m4.segments:
        data = m4.data[offset:offset + filesz]
        for start in range(0, filesz - NVM_BLOCK.size, 4):
            blocks = {}
            pos = start
            while pos + NVM_BLOCK.size <= filesz:
                ident, _, length, _, rom, _ = NVM_BLOCK.unpack_from(data, pos)
                if ident != len(blocks) + 1 or not 0 < length < 0x10000 or \
                        not m4.loaded(rom, length):
                    break
                blocks[ident] = m4.read(rom, length)
                pos += NVM_BLOCK.size
            if len(blocks) > len(best):
                best = blocks
    if len(best) < NVM_MIN_BLOCKS:
        raise ValueError("No NvM block table found; is this the RCC M4 "
                         "app-ipu1.xem4?")
    return best


def m4_defaults(m4, m4_keys):
    """Return {(namespace, key): bytes} the M4 serves from blank NvM.

    @m4_keys are the keys the RCC forwards to the M4; the key map is the
    run of structurally valid 16-byte entries naming most of them.
    """
    blocks = nvm_blocks(m4)
    best = {}
    for vaddr, offset, filesz, _ in m4.segments:
        data = m4.data[offset:offset + filesz]
        for start in range(0, filesz - NVM_KEY.size, 4):
            found = {}
            pos = start
            while pos + NVM_KEY.size <= filesz:
                block, pad, namespace, key, off, size = NVM_KEY.unpack_from(
                    data, pos)
                rom = blocks.get(block)
                if pad or not namespace or rom is None or not size:
                    break
                # The M4 reads a few overlong entries from other RAM instead.
                if (namespace, key) in m4_keys and off + size <= len(rom):
                    found[(namespace, key)] = rom[off:off + size]
                pos += NVM_KEY.size
            if len(found) > len(best):
                best = found
    if not best:
        raise ValueError("No M4 NvM key map found for the RCC's M4 keys")
    return best


def catalog(elf, provision_rcc=False, m4=None):
    defaults = builtin_defaults(elf)
    if m4 is not None:
        m4_keys = {(namespace, key) for namespace, key, owner, _
                   in key_tables(elf) if owner == "m4"}
        m4_values = m4_defaults(m4, m4_keys)
    lines = [MAGIC,
             "# namespace key owner size [default]; generated from the RCC "
             "PERSISTENCE application"]
    if provision_rcc:
        lines.append("# --provision-rcc: RCC-owned keys without a built-in "
                     "default read as zeros (emulated provisioning)")
    if m4 is not None:
        lines.append(f"# --m4-image: {len(m4_values)} M4 keys read as the "
                     "M4's blank-EEPROM NvM ROM defaults")
    for namespace, key, owner, size in key_tables(elf):
        line = f"0x{namespace:x} 0x{key:x} {owner} {size}"
        # Forwarded keys never reach the RCC's missing-row defaults.
        if owner == "rcc":
            default = defaults.get((namespace, key))
        elif m4 is not None:
            default = m4_values.get((namespace, key))
        else:
            default = None
        if default is None and provision_rcc and owner == "rcc":
            default = bytes(size)
        if default is not None:
            line = f"0x{namespace:x} 0x{key:x} {owner} {len(default)} " \
                   f"{default.hex()}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("persistence", type=Path,
                        help="RCC /App/PERSISTENCE ELF")
    parser.add_argument("output", type=Path, help="catalog file to write")
    parser.add_argument("--provision-rcc", action="store_true",
                        help="zero-fill RCC-owned keys without a default")
    parser.add_argument("--m4-image", type=Path,
                        help="RCC M4 app-ipu1.xem4 supplying the defaults "
                             "of keys the RCC forwards to its M4")
    args = parser.parse_args(argv)
    try:
        m4 = Elf(args.m4_image.read_bytes()) if args.m4_image else None
        text = catalog(Elf(args.persistence.read_bytes()), args.provision_rcc,
                       m4)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    args.output.write_text(text)
    keys = sum(1 for line in text.splitlines() if not line.startswith("#"))
    print(f"{args.output}: {keys} keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
