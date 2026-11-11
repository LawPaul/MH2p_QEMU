#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute original P2865 CarPlay coding decoders and policy gates without provisioning."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import tempfile
import time

from probe_rcc import QEMUMachine, ROOT
from probe_carplay_country_policy import InlineProgram
from prepare_carplay_country_policy import file_offset


ORIGINAL_SHA256 = "c140c1b03a9f70cf465af5d8012a2b2cd69f51d915f44167529292f39127ccc0"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Private original smartphone_integrator")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (args.output.resolve() == args.binary.resolve() or
            args.output.exists() and args.binary.exists() and
            args.output.samefile(args.binary)):
        parser.error("Evidence output must not overwrite the original binary")
    source = args.binary.read_bytes()
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unrecognized original P2865 smartphone integrator")

    def original(address, length):
        at = file_offset(source, address, length)
        return source[at:at + length]

    fragments = [
        (0x91954, 0x91af8),  # Availability setter and complete coding predicate.
        (0x8db34, 0x8dc44),  # Original config-gated country notification.
        (0x62f08, 0x630b0),  # Original initialized configuration-cache lookup.
        (0x630fc, 0x63140),  # Original boolean configuration accessor.
        (0x84e1c, 0x84e2c),  # Original typed boolean extraction.
        (0xc543c, 0xc54c8),  # Original USB and app-coding validity gates.
        (0x6c33c, 0x6c6dc),  # Original USB and application-coding decoders.
    ]
    got_base = (0x919b4 + struct.unpack("<I", original(0x91ae4, 4))[0]) & 0xffffffff
    guard_got = (got_base + struct.unpack("<I", original(0x91ae8, 4))[0]) & 0xffffffff
    logger_got = (got_base + struct.unpack("<I", original(0x91aec, 4))[0]) & 0xffffffff
    base, context, results, abi = 0x81000000, 0x81200000, 0x81300000, 0x81400000
    program = InlineProgram()
    program.load(13, 0x81f00000)
    program.write(base + guard_got, abi)
    program.write(abi, 0x5a17c0de)
    program.write(base + logger_got, abi + 0x100)
    # Suppress the unlinked diagnostic branch; no functional dependency is stubbed.
    program.write(abi + 0x140, 7)
    program.write(base + got_base + 0x101c, abi + 0x100)
    program.write(base + got_base + 0x1410, abi + 0x200)
    program.write(abi + 0x200, 1)
    cache = base + 0x10b7d8 + 0x84 * 32
    program.write(cache, 1)
    program.write(cache + 0x10, 1)
    cases = []
    for wired, wireless, authorization, require, transport in itertools.product(
            (0, 1), (0, 1), range(5), (0, 1), (1, 2)):
        program.write(context + 0x5c0, (wired << 16) | (wireless << 24))
        program.write(context + 0x5e8, authorization)
        program.load(0, context)
        program.load(1, 2)  # App selector used by original wireless activation.
        program.load(2, transport)
        program.load(3, require)
        program.load(12, base + 0x919a5)
        program.words.append(0xe12fff3c)
        program.load(1, results + len(cases) * 4)
        program.words.append(0xe5810000)
        permitted = authorization == 3 if require else authorization in (2, 3)
        cases.append({
            "wired_coding": wired, "wireless_coding": wireless,
            "authorization_enum_fixture": authorization,
            "require_authorization_argument": require, "transport": transport,
            "expected": wireless if transport == 2 else int(wired and permitted),
        })
    country_cases = []
    owner = context + 0x1000
    controller = owner + 0x118
    program.write(owner + 0x4c8, 0)
    for adaptation, before, notification, wireless in itertools.product((0, 1), repeat=4):
        program.write(cache + 0x18, adaptation)
        program.write(controller + 0x5f0, before)
        program.write(controller + 0x5c0, wireless << 24)
        program.load(0, owner)
        program.load(1, notification)
        program.load(12, base + 0x8db35)
        program.words.append(0xe12fff3c)
        program.load(1, controller + 0x5f0)
        program.words.append(0xe5d10000)  # ldrb r0, [r1]
        result = results + 0x400 + len(country_cases) * 8
        program.load(1, result)
        program.words.append(0xe5810000)
        program.load(0, controller)
        program.load(1, 2)
        program.load(2, 2)
        program.load(3, 1)
        program.load(12, base + 0x919a5)
        program.words.append(0xe12fff3c)
        program.load(1, result + 4)
        program.words.append(0xe5810000)
        country_cases.append({
            "country_adaptation_config_fixture": adaptation,
            "cached_availability_before": before,
            "availability_notification": notification,
            "wireless_coding": wireless,
            "expected_availability": notification if adaptation else before,
            "expected_coding_result": wireless,
        })
    ingestion_cases = []
    payload_address = 0x81f0000c
    configuration_owner = abi + 0x400
    payload_order = (2, 3, 0, 1, 4, 5, 6, 7, 8, 9)
    decoders = (
        (0x6c384, 8, 0), (0x6c3c8, 23, 3),
        (0x6c40c, 7, 7), (0x6c44c, 23, 2),
        (0x6c5a0, 8, 5), (0x6c490, 7, 6),
        (0x6c4d4, 23, 0), (0x6c518, 20, 2),
        (0x6c55c, 23, 1), (0x6c5e4, 23, 4),
    )
    blob_address, vector, parser_object = abi + 0xa00, abi + 0x900, abi + 0x980
    program.write(parser_object + 4, vector)
    program.write(vector, blob_address)
    for wired, wireless, unrelated, length in itertools.product(
            (0, 1), (0, 1), (0, 0xff), (0, 7, 8, 9, 20, 21, 23, 24, 25)):
        blob = bytearray([unrelated] * 28)
        blob[8] = (unrelated & ~1) | wired
        blob[23] = (unrelated & ~8) | (wireless << 3)
        expected_wired = wired if length > 8 else 0
        expected_wireless = wireless if length > 23 else 0
        payload = bytearray(10)
        for field, (_, byte, bit) in enumerate(decoders):
            payload[payload_order[field]] = (blob[byte] >> bit) & 1 if length > byte else 0
        for offset in range(0, len(blob), 4):
            program.write(blob_address + offset,
                          int.from_bytes(blob[offset:offset + 4], "little"))
        program.write(vector + 4, blob_address + length)
        program.load(4, configuration_owner)
        program.load(6, parser_object)
        program.load(12, base + 0xc80fd)
        program.words.append(0xe12fff3c)
        result = results + 0x800 + len(ingestion_cases) * 40
        for field in (0, 1):
            program.load(1, configuration_owner + 0x29c + field * 8)
            program.words.append(0xe5910000)
            program.load(1, result + 24 + field * 4)
            program.words.append(0xe5810000)
        program.load(0, configuration_owner + 0x298)
        program.load(12, base + 0xc54a5)
        program.words.append(0xe12fff3c)
        program.load(1, result + 32)
        program.words.append(0xe5810000)
        program.load(4, configuration_owner)
        program.load(7, payload_address)
        program.load(12, base + 0xc766b)
        program.words.append(0xe12fff3c)
        program.write(owner + 0x114, 0)
        program.load(4, payload_address)
        program.load(5, owner)
        program.load(12, base + 0x90459)
        program.words.append(0xe12fff3c)
        program.load(1, owner + 0x6d8)
        program.words.append(0xe891000d)  # ldmia r1, {r0, r2, r3}
        program.load(1, result)
        program.words.append(0xe881000d)  # stmia r1, {r0, r2, r3}
        program.load(1, owner + 0x115)
        program.words.append(0xe5d10000)
        program.load(1, result + 12)
        program.words.append(0xe5810000)
        program.write(controller + 0x5e8, 3)
        for transport in (1, 2):
            program.load(0, controller)
            program.load(1, 2)
            program.load(2, transport)
            program.load(3, 1)
            program.load(12, base + 0x919a5)
            program.words.append(0xe12fff3c)
            program.load(1, result + 12 + transport * 4)
            program.words.append(0xe5810000)
        ingestion_cases.append({
            "blob_hex": bytes(blob[:length]).hex(),
            "blob_length": length,
            "payload_hex": payload.hex(),
            "wired_coding": expected_wired, "wireless_coding": expected_wireless,
            "expected_wired_valid": int(length > 8),
            "expected_wireless_valid": int(length > 23),
            "expected_aggregate_valid": int(length > 23),
            "unrelated_payload_bytes": unrelated,
        })
    validity_rows = bytearray()
    validity_cases = []
    for mask in range(1024):
        row = bytearray(80)
        for field in range(10):
            row[field * 8 + 4] = (mask >> field) & 1
            row[field * 8 + 5] = field & 1
        validity_rows += row
        validity_cases.append({
            "validity_mask": mask,
            "expected": int(all(mask & (1 << field) for field in (0, 2, 4, 5, 7, 9))),
        })
    program.load(8, abi + 0x1000)
    program.load(9, results + 0x2000)
    program.load(10, abi + 0x1000 + len(validity_rows))
    loop = len(program.words)
    program.words.append(0xe1a00008)
    program.load(12, base + 0xc54a5)
    program.words += [
        0xe12fff3c, 0xe4890004,  # blx r12; str r0, [r9], #4
        0xe2888050, 0xe158000a,  # add r8, #80; cmp r8, r10
    ]
    program.words.append(0x1a000000 | ((loop - len(program.words) - 2) & 0xffffff))
    publication_base = 0x84000000
    publication_rows = bytearray()
    publication_cases = []
    for mask, previous_equal in itertools.product(range(1024), (0, 1)):
        current, previous = bytearray(80), bytearray(80)
        for field in range(10):
            valid = (mask >> field) & 1
            current[field * 8 + 4] = previous[field * 8 + 4] = valid
            current[field * 8 + 5] = field & 1
            previous[field * 8 + 5] = (field & 1) ^ (not previous_equal)
        publication_rows += current + previous
        publication_cases.append({
            "validity_mask": mask, "previous_values_equal": bool(previous_equal),
            "expected_publication_branch": int(
                all(mask & (1 << field) for field in (0, 2, 4, 5, 7, 9)) and
                not previous_equal),
        })
    program.write(publication_base + guard_got, abi)
    program.write(publication_base + logger_got, abi + 0x100)
    program.load(8, results + 0x5000)
    program.load(10, 0x81600000)
    program.load(11, 0x81600000 + len(publication_rows))
    loop = len(program.words)
    program.load(9, configuration_owner + 0x298)
    for _ in range(5):
        program.words += [0xe8ba00ff, 0xe8a900ff]  # Copy current/previous objects.
    program.load(0, configuration_owner)
    program.load(12, publication_base + 0xc7569)
    program.words += [0xe12fff3c, 0xe4880004, 0xe15a000b]
    program.words.append(0x1a000000 | ((loop - len(program.words) - 2) & 0xffffff))
    usb_validity_cases = []
    usb_object = abi + 0x400
    for mask, nonzero_values in itertools.product(range(8), (0, 1)):
        program.write(usb_object + 4, mask & 1)
        program.write(usb_object + 8, 3 if nonzero_values else 0)
        program.write(usb_object + 0x10,
                      ((mask >> 1) & 1) | (15 << 8 if nonzero_values else 0))
        program.write(usb_object + 0x18,
                      ((mask >> 2) & 1) | (1 << 8 if nonzero_values else 0))
        program.load(0, usb_object)
        program.load(12, base + 0xc543d)
        program.words.append(0xe12fff3c)
        program.load(1, results + 0x9000 + len(usb_validity_cases) * 4)
        program.words.append(0xe5810000)
        usb_validity_cases.append({
            "validity_mask": mask, "nonzero_values": bool(nonzero_values),
            "expected": int(mask == 7),
        })
    usb_port_cases = []
    for mask, length, high_bits in itertools.product(range(16), range(6), (0, 0xfe)):
        blob = bytes(high_bits | ((mask >> port) & 1) for port in range(4)) + b"\xff\0\0\0"
        program.write(blob_address, int.from_bytes(blob[:4], "little"))
        program.write(blob_address + 4, int.from_bytes(blob[4:], "little"))
        program.write(vector + 4, blob_address + length)
        program.load(0, parser_object)
        program.load(12, base + 0x6c629)
        program.words.append(0xe12fff3c)
        program.load(1, results + 0x9100 + len(usb_port_cases) * 4)
        program.words.append(0xe5810000)
        usb_port_cases.append({
            "blob_hex": blob[:length].hex(), "length": length,
            "input_port_mask": mask, "high_bits": high_bits,
            "expected": mask & ((1 << min(length, 4)) - 1),
        })
    completion = results + 0x8000
    program.write(completion, 0x5a17c0de)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-original-carplay-coding-") as name:
        root = Path(name)
        (root / "kernel").write_bytes(program.image()[0x100:])
        command = [
            "-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
            "-S", "-display", "none", "-serial", "null", "-nic", "none",
        ]
        for start, end in fragments:
            path = root / f"original-{start:x}"
            path.write_bytes(original(start, end - start))
            command += [
                "-device",
                f"loader,file={path},addr={base + start:#x},force-raw=on",
            ]
        copy_path = root / "original-coding-copy"
        copy_path.write_bytes(original(0x90458, 0x18) + b"\x70\x47")
        command += [
            "-device",
            f"loader,file={copy_path},addr={base + 0x90458:#x},force-raw=on",
        ]
        event_path = root / "original-coding-event-pack"
        # The fragment uses LR as a data register; preserve the fixture return.
        event_path.write_bytes(
            b"\x00\xb5" + original(0xc766c, 0xc76c2 - 0xc766c) + b"\x00\xbd")
        rows_path = root / "validity-inputs"
        rows_path.write_bytes(validity_rows)
        update_path = root / "original-coding-field-update"
        # Match the original local stack frame and preserve caller registers.
        update_path.write_bytes(
            b"\xf0\xb5\x9c\xb0\x05\xad\x28\x46\x31\x46\x00\xbf" +
            original(0xc8108, 0xc8246 - 0xc8108) + b"\x1c\xb0\xf0\xbd")
        publication_path = root / "original-coding-publication-gate"
        publication_gate = bytearray(original(0xc7568, 0xc77f4 - 0xc7568))
        for address, marker in ((0xc759a, 0), (0xc7666, 1)):
            endpoint = struct.pack("<H", 0x2000 | marker) + original(0xc7646, 6)
            publication_gate[address - 0xc7568:address - 0xc7568 + len(endpoint)] = endpoint
        publication_path.write_bytes(publication_gate)
        equality_path = root / "original-coding-validity-equality"
        equality_path.write_bytes(original(0xc54a4, 0xc55fc - 0xc54a4))
        publication_rows_path = root / "publication-inputs"
        publication_rows_path.write_bytes(publication_rows)
        command += [
            "-device",
            f"loader,file={event_path},addr={base + 0xc766a:#x},force-raw=on",
            "-device",
            f"loader,file={rows_path},addr={abi + 0x1000:#x},force-raw=on",
            "-device",
            f"loader,file={update_path},addr={base + 0xc80fc:#x},force-raw=on",
            "-device",
            f"loader,file={publication_path},addr={publication_base + 0xc7568:#x},force-raw=on",
            "-device",
            f"loader,file={equality_path},addr={publication_base + 0xc54a4:#x},force-raw=on",
            "-device",
            f"loader,file={publication_rows_path},addr=0x81600000,force-raw=on",
        ]
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
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    raise TimeoutError(f"Original coding predicate did not finish: {registers}")
                time.sleep(0.01)
            vm.cmd("stop")
            for index, case in enumerate(cases):
                case["observed"] = word(results + index * 4)
                if case["observed"] != case["expected"]:
                    raise AssertionError(f"Original coding predicate mismatch: {case}")
            for index, case in enumerate(country_cases):
                case["observed_availability"] = word(results + 0x400 + index * 8)
                case["observed_coding_result"] = word(results + 0x404 + index * 8)
                if (case["observed_availability"] != case["expected_availability"] or
                        case["observed_coding_result"] != case["expected_coding_result"]):
                    raise AssertionError(f"Original country callback mismatch: {case}")
            for index, case in enumerate(ingestion_cases):
                result = results + 0x800 + index * 40
                copied = b"".join(struct.pack("<I", word(result + offset))
                                  for offset in (0, 4, 8))[:10]
                case["observed_payload_hex"] = copied.hex()
                case["observed_initialized"] = word(result + 12)
                case["observed_wired_result"] = word(result + 16)
                case["observed_wireless_result"] = word(result + 20)
                case["observed_wired_valid"] = word(result + 24) & 0xff
                case["observed_wireless_valid"] = word(result + 28) & 0xff
                case["observed_aggregate_valid"] = word(result + 32)
                if (copied.hex() != case["payload_hex"] or
                        case["observed_initialized"] != 1 or
                        case["observed_wired_result"] != case["wired_coding"] or
                        case["observed_wireless_result"] != case["wireless_coding"] or
                        case["observed_wired_valid"] != case["expected_wired_valid"] or
                        case["observed_wireless_valid"] != case["expected_wireless_valid"] or
                        case["observed_aggregate_valid"] != case["expected_aggregate_valid"]):
                    raise AssertionError(f"Original coding ingestion mismatch: {case}")
            validity_output = root / "validity-results"
            reply = vm.cmd(
                "human-monitor-command",
                command_line=f"pmemsave {results + 0x2000:#x} 0x1000 "
                f"{json.dumps(str(validity_output))}")
            if reply:
                raise RuntimeError(f"Native coding validity readback failed: {reply}")
            observed = struct.unpack("<1024I", validity_output.read_bytes())
            for case, value in zip(validity_cases, observed):
                case["observed"] = value
                if value != case["expected"]:
                    raise AssertionError(f"Original coding validity mismatch: {case}")
            publication_output = root / "publication-results"
            reply = vm.cmd(
                "human-monitor-command",
                command_line=f"pmemsave {results + 0x5000:#x} 0x2000 "
                f"{json.dumps(str(publication_output))}")
            if reply:
                raise RuntimeError(f"Native coding publication readback failed: {reply}")
            observed = struct.unpack("<2048I", publication_output.read_bytes())
            for case, value in zip(publication_cases, observed):
                case["observed_publication_branch"] = value
                if value != case["expected_publication_branch"]:
                    raise AssertionError(f"Original coding publication gate mismatch: {case}")
            for offset, rows in ((0x9000, usb_validity_cases), (0x9100, usb_port_cases)):
                for index, case in enumerate(rows):
                    case["observed"] = word(results + offset + index * 4)
                    if case["observed"] != case["expected"]:
                        raise AssertionError(f"Original USB metadata mismatch: {case}")
    result = {
        "scope": "original blob decoders, coding event fragments, validity and policy gates",
        "coding_persistence_record": {
            "namespace": struct.unpack("<I", original(0xe1344, 4))[0],
            "key": struct.unpack("<I", original(0xe1360, 4))[0],
        },
        "source_sha256": ORIGINAL_SHA256,
        "wireless_coding_byte": "controller+0x5c3",
        "wired_coding_byte": "controller+0x5c2",
        "functional_dependency_returns_stubbed": False,
        "country_update_fixture": {
            "entry": "unchanged original 0x8db34",
            "original_config_accessors_executed": True,
            "initialized_config_cache_fixture": True,
            "connected_device_list_empty": True,
            "session_dispatch_verified": False,
            "radio_availability_verified": False,
        },
        "coding_ingestion_fixture": {
            "original_wired_blob_decoder": "0x6c384",
            "original_wireless_blob_decoder": "0x6c3c8",
            "wired_blob_bit": "byte 0x08 bit 0",
            "wireless_blob_bit": "byte 0x17 bit 3",
            "all_ten_original_blob_decoders_executed": True,
            "original_decoder_to_field_transfer_executed": True,
            "decoder_to_field_original_range": "0xc8108..0xc8246",
            "decoder_to_field_fixture_stack_frame": True,
            "other_application_fields_are_fixtures": False,
            "original_aggregate_validity_gate_executed": True,
            "invalid_aggregate_publication_verified": False,
            "aggregate_publication_decision_separately_verified": True,
            "event_pack_original_range": "0xc766c..0xc76c2",
            "event_pack_return_endpoint": "0xc76c2, before event publication",
            "event_pack_fixture_return_saved_on_stack": True,
            "event_pack_stack_payload_fixture": "original sp+0x10 after fixture push",
            "original_range": "0x90458..0x90470",
            "return_endpoint": "0x90470, before event signaling and session redispatch",
            "controller_owner_offset": "0x118",
            "owner_payload_offset": "0x6d8",
            "wired_payload_byte": 2,
            "wireless_payload_byte": 3,
            "persistence_subscription_executed": False,
            "complete_coding_callback_executed": False,
        },
        "coding_validity_fixture": {
            "entry": "unchanged original 0xc54a4",
            "required_field_indices": [0, 2, 4, 5, 7, 9],
            "config_record_loading_executed": False,
        },
        "coding_publication_fixture": {
            "original_entry": "0xc7568",
            "unchanged_validity_and_equality_helpers_executed": True,
            "branch_observation_endpoints": {
                "0xc759a": "not published; before previous-state synchronization",
                "0xc7666": "publication selected; before event construction",
            },
            "actual_event_queue_delivery_executed": False,
        },
        "licenses_provisioned": False, "qnx_boot": False, "iphone_session": False,
        "elapsed_host_seconds": time.monotonic() - started,
        "cases": cases,
        "country_update_cases": country_cases,
        "coding_ingestion_cases": ingestion_cases,
        "coding_validity_cases": validity_cases,
        "coding_publication_cases": publication_cases,
        "usb_metadata_fixture": {
            "validity_entry": "unchanged original 0xc543c",
            "port_decoder_entry": "unchanged original 0x6c628",
            "port_record": {"namespace": 24354, "key": 2433},
            "port_record_encoding": "bit 0 of each of the first four bytes",
            "persistence_read_or_event_delivery_verified": False,
            "unit_metadata_provisioned": False,
        },
        "usb_validity_cases": usb_validity_cases,
        "usb_port_decoder_cases": usb_port_cases,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("cases", "country_update_cases",
                                     "coding_ingestion_cases", "coding_validity_cases",
                                     "coding_publication_cases", "usb_validity_cases",
                                     "usb_port_decoder_cases")},
                     indent=2))
    print(f"Verified {len(cases)} original coding cases and "
          f"{len(country_cases)} original config/country-update cases and "
          f"{len(ingestion_cases)} original coding-ingestion cases and "
          f"{len(validity_cases)} original coding-validity cases and "
          f"{len(publication_cases)} original coding-publication gate cases and "
          f"{len(usb_validity_cases)} original USB-validity cases and "
          f"{len(usb_port_cases)} original USB-port decoder cases")


if __name__ == "__main__":
    main()
