#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebuild local graphics adapters in a separate, existing diagnostic IFS."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess

from prepare import MAX_IMAGE_SIZE
from prepare_graphics_smoke import add_resources, file_entries

ROOT = Path(__file__).resolve().parents[2]
MODULES = {
    "lib/screen-mh2p-cpu.so": "tools/mh2p/screen_cpu_fill.c",
    "lib/libmh2p-screen.so": "tools/mhi2/glforward/guest/gl_shim.c",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dependencies(depfile):
    names = shlex.split(depfile.read_text().replace("\\\n", "").partition(":")[2])
    if not names:
        raise ValueError(f"Missing compiler dependencies: {depfile.name}")
    return {Path(name).resolve().relative_to(ROOT) for name in names}


def replace_resources(source, replacements):
    _, offset = add_resources(source, [])
    original = source[offset:]
    entries = file_entries(original)
    if not replacements or not replacements.keys() <= MODULES.keys():
        raise ValueError("Only the two diagnostic graphics adapters may be replaced")
    if not replacements.keys() <= entries.keys():
        raise ValueError("Source must already contain the diagnostic graphics adapters")
    fs = bytearray(original[:-4])
    locations = {}
    for name, payload in replacements.items():
        if (len(payload) < 52 or payload[:7] != b"\x7fELF\x01\x01\x01" or
                struct.unpack_from("<HH", payload, 16) != (3, 40) or
                struct.unpack_from("<I", payload, 36)[0] & 0xff000400 != 0x05000000):
            raise ValueError(f"Expected soft-float ARM shared library: {name}")
        fs.extend(bytes((-len(fs)) % 4096))
        locations[name] = len(fs), len(payload)
        fs.extend(payload)
    fs.extend(bytes((-len(fs)) % 4) + bytes(4))
    cursor = struct.unpack_from("<I", fs, 16)[0]
    while (length := struct.unpack_from("<H", fs, cursor)[0]):
        if struct.unpack_from("<I", fs, cursor + 8)[0] & 0o170000 == 0o100000:
            name = fs[cursor + 32:cursor + length].split(b"\0")[0].decode()
            if name in locations:
                struct.pack_into("<II", fs, cursor + 24, *locations[name])
        cursor += length
    struct.pack_into("<I", fs, 8, len(fs))
    total = sum(word[0] for word in struct.iter_unpack("<I", fs[:-4]))
    struct.pack_into("<I", fs, len(fs) - 4, -total & 0xffffffff)
    image = bytearray(source[:offset]) + fs
    if len(image) > MAX_IMAGE_SIZE:
        raise ValueError("Copied startup exceeds supported image size")
    for field in (36, 44):
        struct.pack_into("<I", image, field, len(image) - 8)
    struct.pack_into("<I", image, 52, len(fs))
    total = sum(word[0] for word in struct.iter_unpack("<I", image[8:offset - 4]))
    struct.pack_into("<I", image, offset - 4, -total & 0xffffffff)
    add_resources(bytes(image), [])
    updated = file_entries(fs)
    if updated.keys() != entries.keys():
        raise ValueError("ImageFS resource inventory changed")
    for name, (at, size) in entries.items():
        new_at, new_size = updated[name]
        expected = replacements.get(name, original[at:at + size])
        if fs[new_at:new_at + new_size] != expected:
            raise ValueError(f"Unexpected resource change: {name}")
    return bytes(image)


def build(source, output, compiler):
    source = Path(source).resolve()
    output = Path(output)
    original = source.read_bytes()
    _, offset = add_resources(original, [])
    if not MODULES.keys() <= file_entries(original[offset:]).keys():
        raise ValueError("Use an existing copied graphics-development IFS, not stock firmware")
    output.mkdir(parents=True, exist_ok=False)
    replacements = {}
    source_hashes = {}
    commands = []
    for name, source_name in MODULES.items():
        target = output / Path(name).name
        depfile = target.with_suffix(".d")
        command = [
            str(compiler), "-mfloat-abi=soft", "-mcpu=cortex-a9", "-marm",
            "-mno-unaligned-access", "-ffreestanding", "-fno-builtin",
            "-nostdlib", "-O2", "-fPIC", "-shared",
            "-I", str(ROOT / "tools/mh2p"),
            "-Wl,--hash-style=sysv,-soname," + target.name,
            "-MMD", "-MF", str(depfile),
        ]
        if name == "lib/libmh2p-screen.so":
            command.append("-DGLBRIDGE_MH2P_SCREEN")
        command += ["-o", str(target), str(ROOT / source_name)]
        subprocess.run(command + ["-MM"], check=True)
        paths = dependencies(depfile)
        before = {str(path): digest((ROOT / path).read_bytes()) for path in paths}
        if any(source_hashes.get(path, value) != value
               for path, value in before.items()):
            raise ValueError("Graphics sources changed during build")
        subprocess.run(command, check=True)
        if paths != dependencies(depfile) or any(
                digest((ROOT / path).read_bytes()) != value
                for path, value in before.items()):
            raise ValueError("Graphics sources changed during build")
        commands.append(command)
        replacements[name] = target.read_bytes()
        source_hashes.update(before)
    if any(digest((ROOT / path).read_bytes()) != value
           for path, value in source_hashes.items()):
        raise ValueError("Graphics sources changed during build")
    image = replace_resources(original, replacements)
    (output / "startup.bin").write_bytes(image)
    manifest = {
        "format": 1, "local_development_only": True,
        "source_startup_sha256": digest(original),
        "startup_sha256": digest(image),
        "source_sha256": dict(sorted(source_hashes.items())),
        "resource_sha256": {name: digest(data) for name, data in replacements.items()},
        "all_other_resources_exact": True,
        "compiler_commands": commands,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    verify(output)
    return manifest


def verify(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest["format"] != 1 or not manifest["local_development_only"] or
            not manifest["source_sha256"] or
            manifest["resource_sha256"].keys() != MODULES.keys()):
        raise ValueError("Invalid graphics-development build manifest")
    for name, expected in manifest["source_sha256"].items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT) or digest(path.read_bytes()) != expected:
            raise ValueError(f"Graphics sources changed; rebuild before launching: {name}")
    image = (directory / "startup.bin").read_bytes()
    if digest(image) != manifest["startup_sha256"]:
        raise ValueError("Graphics startup identity changed")
    _, offset = add_resources(image, [])
    fs = image[offset:]
    entries = file_entries(fs)
    for name, expected in manifest["resource_sha256"].items():
        at, size = entries[name]
        if (digest(fs[at:at + size]) != expected or
                digest((directory / Path(name).name).read_bytes()) != expected):
            raise ValueError(f"Graphics adapter identity changed: {name}")
    return directory / "startup.bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Copied diagnostic startup that already contains both adapters")
    parser.add_argument("--output", type=Path, required=True,
                        help="New build directory; existing directories are never overwritten")
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.output, args.compiler), indent=2))


if __name__ == "__main__":
    main()
