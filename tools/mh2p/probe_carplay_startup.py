#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Request original TerminalMode startup without bypassing its dependencies."""
import hashlib
import json
import logging
import re
import struct

from hmi_diagnosis import DiagnosisError
from probe_carplay_coding import ORIGINAL_SHA256


START_COMMAND = "processcommand,101,cmdStartApp,TerminalMode"
SUCCESS = "response_processcommand   Call executed successfully."
READS = {
    "applications": ("getapps", "response_getapps"),
    "bundles": ("getbundles", "response_getbundles"),
    "startup_commands": ("getcommands,101", "response_getcommands"),
    "domain_status": ("getvalue,101,getDomainStatus", "response_getvalue"),
    "lastmode": ("getvalue,101,getLastmodeApp", "response_getvalue"),
    "screen": ("getvalue,102,getCurrentScreenData", "response_getvalue"),
}

CONTROLLER_SIZE = 0xe50
CONFIGURATION_OFFSET = 0xa70


def parse_memory_dump(reply, length):
    matches = re.findall(r"(?m)^CARPLAY_MEMORY_BEGIN\n(.*?)^CARPLAY_MEMORY_END$",
                         reply.replace("\r", ""), re.S)
    if len(matches) != 1:
        raise DiagnosisError("Expected one original process memory dump")
    data = bytearray()
    lines = matches[0].strip().splitlines()
    for line in lines[:-1]:
        if re.fullmatch(
                r'\[pfsn\] (?:New|Gone) PID=[0-9]+ PPID=[0-9]+'
                r'(?: Prio=[0-9]+)?(?: "[^"\n]*")+(?: \.\.\.)?', line):
            logging.getLogger(__name__).warning(
                "Separate process notification in original memory dump: %s", line)
            continue
        fields = line.split()
        if (len(fields) < 2 or not re.fullmatch(r"[0-9A-Fa-f]+", fields[0]) or
                int(fields[0], 16) != len(data) or
                any(not re.fullmatch(r"[0-9A-Fa-f]{2}", value) for value in fields[1:])):
            raise DiagnosisError("Malformed or failed original process memory read")
        data.extend(bytes.fromhex(" ".join(fields[1:])))
    if (not lines or not re.fullmatch(r"[0-9A-Fa-f]+", lines[-1]) or
            int(lines[-1], 16) != length or len(data) != length):
        raise DiagnosisError("Incomplete original process memory read")
    return bytes(data)


def decode_native_readiness(data, base):
    if len(data) != CONTROLLER_SIZE:
        raise DiagnosisError("Incomplete original controller object")
    def word(offset):
        return struct.unpack_from("<I", data, offset)[0]

    if (word(0) != base + 0x106268 or
            word(CONFIGURATION_OFFSET) != base + 0x107320):
        raise DiagnosisError("Original controller/configuration vtable identity mismatch")
    flags = data[0x114:0x118]
    if any(value not in (0, 1) for value in flags):
        raise DiagnosisError("Invalid original controller readiness boolean")
    metadata = {}
    for name, offset, wide in (("coding", 0x260, True), ("ports", 0x26c, False),
                               ("hub_support", 0x274, False)):
        at = CONFIGURATION_OFFSET + offset
        valid = data[at + 4]
        if valid not in (0, 1):
            raise DiagnosisError(f"Invalid original USB {name} validity boolean")
        metadata[name] = {
            "valid": bool(valid),
            "value": (word(at + 8) if wide else data[at + 5]) if valid else None,
        }
    return {
        **dict(zip(("USBInfoValid", "AppInfoValid", "DSIStarted", "PersistenceRead"),
                   map(bool, flags))),
        "pending_runmode": word(0x11c),
        "usb_metadata": metadata,
        "missing_usb_metadata": [name for name, field in metadata.items()
                                 if not field["valid"]],
        "readiness_memory_written": False,
        "component_activation_verified": False,
        "live_hmi_dsi_delivery_verified": False,
    }


def capture_native_readiness(shell, binary, output):
    """Resolve each live process afresh; never infer readiness from a failed read."""
    if hashlib.sha256(binary.read_bytes()).hexdigest() != ORIGINAL_SHA256:
        raise DiagnosisError("Expected original P2865 smartphone integrator")
    output.mkdir(parents=True, exist_ok=False)

    def execute(command, name):
        reply = shell(command)
        if isinstance(reply, bytes):
            reply = reply.decode()
        (output / name).write_text(reply)
        return reply

    reply = execute("pidin -p smartphone_integrator mem", "process-map.log")
    pids = set(re.findall(r"(?m)^ *([0-9]+) +[0-9]+ +rtphone_integrator +", reply))
    bases = re.findall(r"(?m)^ +rtphone_integrator @([0-9a-fA-F]+) +1028K +4096 *\r?$",
                       reply)
    if len(pids) != 1 or len(bases) != 1:
        raise DiagnosisError("Expected one current original integrator process and text map")
    pid, base = int(pids.pop()), int(bases[0], 16)

    def memory(address, length, name):
        if (address % 4 or length % 4 or
                not 0 < address < address + length <= 0x100000000):
            raise DiagnosisError("Invalid original process memory range")
        command = (
            "echo CARPLAY_MEMORY_BEGIN; "
            f"dd if=/proc/{pid}/as bs=4 skip={address // 4} count={length // 4} "
            "2>/tmp/carplay-memory-error | /armle/usr/bin/hd -v -A x -t x1; "
            "echo CARPLAY_MEMORY_END; cat /tmp/carplay-memory-error")
        return parse_memory_dump(execute(command, name), length)

    pointer = struct.unpack("<I", memory(base + 0x10e180, 4, "singleton.log"))[0]
    data = memory(pointer, CONTROLLER_SIZE, "controller.log")
    report = decode_native_readiness(data, base)
    report.update(pid=pid, elf_load_base=base, controller_address=pointer)
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def parse_domains(reply):
    prefix = "response_getvalueDomain Startup Times:"
    if not isinstance(reply, str) or not reply.startswith(prefix):
        raise DiagnosisError("Expected original startup domain-status reply")
    domains = {}
    current = None
    for line in reply[len(prefix):].replace("<newline>", "\n").splitlines():
        line = line.strip()
        if not line:
            continue
        header = re.fullmatch(r"Domain: ([0-9]+) = ([A-Za-z0-9_]+)", line)
        if header:
            identity = int(header[1])
            if identity in domains:
                raise DiagnosisError(f"Duplicate startup domain {identity}")
            current = {"name": header[2], "events": []}
            domains[identity] = current
            continue
        event = re.fullmatch(r"([0-9]+) (request|response|update)\(0x([0-9a-fA-F]+)\)", line)
        if current is None or event is None:
            raise DiagnosisError(f"Unexpected startup domain-status line: {line!r}")
        observed = {"time": int(event[1]), "kind": event[2], "state": int(event[3], 16)}
        if current["events"] and observed["time"] < current["events"][-1]["time"]:
            raise DiagnosisError("Startup domain timestamps moved backwards")
        current["events"].append(observed)
    for identity, name in ((3, "Media"), (37, "SmartphoneIntegration")):
        if identity not in domains or domains[identity]["name"] != name:
            raise DiagnosisError(f"Expected original {name} domain {identity}")
    return domains


def assess_domains(before, after):
    initial = parse_domains(before)
    final = parse_domains(after)
    previous = initial[37]["events"]
    observed = final[37]["events"]
    if observed[:len(previous)] != previous:
        raise DiagnosisError("Smartphone domain history no longer extends the initial capture")
    fresh = observed[len(previous):]
    request_indices = [index for index, event in enumerate(fresh)
                       if event["kind"] == "request" and event["state"] == 4]
    following = fresh[request_indices[-1] + 1:] if request_indices else []
    smartphone_updates = [event for event in observed if event["kind"] == "update"]
    media_updates = [event for event in final[3]["events"] if event["kind"] == "update"]
    state = smartphone_updates[-1]["state"] if smartphone_updates else None
    media_state = media_updates[-1]["state"] if media_updates else None
    return {
        "fresh_smartphone_domain_request_observed": bool(request_indices),
        "fresh_smartphone_events": fresh,
        "required_smartphone_state": 4,
        "last_observed_smartphone_state": state,
        "smartphone_dependency_bits_currently_satisfied": (
            state & 4 == 4 if state is not None else None),
        "smartphone_required_state_observed_after_request": any(
            event["kind"] in ("update", "response") and event["state"] & 4 == 4
            for event in following),
        "required_media_dependency_state": 0x4d00,
        "last_observed_media_state": media_state,
        "media_dependency_bits_currently_satisfied": (
            media_state & 0x4d00 == 0x4d00 if media_state is not None else None),
        "component_activation_verified": False,
        "target_classes_loaded_verified": False,
        "live_hmi_dsi_delivery_verified": False,
    }


def snapshot(diagnosis):
    return {name: diagnosis(command, expected)
            for name, (command, expected) in READS.items()}


def assess_class_loading(reply):
    """Observe exact original JVM class-load lines, not classpath installation."""
    from prepare_carplay_java_overlay import OVERLAY_ENTRIES

    targets = [name.removesuffix(".class") for name in OVERLAY_ENTRIES]
    loaded = set(re.findall(r"(?m)^class load: ([A-Za-z0-9_/$]+)\r?$", reply))
    missing = [name for name in targets if name not in loaded]
    return {
        "loaded_target_classes": [name for name in targets if name in loaded],
        "missing_target_classes": missing,
        "target_classes_loaded_verified": not missing,
        "contact_slot_helper_loaded": "mh2p/lab/CarplayContactSlots" in loaded,
        "component_activation_verified": False,
        "live_hmi_dsi_delivery_verified": False,
        "iphone_session_verified": False,
    }


def terminalmode_registered(reply):
    prefix = "response_getapps"
    if not isinstance(reply, str) or not reply.startswith(prefix):
        raise DiagnosisError("Expected original application listing")
    values = reply[len(prefix):].split(",")
    if values[-1] == "":
        values.pop()
    if len(values) % 2:
        raise DiagnosisError("Incomplete original application listing")
    applications = {}
    for index in range(0, len(values), 2):
        identity, name = values[index:index + 2]
        if not re.fullmatch(r"[0-9]+", identity) or not name.strip():
            raise DiagnosisError("Malformed original application entry")
        identity = int(identity)
        if identity == 32 and identity in applications:
            raise DiagnosisError("Duplicate original TerminalMode application identity")
        applications[identity] = name
    name = applications.get(32)
    if name is not None and name not in ("AppTerminalMode", "AppTerminalMode [MAIN]"):
        raise DiagnosisError("Unexpected original TerminalMode application identity")
    return name is not None


def request_start(diagnosis, output):
    """The original method catches its own errors; its reply is not activation proof."""
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    output.mkdir(parents=True)
    before = snapshot(diagnosis)
    (output / "before.json").write_text(json.dumps(before, indent=2) + "\n")
    commands = before["startup_commands"]
    prefix = "response_getcommands"
    if (not commands.startswith(prefix) or
            "cmdStartApp(java.lang.String)" not in commands[len(prefix):].split(",")):
        raise DiagnosisError("Original startup diagnostic does not advertise cmdStartApp(String)")
    response = diagnosis(START_COMMAND, "response_processcommand")
    (output / "request-response.json").write_text(json.dumps(response) + "\n")
    if response != SUCCESS:
        raise DiagnosisError(f"Original TerminalMode startup request failed: {response!r}")
    report = {
        "command": START_COMMAND,
        "original_diagnostic_method_returned": True,
        "startup_work_enqueued_verified": False,
        "original_component_dependencies_preserved": True,
        "bundle_relocated_or_started_directly": False,
        "component_activation_verified": False,
        "target_classes_loaded_verified": False,
        "live_hmi_dsi_delivery_verified": False,
        "iphone_session_verified": False,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report
