#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute original radio enumeration-ROM entry and address descriptor decoders."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import tempfile
import time

from prepare_carplay_country_policy import file_offset
from probe_carplay_country_policy import InlineProgram
from probe_rcc import QEMUMachine, ROOT
from probe_wifi_pci_match import DRIVER_SHA256


def expected_entry(words, mask, match):
    mismatched_bytes = 0
    for index, word in enumerate(words):
        consumed = (index + 1) * 4
        if not mask:
            return word, consumed
        if not word & 1:
            continue
        if word == 15 or word & mask == match:
            return word, consumed
        mismatched_bytes += 4
        if mismatched_bytes > 4095:
            return word, consumed
    raise ValueError("EROM fixture must terminate or reach the original mismatch limit")


def cases():
    rows = []
    for word, mask, match, prefix in itertools.product(
            (*range(16), 0x18000005, 0x180010c5, 0x43b83c01, 0xffffffff),
            (0, 1, 0xe, 0xffffffff), (0, 1, 4, 0xe),
            ((), (0, 2), (0, 7, 0))):
        stream = (*prefix, word, 15)
        rows.append((stream, mask, match, expected_entry(stream, mask, match)))
    for count in (1023, 1024, 1025):
        stream = (7,) * count + (1, 15)
        rows.append((stream, 0xe, 0, expected_entry(stream, 0xe, 0)))
    return rows


def validate_results(rows, data, starts):
    if len(data) != len(rows) * 8 or len(starts) != len(rows):
        raise RuntimeError("Incomplete original EROM observations")
    for index, (_, _, _, expected) in enumerate(rows):
        word, pointer = struct.unpack_from("<II", data, index * 8)
        if (word, pointer - starts[index]) != expected:
            raise RuntimeError(
                f"Original EROM filter mismatch in case {index}: "
                f"{word:#x}, {pointer - starts[index]} versus {expected}")


def address_cases():
    rows = []
    for port, kind, size_code, high_address, variant in itertools.product(
            (0, 3), (0, 0x40, 0x80, 0xc0), range(4), (0, 1),
            ("match", "wrong_port", "wrong_type", "wrong_tag")):
        for high_size in ((0, 1) if size_code == 3 else (0,)):
            descriptor = (0x18000000 | port << 8 | kind | size_code << 4 |
                          high_address << 3 | (1 if variant == "wrong_tag" else 5))
            words = [0, 2, descriptor]
            if high_address:
                words.append(0x12345678)
            if size_code == 3:
                words.append(0x200000 | high_size << 3)
                if high_size:
                    words.append(0x87654321)
            expected = (
                (descriptor, len(words) * 4, 0x18000000,
                 0x12345678 if high_address else 0,
                 0x200000 if size_code == 3 else 0x1000 << size_code,
                 0x87654321 if high_size else 0)
                if variant == "match" else (0, 8, *([0xdeadbeef] * 4)))
            words.append(15)
            requested_port = port if variant != "wrong_port" else port + 1
            requested_type = kind if variant != "wrong_type" else (kind + 0x40) & 0xc0
            rows.append((words, requested_port, requested_type, expected))
    return rows


def validate_addresses(rows, data, starts):
    if len(data) != len(rows) * 24 or len(starts) != len(rows):
        raise RuntimeError("Incomplete original EROM address observations")
    for index, (_, _, _, expected) in enumerate(rows):
        word, pointer, *fields = struct.unpack_from("<6I", data, index * 24)
        if (word, pointer - starts[index], *fields) != expected:
            raise RuntimeError(f"Original EROM address decoder mismatch in case {index}")


def probe(qemu, driver, output):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    source = driver.read_bytes()
    if hashlib.sha256(source).hexdigest() != DRIVER_SHA256:
        raise ValueError("Expected original P2865 BCM4359 QWDI driver SHA-256")

    def original(address, length):
        at = file_offset(source, address, length)
        return source[at:at + length]

    base, context, stack, streams, results = (
        0x100000, 0x200000, 0x240000, 0x280000, 0x300000)
    pointer, logger = context + 0x100, context + 0x200
    got = 0xbc8c + struct.unpack("<I", original(0xbd84, 4))[0]
    log_entry = got + struct.unpack("<I", original(0xbd88, 4))[0]
    rows = cases()
    program = InlineProgram()
    program.load(13, stack)
    program.write(base + log_entry, logger)
    program.write(logger, 0)
    program.load(11, base + 0xbc79)
    data = bytearray()
    starts = []
    for index, (words, mask, match, _) in enumerate(rows):
        start = streams + len(data)
        starts.append(start)
        data.extend(struct.pack(f"<{len(words)}I", *words))
        program.write(pointer, start)
        program.load(0, context)
        program.load(1, pointer)
        program.load(2, mask)
        program.load(3, match)
        program.words.append(0xe12fff3b)
        program.load(1, results + index * 8)
        program.words.append(0xe5810000)
        program.copy(pointer, results + index * 8 + 4)
    address_rows = address_cases()
    address_results = results + len(rows) * 8
    fields = context + 0x300
    address_starts = []
    program.load(11, base + 0xbd9d)
    address_got = 0xbdb0 + struct.unpack("<I", original(0xbee0, 4))[0]
    address_log_entry = address_got + struct.unpack("<I", original(0xbee4, 4))[0]
    program.write(base + address_log_entry, logger)
    for index, (words, port, kind, _) in enumerate(address_rows):
        start = streams + len(data)
        address_starts.append(start)
        data.extend(struct.pack(f"<{len(words)}I", *words))
        program.write(pointer, start)
        program.write(stack, kind)
        for field in range(4):
            program.write(fields + field * 4, 0xdeadbeef)
            program.write(stack + 4 + field * 4, fields + field * 4)
        program.load(0, context)
        program.load(1, pointer)
        program.load(2, port)
        program.load(3, 0)
        program.words.append(0xe12fff3b)
        result = address_results + index * 24
        program.load(1, result)
        program.words.append(0xe5810000)
        program.copy(pointer, result + 4)
        for field in range(4):
            program.copy(fields + field * 4, result + 8 + field * 4)
    completion = address_results + len(address_rows) * 24
    program.write(completion, 0x5a17c0de)
    output = output.resolve()
    output.mkdir(parents=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-wifi-erom-") as name:
        root = Path(name)
        args = ["-M", "none", "-cpu", "cortex-r5", "-m", "4M", "-S",
                "-display", "none", "-nic", "none",
                "-device", "loader,addr=0x1000,cpu-num=0"]
        for label, address, chunk in (
                ("fixture", 0x1000, program.image()[0x100:]),
                ("streams", streams, data),
                ("filter-and-address-decoder", base + 0xbc44, original(0xbc44, 0x2a8)),
                ("osh", base + 0x430b4, original(0x430b4, 0x1c))):
            path = root / label
            path.write_bytes(chunk)
            args.extend(["-device",
                         f"loader,file={path},addr={address:#x},force-raw=on"])
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
                    raise TimeoutError(f"Original EROM filter timed out: {state}")
                time.sleep(0.01)
            vm.cmd("stop")
            readback = root / "results"
            reply = vm.cmd(
                "human-monitor-command",
                command_line=f"pmemsave {results:#x} {completion - results:#x} "
                             f"{json.dumps(str(readback))}")
            if reply:
                raise RuntimeError(f"Original EROM readback failed: {reply}")
            captured = readback.read_bytes()
            validate_results(rows, captured[:len(rows) * 8], starts)
            validate_addresses(address_rows, captured[len(rows) * 8:], address_starts)
    if driver.read_bytes() != source:
        raise RuntimeError("Original radio driver changed during EROM probe")
    report = {
        "source_sha256": DRIVER_SHA256,
        "original_entry_filter_unmodified": True,
        "complete_function_executed": "0xbc78",
        "original_register_read_and_osh_accessor_executed": True,
        "entry_filter_cases": len(rows),
        "address_decoder_cases": len(address_rows),
        "cases": len(rows) + len(address_rows),
        "complete_original_address_decoder_executed": "0xbd9c",
        "address_size_high_words_and_rewind_verified": True,
        "returned_words_and_pointer_advancement_verified": True,
        "mismatch_scan_boundary_verified": [1023, 1024, 1025],
        "diagnostic_logging_disabled_by_fixture": True,
        "functional_dependency_returns_stubbed": False,
        "descriptor_streams_are_synthetic_fixtures": True,
        "actual_chip_erom_captured": False,
        "complete_ai_scan_executed": False,
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
