#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute original and prepared CarPlay coding/device/country activation gates."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import tempfile
import time

from prepare_carplay_country_policy import file_offset
from prepare_carplay_wireless_policy import ORIGINAL_SHA256, patch_wireless_policy
from probe_carplay_country_policy import InlineProgram
from probe_rcc import QEMUMachine, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--modified", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inputs = (args.original, args.modified)
    if (args.output.resolve() in {path.resolve() for path in inputs} or
            args.output.exists() and any(
                path.exists() and args.output.samefile(path) for path in inputs)):
        parser.error("Evidence output must not overwrite an input binary")
    source, modified = (path.read_bytes() for path in inputs)
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unrecognized original P2865 smartphone integrator")
    if modified != patch_wireless_policy(source):
        raise ValueError("Modified binary is not exactly the guarded CarPlay-only patch")

    def original(address, length, binary=source):
        at = file_offset(binary, address, length)
        return binary[at:at + length]

    stack, context, results, abi = 0x81f00000, 0x81200000, 0x81300000, 0x81400000
    device, handle, output = context + 0x1000, context + 0x2000, context + 0x3000
    bases = (0x81000000, 0x83000000)
    session_table, first_session, second_session = (
        context + 0x4000, context + 0x4100, context + 0x4200)
    session_states = (
        ("empty", 0, 1, 0, 0, False),
        ("wrong_type", 1, 1, 2, 0, False),
        ("first_clear", 1, 2, 1, 0, False),
        ("first_set", 1, 2, 2, 0, True),
        ("second_clear", 2, 1, 3, 1, False),
        ("second_set", 2, 1, 3, 2, True),
    )
    got = (0x919b4 + struct.unpack("<I", original(0x91ae4, 4))[0]) & 0xffffffff
    guard = (got + struct.unpack("<I", original(0x91ae8, 4))[0]) & 0xffffffff
    logger = (got + struct.unpack("<I", original(0x91aec, 4))[0]) & 0xffffffff
    program = InlineProgram()
    program.load(13, stack)
    for base in bases:
        program.write(base + guard, abi)
        program.write(base + logger, abi + 0x100)
    program.write(abi, 0x5a17c0de)
    program.write(abi + 0x140, 7)
    program.write(handle, 0x12345678)
    program.write(stack + 0xc, 1)
    program.write(stack + 0x1c, 0x76543210)
    program.write(stack + 0x20, device)
    program.write(device + 0x14, session_table)
    program.write(session_table, first_session)
    program.write(session_table + 4, second_session)
    program.write(second_session, 2)
    program.write(first_session + 0xc, 0)
    program.write(second_session + 0xc, 0)
    cases = []
    for (patched, wired, wireless, authorization, available, flags, state,
         session) in itertools.product(
            (0, 1), (0, 1), (0, 1), range(5), (0, 1),
            (0, 1, 2, 3, 0x80000000, 0x80000002), (0, 1, 2, 0xffffffff),
            session_states):
        label, count, first_type, first_flags, second_flags, existing = session
        program.write(context + 0x5c0, (wired << 16) | (wireless << 24))
        program.write(context + 0x5e8, authorization)
        program.write(context + 0x5f0, available)
        program.write(device + 0x198, flags)
        program.write(device + 0x19c, 0)
        program.write(device + 0xa8, state)
        program.write(device + 0x110, 0xdeadbeef)
        program.write(device + 0x10, count)
        program.write(first_session, first_type)
        program.write(first_session + 8, first_flags)
        program.write(second_session + 8, second_flags)
        program.load(4, output)
        program.load(8, context)
        program.load(9, handle)
        program.load(12, abi + 0x200 + patched * 0x10)
        program.words.append(0xe12fff3c)
        result = results + len(cases) * 12
        program.load(1, result)
        program.words.append(0xe5810000)
        for offset, address in ((4, context + 0x5f0), (8, device + 0x110)):
            program.load(1, address)
            program.words.append(0xe5910000)
            program.load(1, result + offset)
            program.words.append(0xe5810000)
        expected = (0 if not wireless else 1 if not (flags & 2 and state) else
                    2 if not (patched or available) else 4 if existing else 3)
        cases.append({
            "patched": bool(patched), "wired_coding": wired, "wireless_coding": wireless,
            "authorization_enum_fixture": authorization,
            "cached_country_availability": available, "device_flags_fixture": flags,
            "device_state_fixture": state, "expected_branch": expected,
            "session_list_fixture": label,
            "expected_transport_mode": 2 if wireless else 0xdeadbeef,
        })
    completion = results + len(cases) * 12
    program.write(completion, 0x5a17c0de)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-carplay-wireless-gates-") as name:
        root = Path(name)
        (root / "kernel").write_bytes(program.image()[0x100:])
        command = ["-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
                   "-S", "-display", "none", "-serial", "null", "-nic", "none"]

        def load(label, address, data):
            path = root / label
            path.write_bytes(data)
            command.extend(["-device",
                            f"loader,file={path},addr={address:#x},force-raw=on"])

        for index, (base, binary) in enumerate(zip(bases, (source, modified))):
            gate = bytearray(original(0x97d18, 0x97f0c - 0x97d18, binary))
            for address, marker in ((0x97d18, 0), (0x97e46, 1),
                                    (0x97d74, 2), (0x97f06, 3), (0x97eea, 4)):
                # Observe original branches, then return via the fixture-saved LR.
                gate[address - 0x97d18:address - 0x97d18 + 6] = struct.pack(
                    "<3H", 0x2000 | marker, 0x9924, 0x4708)
            load(f"{index}-activation", base + 0x97d18, gate)
            load(f"{index}-coding", base + 0x91954,
                 original(0x91954, 0x91af8 - 0x91954))
            load(f"{index}-device-state", base + 0x9d928, original(0x9d928, 0x22))
            load(f"{index}-session-list", base + 0x9ce0c, original(0x9ce0c, 0x64))
            load(f"{index}-session-state", base + 0x9d028, original(0x9d028, 0x38))
            load(f"{index}-entry", abi + 0x200 + index * 0x10,
                 struct.pack("<3I", 0xe58de090, 0xe51ff004, base + 0x97d39))
        with QEMUMachine(str(ROOT / "build/qemu-system-arm"), args=command) as vm:
            vm.launch()
            vm._qmp.settimeout(5)

            def word(address):
                reply = vm.cmd("human-monitor-command",
                               command_line=f"xp /1wx {address:#x}")
                return int(reply.split(":")[1].strip(), 16)

            vm.cmd("cont")
            deadline = time.monotonic() + 5
            while word(completion) != 0x5a17c0de:
                if time.monotonic() >= deadline:
                    registers = vm.cmd("human-monitor-command", command_line="info registers")
                    raise TimeoutError(f"Native wireless activation gates did not finish: {registers}")
                time.sleep(0.01)
            vm.cmd("stop")
            readback = root / "results"
            reply = vm.cmd(
                "human-monitor-command",
                command_line=f"pmemsave {results:#x} {len(cases) * 12:#x} "
                f"{json.dumps(str(readback))}")
            if reply:
                raise RuntimeError(f"Native wireless result readback failed: {reply}")
            observed = struct.unpack(f"<{len(cases) * 3}I", readback.read_bytes())
            for index, case in enumerate(cases):
                branch, cached, mode = observed[index * 3:index * 3 + 3]
                case.update(observed_branch=branch, observed_cached_availability=cached,
                            observed_transport_mode=mode)
                if (branch != case["expected_branch"] or
                        cached != case["cached_country_availability"] or
                        mode != case["expected_transport_mode"]):
                    raise AssertionError(f"Native wireless activation mismatch: {case}")
    result = {
        "scope": "continuous native CarPlay coding/device/cached-country activation gates",
        "source_sha256": ORIGINAL_SHA256,
        "modified_sha256": hashlib.sha256(modified).hexdigest(),
        "original_entry": "0x97d38, after device selection",
        "original_coding_predicate_executed": "0x919a4",
        "original_device_state_gate_executed": "0x9d928",
        "original_session_state_getter_executed": "0x9d028",
        "original_session_list_lookup_executed": "0x9ce0c",
        "branch_endpoints": {
            "0": "0x97d18: coding rejected",
            "1": "0x97e46: device state rejected",
            "2": "0x97d74: cached country availability veto",
            "3": "0x97f06: passed policy gates; before session activation call",
            "4": "0x97eea: passed policy gates; existing matching session state",
        },
        "fixture": {
            "selected_device_object": True, "wireless_transport_argument": True,
            "caller_stack_and_return": True, "logging_suppressed": True,
            "session_list": [state[0] for state in session_states],
            "functional_dependency_returns_stubbed": False,
        },
        "cached_availability_unchanged": True,
        "full_activation_function_executed": False,
        "device_selection_executed": False,
        "downstream_session_list_check_executed": True,
        "session_activation_executed": False,
        "shared_ap_policy_changed": False,
        "installed": False, "radio_ready_verified": False, "iphone_session_verified": False,
        "elapsed_host_seconds": time.monotonic() - started,
        "case_count": len(cases), "cases": cases,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "cases"}, indent=2))


if __name__ == "__main__":
    main()
