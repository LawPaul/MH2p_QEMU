#!/usr/bin/env python3
"""Prepare a stock P2865 variant-22 QNX startup payload for local emulation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


LOADIMG_SHA256 = "b22c54ad94c664786ea8f0502fb8795787a45b59f1fc25c488cc081f58fdf9e6"
DTB_SHA256 = "dea94a02537dff8db75a1b5cfbf3ce4c8d4fe2579c95bd0812722c28b5e8f5c5"
MAX_IMAGE_SIZE = 32 * 1024 * 1024
LOAD_ADDRESS = 0x80008000
STARTUP_MAGIC = 0x00FF7EEB


def decode_loadimg(raw):
    if len(raw) < 2048 or raw[:8] != b"ANDROID!":
        raise ValueError("Expected an Android-style loadimg wrapper")
    stored_size = struct.unpack_from("<I", raw, 8)[0]
    page_size = struct.unpack_from("<I", raw, 36)[0]
    if page_size != 2048 or not 0 < stored_size <= len(raw) - page_size:
        raise ValueError("Invalid loadimg payload bounds")
    inflater = zlib.decompressobj()
    image = inflater.decompress(raw[page_size:page_size + stored_size], MAX_IMAGE_SIZE)
    if not inflater.eof or inflater.unconsumed_tail:
        raise ValueError("Truncated or oversized loadimg zlib stream")
    if len(image) < 0x108:
        raise ValueError("Truncated QNX startup header")
    magic, version, flags, _, header_size, machine = struct.unpack_from("<IHBBHH", image, 8)
    entry, _, address, _, _, startup_size, image_size, _, fs_size = struct.unpack_from(
        "<9I", image, 20
    )
    if (magic != STARTUP_MAGIC or version != 1 or machine != 40
            or flags != 1 or header_size != 256 or address != LOAD_ADDRESS + 8):
        raise ValueError("Unsupported QNX startup header")
    if (image_size != len(image) - 8 or not header_size <= startup_size < image_size
            or not LOAD_ADDRESS + 8 <= entry < LOAD_ADDRESS + 8 + startup_size):
        raise ValueError("Invalid QNX startup extents or entry point")
    fs_offset = 8 + startup_size
    fs = image[fs_offset:]
    if len(fs) != fs_size or fs[:8] != b"imagefs\x04":
        raise ValueError("Invalid startup ImageFS")
    if fs_size % 4 or sum(x[0] for x in struct.iter_unpack("<I", fs)) & 0xFFFFFFFF:
        raise ValueError("Startup ImageFS checksum mismatch")
    return image, {
        "load_address": LOAD_ADDRESS,
        "entry": entry,
        "imagefs_offset": fs_offset,
        "imagefs_size": fs_size,
        "wrapper_trailing_bytes": len(inflater.unused_data),
    }


def diagnostic_shell(image, fs_offset):
    """Replace only the pinned image's startup script and ImageFS checksum."""
    fs_size, directory_end, cursor = struct.unpack_from("<III", image, fs_offset + 8)
    if not 88 <= cursor <= directory_end <= fs_size - 4:
        raise ValueError("Invalid ImageFS directory")
    matches = []
    while cursor < directory_end:
        entry = fs_offset + cursor
        length = struct.unpack_from("<H", image, entry)[0]
        if not length:
            break
        if length < 24 or length % 4 or cursor + length > directory_end:
            raise ValueError("Invalid ImageFS directory entry")
        mode = struct.unpack_from("<I", image, entry + 8)[0]
        if mode & 0o170000 == 0o100000:
            if length < 33:
                raise ValueError("Truncated ImageFS file entry")
            offset, size = struct.unpack_from("<II", image, entry + 24)
            name_end = image.find(b"\0", entry + 32, entry + length)
            if name_end < 0 or offset + size > fs_size - 4:
                raise ValueError("Invalid ImageFS file bounds")
            if image[entry + 32:name_end] == b"usr/sbin/main_stage1.sh":
                matches.append((fs_offset + offset, size))
        cursor += length
    if len(matches) != 1:
        raise ValueError("Expected exactly one main_stage1.sh")
    offset, size = matches[0]
    script = (
        b"#!/bin/ksh\n"
        b"echo 'MH2P diagnostic shell: normal services are NOT started.'\n"
        b"export PS1='mh2p# '\n"
        b"exec /bin/ksh -i\n"
    )
    if len(script) > size:
        raise ValueError("Diagnostic shell script does not fit")
    patched = bytearray(image)
    patched[offset:offset + size] = script.ljust(size, b"\n")
    checksum_offset = fs_offset + fs_size - 4
    total = sum(x[0] for x in struct.iter_unpack(
        "<I", patched[fs_offset:checksum_offset]
    ))
    struct.pack_into("<I", patched, checksum_offset, -total & 0xFFFFFFFF)
    return bytes(patched), {"path": "usr/sbin/main_stage1.sh", "offset": offset,
                            "size": size, "checksum_offset": checksum_offset}


def prepare(source, dtb, output, shell=False):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if source.stat().st_size > MAX_IMAGE_SIZE:
        raise ValueError("Input exceeds the supported image size")
    raw = source.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != LOADIMG_SHA256:
        raise ValueError(f"Unsupported loadimg SHA-256: {digest}; expected P2865 variant 22")
    if dtb.stat().st_size > 1024 * 1024:
        raise ValueError("Device tree exceeds size limit")
    tree = dtb.read_bytes()
    if hashlib.sha256(tree).hexdigest() != DTB_SHA256:
        raise ValueError("Expected P2865 board 699-61857-5017-202 Quickboot DTB 8")
    magic, tree_size = struct.unpack_from(">II", tree)
    if magic != 0xD00DFEED or not 40 <= tree_size <= len(tree):
        raise ValueError("Invalid FDT header")
    image, metadata = decode_loadimg(raw)
    patches = []
    if shell:
        image, patch = diagnostic_shell(image, metadata["imagefs_offset"])
        patches.append(patch)
    output.mkdir(parents=True)
    (output / "startup.bin").write_bytes(image)
    (output / "board.dtb").write_bytes(tree[:tree_size])
    metadata.update({
        "firmware": "MH2p_ER_POG35_P2865",
        "variant": "22",
        "source": str(source.resolve()),
        "source_sha256": digest,
        "startup_sha256": hashlib.sha256(image).hexdigest(),
        "startup_bytes": len(image),
        "native_executables_patched": False,
        "diagnostic_shell": shell,
        "script_patches": patches,
        "dtb_source_sha256": DTB_SHA256,
        "dtb_sha256": hashlib.sha256(tree[:tree_size]).hexdigest(),
        "dtb_bytes": tree_size,
    })
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loadimg", type=Path, required=True)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--diagnostic-shell", action="store_true",
                        help="replace main_stage1.sh with an interactive shell")
    args = parser.parse_args()
    try:
        result = prepare(args.loadimg, args.dtb, args.output, args.diagnostic_shell)
    except (OSError, ValueError, zlib.error) as exc:
        parser.exit(1, f"MH2P preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
