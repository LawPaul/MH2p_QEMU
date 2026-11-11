#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an isolated P2865 CarPlay-only cached-country veto experiment."""
import argparse
import hashlib
import json
from pathlib import Path

from prepare_carplay_country_policy import file_offset


ORIGINAL_SHA256 = "c140c1b03a9f70cf465af5d8012a2b2cd69f51d915f44167529292f39127ccc0"
PATCH_ADDRESS = 0x97d6a
PREIMAGE = bytes.fromhex("98f8f035")
REPLACEMENT = bytes.fromhex("012300bf")


def patch_wireless_policy(source):
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Smartphone integrator identity is not the inspected original P2865")
    offset = file_offset(source, PATCH_ADDRESS, len(PREIMAGE))
    if source[offset:offset + len(PREIMAGE)] != PREIMAGE:
        raise ValueError(f"Unexpected original wireless policy instructions at {PATCH_ADDRESS:#x}")
    return source[:offset] + REPLACEMENT + source[offset + len(PREIMAGE):]


def prepare(source, output):
    raw = source.read_bytes()
    modified = patch_wireless_policy(raw)
    manifest = {
        "mod": "experimental-p2865-carplay-cached-country-veto",
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "output_sha256": hashlib.sha256(modified).hexdigest(),
        "patches": [{"address": hex(PATCH_ADDRESS),
                     "before": PREIMAGE.hex(), "after": REPLACEMENT.hex()}],
        "scope": "CarPlay wireless activation only; not shared AP/channel policy",
        "effect": "Ignore cached country availability after original coding and device-state gates",
        "coding_instructions_changed": False,
        "device_state_gate_instructions_changed": False,
        "cached_availability_storage_changed": False,
        "radio_driver_or_channel_table_changed": False,
        "installed": False,
        "wireless_carplay_verified": False,
        "warning": (
            "Laboratory experiment, not a complete wireless fix or regulatory authorization. "
            "Shared AP policy can still disable 5 GHz. Does not enable coding, provision "
            "licenses, create a radio/session, or bypass downstream device-state checks."),
    }
    output.mkdir(parents=True)
    with (output / "smartphone_integrator").open("xb") as stream:
        stream.write(modified)
    (output / "smartphone_integrator").chmod(0o700)
    with (output / "manifest.json").open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New isolated output directory")
    parser.add_argument("--experimental-carplay-country-veto", action="store_true",
                        required=True, help="Acknowledge this is an uninstalled experiment")
    args = parser.parse_args()
    try:
        result = prepare(args.binary, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"CarPlay wireless preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
