#!/usr/bin/env python3
"""Run a bounded MH2P boot probe; always retain serial output and CPU registers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def probe(qemu, media, output, seconds, trace=False, nor=None, mnand=None,
          trace_sd=False, icount_shift=None, read32=()):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    manifest = json.loads((media / "manifest.json").read_text())
    startup = media / "startup.bin"
    if hashlib.sha256(startup.read_bytes()).hexdigest() != manifest["startup_sha256"]:
        raise ValueError("Startup media differs from its manifest")
    dtb = media / "board.dtb"
    if hashlib.sha256(dtb.read_bytes()).hexdigest() != manifest["dtb_sha256"]:
        raise ValueError("Device tree differs from its manifest")
    output.mkdir(parents=True)
    command = [
        str(qemu.resolve()), "-M", "mh2p", "-kernel", str(startup.resolve()),
        "-dtb", str(dtb.resolve()),
        "-display", "none", "-monitor", "stdio", "-nic", "none", "-no-reboot",
    ]
    if icount_shift is not None:
        command += ["-icount", f"shift={icount_shift},align=off,sleep=off"]
    if nor:
        command += ["-bios", str(nor.resolve())]
    if mnand:
        filename = str(mnand.resolve()).replace(",", ",,")
        command += ["-drive", f"if=sd,index=0,format=raw,snapshot=on,file={filename}"]
    for port in "abcd":
        command += ["-serial", f"file:{(output / ('uart-' + port + '.log')).resolve()}"]
    flags = "in_asm,unimp,guest_errors" if trace else "unimp,guest_errors"
    log = output / ("instructions.log" if trace else "unimplemented.log")
    command += ["-d", flags, "-D", str(log.resolve())]
    if trace_sd:
        command += ["-trace", "enable=sdcard_normal_command",
                    "-trace", "enable=sdcard_response"]
    with (output / "host.log").open("wb") as errors:
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors
        )
        try:
            time.sleep(seconds)
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited early ({process.returncode}); see {output / 'host.log'}")
            monitor = ["stop", "info registers"]
            monitor.extend(f"xp /1wx 0x{address:08x}" for address in read32)
            monitor.append("quit")
            registers, _ = process.communicate(
                ("\n".join(monitor) + "\n").encode(), timeout=5
            )
            if process.returncode:
                raise RuntimeError(f"QEMU exited with status {process.returncode}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdin.close()
            process.stdout.close()
    (output / "registers.log").write_bytes(registers)
    serial = {
        port: (output / f"uart-{port}.log").read_text(errors="replace")
        for port in "abcd"
    }
    result = {
        "command": command,
        "seconds": seconds,
        "startup_sha256": manifest["startup_sha256"],
        "serial_bytes": {port: len(text.encode()) for port, text in serial.items()},
        "userspace_marker": any("Starting /usr/sbin/main_stage1.sh" in text
                                for text in serial.values()),
        "read32_addresses": [f"0x{address:08x}" for address in read32],
        "note": "A completed probe is not a successful OS boot or shell.",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def read32_address(text):
    try:
        address = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Expected an integer physical address") from exc
    if not 0 <= address <= 0xfffffffc or address % 4:
        raise argparse.ArgumentTypeError("Expected an aligned 32-bit physical address")
    return address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path,
                        default=Path(__file__).resolve().parents[2] / "build/qemu-system-arm")
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=2)
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--nor", type=Path, help="optional 64 MiB experimental NOR image")
    parser.add_argument("--mnand", type=Path, help="optional primary mNAND disk (snapshot)")
    parser.add_argument("--trace-sd", action="store_true", help="include card commands in debug log")
    parser.add_argument("--icount-shift", type=int, choices=range(11),
                        help="instruction-counted virtual time (nanoseconds per instruction = 2^shift)")
    parser.add_argument("--read32", action="append", type=read32_address, default=[],
                        help="read a physical word after stopping the guest; repeatable, "
                             "may have MMIO side effects; results are in registers.log")
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be greater than zero and at most 60")
    try:
        result = probe(args.qemu, args.media, args.output, args.seconds,
                       args.trace, args.nor, args.mnand, args.trace_sd,
                       args.icount_shift, args.read32)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f"MH2P probe failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
