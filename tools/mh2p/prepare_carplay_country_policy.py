#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an experimental P2865 country-bitmap patch, never a live install."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


ORIGINAL_SHA256 = "805b22b13481fe2f79c3e98f7d27eff1f364ef5705f87aeda739fcf36d6ae0a6"
PATCHES = (
    (0x6da74, bytes.fromhex("002e"), bytes.fromhex("0126")),
    (0x6db3e, bytes.fromhex("9cdc"), bytes.fromhex("8fe7")),
    (0x6db66, bytes.fromhex("8ae7"), bytes.fromhex("7be7")),
    (0x6dbba, bytes.fromhex("3eaa134413f85c3c23b1"),
     bytes.fromhex("0d3b082b06d800bf00bf")),
)


def file_offset(source, address, length):
    if (len(source) < 52 or source[:7] != b"\x7fELF\x01\x01\x01" or
            struct.unpack_from("<H", source, 18)[0] != 40):
        raise ValueError("Expected little-endian ARM ELF32")
    phoff = struct.unpack_from("<I", source, 28)[0]
    stride, count = struct.unpack_from("<HH", source, 42)
    if stride != 32 or phoff + stride * count > len(source):
        raise ValueError("Invalid ELF program-header table")
    matches = []
    for index in range(count):
        kind, offset, base, _, size, _, flags, _ = struct.unpack_from(
            "<8I", source, phoff + stride * index)
        if (kind == 1 and flags & 1 and
                base <= address and address + length <= base + size):
            at = offset + address - base
            if at + length > len(source):
                raise ValueError("ELF executable segment exceeds file")
            matches.append(at)
    if len(matches) != 1:
        raise ValueError(f"Expected one executable mapping for {address:#x}")
    return matches[0]


def patch_country_policy(source):
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Connection manager identity is not the inspected original P2865")
    modified = bytearray(source)
    for address, expected, replacement in PATCHES:
        at = file_offset(source, address, len(expected))
        if len(expected) != len(replacement) or source[at:at + len(expected)] != expected:
            raise ValueError(f"Unexpected original policy instructions at {address:#x}")
        modified[at:at + len(expected)] = replacement
    return bytes(modified)


def prepare(source, output):
    raw = source.read_bytes()
    modified = patch_country_policy(raw)
    manifest = {
        "mod": "experimental-p2865-5ghz-country-bitmap",
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "output_sha256": hashlib.sha256(modified).hexdigest(),
        "patches": [
            {"address": hex(address), "before": before.hex(), "after": after.hex()}
            for address, before, after in PATCHES
        ],
        "effect": (
            "Ignore known/unknown country bitmap; require supported 5 GHz channel "
            "and enabled flag"),
        "scope": "Connection-manager AP policy, not solely CarPlay; laboratory use only",
        "coding_and_adaptation_instructions_changed": False,
        "radio_driver_or_channel_table_changed": False,
        "installed": False,
        "full_country_resolver_verified": False,
        "wireless_carplay_verified": False,
        "warning": (
            "Does not grant regulatory authorization or override radio firmware. "
            "Does not create an enabled radio, select a new channel, enable coding, "
            "or bypass licenses. End-to-end QNX and phone behavior is unverified."),
    }
    output.mkdir(parents=True)
    with (output / "connectionmanager").open("xb") as stream:
        stream.write(modified)
    (output / "connectionmanager").chmod(0o700)
    with (output / "manifest.json").open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New isolated output directory")
    parser.add_argument("--experimental-country-bitmap", action="store_true",
                        required=True, help="Acknowledge this is a shared AP-policy experiment")
    args = parser.parse_args()
    try:
        result = prepare(args.binary, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Country-policy preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
