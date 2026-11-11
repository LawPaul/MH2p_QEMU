#!/usr/bin/env python3
"""Bounded execution of the original P2865 RCC startup, not a boot-success test."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from qemu.machine import QEMUMachine


RCC_SHA256 = "776813bf4a6aff13b528b1d07645add7c9af3afb6167dd1b138ee2386e510bb9"
RCC_IMAGE_SIZE = 9998164


def verify_image(image):
    if image.stat().st_size != RCC_IMAGE_SIZE:
        raise ValueError("Expected original P2865 RCC IFS size")
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if digest != RCC_SHA256:
        raise ValueError("Expected original P2865 RCC IFS SHA-256")
    return digest


def probe(qemu, image, output, seconds=2, trace=False):
    if not 0 < seconds <= 30:
        raise ValueError("Observation must be between 0 and 30 seconds")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    digest = verify_image(image)
    output = output.resolve()
    output.mkdir(parents=True)
    args = [
        "-M", "mh2p-rcc", "-kernel", str(image.resolve()),
        "-display", "none", "-nic", "none", "-S",
        "-icount", "shift=0,align=off,sleep=on",
        "-d", "in_asm,unimp,guest_errors" if trace else "unimp,guest_errors",
        "-D", str(output / "unimplemented.log"),
    ]
    for port in range(3):
        args += ["-serial", f"file:{output / f'uart-{port}.log'}"]
    with QEMUMachine(str(qemu.resolve()), args=args) as vm:
        vm.launch()
        vm._qmp.settimeout(5)
        try:
            vm.cmd("cont")
            time.sleep(seconds)
            vm.cmd("stop")
            registers = vm.cmd("human-monitor-command",
                               command_line="info registers")
        except TimeoutError:
            print("RCC probe QMP timed out; terminating the probe VM",
                  file=sys.stderr)
            vm.kill()
            raise
        (output / "registers.log").write_text(registers)
    report = {
        "command": [str(qemu.resolve())] + args,
        "source_sha256": digest,
        "original_executable_unmodified": True,
        "observation_wall_seconds": seconds,
        "serial_bytes": {
            str(port): (output / f"uart-{port}.log").stat().st_size
            for port in range(3)
        },
        "note": "Execution evidence only; completion does not prove a QNX boot, RCC services, or an MMX link.",
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=2)
    parser.add_argument("--trace", action="store_true")
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.image, args.output,
                           args.seconds, args.trace), indent=2))


if __name__ == "__main__":
    main()
