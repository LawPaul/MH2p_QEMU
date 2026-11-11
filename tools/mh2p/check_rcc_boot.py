#!/usr/bin/env python3
"""Verify original RCC console/process startup across cold hosts and warm resets."""
import argparse
import json
from pathlib import Path
import re
import select
import sys
import time
import uuid

from probe_rcc import QEMUMachine, ROOT, verify_image


FLASH_HANDOFF = "loader,addr=0x8cbff008,data=0x5a,data-len=1"
BANNER = b"Welcome to QNX Neutrino 6.6.0"


def process_inventory(log, marker, diagnostic_flash_handoff=False):
    normalized = log.replace(b"\r", b"")
    begin = b"\n" + marker + b":BEGIN\n"
    end = b"\n" + marker + b":END:42\n"
    if normalized.count(begin) != 1 or normalized.count(end) != 1:
        raise RuntimeError("Missing unique executed RCC shell markers")
    if normalized.index(begin) >= normalized.index(end):
        raise RuntimeError("RCC shell markers are out of order")
    table = normalized.split(begin, 1)[1].split(end, 1)[0]
    rows = re.findall(rb"(?m)^[ \t]*(\d+)[ \t]+(\S[^\n]*)$", table)
    processes = [
        {"pid": int(pid), "arguments": arguments.decode(errors="replace")}
        for pid, arguments in rows
    ]
    required = {
        "procnto-instr", "devc-seromap", "slogger", "ProcessManager", "sh",
    }
    if diagnostic_flash_handoff:
        required.update(("devf-generic", "inflator", "io-pkt-v6-hc"))
    executables = {Path(row["arguments"].split()[0]).name for row in processes}
    missing = required - executables
    if missing:
        raise RuntimeError(f"Missing original RCC processes: {sorted(missing)}")
    if diagnostic_flash_handoff and not any(
        Path(row["arguments"].split()[0]).name == "devc-seromap" and
        "-u6" in row["arguments"].split() and
        "0x48068000^2,138" in row["arguments"].split()
        for row in processes
    ):
        raise RuntimeError("Missing original UART6 driver instance")
    return processes


def receive_until(console, log, output, predicate, timeout):
    deadline = time.monotonic() + timeout
    while not predicate(log):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Timed out waiting for original RCC console evidence")
        if not select.select([console], [], [], remaining)[0]:
            raise TimeoutError("Timed out waiting for original RCC console evidence")
        chunk = console.recv(65536)
        if not chunk:
            raise RuntimeError("Original RCC console closed before evidence arrived")
        log.extend(chunk)
        output.write(chunk)
        output.flush()


def check_console(console, output, timeout, diagnostic_flash_handoff,
                  uart6_log):
    log = bytearray()
    receive_until(console, log, output,
                  lambda data: BANNER in data and
                  b"\n# " in data.split(BANNER, 1)[1], timeout)
    marker = ("RCC_" + uuid.uuid4().hex).encode()
    console.sendall(b"echo " + marker + b":BEGIN; pidin ar; echo " +
                    marker + b":END:$((6 * 7))\n")
    receive_until(console, log, output,
                  lambda data: b"\n" + marker + b":END:42\n" in
                  data.replace(b"\r", b""), timeout)
    processes = process_inventory(bytes(log), marker, diagnostic_flash_handoff)
    audit = ("RCC_AUDIT_" + uuid.uuid4().hex).encode()
    uart6 = (b"if test -c /dev/ser6; then echo " + audit +
             b":UART6:$((6 * 7)); echo " + audit +
             b":UART6_TX:$((6 * 7)) > /dev/ser6; fi; "
             ) if diagnostic_flash_handoff else b""
    console.sendall(b"ls -l /dev; ls -l /App; sloginfo; " + uart6 + b"echo " +
                    audit + b":$((6 * 7))\n")
    receive_until(console, log, output,
                  lambda data: b"\n" + audit + b":42\n" in
                  data.replace(b"\r", b""), timeout)
    if diagnostic_flash_handoff and (
        b"\n" + audit + b":UART6:42\n" not in log.replace(b"\r", b"")
    ):
        raise RuntimeError("Original UART6 character device /dev/ser6 is missing")
    if diagnostic_flash_handoff:
        expected = audit + b":UART6_TX:42"
        deadline = time.monotonic() + timeout
        while uart6_log.read_bytes().splitlines().count(expected) != 1:
            if time.monotonic() >= deadline:
                raise TimeoutError("Original UART6 transmit marker did not reach host")
            time.sleep(0.01)
    return {
        "processes": processes,
        "console_roundtrip_verified": True,
        "uart6_character_device_verified": diagnostic_flash_handoff,
        "uart6_transmit_verified": diagnostic_flash_handoff,
    }


def check(qemu, image, output, timeout=15, diagnostic_flash_handoff=False, *,
          cold_boots=1):
    if not 0 < timeout <= 30:
        raise ValueError("Console timeout must be between 0 and 30 seconds")
    if type(cold_boots) is not int or not 1 <= cold_boots <= 10:
        raise ValueError("Cold boot count must be an integer between 1 and 10")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    digest = verify_image(image)
    output = output.resolve()
    output.mkdir(parents=True)
    args = [
        "-M", "mh2p-rcc", "-kernel", str(image.resolve()),
        "-display", "none", "-nic", "none", "-S",
        "-icount", "shift=0,align=off,sleep=on",
    ]
    if diagnostic_flash_handoff:
        args += [
            "-device", FLASH_HANDOFF,
            "-serial", "null", "-serial", "null",
        ]
    boots = []
    hosts = []
    for cold_boot in range(cold_boots):
        suffix = f"-cold-{cold_boot}" if cold_boot else ""
        uart6_log = output / f"uart-5{suffix}.log"
        host_args = args + [
            "-d", "unimp", "-D", str(output / f"unimplemented{suffix}.log"),
        ]
        if diagnostic_flash_handoff:
            host_args += ["-serial", f"file:{uart6_log}"]
        with QEMUMachine(str(qemu.resolve()), args=host_args) as vm:
            vm.set_console(console_index=2)
            vm.launch()
            command = list(vm._qemu_full_args)
            pid = vm.get_pid()
            if type(pid) is not int or pid <= 0 or pid in hosts:
                raise RuntimeError("Missing or reused owned RCC host PID")
            hosts.append(pid)
            (output / f"owned-host-{cold_boot}.json").write_text(
                json.dumps({"qemu_pid": pid, "command": command}, indent=2) + "\n")
            vm._qmp.settimeout(5)
            try:
                for reset in range(2):
                    boot = cold_boot * 2 + reset
                    if reset:
                        vm.cmd("system_reset")
                    vm.cmd("cont")
                    with (output / f"boot-{boot}.log").open("wb") as log:
                        evidence = check_console(vm.console_socket, log, timeout,
                                                 diagnostic_flash_handoff,
                                                 uart6_log)
                    vm.cmd("stop")
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    (output / f"registers-{boot}.log").write_text(registers)
                    boots.append(dict(evidence, cold_boot=cold_boot,
                                      system_reset=bool(reset)))
            except TimeoutError:
                print("RCC console check timed out; terminating the check VM",
                      file=sys.stderr)
                vm.kill()
                raise
        if Path(f"/proc/{pid}").exists():
            raise RuntimeError("Owned RCC host remains after shutdown")
    report = {
        "command": command,
        "console_index": 2,
        "source_sha256": digest,
        "original_executable_unmodified": True,
        "diagnostic_flash_handoff": diagnostic_flash_handoff,
        "handoff_fixture": {
            "address": "0x8cbff008", "value": "0x5a",
            "producer_verified": False,
        } if diagnostic_flash_handoff else None,
        "boots": boots,
        "console_verified_across_reset": True,
        "cold_boot_count": cold_boots,
        "owned_host_pids": hosts,
        "all_owned_hosts_absent": True,
        "full_rcc_services_verified": False,
        "note": (
            "Original console/process inventory, plus diagnostic UART6 transmit "
            "when enabled; not full RCC service operation, persistence, "
            "authentication, an MMX link or HMI frames. "
            "Diagnostic flash handoff, when enabled, supplies a synthetic RAM "
            "wakeup-lock token on every reset; its original producer is unknown."
        ),
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--cold-boots", type=int, default=1,
                        help="Repeat in 1-10 distinct QEMU processes, each with a warm reset")
    parser.add_argument(
        "--diagnostic-flash-handoff", action="store_true",
        help="Supply an explicit synthetic RAM wakeup-lock token; NOT normal boot",
    )
    args = parser.parse_args()
    print(json.dumps(check(args.qemu, args.image, args.output, args.timeout,
                           args.diagnostic_flash_handoff,
                           cold_boots=args.cold_boots), indent=2))


if __name__ == "__main__":
    main()
