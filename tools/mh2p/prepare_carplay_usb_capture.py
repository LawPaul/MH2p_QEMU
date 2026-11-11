#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate private USB captures, with a separate explicit empty laboratory fixture."""
import argparse
import hashlib
import json
import logging
from pathlib import Path
import re


FORMATS = {
    "raw": {"namespace": 24354, "key": 2433, "length": 4, "offset": 0},
    "composite": {"namespace": 28442848, "key": 100, "length": 90, "offset": 30},
}
PROVENANCE_FIELDS = {
    "firmware", "hardware", "capture_method", "captured_at",
    "namespace", "key", "type", "length", "sha256",
}
MAPPING_SHA256 = "431747151c18bc4a1e9cc50ef183cfb30683e35d143ee88c3bf7ca0b082167a0"
EMPTY_LAB_RECORD = {
    "ns": 24354, "key": 2433, "length": 4, "type": "blob",
    "stonebackup": {
        "pathA": "/tmp/mh2p-lab-usb-ports-A.dat",
        "pathB": "/tmp/mh2p-lab-usb-ports-B.dat",
        "default": [0, 0, 0, 0], "waitforpath": "/tmp",
    },
}


def empty_lab_mapping(source):
    """Prepend one synthetic default using the original BLOB stonebackup schema."""
    if hashlib.sha256(source).hexdigest() != MAPPING_SHA256 or not source.startswith(b"[\n"):
        raise ValueError("Unrecognized original persistence mapping")
    entry = json.dumps(EMPTY_LAB_RECORD, separators=(",", ":")).encode()
    return b"[\n" + entry + b",\n" + source[2:]


def validate_capture(raw, provenance, source_format):
    if source_format not in FORMATS:
        raise ValueError("Select raw or composite capture format")
    spec = FORMATS[source_format]
    if not isinstance(provenance, dict) or set(provenance) != PROVENANCE_FIELDS:
        raise ValueError("Expected exactly the documented capture provenance fields")
    for field in ("firmware", "hardware", "capture_method", "captured_at"):
        value = provenance[field]
        if (not isinstance(value, str) or not value.strip() or
                any(ord(character) < 32 or ord(character) == 127 for character in value)):
            raise ValueError(f"Expected nonempty, single-line provenance {field}")
    for field in ("namespace", "key", "length"):
        if type(provenance[field]) is not int or provenance[field] != spec[field]:
            raise ValueError(f"Capture {field} does not match selected format")
    if provenance["type"] != "BLOB":
        raise ValueError("Capture type must be BLOB")
    if len(raw) != spec["length"]:
        raise ValueError(f"Expected exactly {spec['length']} captured bytes")
    digest = hashlib.sha256(raw).hexdigest()
    if provenance["sha256"] != digest:
        raise ValueError("Capture SHA-256 does not match provenance")
    return raw[spec["offset"]:spec["offset"] + 4]


def prepare(source, provenance_source, output, source_format):
    raw = source.read_bytes()
    provenance_raw = provenance_source.read_bytes()
    provenance = json.loads(provenance_raw)
    ports = validate_capture(raw, provenance, source_format)
    manifest = {
        "operation": "prepare-private-carplay-usb-capture",
        "source": str(source.resolve()),
        "provenance_source": str(provenance_source.resolve()),
        "provenance_sha256": hashlib.sha256(provenance_raw).hexdigest(),
        "source_format": source_format,
        "provenance": provenance,
        "extraction": {"offset": FORMATS[source_format]["offset"], "length": 4},
        "record": {
            "namespace": 24354, "key": 2433, "type": "BLOB", "length": 4,
            "sha256": hashlib.sha256(ports).hexdigest(),
        },
        "decoded_port_mask": sum((byte & 1) << index
                                 for index, byte in enumerate(ports)),
        "source_authenticity_independently_verified": False,
        "installed": False,
        "coding_or_fec_supplied": False,
        "component_activation_verified": False,
        "live_hmi_dsi_delivery_verified": False,
    }
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    with (output / "usb-ports.blob").open("xb") as stream:
        stream.write(ports)
    with (output / "manifest.json").open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    return manifest


def restore_capture(shell, source, provenance_source, output, source_format):
    """Write only an explicitly supplied capture, then require exact original readback."""
    manifest = prepare(source, provenance_source, output, source_format)
    ports = (output / "usb-ports.blob").read_bytes()
    result = _write_ports_record(shell, ports, output)
    result["record"] = manifest["record"]
    result["source_authenticity_independently_verified"] = False
    (output / "restore-result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def install_empty_lab_ports(shell, output, *, mapping_default=False):
    """Explicit disposable-guest fixture, never represented as captured vehicle data."""
    ports = bytes(4)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / "usb-ports.blob").write_bytes(ports)
    manifest = {
        "operation": "install-synthetic-empty-usb-ports",
        "synthetic": True,
        "initial_mapping_default": mapping_default,
        "record": {
            "namespace": 24354, "key": 2433, "type": "BLOB", "length": 4,
            "sha256": hashlib.sha256(ports).hexdigest(),
        },
        "decoded_port_mask": 0,
        "coding_or_fec_supplied": False,
        "authentic_vehicle_configuration": False,
        "warning": "Disposable emulator fixture: no USB ports enabled; not captured unit data.",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    result = _write_ports_record(shell, ports, output)
    result.update(manifest)
    (output / "restore-result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def _write_ports_record(shell, ports, output):
    if len(ports) != 4:
        raise ValueError("Expected exactly four USB-port record bytes")
    write_command = (
        f"/eso/bin/dumb_persistence_writer --flush 24354 2433 {ports.hex()} "
        ">/tmp/carplay-usb-write.log 2>&1; "
        "USB_WRITE_STATUS=$?; "
        "cat /tmp/carplay-usb-write.log; "
        'echo CARPLAY_USB_WRITE_STATUS:$USB_WRITE_STATUS')
    update_command = (
        "/eso/bin/dumb_persistence_subscriber 24354 2433 "
        ">/tmp/carplay-usb-events 2>/tmp/carplay-usb-sub-error & USB_SUB_PID=$!; "
        f"USB_EXPECTED=$(printf 'BLOB\\t{ports.hex()}'); USB_SEEN=0; USB_N=0; "
        'while [ "$USB_N" -lt 256 ]; do '
        'if grep -x "$USB_EXPECTED" /tmp/carplay-usb-events >/dev/null; then '
        'USB_SEEN=1; break; fi; if ! kill -0 "$USB_SUB_PID" 2>/dev/null; then break; fi; '
        "USB_N=$((USB_N + 1)); done; "
        'kill -9 "$USB_SUB_PID"; wait "$USB_SUB_PID"; '
        "cat /tmp/carplay-usb-events; "
        'if [ "$USB_SEEN" = 1 ] && ! kill -0 "$USB_SUB_PID" 2>/dev/null; then '
        "echo CARPLAY_USB_UPDATE_OBSERVED; else "
        "cat /tmp/carplay-usb-sub-error; echo CARPLAY_USB_UPDATE_FAILED; fi")
    read_command = (
        "/eso/bin/dumb_persistence_reader 24354 2433 "
        ">/tmp/carplay-usb-read.log 2>&1; "
        "USB_READ_STATUS=$?; "
        "echo CARPLAY_USB_READ_BEGIN; cat /tmp/carplay-usb-read.log; "
        "echo CARPLAY_USB_READ_END; "
        'echo CARPLAY_USB_READ_STATUS:$USB_READ_STATUS')

    def execute(command, name):
        response = shell(command)
        if isinstance(response, bytes):
            response = response.decode()
        (output / name).write_text(response)
        return response.replace("\r", "")

    def fail(message):
        diagnostic = shell(
            "echo CARPLAY_USB_FAILURE_DIAGNOSTIC_BEGIN; "
            "pidin -p persistenceTNG_mibhigh ar; pidin -p rccpersistencebackend ar; "
            "tail -100 /tmp/persistence.log; "
            "tail -100 /tmp/development-persistence-transport.log; "
            "grep -E 'partition=24354|key=2433' /tmp/native-traceserver.log | tail -80; "
            "echo CARPLAY_USB_FAILURE_DIAGNOSTIC_END")
        if isinstance(diagnostic, bytes):
            diagnostic = diagnostic.decode()
        (output / "failure-diagnostic.log").write_text(diagnostic)
        raise ValueError(message)

    write_reply = execute(write_command, "guest-write.log")
    (output / "guest-restore.log").write_text(write_reply)
    if re.findall(r"(?m)^CARPLAY_USB_WRITE_STATUS:([0-9]+)$", write_reply) != ["0"]:
        fail("Original USB record write failed or is ambiguous")
    update_reply = execute(update_command, "guest-update.log")
    if re.findall(r"(?m)^CARPLAY_USB_UPDATE_OBSERVED$", update_reply) != [
            "CARPLAY_USB_UPDATE_OBSERVED"] or not re.search(
                rf"(?m)^BLOB\t{ports.hex()}$", update_reply):
        fail("Original USB record update notification missing or ambiguous")
    normalized = execute(read_command, "guest-restore.log")
    if re.findall(r"(?m)^CARPLAY_USB_READ_STATUS:([0-9]+)$", normalized) != ["0"]:
        fail("Original USB record read failed or is ambiguous")
    blocks = re.findall(
        r"(?m)^CARPLAY_USB_READ_BEGIN\n(.*?)^CARPLAY_USB_READ_END$",
        normalized, re.S)
    if len(blocks) != 1:
        fail("Original USB record readback is missing or ambiguous")
    notifications = []
    data = []
    for line in blocks[0].strip().splitlines():
        if re.fullmatch(
                r'\[pfsn\] (?:New|Gone) PID=[0-9]+ PPID=[0-9]+'
                r'(?: Prio=[0-9]+)?(?: "[^"\n]*")+(?: \.\.\.)?', line):
            notifications.append(line)
            logging.getLogger(__name__).warning(
                "Separate process notification in original USB readback: %s", line)
        else:
            data.append(line)
    if data != [ports.hex()]:
        fail("Original USB record readback differs from supplied bytes")
    result = {
        "decoded_port_mask": sum((byte & 1) << index for index, byte in enumerate(ports)),
        "original_writer_exit_status": 0,
        "original_reader_exit_status": 0,
        "exact_guest_readback_verified": True,
        "original_value_notification_verified": True,
        "owned_subscriber_stopped": True,
        "separate_process_notifications": notifications,
        "source_authenticity_independently_verified": False,
        "component_activation_verified": False,
        "live_hmi_dsi_delivery_verified": False,
    }
    return result


def verify_restored_ports(readiness, restored):
    ports = readiness.get("usb_metadata", {}).get("ports", {})
    if (ports.get("valid") is not True or type(ports.get("value")) is not int or
            ports["value"] != restored["decoded_port_mask"]):
        raise ValueError("Original integrator has not accepted the restored USB port value")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--format", choices=FORMATS, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New private directory, outside the repository")
    args = parser.parse_args()
    try:
        manifest = prepare(args.capture, args.provenance, args.output, args.format)
    except (OSError, UnicodeError, ValueError) as exc:
        parser.exit(1, f"USB capture preparation failed: {exc}\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
