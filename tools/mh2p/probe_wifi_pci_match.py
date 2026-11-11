#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exhaustively execute the original radio driver's PCI device matcher."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import time

from prepare_carplay_country_policy import file_offset
from probe_carplay_country_policy import InlineProgram
from probe_rcc import QEMUMachine, ROOT


DRIVER_SHA256 = "1d6d3bd497cd40b46f1104c5b82cc07da2503a08bc2a04d942ce8041abef67f4"
VENDORS = (0x14e4, 0, 0xffff)
ACCEPTED_IDS = frozenset((
    0x43a3, 0x43a4, 0x43a5, 0x4350, 0xaa31,
    0x43df, 0x43e0, 0x43e1, 0x4354,
    0x43ec, 0x43ed, 0x43ee, 0x4356,
    0x43ab, 0x43ac, 0x43ad, 0x4345, 0xa9be, 0xa9bf, 0xa9c1, 0xa9c2,
    0x43ae, 0x43af, 0x43b0, 0x4335,
    0x43ba, 0x43bb, 0x43bc, 0xaa52,
    0x43d9, 0x43da, 0x43db,
    0x43e9, 0x43ea, 0x43eb,
    0x4349, 0x43dd, 0x43de,
    0x43dc, 0x43fc, 0x43fd, 0x4355,
    0x43ef, 0x43fe, 0x43ff,
    0x43ca, 0x43cb, 0x43cc, 0x4365,
    0x43c3, 0x43c4, 0x43c5, 0x4366,
))


def validate_results(data):
    count = len(VENDORS) * 65536
    if len(data) != count * 4:
        raise RuntimeError("Incomplete native Wi-Fi PCI matcher results")
    accepted = {vendor: [] for vendor in VENDORS}
    for index, (observed,) in enumerate(struct.iter_unpack("<I", data)):
        vendor, device = VENDORS[index // 65536], index % 65536
        expected = 0 if vendor == 0x14e4 and device in ACCEPTED_IDS else 1
        if observed != expected:
            raise RuntimeError(
                f"Native PCI matcher mismatch for {vendor:04x}:{device:04x}: "
                f"expected {expected}, observed {observed}")
        if not observed:
            accepted[vendor].append(device)
    return accepted


def probe(qemu, driver, output):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    source = driver.read_bytes()
    if hashlib.sha256(source).hexdigest() != DRIVER_SHA256:
        raise ValueError("Expected original P2865 BCM4359 QWDI driver SHA-256")

    def original(address, length):
        offset = file_offset(source, address, length)
        return source[offset:offset + length]

    base, results, logger = 0x100000, 0x200000, 0x350000
    got = 0x37a78 + struct.unpack("<I", original(0x37d48, 4))[0]
    log_entry = got + struct.unpack("<I", original(0x37d4c, 4))[0]
    program = InlineProgram()
    program.load(13, 0x300000)
    program.write(base + log_entry, logger)
    program.write(logger, 0)
    program.load(9, results)
    program.load(11, base + 0x37a65)
    program.load(6, 65536)
    for vendor in VENDORS:
        program.load(10, vendor)
        program.load(8, 0)
        loop = len(program.words)
        program.words += [
            0xe1a0000a, 0xe1a01008, 0xe12fff3b,
            0xe4890004, 0xe2888001, 0xe1580006,
        ]
        program.words.append(
            0x1a000000 | ((loop - len(program.words) - 2) & 0xffffff))
    count = len(VENDORS) * 65536
    completion = results + count * 4
    program.write(completion, 0x5a17c0de)
    output = output.resolve()
    output.mkdir(parents=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-wifi-pci-") as name:
        root = Path(name)
        args = ["-M", "none", "-cpu", "cortex-r5", "-m", "4M", "-S",
                "-display", "none", "-nic", "none",
                "-device", "loader,addr=0x1000,cpu-num=0"]
        for label, address, data in (
                ("fixture", 0x1000, program.image()[0x100:]),
                ("matcher", base + 0x37a64, original(0x37a64, 0x37d60 - 0x37a64))):
            path = root / label
            path.write_bytes(data)
            args += ["-device",
                     f"loader,file={path},addr={address:#x},force-raw=on"]
        with QEMUMachine(str(qemu.resolve()), args=args) as vm:
            vm.launch()
            vm._qmp.settimeout(5)
            vm.cmd("cont")
            deadline = time.monotonic() + 10
            while True:
                reply = vm.cmd("human-monitor-command",
                               command_line=f"xp /1wx {completion:#x}")
                if int(reply.split(":")[1].strip(), 16) == 0x5a17c0de:
                    break
                if time.monotonic() >= deadline:
                    state = vm.cmd("human-monitor-command",
                                   command_line="info registers")
                    raise TimeoutError(f"Native Wi-Fi PCI matcher timed out: {state}")
                time.sleep(0.01)
            vm.cmd("stop")
            readback = root / "results"
            reply = vm.cmd(
                "human-monitor-command",
                command_line=f"pmemsave {results:#x} {count * 4:#x} "
                             f"{json.dumps(str(readback))}")
            if reply:
                raise RuntimeError(f"Native PCI matcher readback failed: {reply}")
            accepted = validate_results(readback.read_bytes())
    if driver.read_bytes() != source:
        raise RuntimeError("Original radio driver changed during probe")
    report = {
        "source_sha256": DRIVER_SHA256,
        "original_matcher_unmodified": True,
        "original_function_address": "0x37a64",
        "complete_function_executed": True,
        "cases": count,
        "device_id_range": [0, 65535],
        "accepted_ids_by_vendor": {
            f"{vendor:04x}": [f"{device:04x}" for device in ids]
            for vendor, ids in accepted.items()
        },
        "supplied_nvram_device_43ef_accepted": 0x43ef in accepted[0x14e4],
        "diagnostic_logging_disabled_by_fixture": True,
        "functional_dependency_returns_stubbed": False,
        "pcie_endpoint_emulated": False,
        "wifi_ap_verified": False,
        "owned_vm_exited": True,
        "elapsed_seconds": time.monotonic() - started,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.driver, args.output), indent=2))


if __name__ == "__main__":
    main()
