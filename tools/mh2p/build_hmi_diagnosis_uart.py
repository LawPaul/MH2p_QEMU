#!/usr/bin/env python3
"""Build the opt-in ARM QNX UARTD diagnosis tunnel without a QNX SDK."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def build(compiler, output):
    output.mkdir(parents=True, exist_ok=False)
    flags = [
        compiler, "-mfloat-abi=soft", "-mcpu=cortex-a9", "-marm",
        "-mno-unaligned-access", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-O2",
    ]
    with tempfile.TemporaryDirectory(prefix="mh2p-diagnosis-link-") as temporary:
        work = Path(temporary)
        source = work / "libc.c"
        # Link-only names; the guest loads its unchanged original libc.so.3.
        source.write_text(
            "void write(void) {} void exit(void) {} "
            "void *dlopen(void) { return 0; } "
            "void *dlsym(void) { return 0; }\n")
        libc = work / "libc.so"
        subprocess.run(flags + [
            "-fPIC", "-shared", "-Wl,--hash-style=sysv,-soname,libc.so.3",
            str(source), "-o", str(libc),
        ], check=True)
        subprocess.run(flags + [
            "-Wall", "-Wextra", "-Werror",
            "-Wl,--hash-style=sysv,--dynamic-linker=/usr/lib/ldqnx.so.2,-e,_start",
            str(Path(__file__).with_name("hmi_diagnosis_uart.c")), str(libc),
            "-o", str(output / "hmi-diagnosis-uart"),
        ], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build(args.compiler, args.output.resolve())


if __name__ == "__main__":
    main()
