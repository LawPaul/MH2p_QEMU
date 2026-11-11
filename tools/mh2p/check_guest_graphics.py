#!/usr/bin/env python3
"""Run the private EGL/GLES smoke client in QNX and verify pixels across reset."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time
import uuid

from check_graphics import ROOT, verify_frame
from qemu.machine import QEMUMachine


def wait_console(console, captured, log, pattern, start, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        match = re.search(pattern, captured[start:], re.MULTILINE)
        if match:
            return match
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Missing guest response: {pattern!r}")
        if select.select([console], [], [], 0.1)[0]:
            chunk = console.recv(65536)
            if not chunk:
                raise RuntimeError("Guest console closed")
            captured.extend(chunk)
            log.write(chunk)
            log.flush()


def check(qemu, media, renderer, output):
    manifest = json.loads((media / "manifest.json").read_text())
    if not manifest.get("graphics_smoke") or not manifest["diagnostic_shell"]:
        raise ValueError("Expected separately prepared graphics-smoke media")
    for name, field in (("startup.bin", "startup_sha256"), ("board.dtb", "dtb_sha256")):
        if hashlib.sha256((media / name).read_bytes()).hexdigest() != manifest[field]:
            raise ValueError(f"Media hash mismatch: {name}")
    output.mkdir(parents=True)
    hashes = []
    with tempfile.TemporaryDirectory(prefix="mh2p-guest-gl-") as temporary:
        scratch = Path(temporary)
        pipe, serial = scratch / "bridge", scratch / "uart"
        os.mkfifo(str(pipe) + ".in")
        os.mkfifo(str(pipe) + ".out")
        args = [
            "-M", "mh2p", "-S", "-kernel", str(media / "startup.bin"),
            "-dtb", str(media / "board.dtb"), "-display", "none", "-nic", "none",
            "-icount", "shift=0,align=off,sleep=off", "-serial", "null",
            "-serial", f"unix:{serial},server=on,wait=off",
            "-chardev", f"pipe,id=glbridge,path={pipe}",
            "-run-with", "exit-with-parent=on",
        ]
        vm = QEMUMachine(str(qemu), args=args, base_temp_dir=temporary, qmp_timer=10)
        with open(str(pipe) + ".out", "r+b", buffering=0) as tx, \
                open(str(pipe) + ".in", "r+b", buffering=0) as rx, \
                (output / "uart.log").open("wb") as log:
            try:
                with vm:
                    vm.launch()
                    with socket.socket(socket.AF_UNIX) as console:
                        console.settimeout(5)
                        console.connect(str(serial))
                        captured = bytearray()
                        for boot in range(2):
                            frame = output / f"frame-{boot}.ppm"
                            env = dict(os.environ, MHI2_GL_FRAME=str(frame),
                                       EGL_PLATFORM="surfaceless", LIBGL_ALWAYS_SOFTWARE="1")
                            for key in ("MHI2_GL_DUMP_CLIENTS", "MHI2_GL_CAPTURE", "MHI2_GL_TRACE"):
                                env.pop(key, None)
                            with (output / f"renderer-{boot}.log").open("wb") as errors:
                                host = subprocess.Popen([str(renderer)], stdin=tx,
                                                        stdout=rx, stderr=errors, env=env)
                                try:
                                    start = len(captured)
                                    vm.cmd("cont")
                                    wait_console(console, captured, log,
                                                 rb"MH2P diagnostic shell: normal services are NOT started.",
                                                 start)
                                    wait_console(console, captured, log, rb"mh2p# ", start)
                                    start = len(captured)
                                    token = "MH2P_GUEST_GL_" + uuid.uuid4().hex
                                    console.sendall((
                                        "GLBRIDGE_W=64 GLBRIDGE_H=32 /sbin/mh2p-egl-smoke; "
                                        f"echo {token}:$?\n"
                                    ).encode())
                                    status = wait_console(
                                        console, captured, log,
                                        rb"^" + token.encode() + rb":(\d+)\r?$",
                                        start, timeout=45,
                                    )
                                    if status[1] != b"0":
                                        raise RuntimeError(f"Guest smoke client exited {status[1]!r}")
                                    if len(re.findall(
                                        rb"^MH2P_EGL_SMOKE_PASS 64x32 pixels=2048\r?$",
                                        captured[start:], re.MULTILINE,
                                    )) != 1:
                                        raise RuntimeError("Missing exact guest pixel-verification marker")
                                    deadline = time.monotonic() + 10
                                    while not frame.exists():
                                        if host.poll() is not None:
                                            raise RuntimeError("Renderer exited before frame publication")
                                        if time.monotonic() >= deadline:
                                            raise TimeoutError("No host frame from guest EGL client")
                                        time.sleep(0.01)
                                    hashes.append(verify_frame(frame.read_bytes()))
                                    vm.cmd("stop")
                                finally:
                                    if host.poll() is None:
                                        host.terminate()
                                        try:
                                            host.wait(timeout=5)
                                        except subprocess.TimeoutExpired:
                                            host.kill()
                                            host.wait()
                            if boot == 0:
                                vm.cmd("system_reset")
            finally:
                (output / "qemu.log").write_text(vm.get_log() or "")
    result = {
        "guest_egl_smoke_verified": True, "boot_count": 2,
        "guest_readback_pixels_per_boot": 2048, "frame_sha256": hashes,
        "startup_sha256": manifest["startup_sha256"],
        "graphics_libraries_replaced": False,
        "note": "Private QNX pbuffer client with MHI2 shim; no native window or HMI verified",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.qemu.resolve(), args.media.resolve(),
                           args.renderer.resolve(), args.output.resolve()), indent=2))


if __name__ == "__main__":
    main()
