#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Wrap the unchanged P2865 SubCPU image for CPU-only RL78 investigation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


IMAGE_SIZE = 131088
IMAGE_SHA256 = (
    "f2c51ec598c4c3886263951b77e6a333203e9c7d35e77a5a55dafd1f8db0242d")
PAYLOAD_OFFSET = 0x1000


def prepare(source, output):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if source.stat().st_size != IMAGE_SIZE:
        raise ValueError("Expected the original 131088-byte P2865 SubCPU image")
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != IMAGE_SHA256:
        raise ValueError("Unsupported SubCPU SHA-256; expected P2865")
    entry = struct.unpack_from("<H", raw)[0]
    if entry != 0xd8 or raw[entry:entry + 2] != b"\x61\xcf":
        raise ValueError(
            "Unexpected SubCPU reset vector or register-bank select")

    names = b"\0.text\0.shstrtab\0"
    names_offset = PAYLOAD_OFFSET + len(raw)
    sections_offset = (names_offset + len(names) + 3) & ~3
    elf = bytearray(sections_offset + 3 * 40)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    # EM_RL78, with no MCU-variant flags or invented initial register values.
    struct.pack_into("<16sHHIIIIIHHHHHH", elf, 0, ident, 2, 197, 1,
                     entry, 52, sections_offset, 0, 52, 32, 1, 40, 3, 2)
    struct.pack_into("<8I", elf, 52, 1, PAYLOAD_OFFSET, 0, 0,
                     len(raw), len(raw), 5, 0x1000)
    elf[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(raw)] = raw
    elf[names_offset:names_offset + len(names)] = names
    struct.pack_into("<10I", elf, sections_offset + 40,
                     1, 1, 6, 0, PAYLOAD_OFFSET, len(raw), 0, 0, 1, 0)
    struct.pack_into("<10I", elf, sections_offset + 80,
                     7, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0)

    output.mkdir(parents=True)
    image = output / "subcpu.elf"
    with image.open("xb") as stream:
        stream.write(elf)
    metadata = {
        "firmware": "MH2p_ER_POG35_P2865",
        "source": str(source.resolve()),
        "source_sha256": IMAGE_SHA256,
        "source_bytes": len(raw),
        "elf_sha256": hashlib.sha256(elf).hexdigest(),
        "elf_machine": "EM_RL78",
        "entry_from_original_reset_vector": hex(entry),
        "payload_file_offset": PAYLOAD_OFFSET,
        "diagnostic_flat_load_address": 0,
        "all_source_bytes_preserved": True,
        "exact_mcu_variant_verified": False,
        "board_memory_map_verified": False,
        "subcpu_board_boot_verified": False,
        "rstp_verified": False,
        "note": (
            "ELF transport only, not a QEMU SubCPU machine or board boot. "
            "Every original byte, including the final 16 bytes, is preserved "
            "in a flat diagnostic load segment. Trailer meaning and actual "
            "chip placement are not established. No peripheral, EEPROM, "
            "wakeup event or protocol reply is supplied. A generic RL78 "
            "simulator's default peripheral values are not board evidence."),
    }
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.image, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"MH2P SubCPU ELF preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
