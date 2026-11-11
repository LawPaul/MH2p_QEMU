#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded direct original-backend observation; no readiness or phone fixtures."""
import hashlib
import json
import re
import shlex

from prepare_hmi_development import original_json


BACKEND_SHA256 = "429ef95595bd67d13acf51df63cb6fb3f86e7ed8effa2eeec25e085c6cdadb77"
LAUNCHER_SHA256 = "32342a7ab6dcf90bf91ae403b14cda35ee89c1daae4a4294cd81c134bde7eb35"
MAX_SNAPSHOTS = 128
BACKEND_STARTUP_CHANNELS = (
    "DIO_Main", "DIO_DIOManager", "DIO_SIService", "DIO_DSICarPlay",
    "DIO_DIOModules", "DIO_DIOServices", "DIOCodingProvider",
)


def backend_debug_tracing(source):
    config = original_json(source)
    try:
        client = config["client"]["dio_manager"]["levels"]["channel"]
        console = config["server"]["default"]["backends"]["ConsoleBackend"]["levels"]["channel"]
    except (KeyError, TypeError) as exc:
        raise ValueError("Expected original CarPlay tracing configuration") from exc
    for levels in (client, console):
        if (not isinstance(levels, dict) or not levels or
                not all(isinstance(key, str) and isinstance(value, str)
                        for key, value in levels.items())):
            raise ValueError("Expected original CarPlay trace channel levels")
        levels.update({channel: "debug" for channel in BACKEND_STARTUP_CHANNELS})
    return config


def observation_script(launcher):
    child = launcher["children"]["carplay"]
    if child["exec"] != "dio_manager" or child["params"] != "":
        raise ValueError("Expected the original CarPlay child launch entry")
    envs = child["envs"]
    if (len(envs) != 3 or
            not all(isinstance(value, str) and "\n" not in value for value in envs) or
            "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production" not in envs):
        raise ValueError("Expected original CarPlay child environment")
    environment = " ".join(shlex.quote(value) for value in envs)
    return (
        "set -u\n"
        "pidin ar >/tmp/carplay-direct-before.txt || exit 1\n"
        "if grep -E '[ /]dio_manager( |$)' /tmp/carplay-direct-before.txt; then\n"
        " echo CARPLAY_DIRECT_REFUSED_EXISTING\n exit 1\nfi\n"
        "echo CARPLAY_DIRECT_ORIGINAL_CONFIG\n"
        "cat /etc/eso/production/dio_manager.json || exit 1\n"
        f"env LD_PRELOAD= {environment} /eso/bin/apps/dio_manager "
        ">/tmp/carplay-direct.log 2>&1 &\n"
        "DIO_DIRECT_PID=$!\n"
        'echo CARPLAY_DIRECT_PID:$DIO_DIRECT_PID\n'
        "THREADS_OBSERVED=0\n"
        "SAMPLE=1\n"
        f"while test \"$SAMPLE\" -le {MAX_SNAPSHOTS}; do\n"
        ' echo CARPLAY_DIRECT_SAMPLE:$SAMPLE\n'
        ' if kill -0 "$DIO_DIRECT_PID" 2>/dev/null; then\n'
        '  echo CARPLAY_DIRECT_ALIVE:$DIO_DIRECT_PID\n'
        '  pidin -p "$DIO_DIRECT_PID" threads >/tmp/carplay-direct-threads.txt\n'
        '  if test "$THREADS_OBSERVED" -eq 0 &&\n'
        "     grep ' DIOController ' /tmp/carplay-direct-threads.txt >/dev/null &&\n"
        "     grep ' HIDService ' /tmp/carplay-direct-threads.txt >/dev/null &&\n"
        "     grep ' IAP2Service ' /tmp/carplay-direct-threads.txt >/dev/null; then\n"
        "   echo CARPLAY_DIRECT_NATIVE_THREADS_OBSERVED\n"
        "   THREADS_OBSERVED=1\n"
        "  fi\n"
        " else\n"
        '  wait "$DIO_DIRECT_PID"; DIO_DIRECT_STATUS=$?\n'
        '  echo CARPLAY_DIRECT_EXIT:$DIO_DIRECT_STATUS\n'
        "  break\n fi\n"
        " SAMPLE=$((SAMPLE + 1))\n"
        "done\n"
        'pidin -p "$DIO_DIRECT_PID" ar\n'
        "cat /tmp/carplay-direct-threads.txt\n"
        "echo CARPLAY_DIRECT_STDOUT_BEGIN\n"
        "cat /tmp/carplay-direct.log\n"
        "echo CARPLAY_DIRECT_STDOUT_END\n"
        'pidin -p "$DIO_DIRECT_PID" mem\n'
        "echo CARPLAY_DIRECT_TRACE_BEGIN\n"
        "grep -E 'dio_manager|DIOManager|AirPlay|CarPlay|carplay' "
        "/tmp/native-traceserver.log | tail -400\n"
        "echo CARPLAY_DIRECT_TRACE_END\n"
        'if kill -0 "$DIO_DIRECT_PID" 2>/dev/null; then\n'
        ' pidin -p "$DIO_DIRECT_PID" ar >/tmp/carplay-direct-owned.txt\n'
        " if grep -E '[ /]dio_manager( |$)' /tmp/carplay-direct-owned.txt; then\n"
        '  kill "$DIO_DIRECT_PID"\n'
        '  if kill -0 "$DIO_DIRECT_PID" 2>/dev/null; then\n'
        '   kill -9 "$DIO_DIRECT_PID"\n'
        "  fi\n"
        '  wait "$DIO_DIRECT_PID"; echo CARPLAY_DIRECT_STOP_STATUS:$?\n'
        " else\n echo CARPLAY_DIRECT_CLEANUP_IDENTITY_ERROR\n exit 1\n fi\n"
        "fi\n"
        'if kill -0 "$DIO_DIRECT_PID" 2>/dev/null; then\n'
        " echo CARPLAY_DIRECT_CLEANUP_FAILED\n exit 1\nfi\n"
        "echo CARPLAY_DIRECT_STOPPED\n"
    ).encode()


def parse_observation(reply):
    text = reply.decode("utf-8", errors="backslashreplace").replace("\r", "")
    pids = re.findall(r"^CARPLAY_DIRECT_PID:(\d+)$", text, re.M)
    stopped = re.findall(r"^CARPLAY_DIRECT_STOPPED$", text, re.M)
    if len(pids) != 1 or len(stopped) != 1:
        raise RuntimeError("Missing or ambiguous direct-backend PID/cleanup evidence")
    pid = int(pids[0])
    alive = [int(value) for value in re.findall(
        r"^CARPLAY_DIRECT_ALIVE:(\d+)$", text, re.M)]
    if any(value != pid for value in alive):
        raise RuntimeError("Direct-backend sample PID changed")
    exits = re.findall(r"^CARPLAY_DIRECT_EXIT:(\d+)$", text, re.M)
    samples = [int(value) for value in re.findall(
        r"^CARPLAY_DIRECT_SAMPLE:(\d+)$", text, re.M)]
    if samples != list(range(1, len(samples) + 1)):
        raise RuntimeError("Invalid direct-backend sample ordering")
    threads_observed = bool(re.search(
        r"^CARPLAY_DIRECT_NATIVE_THREADS_OBSERVED$", text, re.M))
    if (len(exits) > 1 or
            not 1 <= len(alive) + len(exits) == len(samples) <= MAX_SNAPSHOTS):
        raise RuntimeError("Invalid direct-backend observation sequence")
    if not exits and len(alive) != MAX_SNAPSHOTS and not threads_observed:
        raise RuntimeError("Incomplete direct-backend observation")
    if threads_observed and (exits or not all(
            re.search(rf"(?m)^\s*{pid}\s+\S+\s+{name}\s+", text)
            for name in ("DIOController", "HIDService", "IAP2Service"))):
        raise RuntimeError("Native service thread marker lacks matching PID evidence")
    backend_lines = [
        line for line in text.splitlines()
        if re.match(r"^\d+:\d+:\d+\.\d+\s+dio_manager\s+", line)]
    partition_states = {}
    for line in backend_lines:
        match = re.search(
            r"Opened partition=(\d+) \(persistencyReadable=([01]),", line)
        if match:
            partition_states[match[1]] = bool(int(match[2]))
    return {
        "guest_pid": pid, "alive_samples": len(alive),
        "early_exit_status": int(exits[0]) if exits else None,
        "survived_all_samples": not exits,
        "native_service_threads_observed": threads_observed,
        "persistence_partition_readability": partition_states,
        "configuration_read_results": [
            line for line in backend_lines
            if re.search(r"\[PersistencyProvider\].*(?:No value available|Value ')", line)],
        "integrator_callbacks": [
            line for line in backend_lines if "[CSIService]" in line],
        "sampling": f"at most {MAX_SNAPSHOTS} process snapshots, no guest-clock delay",
        "owned_process_stopped": True,
        "scope": "direct manual launch, not original integrator-controlled activation",
        "usb_record_injected": False, "readiness_memory_written": False,
        "original_production_configuration": True,
        "airplay_server_ready_verified": False, "iphone_session_verified": False,
        "live_hmi_dsi_delivery_verified": False,
    }


def observe(shell, backend, launcher, output, *, usb_record_source="none"):
    if usb_record_source not in ("none", "supplied-capture", "synthetic-empty"):
        raise ValueError("Unknown USB record source")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    binary = backend.read_bytes()
    raw = launcher.read_bytes()
    if hashlib.sha256(binary).hexdigest() != BACKEND_SHA256:
        raise ValueError("Unrecognized original CarPlay backend")
    if hashlib.sha256(raw).hexdigest() != LAUNCHER_SHA256:
        raise ValueError("Unrecognized original smartphone launcher")
    script = observation_script(original_json(raw.decode()))
    output.mkdir()
    (output / "script.sh").write_bytes(script)
    target = "/tmp/carplay-direct-observe.sh"
    shell(f": >{target}")
    for at in range(0, len(script), 128):
        escaped = "".join(f"\\{value:03o}" for value in script[at:at + 128])
        shell(f"printf '{escaped}' >>{target}")
    readback = shell(f"echo CARPLAY_SCRIPT_BEGIN; cat {target}; "
                     "echo CARPLAY_SCRIPT_END")
    (output / "script-readback.log").write_bytes(readback)
    normalized = readback.replace(b"\r", b"")
    matches = re.findall(rb"(?m)^CARPLAY_SCRIPT_BEGIN\n(.*?)^CARPLAY_SCRIPT_END$",
                         normalized, re.S)
    if matches != [script]:
        raise RuntimeError("Direct-backend observation script readback differs")
    reply = shell(f"ksh {target}")
    (output / "observation.log").write_bytes(reply)
    result = parse_observation(reply)
    result["usb_record_injected"] = usb_record_source != "none"
    result["usb_record_source"] = usb_record_source
    result["backend_sha256"] = BACKEND_SHA256
    result["launcher_sha256"] = LAUNCHER_SHA256
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result
