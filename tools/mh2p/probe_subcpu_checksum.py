#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Call the original SubCPU checksum routine, not its reset or board startup."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from prepare_subcpu_elf import PAYLOAD_OFFSET, prepare
from probe_subcpu import DIAGNOSTICS, inspect_trace


CALL_ADDRESS = 0x30000
CHECKSUM_ADDRESS = 0xbb11
RETURN_MARKER = 0xf0002


def validate_frame(frame):
    if not 14 <= len(frame) <= 270 or frame[:2] != b"\xf7\xc1":
        raise ValueError("Expected one complete F7 C1 SubCPU frame")
    length = int.from_bytes(frame[3:5], "little")
    if not 10 <= length <= 266 or len(frame) != length + 4:
        raise ValueError("SubCPU frame length does not match its wire size")


def prepare_call(original_elf, frame):
    validate_frame(frame)
    # The strict loader rejects RAM segments; use CPU stores for the input.
    writes = b"".join(
        b"\xcf" + struct.pack("<HB", 0xf010 + index, byte)
        for index, byte in enumerate(frame[3:]))
    code = b"\x61\xcf\xcb\xf8\x20\xfe" + writes  # sel rb0; movw sp,#0xfe20
    code += b"\xfc" + CHECKSUM_ADDRESS.to_bytes(3, "little")
    code += (b"\x8f\xfa\xfe"     # mov a,!0xffefa (return register C)
             b"\x4c\x01\xdd\x04"  # cmp a,#1; bz accepted
             b"\xcf\x01\x00\x00"  # wrong-return stop
             b"\xcf\x02\x00\x00")  # accepted-return stop
    return prepare_diagnostic_call(original_elf, code)


def prepare_diagnostic_call(original_elf, code):
    call_offset = (len(original_elf) + 0xfff) & ~0xfff
    names = b"\0.text\0.shstrtab\0.diagnostic_call\0"
    names_offset = call_offset + len(code)
    sections_offset = (names_offset + len(names) + 3) & ~3
    elf = bytearray(sections_offset + 4 * 40)
    elf[:len(original_elf)] = original_elf
    struct.pack_into("<I", elf, 24, CALL_ADDRESS)
    struct.pack_into("<I", elf, 32, sections_offset)
    struct.pack_into("<H", elf, 44, 2)
    struct.pack_into("<H", elf, 48, 4)
    struct.pack_into("<8I", elf, 84, 1, call_offset, CALL_ADDRESS, CALL_ADDRESS,
                     len(code), len(code), 5, 1)
    elf[call_offset:call_offset + len(code)] = code
    elf[names_offset:names_offset + len(names)] = names
    sections = (
        (1, 1, 6, 0, PAYLOAD_OFFSET,
         struct.unpack_from("<I", original_elf, 52 + 16)[0], 0, 0, 1, 0),
        (7, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
        (17, 1, 6, CALL_ADDRESS, call_offset, len(code), 0, 0, 1, 0),
    )
    for index, section in enumerate(sections, 1):
        struct.pack_into("<10I", elf, sections_offset + index * 40, *section)
    return bytes(elf)


def observe(source, frame_path, simulator, output, *, timeout=5):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Timeout must be positive")
    if frame_path.stat().st_size > 270:
        raise ValueError("Expected one SubCPU frame, at most 270 bytes")
    frame = frame_path.read_bytes()
    # Validate input before creating an observation directory.
    validate_frame(frame)
    simulator = simulator.resolve(strict=True)
    simulator_hash = hashlib.sha256(simulator.read_bytes()).hexdigest()
    metadata = prepare(source, output)
    call = prepare_call((output / "subcpu.elf").read_bytes(), frame)
    image = output / "checksum-call.elf"
    image.write_bytes(call)
    (output / "frame.bin").write_bytes(frame)
    environment = dict(os.environ)
    for variable in DIAGNOSTICS.values():
        environment.pop(variable, None)
    timed_out = False
    log = output / "checksum-trace.log"
    with log.open("xb") as stream:
        try:
            result = subprocess.run(
                [str(simulator), "-v", "-d", str(image)],
                stdout=stream, stderr=subprocess.STDOUT, env=environment,
                timeout=timeout, check=False)
            exit_code = result.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            exit_code = None
    signature, access, ram_read = inspect_trace(log)
    return_pc = CALL_ADDRESS + struct.unpack_from("<8I", call, 84)[4]
    returned = (
        signature and not timed_out and exit_code == 1 and ram_read is None
        and access == {
            "operation": "write", "address": f"{RETURN_MARKER:#07x}",
            "pc_after_instruction": f"{return_pc:#07x}",
        })
    report = {
        "source_sha256": metadata["source_sha256"],
        "all_source_bytes_preserved": True,
        "diagnostic_entry": hex(CALL_ADDRESS),
        "original_checksum_routine": hex(CHECKSUM_ADDRESS),
        "diagnostic_call_sha256": hashlib.sha256(call).hexdigest(),
        "frame_sha256": hashlib.sha256(frame).hexdigest(),
        "frame_bytes": len(frame),
        "host_checksum_matches": sum(frame[3:-1]) & 0xff == frame[-1],
        "simulator_sha256": simulator_hash,
        "strict_diagnostic_signature_present": signature,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "first_unsupported_access": access,
        "first_uninitialized_ram_read": ram_read,
        "original_checksum_returned_accept": returned,
        "original_reset_executed": False,
        "subcpu_board_boot_verified": False,
        "subcpu_exchange_verified": False,
        "rstp_verified": False,
        "hmi_ready": False,
        "note": (
            "Isolated original-code call with synthetic SP, register-bank "
            "selection, input RAM and return-check instructions. No board "
            "RAM, GPIO, MCU ROM, serial peer or startup state is supplied. "
            "Rejection may stop in original diagnostic logging before return. "
            "This is not original reset execution or a verified UART "
            "exchange."),
    }
    (output / "observation.json").write_text(
        json.dumps(report, indent=2) + "\n")
    if not signature:
        raise ValueError("Runner lacks the strict MH2P diagnostic signature")
    if timed_out:
        raise TimeoutError(f"SubCPU checksum call timed out; see {log}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--frame", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args()
    try:
        report = observe(args.image, args.frame, args.simulator,
                         args.output.resolve(), timeout=args.timeout)
    except (OSError, ValueError, TimeoutError) as exc:
        parser.exit(1, f"MH2P SubCPU checksum diagnostic failed: {exc}\n")
    print(json.dumps(report, indent=2))
    if not report["original_checksum_returned_accept"]:
        parser.exit(1, "Original checksum acceptance was not observed.\n")


if __name__ == "__main__":
    main()
