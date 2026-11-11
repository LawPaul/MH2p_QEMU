#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe unchanged SubCPU reset execution, without claiming a board boot."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess

from prepare_subcpu_elf import prepare


DIAGNOSTICS = {
    "erased_data_flash": "MH2P_DIAGNOSTIC_ERASED_DATA_FLASH",
    "pfdl_api": "MH2P_DIAGNOSTIC_PFDL_API",
    "f13_pior": "MH2P_DIAGNOSTIC_F13_PIOR",
    "f13_clock": "MH2P_DIAGNOSTIC_F13_CLOCK",
    "f13_tau": "MH2P_DIAGNOSTIC_F13_TAU",
    "f13_trd": "MH2P_DIAGNOSTIC_F13_TRD",
    "f13_irq": "MH2P_DIAGNOSTIC_F13_IRQ",
    "f13_ports": "MH2P_DIAGNOSTIC_F13_PORTS",
}
SIGNATURE = "MH2P strict RL78 diagnostic v2;"
UNSUPPORTED_ACCESS = re.compile(
    rb"Unmodeled MH2P SubCPU peripheral (read|write) at "
    rb"(0x[0-9a-f]+), PC (0x[0-9a-f]+)(?=[^0-9a-f])")
UNINITIALIZED_RAM = re.compile(
    rb"Uninitialized MH2P SubCPU RAM read at "
    rb"(0x[0-9a-f]+), PC (0x[0-9a-f]+)(?=[^0-9a-f])")


def inspect_trace(path):
    signature_present = False
    unsupported_access = None
    uninitialized_ram_read = None
    tail = b""
    with path.open("rb") as stream:
        while chunk := stream.read(4096):
            data = tail + chunk
            signature_present |= SIGNATURE.encode() in data
            match = UNSUPPORTED_ACCESS.search(data)
            if match and unsupported_access is None:
                unsupported_access = {
                    "operation": match[1].decode("ascii"),
                    "address": match[2].decode("ascii"),
                    "pc_after_instruction": match[3].decode("ascii"),
                }
            match = UNINITIALIZED_RAM.search(data)
            if match and uninitialized_ram_read is None:
                uninitialized_ram_read = {
                    "address": match[1].decode("ascii"),
                    "pc_after_instruction": match[2].decode("ascii"),
                }
            tail = data[-256:]
    return signature_present, unsupported_access, uninitialized_ram_read


def observe(source, simulator, output, *, erased_data_flash=False,
            pfdl_api=False, f13_pior=False, f13_clock=False, f13_tau=False,
            f13_irq=False, f13_ports=False, f13_trd=False, timeout=10):
    if pfdl_api and not erased_data_flash:
        raise ValueError("PFDL API substitution requires explicit erased flash")
    if f13_tau and not f13_clock:
        raise ValueError("TAU0 diagnostic requires explicit clock retention")
    if f13_trd and not f13_clock:
        raise ValueError("Timer RD diagnostic requires explicit clock retention")
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Timeout must be positive")
    simulator = simulator.resolve(strict=True)
    simulator_hash = hashlib.sha256(simulator.read_bytes()).hexdigest()
    metadata = prepare(source, output)
    enabled = {
        "erased_data_flash": erased_data_flash,
        "pfdl_api": pfdl_api,
        "f13_pior": f13_pior,
        "f13_clock": f13_clock,
        "f13_tau": f13_tau,
        "f13_trd": f13_trd,
        "f13_irq": f13_irq,
        "f13_ports": f13_ports,
    }
    environment = dict(os.environ)
    for name, variable in DIAGNOSTICS.items():
        environment.pop(variable, None)
        if enabled[name]:
            environment[variable] = "1"
    timed_out = False
    log = output / "reset-trace.log"
    with log.open("xb") as stream:
        try:
            result = subprocess.run(
                [str(simulator), "-v", "-d", str(output / "subcpu.elf")],
                stdout=stream, stderr=subprocess.STDOUT, env=environment,
                timeout=timeout, check=False)
            exit_code = result.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            exit_code = None
    # Keep the trace on disk; even a failed diagnostic may produce a large log.
    signature_present, unsupported_access, uninitialized_ram_read = (
        inspect_trace(log))
    observation = {
        "source_sha256": metadata["source_sha256"],
        "source_bytes": metadata["source_bytes"],
        "elf_sha256": metadata["elf_sha256"],
        "all_source_bytes_preserved": True,
        "entry_from_original_reset_vector":
            metadata["entry_from_original_reset_vector"],
        "simulator": str(simulator),
        "simulator_sha256": simulator_hash,
        "strict_diagnostic_signature_present": signature_present,
        "diagnostics_requested": enabled,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "first_unsupported_access": unsupported_access,
        "first_uninitialized_ram_read": uninitialized_ram_read,
        "trace": str(log.resolve()),
        "exact_mcu_variant_verified": False,
        "original_internal_mcu_rom_execution_verified": False,
        "subcpu_board_boot_verified": False,
        "rstp_verified": False,
        "hmi_ready": False,
        "note": (
            "CPU observation only. An API substitution is not original MCU "
            "ROM execution. Erased flash is not recovered factory data. "
            "PIOR retention does not verify package or external wiring. "
            "Clock retention supplies no PLL lock or oscillator readiness. "
            "TAU0 configuration supplies no counter progress or interrupt. "
            "Timer RD start/PWM-delay configuration supplies no counter "
            "progress, waveform output or interrupt. "
            "IRQ configuration supplies no request generation or dispatch. "
            "Port configuration supplies no external pin input levels. "
            "No UART acknowledgment, power token or readiness is supplied."),
    }
    (output / "observation.json").write_text(
        json.dumps(observation, indent=2) + "\n")
    if not signature_present:
        raise ValueError(
            "Runner lacks the strict MH2P diagnostic signature; "
            "see retained observation and trace")
    if timed_out:
        raise TimeoutError(f"SubCPU observation timed out; see {log}")
    return observation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=10)
    for name in DIAGNOSTICS:
        parser.add_argument(
            "--diagnostic-" + name.replace("_", "-"), action="store_true",
            dest=name)
    args = parser.parse_args()
    try:
        observation = observe(
            args.image, args.simulator, args.output.resolve(),
            erased_data_flash=args.erased_data_flash,
            pfdl_api=args.pfdl_api, f13_pior=args.f13_pior,
            f13_clock=args.f13_clock,
            f13_tau=args.f13_tau,
            f13_trd=args.f13_trd,
            f13_irq=args.f13_irq,
            f13_ports=args.f13_ports,
            timeout=args.timeout)
    except (OSError, ValueError, TimeoutError) as exc:
        parser.exit(1, f"MH2P SubCPU observation failed: {exc}\n")
    print(json.dumps(observation, indent=2))
    # A captured hardware boundary is not a successful boot.
    parser.exit(1, "SubCPU board boot and UART exchange remain unverified.\n")


if __name__ == "__main__":
    main()
