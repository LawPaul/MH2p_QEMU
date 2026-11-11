#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create and validate persistent development disks with an untouched raw backing image."""
import argparse
import json
from pathlib import Path
import subprocess

from prepare_mnand import digest


def validate(source, directory, source_hash=None):
    source = source.resolve()
    directory = directory.resolve()
    image = directory / "mnand.qcow2"
    identity = json.loads((directory / "identity.json").read_text())
    expected = {"source": str(source), "source_sha256": source_hash or digest(source),
                "source_bytes": source.stat().st_size}
    if identity != expected:
        raise ValueError("Persistent mNAND backing identity differs")
    if image.is_symlink() or not image.is_file():
        raise ValueError("Expected a regular persistent mNAND overlay")
    info = json.loads(subprocess.run(
        ["qemu-img", "info", "--output=json", str(image)],
        check=True, capture_output=True, text=True).stdout)
    if (info.get("format") != "qcow2" or
            info.get("virtual-size") != expected["source_bytes"] or
            info.get("backing-filename-format") != "raw" or
            info.get("full-backing-filename") != str(source)):
        raise ValueError("Persistent mNAND overlay geometry or backing file differs")
    return image


def create(source, directory):
    source = source.resolve()
    if not source.is_file() or source.stat().st_size == 0:
        raise ValueError("Expected a nonempty raw mNAND source")
    identity = {"source": str(source), "source_sha256": digest(source),
                "source_bytes": source.stat().st_size}
    directory.mkdir(parents=True, exist_ok=False)
    image = directory / "mnand.qcow2"
    subprocess.run(["qemu-img", "create", "-f", "qcow2", "-F", "raw",
                    "-b", str(source), str(image)], check=True, capture_output=True)
    (directory / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    validate(source, directory)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        image = (validate if args.check else create)(args.source, args.directory)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Persistent mNAND preparation failed: {exc}\n")
    print(image)


if __name__ == "__main__":
    main()
