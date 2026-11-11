#!/usr/bin/env python3
"""Check preview opt-out and exact readback on the real host renderer."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("renderer", type=Path)
args = parser.parse_args()


def record(op, payload=b""):
    return struct.pack("<II", op, len(payload)) + payload


commands = (
    record(1, struct.pack("<2I", 3, 2)) +
    record(3, struct.pack("<4f", 1, 0, 0, 1)) +
    record(4, struct.pack("<I", 0x4000)) +
    record(5) +
    record(120, struct.pack("<6I", 0, 0, 3, 2, 0x1908, 0x1401))
)
with tempfile.TemporaryDirectory(prefix="glhost-preview-") as directory:
    for disabled in (False, True):
        frame = Path(directory) / f"frame-{disabled}.ppm"
        env = dict(os.environ, EGL_PLATFORM="surfaceless",
                   MHI2_GL_FRAME=str(frame),
                   MHI2_GL_NO_FRAME_DUMP="1" if disabled else "0")
        for key in ("MHI2_GL_CAPTURE", "MHI2_GL_TRACE", "MHI2_GL_DUMP_CLIENTS"):
            env.pop(key, None)
        result = subprocess.run(
            [str(args.renderer.resolve())], input=commands, capture_output=True,
            env=env, timeout=15, check=True)
        if result.stdout != bytes((255, 0, 0, 255)) * 6:
            raise AssertionError(f"Native readback changed, preview disabled={disabled}")
        if disabled:
            if frame.exists() or frame.with_suffix(".ppm.tmp").exists():
                raise AssertionError("Disabled preview was still published")
        elif frame.read_bytes() != b"P6\n3 2\n255\n" + bytes((255, 0, 0)) * 6:
            raise AssertionError("Default preview pixels differ")
        print(f"Preview disabled={disabled}: exact readback and publication verified")
