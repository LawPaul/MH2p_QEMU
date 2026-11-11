#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a local BlueZ HCI controller or connect its H4 socket to QEMU UART D."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import selectors
import socket
import subprocess
import time


BLUEZ_COMMIT = "0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36"
BLUEZ_SOURCES = (
    "emulator/btdev.c", "src/shared/queue.c",
    "src/shared/util.c", "src/shared/crypto.c", "src/shared/ecc.c",
    "src/shared/io-mainloop.c", "src/shared/timeout-mainloop.c",
    "src/shared/mainloop.c", "src/shared/mainloop-notify.c", "lib/bluetooth.c",
)


def build(source, output, compiler="cc"):
    source = source.resolve(strict=True)
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    commit = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"], check=True,
        capture_output=True, text=True).stdout.strip()
    if commit != BLUEZ_COMMIT:
        raise ValueError(f"Expected upstream BlueZ 5.79 commit {BLUEZ_COMMIT}")
    changes = subprocess.run(
        ["git", "-C", str(source), "diff", "--name-only", "HEAD"], check=True,
        capture_output=True, text=True).stdout
    if changes.strip():
        raise ValueError("BlueZ source has tracked changes; use unchanged upstream")
    wrapper = Path(__file__).with_suffix(".c")
    sources = [wrapper, *(source / name for name in BLUEZ_SOURCES)]
    hashes = {
        str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sources
    }
    output.mkdir(parents=True)
    binary = output.resolve() / "mh2p-hci"
    command = [compiler, "-std=gnu11", "-O3", "-g", "-I", str(source),
               "-I", str(source / "lib"), "-o", str(binary),
               *(str(path) for path in sources), "-lrt"]
    with (output / "build.log").open("wb") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    report = {
        "controller": "upstream BlueZ BR/EDR/LE emulator",
        "transport": "bounded nonblocking local H4 stream server",
        "le_p256": "local HCI commands using upstream ECC key generation/ECDH",
        "le_p256_keys": "ephemeral per-controller; not unit provisioning",
        "bluez_commit": commit,
        "command": command,
        "source_sha256": hashes,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "broadcom_patchram_emulated": False,
        "original_broadcom_firmware_executed": False,
        "bluetooth_pairing_verified": False,
        "hmi_readiness_verified": False,
    }
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def relay(uart, controller, seconds):
    if not math.isfinite(seconds) or not 0 < seconds <= 3600:
        raise ValueError("Relay duration must be finite and between 0 and 3600 seconds")
    counts = {"guest_to_controller": 0, "controller_to_guest": 0}
    started = time.monotonic()
    deadline = started + seconds
    with socket.socket(socket.AF_UNIX) as guest, \
            socket.socket(socket.AF_UNIX) as peer, \
            selectors.DefaultSelector() as selector:
        for endpoint, path in ((guest, uart), (peer, controller)):
            endpoint.settimeout(3)
            endpoint.connect(str(path))
        selector.register(guest, selectors.EVENT_READ,
                          (peer, "guest_to_controller"))
        selector.register(peer, selectors.EVENT_READ,
                          (guest, "controller_to_guest"))
        while time.monotonic() < deadline:
            for key, _ in selector.select(
                    max(0, min(1, deadline - time.monotonic()))):
                data = key.fileobj.recv(65536)
                if not data:
                    raise ConnectionError(
                        f"H4 endpoint closed while relaying {key.data[1]}")
                destination, direction = key.data
                destination.sendall(data)
                counts[direction] += len(data)
    return {
        "transport": "unmodified bidirectional H4",
        "bytes": counts,
        "elapsed_seconds": time.monotonic() - started,
        "payload_logged": False,
        "broadcom_patchram_emulated": False,
        "bluetooth_pairing_verified": False,
        "hmi_readiness_verified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--bluez-source", type=Path, required=True)
    builder.add_argument("--output", type=Path, required=True)
    builder.add_argument("--cc", default="cc")
    bridge = commands.add_parser("relay")
    bridge.add_argument("--uart", type=Path, required=True)
    bridge.add_argument("--controller", type=Path, required=True)
    bridge.add_argument("--seconds", type=float, default=120)
    args = parser.parse_args()
    try:
        if args.action == "build":
            result = build(args.bluez_source, args.output, args.cc)
        else:
            result = relay(args.uart, args.controller, args.seconds)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"MH2P Bluetooth HCI {args.action} failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
