#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply guarded CarPlay experiments only to a new isolated QNX6 mNAND copy."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from prepare_carplay_country_policy import patch_country_policy
from prepare_carplay_wireless_policy import patch_wireless_policy

sys.path.append(str(Path(__file__).resolve().parents[1] / "mhi2"))
from qnx6_read import Qnx6


def find_application(fs, name):
    inode = 1
    for component in ("eso", "bin", "apps", name):
        entries = dict(fs.listdir(inode))
        if component not in entries:
            raise ValueError(f"APP filesystem is missing eso/bin/apps/{name}")
        inode = entries[component]
    if fs.inode(inode)["mode"] & 0xf000 != 0x8000:
        raise ValueError(f"APP application {name} is not a regular file")
    return inode


def prepare(source, output, *, partition_offset=4096 * 512,
            wireless=True, shared_ap=False):
    if not wireless and not shared_ap:
        raise ValueError("Select at least one explicit laboratory policy experiment")
    if type(partition_offset) is not int or partition_offset < 0:
        raise ValueError("Partition offset must be a nonnegative integer")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    plans = []
    fs = Qnx6(source, partition_offset)
    try:
        for name, enabled, patch in (
                ("smartphone_integrator", wireless, patch_wireless_policy),
                ("connectionmanager", shared_ap, patch_country_policy)):
            if not enabled:
                continue
            inode = find_application(fs, name)
            node = fs.inode(inode)
            raw = fs.read_file(inode)
            modified = patch(raw)
            if len(raw) != len(modified):
                raise ValueError("Laboratory patches must preserve executable length")
            writes = []
            for index, (before, after) in enumerate(zip(raw, modified)):
                if before == after:
                    continue
                physical = fs._bmap(node, index // fs.bs)
                if physical is None or not 0 <= physical < fs.num_blocks:
                    raise ValueError(f"APP patch site in {name} is not allocated")
                address = (fs.base + (physical + fs.sblks_off) * fs.bs +
                           index % fs.bs)
                if address >= source.stat().st_size:
                    raise ValueError(f"APP patch site in {name} exceeds image size")
                writes.append({"image_offset": address, "file_offset": index,
                               "before": before, "after": after})
            plans.append({
                "path": f"eso/bin/apps/{name}", "inode": inode,
                "source_sha256": hashlib.sha256(raw).hexdigest(),
                "output_sha256": hashlib.sha256(modified).hexdigest(),
                "writes": writes,
            })
        addresses = [write["image_offset"] for plan in plans for write in plan["writes"]]
        if len(addresses) != len(set(addresses)):
            raise ValueError("APP patch sites overlap physically")
    finally:
        fs.f.close()
    output.mkdir(parents=True)
    image = output / "mnand.bin"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", "--",
                    str(source.resolve()), str(image)], check=True)
    if image.stat().st_size != source.stat().st_size or image.samefile(source):
        raise RuntimeError("Laboratory image must be an independent same-sized copy")
    with image.open("r+b") as stream:
        for plan in plans:
            for write in plan["writes"]:
                stream.seek(write["image_offset"])
                if stream.read(1) != bytes([write["before"]]):
                    raise ValueError("Copied APP patch preimage differs")
                stream.seek(write["image_offset"])
                stream.write(bytes([write["after"]]))
    for path, digest_key in ((source, "source_sha256"), (image, "output_sha256")):
        fs = Qnx6(path, partition_offset)
        try:
            for plan in plans:
                inode = find_application(fs, plan["path"].split("/")[-1])
                observed = hashlib.sha256(fs.read_file(inode)).hexdigest()
                if observed != plan[digest_key]:
                    raise RuntimeError(f"APP executable verification failed in {path}")
        finally:
            fs.f.close()
    manifest = {
        "mod": "isolated-carplay-laboratory-mnand",
        "source": str(source.resolve()), "image": str(image.resolve()),
        "image_bytes": image.stat().st_size, "partition_offset": partition_offset,
        "carplay_cached_country_experiment": wireless,
        "shared_ap_country_experiment": shared_ap,
        "applications": plans, "source_executables_preserved": True,
        "output_executables_verified": True,
        "filesystem_metadata_changed": False,
        "guest_startup_verified": False, "radio_ready_verified": False,
        "iphone_session_verified": False,
        "warning": "Laboratory-only copy; not an update package or real-unit installer.",
    }
    with (output / "manifest.json").open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mnand", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New isolated output directory")
    parser.add_argument("--partition-offset", type=lambda value: int(value, 0),
                        default=4096 * 512)
    parser.add_argument("--experimental-carplay-country-veto", action="store_true")
    parser.add_argument("--experimental-shared-ap-country-policy", action="store_true")
    args = parser.parse_args()
    try:
        result = prepare(
            args.mnand, args.output, partition_offset=args.partition_offset,
            wireless=args.experimental_carplay_country_veto,
            shared_ap=args.experimental_shared_ap_country_policy)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Laboratory image preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
