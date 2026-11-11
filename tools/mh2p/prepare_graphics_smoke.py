#!/usr/bin/env python3
"""Add private smoke-test resources to a separate diagnostic startup image."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from prepare import LOAD_ADDRESS, MAX_IMAGE_SIZE, STARTUP_MAGIC

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mhi2/glforward"))
from ifs_resource import add_file


def file_entries(fs):
    if len(fs) < 96:
        raise ValueError("Truncated ImageFS directory")
    end, cursor = struct.unpack_from("<II", fs, 12)
    entries = {}
    if not 88 <= cursor <= end <= len(fs) - 4 or (cursor | end) % 4:
        raise ValueError("Invalid ImageFS directory bounds")
    while cursor < end:
        length = struct.unpack_from("<H", fs, cursor)[0]
        if not length:
            return entries
        if length < 24 or length % 4 or cursor + length > end:
            raise ValueError("Invalid ImageFS directory record")
        if struct.unpack_from("<I", fs, cursor + 8)[0] & 0o170000 == 0o100000:
            name_end = fs.find(b"\0", cursor + 32, cursor + length)
            if length < 33 or name_end < 0:
                raise ValueError("Invalid ImageFS file name")
            name = fs[cursor + 32:name_end].decode()
            offset, size = struct.unpack_from("<II", fs, cursor + 24)
            if name in entries or offset < end or offset + size > len(fs) - 4:
                raise ValueError("Invalid ImageFS file extent")
            entries[name] = (offset, size)
        cursor += length
    raise ValueError("Missing ImageFS directory terminator")


def relocate_xip(original, expanded, physical_base):
    old_entries = file_entries(original)
    new_entries = file_entries(expanded)
    result = bytearray(expanded)
    for name, (old, size) in old_entries.items():
        new, new_size = new_entries[name]
        data = original[old:old + size]
        if new_size != size or result[new:new + size] != data:
            raise ValueError(f"Existing file payload changed: {name}")
        if data[:7] != b"\x7fELF\x01\x01\x01":
            continue
        if len(data) < 52:
            raise ValueError(f"Truncated ELF header: {name}")
        phoff = struct.unpack_from("<I", data, 28)[0]
        stride, count = struct.unpack_from("<HH", data, 42)
        if stride < 32 or phoff + stride * count > size:
            raise ValueError(f"Invalid ELF program headers: {name}")
        for i in range(count):
            at = phoff + i * stride
            kind, file_offset, _, paddr = struct.unpack_from("<4I", data, at)
            # mkifs XIP addresses name bytes inside the physical startup image.
            if kind == 1 and paddr == physical_base + old + file_offset:
                struct.pack_into("<I", result, new + at + 12, paddr + new - old)
    total = sum(x[0] for x in struct.iter_unpack("<I", result[:-4]))
    struct.pack_into("<I", result, len(result) - 4, -total & 0xffffffff)
    return bytes(result)


def add_resources(image, resources):
    if len(image) < 264 or struct.unpack_from("<I", image, 8)[0] != STARTUP_MAGIC:
        raise ValueError("Invalid QNX startup header")
    ram_size, startup_size, stored_size, fs_address, fs_size = struct.unpack_from(
        "<5I", image, 36
    )
    offset = startup_size + 8
    if (stored_size != len(image) - 8 or ram_size != stored_size or fs_address != 0
            or offset < 264 or offset % 4 or offset + fs_size != len(image)):
        raise ValueError("Unsupported QNX startup extents")
    if sum(x[0] for x in struct.iter_unpack("<I", image[8:offset])) & 0xffffffff:
        raise ValueError("Invalid source startup checksum")
    fs = image[offset:]
    if (fs[:8] != b"imagefs\x04" or len(fs) < 96 or len(fs) % 4 or
            struct.unpack_from("<I", fs, 8)[0] != len(fs) or
            sum(x[0] for x in struct.iter_unpack("<I", fs)) & 0xffffffff):
        raise ValueError("Invalid source ImageFS checksum or header")
    file_entries(fs)
    original = fs
    for name, payload in resources:
        fs = add_file(fs, name, payload)
    fs = relocate_xip(original, fs, LOAD_ADDRESS + offset)
    result = bytearray(image[:offset]) + fs
    if len(result) > MAX_IMAGE_SIZE:
        raise ValueError("Expanded startup exceeds supported size")
    struct.pack_into("<I", result, 36, len(result) - 8)
    struct.pack_into("<I", result, 44, len(result) - 8)
    struct.pack_into("<I", result, 52, len(fs))
    total = sum(x[0] for x in struct.iter_unpack("<I", result[8:offset - 4]))
    struct.pack_into("<I", result, offset - 4, -total & 0xffffffff)
    return bytes(result), offset


def prepare(media, build, output, native_display_probe=False):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    manifest = json.loads((media / "manifest.json").read_text())
    if (not manifest["diagnostic_shell"] or manifest.get("graphics_smoke")
            or manifest.get("native_display_probe")):
        raise ValueError("Expected ordinary diagnostic-shell media")
    startup = (media / "startup.bin").read_bytes()
    dtb = (media / "board.dtb").read_bytes()
    for data, field in ((startup, "startup_sha256"), (dtb, "dtb_sha256")):
        if hashlib.sha256(data).hexdigest() != manifest[field]:
            raise ValueError(f"Source media hash mismatch: {field}")
    resources = []
    selected = (
        ("sbin/mh2p-egl-smoke", "mh2p-egl-smoke", 2),
        ("lib/libmh2p-egl-smoke.so", "libmh2p-egl-smoke.so", 3),
    )
    if native_display_probe:
        selected = (("sbin/mh2p-native-display-probe",
                     "mh2p-native-display-probe", 2),)
    for target, filename, kind in selected:
        payload = (build / filename).read_bytes()
        if (len(payload) < 52 or payload[:7] != b"\x7fELF\x01\x01\x01" or
                struct.unpack_from("<HH", payload, 16) != (kind, 40) or
                struct.unpack_from("<I", payload, 36)[0] & 0xff000400 != 0x05000000):
            raise ValueError(f"Expected soft-float ARM ELF: {filename}")
        resources.append((target, payload))
    modified, offset = add_resources(startup, resources)
    manifest.update({
        "native_display_probe" if native_display_probe else "graphics_smoke": True,
        "graphics_libraries_replaced": False,
        "smoke_parent_startup_sha256": manifest["startup_sha256"],
        "startup_sha256": hashlib.sha256(modified).hexdigest(),
        "startup_bytes": len(modified),
        "imagefs_offset": offset, "imagefs_size": len(modified) - offset,
        "startup_checksum_offset": offset - 4,
        "xip_program_headers_relocated": True,
        "native_executables_patched": True,
        "native_executable_instructions_patched": False,
        "added_resources": [
            {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in resources
        ],
    })
    # Original script extents move when the ImageFS directory grows.
    manifest["parent_script_patches"] = manifest.pop("script_patches", [])
    output.mkdir(parents=True)
    (output / "startup.bin").write_bytes(modified)
    (output / "board.dtb").write_bytes(dtb)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--native-display-probe", action="store_true")
    args = parser.parse_args()
    print(json.dumps(prepare(args.media, args.build, args.output,
                             args.native_display_probe), indent=2))


if __name__ == "__main__":
    main()
