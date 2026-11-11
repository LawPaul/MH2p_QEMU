#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a private, firmware-pinned MH2P SD ModKit experimental multitouch module."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from prepare_carplay_java_overlay import OVERLAY_ENTRIES
from prepare_carplay_java_slots import validate_archives
from prepare_carplay_multitouch import multitouch_config


RELEASE = "MH2p_ER_POG35_P2865"
MOD = "CarPlay_Multitouch"
STATE = "/mnt/app/eso/hmi/lsd/.mh2p-carplay-multitouch"
LAUNCHER_SHA256 = "65172403106dda1f29b1fa6b4c22b29a49f8ac83bffdbd05348627bafff6888e"
NATIVE_SHA256 = "429ef95595bd67d13acf51df63cb6fb3f86e7ed8effa2eeec25e085c6cdadb77"
CONFIG_SHA256 = "7901e48ddd5ca35777dcc01d3e8f9006de503ce3bb55be3f8108713356a57762"
ANCHOR = 'BOOTCLASSPATH="$BOOTCLASSPATH:$LSD_JXE"'


def build_checksum(compiler):
    flags = [compiler, "-mfloat-abi=soft", "-mcpu=cortex-a9", "-marm",
             "-mno-unaligned-access", "-ffreestanding", "-fno-builtin",
             "-nostdlib", "-O2", "-Wall", "-Wextra", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="mh2p-modkit-cksum-") as temporary:
        work = Path(temporary)
        stub = work / "libc.c"
        # Link-only symbols; the guest loads its unchanged original libc.so.3.
        stub.write_text("void read(void) {} void write(void) {} void exit(void) {}\n")
        libc = work / "libc.so"
        subprocess.run(flags + ["-fPIC", "-shared",
                       "-Wl,--hash-style=sysv,-soname,libc.so.3",
                       str(stub), "-o", str(libc)], check=True)
        binary = work / "cksum"
        subprocess.run(flags + [
            "-Wl,--hash-style=sysv,--dynamic-linker=/usr/lib/ldqnx.so.2,-e,_start",
            str(Path(__file__).with_name("carplay_modkit_cksum.c")), str(libc),
            "-lgcc", "-o", str(binary)], check=True)
        return binary.read_bytes()


def checksum(data):
    result = subprocess.run(["cksum"], input=data, capture_output=True, check=True)
    fields = result.stdout.decode("ascii").split()
    if len(fields) != 2 or not all(field.isdigit() for field in fields):
        raise RuntimeError("Expected POSIX cksum output")
    return " ".join(fields)


def pinned(data, expected, label):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Expected original {RELEASE} {label}")


def launcher_patch(data):
    source = data.decode("ascii")
    if source.count(ANCHOR) != 1 or "mh2p-carplay-multitouch" in source:
        raise ValueError("Expected one original boot classpath assignment")
    return source.replace(
        ANCHOR,
        f'test -r {STATE}/contacts.jar || {{ '
        'echo "CarPlay_Multitouch ERROR: contact overlay missing" >&2; exit 1; }\n'
        f'BOOTCLASSPATH="{STATE}/contacts.jar:$BOOTCLASSPATH:$LSD_JXE"').encode("ascii")


def write_zip(path, entries):
    with ZipFile(path, "x") as archive:
        for name, data, executable in entries:
            info = ZipInfo(name)
            info.compress_type = ZIP_DEFLATED
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            archive.writestr(info, data)


def prepare(original, prepared, launcher, native, config, output,
            compiler="arm-none-eabi-gcc"):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    identities = validate_archives(original, prepared)
    launcher_raw, native_raw, config_raw = (
        path.read_bytes() for path in (launcher, native, config))
    for raw, expected, label in (
            (launcher_raw, LAUNCHER_SHA256, "HMI launcher"),
            (native_raw, NATIVE_SHA256, "dio_manager executable"),
            (config_raw, CONFIG_SHA256, "dio_manager configuration")):
        pinned(raw, expected, label)
    modified_launcher = launcher_patch(launcher_raw)
    modified_config, old = multitouch_config(config_raw.decode("utf-8"), 2)
    if old != 1:
        raise ValueError("Expected original one-contact CarPlay limit")
    modified_config = (json.dumps(modified_config, indent=4) + "\n").encode("utf-8")
    templates = Path(__file__).with_name("modkit_carplay_multitouch")
    scripts = {name: (templates / name).read_bytes() for name in
               ("common.sh", "install.sh", "uninstall.sh", "dio_manager.sh")}
    checksum_binary = build_checksum(compiler)
    output.mkdir(parents=True)
    module = output / "Mods" / MOD
    update = module / "Update"
    update.mkdir(parents=True)
    overlay = update / "contacts.jar"
    with ZipFile(prepared) as archive:
        write_zip(overlay, [(name, archive.read(name), False)
                            for name in OVERLAY_ENTRIES])
    payloads = {
        **scripts, "cksum": checksum_binary, "lsd.sh": modified_launcher,
        "dio_manager.json": modified_config,
    }
    identity = {
        "ARCHIVE_CKSUM": checksum(original.read_bytes()),
        "LAUNCHER_ORIGINAL_CKSUM": checksum(launcher_raw),
        "NATIVE_CKSUM": checksum(native_raw),
        "LAUNCHER_PATCHED_CKSUM": checksum(modified_launcher),
        "WRAPPER_CKSUM": checksum(scripts["dio_manager.sh"]),
        "CONFIG_CKSUM": checksum(modified_config),
        "OVERLAY_CKSUM": checksum(overlay.read_bytes()),
    }
    payloads["identity.sh"] = "".join(
        f"{name}='{value}'\n" for name, value in identity.items()).encode("ascii")
    payloads["identity.cksum"] = (checksum(payloads["identity.sh"]) + "\n").encode("ascii")
    for name, data in payloads.items():
        (update / name).write_bytes(data)
        (update / name).chmod(0o755 if name.endswith(".sh") or name == "cksum" else 0o644)
    instructions = f"""# Experimental CarPlay multitouch for MH2P SD ModKit

Supported firmware: **{RELEASE} only**, MH2P SD ModKit version 2.
This is a private experimental module, not confirmed iPhone multitouch support.
Do not redistribute its firmware-derived overlay or configuration.

## Install

1. Back up the unit and use the existing MH2P SD ModKit media.
2. Copy `Mods/{MOD}` from this package into the media's `Mods` folder.
3. Create an empty `enable-experimental.txt` in `Mods/{MOD}`.
4. Remove `uninstall.txt` if present, then run the ModKit normally.
5. Read `Logs/{MOD}.log`. A ModKit "Done" line is not proof of success:
   require the module's explicit installed message and no ERROR lines.
6. Reboot and establish a fresh CarPlay connection so capabilities renegotiate.

## Uninstall

Create `Mods/{MOD}/uninstall.txt`, rerun the same ModKit package and reboot.
The original launcher and native executable are restored byte-for-byte.
Verified on-unit backups remain in `{STATE}` for recovery.
Neither installation nor uninstall modifies the source HMI archive.

## Scope and safety

The module sets native `screen.fingers` to 2 and prepends five guarded Java
contact-slot/release classes. The native wrapper selects its private config,
preserves arguments/environment and execs the original backed-up dio_manager.
No coding, FEC, wireless/country policy, development activation property,
emulator Screen module, HID startup gate or synthetic device is installed.
Existing CarPlay authorization and hardware are still required.

The installer refuses different firmware, mismatched original bytes, conflicting
launcher modifications, changed backups and corrupt payloads. Backups are committed
before live changes. Ordinary installation errors restore the original launchers.
Power loss cannot be made atomic across both files: keep the ModKit media and
verified backups. Partial known installations can be uninstalled or rerun.
A stale `.staging` backup directory fails closed before live changes; inspect it
before removing its three known files and empty directories.

P2865 does not include a cksum command. This package includes its own small ARM
QNX POSIX checksum helper, using the unchanged original libc.so.3. It is not a
replacement native CarPlay binary.

Launcher-modifying mods must be removed before installation. Other overlays that
replace the same four TerminalMode classes are incompatible; do not combine them.
Reapply only a newly validated package after firmware changes.

Emulator tests establish native input and Java fixtures, not an active CarPlay
backend, iOS capability acceptance or physical-unit installation safety.
"""
    (module / "README.md").write_text(instructions, encoding="ascii")
    report = {
        "format": "MH2P SD ModKit v2 Mods/CarPlay_Multitouch/Update",
        "supported_release": RELEASE,
        "experimental": True,
        "original_archive_sha256": identities[0],
        "prepared_archive_sha256": identities[1],
        "original_launcher_sha256": LAUNCHER_SHA256,
        "original_native_sha256": NATIVE_SHA256,
        "original_config_sha256": CONFIG_SHA256,
        "native_fingers": 2,
        "overlay_classes": list(OVERLAY_ENTRIES),
        "source_firmware_archive_included": False,
        "original_native_binary_included": False,
        "coding_fec_wireless_or_development_activation_changed": False,
        "installed_on_physical_unit": False,
        "iphone_multitouch_verified": False,
        "payload_sha256": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(update.iterdir())},
    }
    (module / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    bundle = output / f"{MOD}-{RELEASE}.zip"
    write_zip(bundle, [
        (str(path.relative_to(output)), path.read_bytes(),
         path.suffix == ".sh" or path.name == "cksum")
        for path in sorted(module.rglob("*")) if path.is_file()])
    if validate_archives(original, prepared) != identities:
        raise RuntimeError("Source archives changed during package construction")
    for path, raw in ((launcher, launcher_raw), (native, native_raw), (config, config_raw)):
        if path.read_bytes() != raw:
            raise RuntimeError(f"Source changed during packaging: {path}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original", "prepared", "launcher", "native", "config", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--experimental-modkit-package", action="store_true", required=True)
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    args = parser.parse_args()
    try:
        report = prepare(args.original, args.prepared, args.launcher,
                         args.native, args.config, args.output, args.compiler)
    except (OSError, UnicodeError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"CarPlay ModKit packaging failed: {exc}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
