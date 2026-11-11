#!/usr/bin/env python3
"""Build an incomplete experimental NOR image from pinned P2865 partitions.

Stage two is required. The original SYSTEM (/mnt/system) and PERSIST
(/mnt/persist) flash filesystem images may be added at the offsets from
their OTA installer.txt files.

The original Quickboot BCT may also be added at offset 0. Its customer data
points at the QuickBoot partition table (PT) region, so a factory-style PT is
then generated there. The OTA does not ship a PT, which the factory creates.
The generated PT only describes regions whose use is known (the BCT, the PT,
and the EARLY_PERSIST_RAW store that ooc runmode-mgr opens for EPS).

An authorized full NOR capture may instead supply the base. This preserves
every byte outside explicitly replaced images, including its BCT and PT.
Capture-based output is private and does not establish valid provisioning.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import algorithms


NOR_SIZE = 64 * 1024 * 1024
STAGE2_OFFSET = 18087936
STAGE2_SIZE = 32505856
STAGE2_SHA256 = "5e4619ba331ae87cc7f99afe0e1c742bc05fe7f68fbc9ec18042f79b290cdf4e"
# OTA MMX2P.efs-system and MMX2P.efs-persist installer.txt ImageOffset/Length.
PARTITIONS = {
    "system": (52166656, 2097152,
               "433ac815d4e5ea73eca0c7489d763076b747615cb93af481e95c2d6de6f61c21"),
    "persist": (54263808, 8388608,
                "be637a6575e7f866385533762e5ebb2561fd8360b100d7d5a335c8c6517966a6"),
}

# OTA MMX2P.quickboot variant 22 nvflash/alpinesecureboot/
# 699-61857-5017-202_3398/BCT_signed.bct (non-secure, RSA-PSS signed with its
# embedded modulus).
BCT_SIZE = 8192
BCT_SHA256 = "2013b9bfa0daeeffba5a1ce4a673035ac7c12dc618ff4c2b61a346d8dec8525f"
BCT_CUSTOMER_DATA = 0x420

PT_MAGIC = 0xFFFFFFFF8F9E8D8B
PT_VERSION = 0x100
PT_HEADER_SIZE = 72
PT_ENTRY_SIZE = 120
# Quickboot's region gap between DTB_PRIMARY (ends 0x1080000) and MAIN_STAGE2
# (0x1140000) per the OTA installers. The factory location is not recorded in
# the OTA. runmode-mgr uses two banks of 128 KiB erase blocks (bank 1 at half).
EARLY_PERSIST_OFFSET = 0x1080000
EARLY_PERSIST_SIZE = 0x40000


def cmac_zero_key(data):
    """Non-secure Tegra AES-128-CMAC as used by QbPartMgrLoadTable."""
    mac = cmac.CMAC(algorithms.AES(bytes(16)))
    mac.update(data)
    return mac.finalize()


def bct_partition_table_region(bct):
    _, offset, size = struct.unpack_from("<III", bct, BCT_CUSTOMER_DATA)
    if offset % 0x20000 or not 0 < size <= 0x100000 or offset < BCT_SIZE:
        raise ValueError("BCT customer data has no usable partition table region")
    return offset, size


def partition_table(entries):
    """Serialize a QbPartMgr table: header, 120-byte entries, zero-key CMAC."""
    size = PT_HEADER_SIZE + PT_ENTRY_SIZE * len(entries)
    size = (size + 15) & ~15
    table = bytearray(size)
    struct.pack_into("<QII", table, 0, PT_MAGIC, PT_VERSION, size)
    struct.pack_into("<QIIII", table, 48, PT_MAGIC, PT_VERSION, size,
                     len(entries), 0)
    for index, (pid, name, start, length) in enumerate(entries):
        encoded = name.encode("ascii")
        if pid == 0 or len(encoded) > 20:
            raise ValueError(f"Invalid partition entry {name}")
        entry = PT_HEADER_SIZE + index * PT_ENTRY_SIZE
        struct.pack_into("<I24sII", table, entry, pid, encoded, 0, 0)
        struct.pack_into("<IIQQQQII", table, entry + 72, 0, 0, start, length,
                         start, start + length - 1, 0, 0)
    table[16:32] = cmac_zero_key(bytes(table[32:]))
    return bytes(table)


def prepare(source, output, partitions=None, bct=None, base_nor=None):
    partitions = partitions or {}
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    base = None
    if base_nor is not None:
        if bct is not None:
            raise ValueError("A base NOR capture must retain its original BCT "
                             "and partition table; do not combine with --bct")
        if base_nor.stat().st_size != NOR_SIZE:
            raise ValueError("Expected a complete 67,108,864-byte base NOR capture")
        base = base_nor.read_bytes()
        if len(base) != NOR_SIZE:
            raise ValueError("Base NOR capture changed size while reading")
    if source.stat().st_size != STAGE2_SIZE:
        raise ValueError("Expected the 32,505,856-byte P2865 NOR stage-two prefix")
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != STAGE2_SHA256:
        raise ValueError("Unsupported stage-two SHA-256; expected P2865")
    extra = {}
    for name, path in partitions.items():
        offset, size, digest = PARTITIONS[name]
        if path.stat().st_size != size:
            raise ValueError(f"Expected the {size:,}-byte P2865 {name} image")
        payload = path.read_bytes()
        if hashlib.sha256(payload).hexdigest() != digest:
            raise ValueError(f"Unsupported {name} SHA-256; expected P2865")
        extra[name] = (path, offset, payload)
    if bct is not None:
        payload = bct.read_bytes()
        if len(payload) != BCT_SIZE:
            raise ValueError(f"Expected the {BCT_SIZE:,}-byte P2865 BCT")
        if hashlib.sha256(payload).hexdigest() != BCT_SHA256:
            raise ValueError("Unsupported BCT SHA-256; expected P2865 5017-202")
        pt_offset, pt_size = bct_partition_table_region(payload)
        table = partition_table((
            (1, "BCT", 0, pt_offset),
            (2, "PT", pt_offset, pt_size),
            (3, "EARLY_PERSIST_RAW", EARLY_PERSIST_OFFSET, EARLY_PERSIST_SIZE),
        ))
        extra["bct"] = (bct, 0, payload)
        extra["partition_table"] = (None, pt_offset, table)
    output.mkdir(parents=True, mode=0o700 if base is not None else 0o777)
    image = output / "nor.bin"
    with os.fdopen(os.open(image, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                           0o600 if base is not None else 0o666), "wb") as stream:
        if base is not None:
            stream.write(base)
        else:
            erased = b"\xff" * (1024 * 1024)
            for _ in range(NOR_SIZE // len(erased)):
                stream.write(erased)
        stream.seek(STAGE2_OFFSET)
        stream.write(data)
        for _, offset, payload in extra.values():
            stream.seek(offset)
            stream.write(payload)
    metadata = {
        "firmware": "MH2p_ER_POG35_P2865",
        "source": str(source.resolve()),
        "source_sha256": STAGE2_SHA256,
        "stage2_offset": STAGE2_OFFSET,
        "stage2_bytes": STAGE2_SIZE,
        "nor_bytes": NOR_SIZE,
        "nor_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "incomplete_flash": True,
        "note": ("Only stage two" + "".join(f" and {n}" for n in extra) +
                 (" are" if extra else " is") +
                 " populated. All other flash bytes are erased (0xff)."),
    }
    for name, (path, offset, payload) in extra.items():
        metadata[name] = {
            "source": str(path.resolve()) if path else "generated",
            "sha256": hashlib.sha256(payload).hexdigest(),
            "offset": offset,
            "bytes": len(payload),
        }
    if bct is not None:
        metadata["early_persist_raw"] = {
            "offset": EARLY_PERSIST_OFFSET,
            "bytes": EARLY_PERSIST_SIZE,
            "note": "erased; runmode-mgr formats it on first EPS write",
        }
    if base is not None:
        metadata["unreplaced_base_bytes_preserved"] = True
        metadata["base_nor"] = {
            "source": str(base_nor.resolve()),
            "sha256": hashlib.sha256(base).hexdigest(),
            "bytes": len(base),
        }
        metadata["provisioning_verified"] = False
        metadata["note"] = (
            "64 MiB base NOR copied; only stage two" +
            "".join(f" and {n}" for n in extra) +
            (" are" if extra else " is") +
            " replaced. All other bytes, including the original BCT and "
            "partition table, are preserved. Capture compatibility, secure "
            "storage decryption, flash completeness and unit provisioning "
            "are not verified.")
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--system", type=Path,
                        help="original efs-system.img for /mnt/system")
    parser.add_argument("--persist", type=Path,
                        help="original efs-persist.img for /mnt/persist")
    parser.add_argument("--bct", type=Path,
                        help="original 699-61857-5017-202 BCT_signed.bct; "
                             "also generates the partition table")
    parser.add_argument("--base-nor", type=Path,
                        help="authorized full 64 MiB unit NOR capture; preserve "
                             "all bytes outside explicitly replaced images, "
                             "incompatible with --bct; output is private")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    partitions = {name: path for name, path in
                  (("system", args.system), ("persist", args.persist))
                  if path is not None}
    try:
        result = prepare(args.stage2, args.output, partitions, args.bct,
                         args.base_nor)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"MH2P NOR preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
