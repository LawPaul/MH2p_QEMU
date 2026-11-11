#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare original and prepared country/channel branches under ARM TCG."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import tempfile
import time

from probe_rcc import QEMUMachine, ROOT
from test_rcc_ipu import ArmProgram
from prepare_carplay_country_policy import ORIGINAL_SHA256, patch_country_policy


class InlineProgram(ArmProgram):
    def load(self, register, value):
        for opcode, immediate in ((0xe3000000, value & 0xffff),
                                  (0xe3400000, value >> 16)):
            self.words.append(opcode | register << 12 |
                              (immediate & 0xf000) << 4 | (immediate & 0xfff))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--modified", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    inputs = (arguments.original, arguments.modified)
    if (arguments.output.resolve() in {path.resolve() for path in inputs} or
            arguments.output.exists() and any(
                path.exists() and arguments.output.samefile(path) for path in inputs)):
        parser.error("Evidence output must not overwrite an input binary")
    source = arguments.original.read_bytes()
    modified = arguments.modified.read_bytes()
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unrecognized original connection manager")
    if modified != patch_country_policy(source):
        raise ValueError("Modified binary is not exactly the guarded country-policy patch")
    phoff = struct.unpack_from("<I", source, 28)[0]
    stride, count = struct.unpack_from("<HH", source, 42)
    segments = [struct.unpack_from("<8I", source, phoff + i * stride)
                for i in range(count)]

    def original(start, length, binary=source):
        for kind, offset, base, _, size, _, _, _ in segments:
            if kind == 1 and base <= start < start + length <= base + size:
                return binary[offset + start - base:offset + start - base + length]
        raise ValueError(f"Original file range absent: {start:#x}, {length}")

    literal = original(0x6e7c4, 4)
    table_address = 0x6dbba + struct.unpack("<i", literal)[0]
    table = original(table_address, 165)
    codes = [bytearray(original(0x6da60, 0x6dbce - 0x6da60 + 6, binary))
             for binary in (source, modified)]
    # These are branch-observation endpoints, not replacement native returns.
    # Only the unknown-country diagnostic logger is replaced; no functional
    # dependency returns are supplied.
    for address, marker in ((0x6da7e, 1), (0x6dbce, 0)):
        for code in codes:
            instruction = 0x4630 if marker else 0x2000  # Return actual availability r6.
            struct.pack_into("<HHH", code, address - 0x6da60,
                             instruction, 0x9924, 0x4708)
    patterns = {
        "country_no_channels": [0] * 23,
        "country_2ghz_only": [1] * 13 + [0] * 10,
        "country_channel36_only": [0] * 13 + [1] + [0] * 9,
        "country_5ghz_groups": [0] * 13 + [1] * 9 + [0],
        "unknown_country_fallback": [0] * 23,
    }
    stack, results, policy_object = 0x80300000, 0x80400000, 0x80500000
    rows_address = 0x80600000
    inputs = bytearray()
    cases = []
    for pattern, bitmap in patterns.items():
        unknown = pattern == "unknown_country_fallback"
        for channel, log_level in itertools.product(
                (*range(167), 0xffffffff), range(6) if unknown else (4,)):
            for enabled, patched, valid, adaptation in itertools.product((0, 1), repeat=4):
                classification = table[channel - 1] if 1 <= channel <= 165 else 22
                channel_expected = int(
                    (13 <= classification <= 21 if patched else
                     not unknown and any(bitmap[13:22]) and bitmap[classification]) and enabled)
                entry = 0x6db24 if unknown else 0x6da60
                inputs += bytes(bitmap) + b"\0" + struct.pack(
                    "<5I", channel, enabled,
                    (0x81000000 if patched else 0x80000000) + entry + 1,
                    adaptation | (valid << 16), log_level)
                cases.append({
                    "country_pattern": pattern, "selected_channel": channel,
                    "unknown_country_fallback": unknown, "diagnostic_log_level": log_level,
                    "patched": patched,
                    "adaptation_valid": valid, "adaptation_enabled": adaptation,
                    "channel_enabled": bool(enabled), "classification": classification,
                    "expected_channel_branch": channel_expected,
                    "expected_branch": channel_expected & valid & adaptation,
                })
    program = InlineProgram()
    program.load(13, stack)
    program.load(10, rows_address)
    program.load(11, results)
    program.load(8, policy_object)
    program.write(stack + 0x20, policy_object + 0x100)
    program.write(stack + 0x48, 0)
    loop = len(program.words)
    program.words.append(0xe8ba003f)  # LDMIA r10!, bitmap words r0-r5.
    program.load(7, stack + 0x9c)
    program.words.append(0xe887003f)
    program.load(7, policy_object + 0x218)
    program.words.append(0xe887003f)
    program.load(7, policy_object + 0x200)
    program.words.append(0xe8ba001f)
    program.load(12, policy_object + 0x100)
    program.words += [
        0xe58c4040,
        0xe58d007c, 0xe58d1080,  # Selected channel and enabled flag.
        0xe1a09003, 0xe3a06000,
    ]
    program.load(12, 0x80800000)
    program.words += [
        0xe12fff3c,
        0xe58b0000,
    ]
    program.load(1, policy_object + 0x2b0)
    program.words.append(0xe5810000)
    program.load(1, policy_object + 0xee4)
    program.words += [0xe5819000, 0xe1a00008]
    program.load(12, 0x80067f8d)
    program.words += [0xe12fff3c, 0xe58b0004, 0xe28bb008]
    program.load(7, rows_address + len(inputs))
    program.words.append(0xe15a0007)
    program.words.append(0x1a000000 | ((loop - len(program.words) - 2) & 0xffffff))
    completion = results + len(cases) * 8
    program.write(completion, 0x5a17c0de)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-original-channel-check-") as tmp:
        root = Path(tmp)
        (root / "kernel").write_bytes(program.image()[0x100:])
        (root / "inputs").write_bytes(inputs)
        (root / "entry").write_bytes(struct.pack("<II", 0xe58de090, 0xe12fff12))
        args = ["-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
                "-S", "-display", "none", "-serial", "null", "-nic", "none",
                "-device", f"loader,file={root / 'inputs'},addr={rows_address:#x},force-raw=on",
                "-device", f"loader,file={root / 'entry'},addr=0x80800000,force-raw=on"]
        for index, code in enumerate(codes):
            base = 0x80000000 + index * 0x1000000
            for name, address, data in (
                    ("branches", base + 0x6da60, code),
                    ("literal", base + 0x6e7c4, literal),
                    ("unknown-log-literal", base + 0x6e7bc, original(0x6e7bc, 4)),
                    ("diagnostic-logger", base + 0x28924, struct.pack("<I", 0xe12fff1e)),
                    ("classifications", base + table_address, table)):
                path = root / f"{index}-{name}"
                path.write_bytes(data)
                args += ["-device", f"loader,file={path},addr={address:#x},force-raw=on"]
        for start, end in ((0x67f8c, 0x67fa0), (0x44aa8, 0x44aba)):
            path = root / f"predicate-{start:x}"
            path.write_bytes(original(start, end - start))
            args += ["-device", f"loader,file={path},addr={0x80000000 + start:#x},force-raw=on"]
        with QEMUMachine(str(ROOT / "build/qemu-system-arm"), args=args) as vm:
            vm.launch()
            vm._qmp.settimeout(5)

            def word(address):
                response = vm.cmd("human-monitor-command",
                                  command_line=f"xp /1wx {address:#x}")
                return int(response.split(":")[1].strip(), 16)

            vm.cmd("cont")
            deadline = time.monotonic() + 5
            while word(completion) != 0x5a17c0de:
                if time.monotonic() >= deadline:
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    raise TimeoutError(f"Original channel branches did not finish: {registers}")
                time.sleep(0.01)
            vm.cmd("stop")
            readback = root / "results"
            reply = vm.cmd("human-monitor-command",
                           command_line=f"pmemsave {results:#x} {len(cases) * 8:#x} "
                           f"{json.dumps(str(readback))}")
            if reply:
                raise RuntimeError(f"Native policy result readback failed: {reply}")
            observed = readback.read_bytes()
            if len(observed) != len(cases) * 8:
                raise RuntimeError("Incomplete native policy readback")
            for index, case in enumerate(cases):
                case["observed_channel_branch"], case["observed_branch"] = (
                    struct.unpack_from("<II", observed, index * 8))
                if (case["observed_channel_branch"] != case["expected_channel_branch"] or
                        case["observed_branch"] != case["expected_branch"]):
                    raise AssertionError(f"Original channel branch mismatch: {case}")
    result = {
        "scope": "original versus patched country/channel branches with original classification table",
        "source_sha256": ORIGINAL_SHA256,
        "modified_sha256": hashlib.sha256(modified).hexdigest(),
        "branch_markers": {"0x6da7e": "return actual availability register r6",
                           "0x6dbce": "reject before logging"},
        "dependency_returns_stubbed": True,
        "functional_dependency_returns_stubbed": False,
        "diagnostic_logger_only_stubbed": "0x28924",
        "unknown_country_fallback_executed": True,
        "unknown_country_entry": "0x6db24, after fallback-name string construction",
        "fallback_name_string_construction_executed": False,
        "fixture_return_saved_on_stack": True,
        "unchanged_adaptation_predicate_executed": True,
        "country_table_resolution_executed": False,
        "radio_ready": False, "qnx_boot": False, "iphone_session": False,
        "elapsed_host_seconds": time.monotonic() - started,
        "cases": cases,
    }
    arguments.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "cases"},
                     indent=2))
    print(f"Verified {len(cases)} original branch cases")


if __name__ == "__main__":
    main()
