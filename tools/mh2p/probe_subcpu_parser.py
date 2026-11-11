#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe an isolated original SubCPU parser call, not a serial exchange."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from prepare_subcpu_elf import prepare
from probe_subcpu import DIAGNOSTICS, inspect_trace
from probe_subcpu_checksum import (
    CALL_ADDRESS, prepare_diagnostic_call, validate_frame,
)


QUEUE_INITIALIZER = 0x662d
PARSER_INITIALIZER = 0xbf00
PARSER_ADDRESS = 0xc041
RUNTIME_RAM_CLEAR = 0x1257
IMMEDIATE_DISPATCHER = 0x6558
QUEUE_RAM_BOUNDARY = {
    "address": "0xfe720", "pc_after_instruction": "0x06225",
}


def queue_expected(frame):
    expected = {
        0xfe854: 0x8c, 0xfe855: 0x8b,
        0xfe856: 0x40, 0xfe857: 0x8c,
        0xfe858: 0xc3, 0xfe859: 0xf3,
        0xfe85a: 0, 0xfe85b: 0,
        0xfe8a4: 1, 0xfe8ae: 0,
    }
    expected.update({0xfe8a4 + index: 0 for index in range(1, 10)})
    expected.update({
        0xff3c3 + index: byte
        for index, byte in enumerate(frame[3:])
    })
    expected.update({
        0xff3c3 + index: 0
        for index in range(len(frame) - 3, 267)
    })
    return expected


def delivery_expected(frame):
    expected = {
        0xfe758 + index: byte for index, byte in enumerate(frame[5:13])
    }
    expected.update({0xfe838 + index: int(index == 0) for index in range(28)})
    expected.update({0xfe8a4 + index: 0 for index in range(10)})
    expected.update({0xfe747: 1, 0xfe8ae: 1, 0xfe8b9: 2})
    return expected


def prepare_call(original_elf, frame, *, runtime_init=False, dispatch=False):
    validate_frame(frame)
    if dispatch and not runtime_init:
        raise ValueError("Immediate dispatch requires explicit runtime init")
    code = bytearray(b"\x61\xcf\xcb\xf8\x20\xfe")

    def call(address):
        code.extend(b"\xfc" + address.to_bytes(3, "little"))

    def check_bytes(expected):
        for address, byte in expected.items():
            code.extend(
                b"\x8f" + struct.pack("<H", address & 0xffff)
                + b"\x4c" + bytes([byte]) + b"\xdd\x04\xcf\x01\x00\x00")

    if runtime_init:
        # Match the reset routine's HL argument, not host-written RAM contents.
        code.extend(b"\x36\x0c\xe7")
        call(RUNTIME_RAM_CLEAR)
    call(QUEUE_INITIALIZER)
    call(PARSER_INITIALIZER)
    for byte in frame[:-1]:
        code.extend(b"\x30" + struct.pack("<H", byte))
        call(PARSER_ADDRESS)
    expected = {
        0xff4ce: (len(frame) - 4) & 0xff,
        0xff4cf: (len(frame) - 4) >> 8,
        0xff4d0: 5,
        0xff4d2: (len(frame) - 4) & 0xff,
        0xff4d3: (len(frame) - 4) >> 8,
        0xfe8ae: 0,
    }
    expected.update({0xfe8a4 + index: 0 for index in range(10)})
    expected.update({
        0xff2b8 + index: byte
        for index, byte in enumerate(frame[3:-1])
    })
    expected.update({
        0xff2b8 + index: 0
        for index in range(len(frame) - 4, 267)
    })
    expected.update({0xff3c3 + index: 0 for index in range(267)})
    check_bytes(expected)
    code.extend(b"\x30" + struct.pack("<H", frame[-1]))
    call(PARSER_ADDRESS)
    if runtime_init:
        check_bytes(queue_expected(frame))
    if dispatch:
        call(IMMEDIATE_DISPATCHER)
        check_bytes(delivery_expected(frame))
    code.extend(b"\xcf\x02\x00\x00")
    return prepare_diagnostic_call(original_elf, code), len(expected)


def observe(source, frame_path, simulator, output, *, timeout=5,
            runtime_init=False, dispatch=False):
    if dispatch and not runtime_init:
        raise ValueError("Immediate dispatch requires explicit runtime init")
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Timeout must be positive")
    if frame_path.stat().st_size > 270:
        raise ValueError("Expected one SubCPU frame, at most 270 bytes")
    frame = frame_path.read_bytes()
    validate_frame(frame)
    simulator = simulator.resolve(strict=True)
    simulator_hash = hashlib.sha256(simulator.read_bytes()).hexdigest()
    metadata = prepare(source, output)
    call, checks = prepare_call(
        (output / "subcpu.elf").read_bytes(), frame,
        runtime_init=runtime_init, dispatch=dispatch)
    image = output / "parser-call.elf"
    image.write_bytes(call)
    (output / "frame.bin").write_bytes(frame)
    environment = dict(os.environ)
    for variable in DIAGNOSTICS.values():
        environment.pop(variable, None)
    if dispatch:
        environment[DIAGNOSTICS["f13_irq"]] = "1"
    timed_out = False
    log = output / "parser-trace.log"
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
    return_pc = CALL_ADDRESS + struct.unpack_from("<I", call, 100)[0]
    returned = (
        signature and not timed_out and exit_code == 1 and ram_read is None
        and access == {
            "operation": "write", "address": "0xf0002",
            "pc_after_instruction": f"{return_pc:#07x}",
        })
    queue_boundary = (
        signature and not timed_out and exit_code == 1 and access is None
        and ram_read == QUEUE_RAM_BOUNDARY)
    report = {
        "source_sha256": metadata["source_sha256"],
        "all_source_bytes_preserved": True,
        "diagnostic_entry": hex(CALL_ADDRESS),
        "original_queue_initializer": hex(QUEUE_INITIALIZER),
        "original_parser_initializer": hex(PARSER_INITIALIZER),
        "original_parser_routine": hex(PARSER_ADDRESS),
        "diagnostic_runtime_ram_clear_call_added": runtime_init,
        "original_runtime_ram_clear_routine":
            hex(RUNTIME_RAM_CLEAR) if runtime_init else None,
        "original_runtime_ram_clear_hl_argument":
            "0xe70c" if runtime_init else None,
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
        "pre_completion_exact_byte_checks": checks,
        "pre_completion_checks_passed": returned or queue_boundary,
        "original_queue_envelope_ram_boundary_observed": queue_boundary,
        "diagnostic_return_marker_observed": returned,
        "post_completion_exact_byte_checks":
            len(queue_expected(frame)) if runtime_init else 0,
        "original_parser_queue_bytes_verified": runtime_init and returned,
        "diagnostic_immediate_dispatch_call_added": dispatch,
        "original_immediate_dispatcher":
            hex(IMMEDIATE_DISPATCHER) if dispatch else None,
        "diagnostic_f13_irq_configuration_enabled": dispatch,
        "post_dispatch_exact_byte_checks":
            len(delivery_expected(frame)) if dispatch else 0,
        "original_receive_event_delivery_verified": dispatch and returned,
        "original_frame_checksum_validation_verified": False,
        "original_reset_executed": False,
        "subcpu_board_boot_verified": False,
        "subcpu_exchange_verified": False,
        "rstp_verified": False,
        "hmi_ready": False,
        "note": (
            "Isolated original queue/parser initializers and byte-at-a-time "
            "calls. Synthetic caller, register bank, SP and AX arguments "
            "only; expected-state checks read, never seed, original RAM. "
            "The optional runtime clear calls unchanged routine 0x1257 "
            "with the reset code's HL=e70c argument before the initializers. "
            "This explicitly isolated ordering is not original reset "
            "execution or a fix for the pre-runtime board-state dependency. "
            "When enabled, post-call checks verify the queue envelope, "
            "occupancy, index and entire completed buffer. Optional "
            "immediate dispatch calls 0x6558 with explicit IRQ configuration "
            "retention, then checks the exact eight-byte received event, "
            "all normal/immediate occupancy flags and queue counts/state. "
            "No normal-queue application dispatch, MCU ROM, pin level, "
            "interrupt event, "
            "serial completion or peer reply is supplied. "
            "A return marker alone does not prove frame acceptance, queue "
            "delivery, checksum validation or a serial exchange. "
            "An unwritten queue-envelope "
            "read is a diagnostic boundary, not completed parser execution."),
    }
    (output / "observation.json").write_text(
        json.dumps(report, indent=2) + "\n")
    if not signature:
        raise ValueError("Runner lacks the strict MH2P diagnostic signature")
    if timed_out:
        raise TimeoutError(f"SubCPU parser call timed out; see {log}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--frame", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument(
        "--diagnostic-runtime-init", action="store_true",
        help="Call original RAM clear before isolated parser initialization "
             "and check queued bytes; not reset execution or a serial reply")
    parser.add_argument(
        "--diagnostic-dispatch", action="store_true",
        help="Call original immediate consumer and check received event "
             "delivery; requires --diagnostic-runtime-init and enables "
             "IRQ configuration retention, not interrupt generation")
    args = parser.parse_args()
    try:
        report = observe(args.image, args.frame, args.simulator,
                         args.output.resolve(), timeout=args.timeout,
                         runtime_init=args.diagnostic_runtime_init,
                         dispatch=args.diagnostic_dispatch)
    except (OSError, ValueError, TimeoutError) as exc:
        parser.exit(1, f"MH2P SubCPU parser diagnostic failed: {exc}\n")
    print(json.dumps(report, indent=2))
    parser.exit(1, "Isolated observation is not a verified SubCPU exchange.\n")


if __name__ == "__main__":
    main()
