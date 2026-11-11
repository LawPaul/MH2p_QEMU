#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute original radio BAR2 transfer code, including a complete firmware copy."""
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
from probe_wifi_pci_match import DRIVER_SHA256
from probe_wifi_reset import verify_firmware


BAR_SIZE = 0x400000
GUARD_SIZE = 16


def cases(firmware):
    rows = []
    for offset in (0, 0x180000, 0x180004, BAR_SIZE - 4096, BAR_SIZE - 64):
        for size in (*range(18), 31, 32, 63, 64, 2047, 2048, 2049):
            if size > BAR_SIZE - offset:
                continue
            payload = bytes((index * 37 + size) & 255 for index in range(size))
            rows.append((offset, payload))
    rows.append((0x180000, firmware))
    return rows


def verify_bar(rows, data):
    expected = bytearray(b"\xa5" * (BAR_SIZE + GUARD_SIZE * 2))
    for offset, payload in rows:
        start = GUARD_SIZE + offset
        expected[start:start + len(payload)] = payload
    if data != expected:
        raise RuntimeError("Original radio final BAR2 contents or guards differ")


def verify_transfers(rows, returned, copied):
    if len(returned) != len(rows) * 8 or len(copied) != len(rows):
        raise RuntimeError("Incomplete original radio transfer observations")
    for index, ((offset, payload), actual) in enumerate(zip(rows, copied)):
        write_result, read_result = struct.unpack_from("<II", returned, index * 8)
        if write_result or read_result:
            raise RuntimeError(f"Original radio transfer returned failure: case {index}")
        expected = b"\x5a" * GUARD_SIZE + payload + b"\x5a" * GUARD_SIZE
        if actual != expected:
            raise RuntimeError(f"Original BAR2 read or guard mismatch at {offset:#x}")


def probe(qemu, driver, image, output):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    source = driver.read_bytes()
    if hashlib.sha256(source).hexdigest() != DRIVER_SHA256:
        raise ValueError("Expected original P2865 BCM4359 QWDI driver SHA-256")
    firmware = verify_firmware(image)

    def original(address, length):
        at = file_offset(source, address, length)
        return source[at:at + length]

    rows = cases(firmware)
    base, context, guard, stack = 0x100000, 0x200000, 0x201000, 0x280000
    state, dump_base = 0x210000, 0xe00010
    results, bar_base = 0x290000, 0x400010
    data_base, read_base = 0x900000, 0xc00000
    got = 0x3302e + struct.unpack("<I", original(0x33128, 4))[0]
    guard_entry = got + struct.unpack("<I", original(0x3312c, 4))[0]
    program = InlineProgram()
    program.load(13, stack)
    program.write(base + guard_entry, guard)
    program.write(guard, 0x5a17c0de)
    program.write(context + 0xa0, bar_base)
    program.load(11, base + 0x33015)
    slots = []
    used = 0
    for index, (offset, payload) in enumerate(rows):
        slot = (used + 15) & ~15
        used = slot + len(payload) + GUARD_SIZE * 2
        slots.append(slot)
        for writing, host in ((1, data_base + slot), (0, read_base + slot + GUARD_SIZE)):
            program.load(0, context)
            program.load(1, writing)
            program.load(2, offset)
            program.load(3, host)
            program.load(4, len(payload))
            program.words += [0xe58d4000, 0xe12fff3b]
            program.load(1, results + index * 8 + (0 if writing else 4))
            program.words.append(0xe5810000)
    completion = results + len(rows) * 8
    program.write(context, state)
    program.write(context + 0x88, 0x180000)
    program.write(context + 0x2c, len(firmware))
    program.load(0, context)
    program.load(1, dump_base)
    program.load(12, base + 0x32f51)
    program.words.append(0xe12fff3c)
    program.write(completion, 0x5a17c0de)
    output = output.resolve()
    output.mkdir(parents=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-wifi-memory-") as name:
        root = Path(name)
        args = ["-M", "none", "-cpu", "cortex-r5", "-m", "16M", "-S",
                "-display", "none", "-nic", "none",
                "-device", "loader,addr=0x1000,cpu-num=0"]

        def load(label, address, data):
            path = root / label
            path.write_bytes(data)
            args.extend(["-device",
                         f"loader,file={path},addr={address:#x},force-raw=on"])

        load("fixture", 0x1000, program.image()[0x100:])
        load("transfer", base + 0x33014, original(0x33014, 0x11c))
        load("helpers", base + 0x336bc, original(0x336bc, 0x150))
        load("dump", base + 0x32f50, original(0x32f50, 0xac))
        load("bar", bar_base - GUARD_SIZE, b"\xa5" * (BAR_SIZE + GUARD_SIZE * 2))
        data = bytearray(used)
        for slot, (_, payload) in zip(slots, rows):
            data[slot:slot + len(payload)] = payload
        load("payloads", data_base, data)
        load("readback", read_base, b"\x5a" * used)
        load("dump-buffer", dump_base - GUARD_SIZE,
             b"\x5a" * (len(firmware) + GUARD_SIZE * 2))
        with QEMUMachine(str(qemu.resolve()), args=args) as vm:
            vm.launch()
            vm._qmp.settimeout(5)

            def read(address, size, label):
                path = root / label
                reply = vm.cmd(
                    "human-monitor-command",
                    command_line=f"pmemsave {address:#x} {size:#x} "
                                 f"{json.dumps(str(path))}")
                if reply:
                    raise RuntimeError(f"Original radio memory readback failed: {reply}")
                raw = path.read_bytes()
                if len(raw) != size:
                    raise RuntimeError(f"Incomplete radio memory capture: {label}")
                return raw

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
                    raise TimeoutError(f"Original radio transfer timed out: {state}")
                time.sleep(0.01)
            vm.cmd("stop")
            returned = read(results, len(rows) * 8, "results")
            copied = read(read_base, used, "copied")
            captures = []
            for slot, (_, payload) in zip(slots, rows):
                captures.append(copied[slot:slot + len(payload) + GUARD_SIZE * 2])
            verify_transfers(rows, returned, captures)
            verify_bar(rows, read(bar_base - GUARD_SIZE,
                                  BAR_SIZE + GUARD_SIZE * 2, "final-bar"))
            verify_dump(
                firmware, dump_base,
                read(dump_base - GUARD_SIZE, len(firmware) + GUARD_SIZE * 2,
                     "original-dump"),
                read(state + 0x18f4, 8, "dump-metadata"))
    if driver.read_bytes() != source or image.read_bytes() != firmware:
        raise RuntimeError("Private radio inputs changed during probe")
    report = {
        "driver_sha256": DRIVER_SHA256,
        "firmware_sha256": hashlib.sha256(firmware).hexdigest(),
        "original_transfer_and_helpers_unmodified": True,
        "functional_dependency_returns_stubbed": False,
        "cases": len(rows),
        "write_and_read_calls": len(rows) * 2,
        "complete_firmware_bytes_copied_and_read_back": len(firmware),
        "bar_size_fixture": BAR_SIZE,
        "bar_is_plain_ram_fixture": True,
        "original_return_values_verified": True,
        "host_readback_guards_verified": True,
        "entire_final_bar_and_outer_guards_verified": True,
        "per_case_bar_write_guards_verified": False,
        "original_chunked_dump_function_executed": "0x32f50",
        "original_dump_bytes_and_metadata_verified": True,
        "actual_chip_rom_captured": False,
        "pcie_endpoint_emulated": False,
        "radio_firmware_boot_verified": False,
        "wifi_ap_verified": False,
        "owned_vm_exited": True,
        "elapsed_seconds": time.monotonic() - started,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def verify_dump(firmware, address, data, metadata):
    if data != b"\x5a" * GUARD_SIZE + firmware + b"\x5a" * GUARD_SIZE:
        raise RuntimeError("Original chunked radio dump payload or guards differ")
    if metadata != struct.pack("<II", address, len(firmware)):
        raise RuntimeError("Original chunked radio dump metadata differs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.driver, args.image, args.output), indent=2))


if __name__ == "__main__":
    main()
