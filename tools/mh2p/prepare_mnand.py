#!/usr/bin/env python3
"""Prepare experimental MH2P mNAND disks without modifying source media."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

sys.path.append(str(Path(__file__).resolve().parents[1] / "mhi2"))
from sparse_copy import copy_sparse


APP_SIZE = 2147418112
APP_SHA256 = "f61eec3f223ccbd0a1e097611fe8aec1ef8785f24f43a0868e58a18aee60c996"
DISK_SIZE = 4 * 1024**3
EXT_SECTOR = 2048
APP_SECTOR = 4096
CRASH_SECTOR = (DISK_SIZE - 64 * 1024**2) // 512
MEDIA_VOLUME_SECTORS = 64 * 1024**2 // 512


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def partition(kind, start, count):
    return struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", kind,
                       b"\xfe\xff\xff", start, count)


def prepare(source, output):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if source.stat().st_size != APP_SIZE or digest(source) != APP_SHA256:
        raise ValueError("Expected the original P2865 app.img size and SHA-256")
    output.mkdir(parents=True)
    image = output / "mnand.bin"
    with image.open("xb") as stream:
        stream.truncate(DISK_SIZE)
        mbr = bytearray(512)
        mbr[446:462] = partition(177, CRASH_SECTOR, DISK_SIZE // 512 - CRASH_SECTOR)
        mbr[462:478] = partition(5, EXT_SECTOR, CRASH_SECTOR - EXT_SECTOR)
        mbr[510:] = b"\x55\xaa"
        stream.write(mbr)
        ebr = bytearray(512)
        ebr[446:462] = partition(177, APP_SECTOR - EXT_SECTOR, APP_SIZE // 512)
        ebr[510:] = b"\x55\xaa"
        stream.seek(EXT_SECTOR * 512)
        stream.write(ebr)
        stream.seek(APP_SECTOR * 512)
        with source.open("rb") as app:
            shutil.copyfileobj(app, stream, 1024 * 1024)
    metadata = {
        "firmware": "MH2p_ER_POG35_P2865",
        "source": str(source.resolve()), "source_sha256": APP_SHA256,
        "disk_bytes": DISK_SIZE, "app_sector": APP_SECTOR, "app_bytes": APP_SIZE,
        "crash_sector": CRASH_SECTOR, "crash_filesystem_present": False,
        "mnand_sha256": digest(image),
        "incomplete_disk": True,
        "note": "Synthetic layout: logical app plus unformatted primary crashlog partition.",
    }
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def extend_media(source, output):
    """Extend only the three-volume diagnostic layout, not a real unit's disk."""
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if source.stat().st_size != DISK_SIZE:
        raise ValueError("Expected a 4 GiB three-volume diagnostic disk")
    old_volumes = []
    with source.open("rb") as stream:
        mbr = stream.read(512)
        if (mbr[510:] != b"\x55\xaa" or
                mbr[446:462] != partition(
                    177, CRASH_SECTOR, DISK_SIZE // 512 - CRASH_SECTOR) or
                mbr[462:478] != partition(
                    5, EXT_SECTOR, CRASH_SECTOR - EXT_SECTOR) or
                any(mbr[478:510])):
            raise ValueError("Unsupported diagnostic primary partition layout")
        at = EXT_SECTOR
        for index in range(1, 4):
            stream.seek(at * 512)
            ebr = stream.read(512)
            relative, count = struct.unpack_from("<II", ebr, 454)
            start, end = at + relative, at + relative + count
            if (ebr[510:] != b"\x55\xaa" or relative < 1 or not count or
                    ebr[446:462] != partition(177, relative, count) or
                    end > CRASH_SECTOR or any(ebr[478:510])):
                raise ValueError("Invalid diagnostic logical partition")
            old_volumes.append({"logical": index, "ebr_sector": at,
                                "start_sector": start, "sectors": count})
            if index == 3:
                if any(ebr[462:478]):
                    raise ValueError("Expected exactly three logical volumes")
            else:
                link, link_count = struct.unpack_from("<II", ebr, 470)
                next_ebr = EXT_SECTOR + link
                if (ebr[462:478] != partition(5, link, link_count) or
                        next_ebr < end or next_ebr >= CRASH_SECTOR or
                        not link_count or next_ebr + link_count > CRASH_SECTOR):
                    raise ValueError("Invalid diagnostic extended partition link")
                at = next_ebr
        next_ebr = (end + 2047) // 2048 * 2048
        extended_end = next_ebr + 8 * (2048 + MEDIA_VOLUME_SECTORS)
        if extended_end > CRASH_SECTOR:
            raise ValueError("Insufficient free space for diagnostic media volumes")
        stream.seek(end * 512)
        remaining = (extended_end - end) * 512
        while remaining:
            data = stream.read(min(remaining, 1024 * 1024))
            if not data or any(data):
                raise ValueError("Diagnostic extension area is not empty")
            remaining -= len(data)

    source_hash = digest(source)
    output.mkdir(parents=True)
    image = output / "mnand.bin"
    copy_sparse(source, image)
    volumes = []
    with image.open("r+b") as stream:
        for index in range(4, 12):
            stream.seek(at * 512 + 462)
            stream.write(partition(
                5, next_ebr - EXT_SECTOR, CRASH_SECTOR - next_ebr))
            ebr = bytearray(512)
            ebr[446:462] = partition(177, 2048, MEDIA_VOLUME_SECTORS)
            ebr[510:] = b"\x55\xaa"
            stream.seek(next_ebr * 512)
            stream.write(ebr)
            volumes.append({"logical": index, "ebr_sector": next_ebr,
                            "start_sector": next_ebr + 2048,
                            "sectors": MEDIA_VOLUME_SECTORS, "formatted": False})
            at = next_ebr
            next_ebr += 2048 + MEDIA_VOLUME_SECTORS
    metadata = {
        "source": str(source.resolve()), "source_sha256": source_hash,
        "mnand_sha256": digest(image),
        "preserved_logical_partitions": old_volumes,
        "empty_diagnostic_volumes": volumes,
        "incomplete_disk": True, "original_physical_layout": False,
        "factory_values_supplied": False,
        "note": "Empty diagnostic slots 4 through 11; no original volume data "
                "or sizes supplied. Native applications may format these volumes.",
    }
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--app", type=Path)
    source.add_argument("--extend-media", type=Path,
                        help="Extend a three-volume diagnostic disk with empty "
                             "64 MiB logical volumes through slot 11")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = (extend_media(args.extend_media, args.output)
                  if args.extend_media else prepare(args.app, args.output))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"MH2P mNAND preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
