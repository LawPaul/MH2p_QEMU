#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Place the original P2865 RCC App filesystem in an incomplete NOR fixture."""
import argparse
import hashlib
import json
from pathlib import Path


NOR_SIZE = 64 * 1024 * 1024
APP_OFFSET = 32 * 1024 * 1024
APP_SIZE = 30 * 1024 * 1024
APP_SHA256 = "a160ac767cc316f39232aaff2b26cbb064357af0c92732b9111e9b28490e7a20"
PERSISTENCE_SIZE = 2 * 1024 * 1024
PERSISTENCE_SHA256 = (
    "b49d3fc14f25e780bef2e71809d5740cf56273ecc4af52f18baec9138cbf177b")


def prepare(source, output, persistence=None):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if source.stat().st_size != APP_SIZE:
        raise ValueError("Expected the original 30 MiB P2865 RCC App image")
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != APP_SHA256:
        raise ValueError("Unsupported RCC App SHA-256; expected P2865")
    defaults = None
    if persistence is not None:
        if persistence.stat().st_size != PERSISTENCE_SIZE:
            raise ValueError("Expected concatenated 1 MiB Per1 and Per2 defaults")
        defaults = persistence.read_bytes()
        if hashlib.sha256(defaults).hexdigest() != PERSISTENCE_SHA256:
            raise ValueError("Unsupported persistence defaults SHA-256")
    output.mkdir(parents=True)
    image = output / "nor.bin"
    with image.open("xb") as stream:
        erased = b"\xff" * (1024 * 1024)
        for _ in range(APP_OFFSET // len(erased)):
            stream.write(erased)
        stream.write(data)
        if defaults is None:
            for _ in range(PERSISTENCE_SIZE // len(erased)):
                stream.write(erased)
        else:
            stream.write(defaults)
    metadata = {
        "firmware": "MH2p_ER_POG35_P2865",
        "source": str(source.resolve()),
        "source_sha256": APP_SHA256,
        "app_offset": APP_OFFSET,
        "app_bytes": APP_SIZE,
        "nor_bytes": NOR_SIZE,
        "nor_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "incomplete_flash": True,
        "note": (
            "Filesystem-only layout, not recovered chip placement. " +
            ("App and original Per1/Per2 defaults are populated. "
             if defaults is not None else
             "Only App is populated; other bytes are erased. ") +
            "No bootloader, wakeup token or factory persistence supplied."),
    }
    if persistence is not None:
        metadata["persistence_defaults"] = {
            "source": str(persistence.resolve()),
            "sha256": PERSISTENCE_SHA256,
            "offset": APP_OFFSET + APP_SIZE,
            "bytes": PERSISTENCE_SIZE,
            "note": "Original empty Per1 then Per2 filesystem templates, "
                    "not recovered vehicle data.",
        }
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--persistence-defaults", type=Path,
                        help="Original inflated Per1 followed by Per2 (2 MiB)")
    args = parser.parse_args()
    try:
        result = prepare(args.app, args.output, args.persistence_defaults)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"MH2P RCC NOR preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
