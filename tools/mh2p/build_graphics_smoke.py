#!/usr/bin/env python3
"""Build an isolated QNX EGL smoke client or original-library display probe."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def build(compiler, output, native_display_probe=False):
    output.mkdir(parents=True, exist_ok=False)
    flags = [compiler, "-mfloat-abi=soft", "-mcpu=cortex-a9", "-marm",
             "-mno-unaligned-access", "-ffreestanding", "-fno-builtin",
             "-nostdlib", "-O2"]
    shim = output / "libmh2p-egl-smoke.so"
    if not native_display_probe:
        subprocess.run(flags + ["-fPIC", "-shared", "-Wl,--hash-style=sysv",
                       "-Wl,-soname,libmh2p-egl-smoke.so", "-o", str(shim),
                       str(ROOT / "tools/mhi2/glforward/guest/gl_shim.c")], check=True)
    with tempfile.TemporaryDirectory(prefix="mh2p-smoke-link-") as temporary:
        work = Path(temporary)
        # Link-only symbols; the guest resolves its original libc.so.3.
        source = work / "libc.c"
        source.write_text("int write(void){return 0;} void exit(void){} "
                          "char **environ;\n" +
                          ("void *dlopen(void){return 0;} "
                           "void *dlsym(void){return 0;} "
                           "char *dlerror(void){return 0;} "
                           "void *memset(void){return 0;}\n"
                           if native_display_probe else ""))
        libc = work / "libc.so"
        subprocess.run(flags + ["-fPIC", "-shared", "-Wl,-soname,libc.so.3",
                       "-o", str(libc), str(source)], check=True)
        if native_display_probe:
            target = "mh2p-native-display-probe"
            client = "native_display_probe.c"
            libraries = [str(libc)]
            flags += ["-Wall", "-Wextra", "-Werror"]
        else:
            target = "mh2p-egl-smoke"
            client = "graphics_smoke.c"
            libraries = [str(shim), str(libc)]
        subprocess.run(flags + [
            "-Wl,--hash-style=sysv,--dynamic-linker=/usr/lib/ldqnx.so.2,-e,_start",
            "-Wl,--allow-shlib-undefined", "-o", str(output / target),
            str(ROOT / "tools/mh2p" / client),
        ] + libraries, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--native-display-probe", action="store_true")
    args = parser.parse_args()
    build(args.compiler, args.output.resolve(), args.native_display_probe)


if __name__ == "__main__":
    main()
