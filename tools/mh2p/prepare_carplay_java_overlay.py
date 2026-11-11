#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a guarded temporary boot-classpath overlay, never a firmware install."""
import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from prepare_carplay_java_slots import (
    CONTROLLER, FILTER, HELPER, LISTENER, MANAGER, validate_archives)


GUEST_OVERLAY = "/tmp/mh2p-carplay-contacts.jar"
OVERLAY_ENTRIES = (CONTROLLER, MANAGER, LISTENER, FILTER, HELPER)
CLASSPATH_ANCHOR = 'BOOTCLASSPATH="$BOOTCLASSPATH:$LSD_JXE"'
LAB_CARPLAY_PROPERTY = "-Dterminalmode.dio=true"


def overlay_launcher(source, *, laboratory_carplay=False):
    anchors = [CLASSPATH_ANCHOR] + [
        f'BOOTCLASSPATH="{directory}/mh2p-diagnostic-release.jar:$BOOTCLASSPATH:$LSD_JXE"'
        for directory in ("/tmp", "/lib")]
    if sum(source.count(anchor) for anchor in anchors) != 1:
        raise ValueError("Expected one original firmware boot classpath assignment")
    if GUEST_OVERLAY in source:
        raise ValueError("CarPlay contact overlay already present in launcher")
    if "terminalmode.dio" in source:
        raise ValueError("Launcher already contains a CarPlay activation property")
    anchor = next(anchor for anchor in anchors if anchor in source)
    return source.replace(
        anchor,
        f'test -r {GUEST_OVERLAY} || {{ echo "Missing CarPlay contact overlay" >&2; exit 1; }}\n'
        + (f'VMOPTIONS="$VMOPTIONS {LAB_CARPLAY_PROPERTY}"\n' if laboratory_carplay else "")
        + anchor.replace('BOOTCLASSPATH="', f'BOOTCLASSPATH="{GUEST_OVERLAY}:'))


def prepare(original, prepared, launcher, output, *, laboratory_carplay=False):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    identities = validate_archives(original, prepared)
    source = launcher.read_bytes()
    modified = overlay_launcher(source.decode(), laboratory_carplay=laboratory_carplay).encode()
    output.mkdir(parents=True)
    overlay = output / "mh2p-carplay-contacts.jar"
    with ZipFile(prepared) as archive, ZipFile(overlay, "x") as target:
        for name in OVERLAY_ENTRIES:
            info = ZipInfo(name)
            info.compress_type = ZIP_DEFLATED
            target.writestr(info, archive.read(name))
    with ZipFile(prepared) as archive, ZipFile(overlay) as target:
        if target.namelist() != list(OVERLAY_ENTRIES):
            raise RuntimeError("Unexpected boot overlay class set")
        for name in OVERLAY_ENTRIES:
            if target.read(name) != archive.read(name):
                raise RuntimeError(f"Boot overlay class differs: {name}")
    (output / "lsd-carplay-slots.sh").write_bytes(modified)
    if launcher.read_bytes() != source or validate_archives(original, prepared) != identities:
        raise RuntimeError("CarPlay boot overlay preparation changed an input")
    report = {
        "scope": "opt-in temporary HMI boot-classpath contact overlay",
        "original_archive_sha256": identities[0],
        "prepared_archive_sha256": identities[1],
        "original_launcher_sha256": hashlib.sha256(source).hexdigest(),
        "prepared_launcher_sha256": hashlib.sha256(modified).hexdigest(),
        "overlay_sha256": hashlib.sha256(overlay.read_bytes()).hexdigest(),
        "overlay_classes": list(OVERLAY_ENTRIES),
        "guest_overlay": GUEST_OVERLAY,
        "production_properties_preserved": True,
        "laboratory_java_carplay_activation": laboratory_carplay,
        "native_coding_or_fec_supplied": False,
        "source_archives_and_launcher_preserved": True,
        "installed": False,
        "live_hmi_dsi_delivery_verified": False,
        "iphone_session_verified": False,
    }
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original", "prepared", "launcher", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--experimental-carplay-contact-slots", required=True,
                        action="store_true")
    parser.add_argument("--laboratory-java-carplay-activation", action="store_true",
                        help="Use original terminalmode.dio developer property; not native coding/FEC")
    args = parser.parse_args()
    print(json.dumps(prepare(
        args.original, args.prepared, args.launcher, args.output,
        laboratory_carplay=args.laboratory_java_carplay_activation), indent=2))


if __name__ == "__main__":
    main()
