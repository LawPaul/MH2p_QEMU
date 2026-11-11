#!/usr/bin/env python3
"""Verify host-rendered pixels through the optional bridge, without firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from qemu.machine.qtest import QEMUQtestMachine

BASE = 0x5f000000
BULK = 0x5e000000
WIDTH, HEIGHT = 64, 32


class BridgeVM(QEMUQtestMachine):
    def _post_launch(self):
        super()._post_launch()
        assert self._qtest is not None
        self._qtest.settimeout(10)

    def request(self, command):
        response = self.qtest(command).split()
        if not response or response[0] != "OK":
            raise RuntimeError(f"qtest failed: {command}: {response}")
        return response[1:]

    def read32(self, address):
        return int(self.request(f"readl {address:#x}")[0], 16)

    def write32(self, address, value):
        self.request(f"writel {address:#x} {value:#x}")

    def send(self, opcode, payload=b""):
        if self.read32(BASE + 16) != 0:
            raise RuntimeError("Bridge record lock unavailable")
        record = struct.pack("<II", opcode, len(payload)) + payload
        self.request(f"write {BULK:#x} {len(record)} 0x{record.hex()}")
        self.write32(BASE + 28, len(record))
        self.write32(BASE + 16, 0)


def expected_pixels(alpha=False):
    red = bytes((255, 0, 0, 255) if alpha else (255, 0, 0))
    green = bytes((0, 255, 0, 255) if alpha else (0, 255, 0))
    return (red * (WIDTH // 2) + green * (WIDTH // 2)) * HEIGHT


def verify_frame(data):
    header = f"P6\n{WIDTH} {HEIGHT}\n255\n".encode()
    if data != header + expected_pixels():
        raise RuntimeError("Rendered PPM dimensions or red/green pixels differ")
    return hashlib.sha256(data).hexdigest()


def block_linear_address(pitch, shift, x, y):
    return ((y >> (3 + shift)) * pitch * (8 << shift) +
            (x >> 6) * (512 << shift) +
            ((y >> 3) & ((1 << shift) - 1)) * 512 +
            ((x & 32) << 3) + ((y & 6) << 5) +
            ((x & 16) << 1) + ((y & 1) << 4) + (x & 15))


def expected_surface(linear, pitch, shift, extent, fill,
                     width=WIDTH, height=HEIGHT):
    """Flip Mesa's bottom-up rows into a Tegra block-linear RGBA8 surface."""
    native = bytearray([fill]) * extent
    row_bytes = width * 4
    for y in range(height):
        row = linear[(height - y - 1) * row_bytes:(height - y) * row_bytes]
        for x in range(0, row_bytes, 16):
            at = block_linear_address(pitch, shift, x, y)
            count = min(16, row_bytes - x)
            native[at:at + count] = row[x:x + count]
    return bytes(native)


def surface_descriptor(pitch, shift, extent, runs, magic=0x31414d44,
                       width=WIDTH, height=HEIGHT):
    header = struct.pack("<10I", magic, width * height * 4, width, height,
                         pitch, 3, shift, 1, extent, len(runs))
    return header + b"".join(struct.pack("<III", address & 0xffffffff,
                                         address >> 32, length)
                             for address, length in runs)


def check_direct_surface(vm, size):
    """Place readbacks via register 40 into scattered and contiguous pages."""
    place_surface(vm, 128, 0, 2048, [(0x80500000, 2048)], width=17, height=9)
    pitch, shift = 320, 0
    extent = HEIGHT * pitch
    # Page-aligned scattered RAM is mapped; irregular runs retain the bounce path.
    place_surface(vm, pitch, shift, extent,
                  [(0x80100000, 4096), (0x80200000, extent - 4096)])
    place_surface(vm, pitch, shift, extent,
                  [(0x80100003, 4093), (0x80200001, extent - 4093)])
    place_surface(vm, pitch, shift, extent,
                  [(0x80100000, 4096), (0x80200001, extent - 4096)])
    place_surface(vm, pitch, shift, extent,
                  [(0x80100000, 4096), (0x80100000, 4096),
                   (0x80200000, extent - 8192)])
    place_surface(vm, pitch, shift, extent, [(0x80500000, extent)])
    place_surface(vm, 128, 0, 2048, [(0x80500000, 2048)], width=17, height=9)


def place_surface(vm, pitch, shift, extent, runs, width=WIDTH, height=HEIGHT):
    for address, length in runs:
        vm.request(f"write {address:#x} {length} 0x{'cc' * length}")
    descriptor = surface_descriptor(pitch, shift, extent, runs, magic=0,
                                    width=width, height=height)
    vm.request(f"write {BULK:#x} {len(descriptor)} 0x{descriptor.hex()}")
    vm.write32(BASE + 40, len(descriptor))
    if vm.read32(BASE + 44) != 1:
        raise RuntimeError("Invalid direct surface descriptor was accepted")
    if vm.read32(BASE + 16) != 0:
        raise RuntimeError("Bridge record lock unavailable")
    record = struct.pack("<II6I", 120, 24, 0, 0, width, height, 0x1908, 0x1401)
    vm.request(f"write {BULK:#x} {len(record)} 0x{record.hex()}")
    vm.write32(BASE + 28, len(record))
    descriptor = surface_descriptor(pitch, shift, extent, runs,
                                    width=width, height=height)
    vm.request(f"write {BULK:#x} {len(descriptor)} 0x{descriptor.hex()}")
    vm.write32(BASE + 40, len(descriptor))
    if vm.read32(BASE + 44) != 0:
        raise RuntimeError("Direct surface readback was not placed")
    vm.write32(BASE + 16, 0)
    if vm.read32(BASE + 32) != 0:
        raise RuntimeError("Direct surface readback left reply bytes behind")
    native = b"".join(
        bytes.fromhex(vm.request(f"read {address:#x} {length}")[0]
                      .removeprefix("0x"))
        for address, length in runs)
    full = expected_pixels(alpha=True)
    cropped = b"".join(full[y * WIDTH * 4:(y * WIDTH + width) * 4]
                       for y in range(height))
    expected = expected_surface(cropped, pitch, shift, extent, 0xcc, width, height)
    physical = {}
    offset = 0
    for address, length in runs:
        physical.update(zip(range(address, address + length),
                            expected[offset:offset + length]))
        offset += length
    expected = b"".join(bytes(physical[address + i] for i in range(length))
                        for address, length in runs)
    if native != expected:
        raise RuntimeError("Direct block-linear surface pixels differ")


def receive(vm, size):
    deadline = time.monotonic() + 15
    while (available := vm.read32(BASE + 32)) < size:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Expected a {size}-byte bridge reply")
        time.sleep(0.01)
    if available != size:
        raise RuntimeError(f"Unexpected {available}-byte bridge reply")
    raw = vm.request(f"read {BULK + (1 << 20):#x} {size}")[0]
    vm.write32(BASE + 36, size)
    return bytes.fromhex(raw.removeprefix("0x"))


def refresh(vm, image, pixels, address, action, flags=0, magic=0x31534452,
            pitch=WIDTH * 4, width=WIDTH, height=HEIGHT):
    """Ask the device to compare guest pixmap pages and upload only changes."""
    if vm.read32(BASE + 16) != 0:
        raise RuntimeError("Bridge record lock unavailable")
    record = struct.pack("<II5I", 145, 20 + len(pixels), action, image, 0,
                         width, height)
    extent = pitch * height
    descriptor = (struct.pack("<10I", magic, len(pixels), width, height, pitch,
                              1, 0, flags, extent, 1) +
                  struct.pack("<3I", 1234, image, len(record)) + record +
                  struct.pack("<III", address, 0, extent))
    vm.request(f"write {BULK:#x} {len(descriptor)} 0x{descriptor.hex()}")
    vm.write32(BASE + 48, len(descriptor))
    status = vm.read32(BASE + 44)
    if status == 0 and receive(vm, 4) != bytes(4):
        raise RuntimeError("Renderer rejected the device pixmap upload")
    vm.write32(BASE + 16, 0)
    return status


def check_device_refresh(vm):
    """Verify register 48 uploads, skips unchanged pages, and re-uploads edits."""
    image, address = 0x51, 0x80300000
    pixels = bytearray(os.urandom(WIDTH * HEIGHT * 4))
    vm.request(f"write {address:#x} {len(pixels)} 0x{pixels.hex()}")
    if refresh(vm, image, pixels, address, 0, magic=0) != 1:
        raise RuntimeError("Invalid pixmap refresh descriptor was accepted")
    if refresh(vm, image, pixels, address, 0) != 0:
        raise RuntimeError("Initial device pixmap upload was not queued")
    if refresh(vm, image, pixels, address, 2) != 3:
        raise RuntimeError("Unchanged pixmap pages were uploaded again")
    if refresh(vm, image, pixels, address, 2, flags=2) != 0:
        raise RuntimeError("Forced pixmap refresh was skipped")
    pixels[4096:4100] = b"\x01\x02\x03\x04"
    vm.request(f"write {address + 4096:#x} 4 0x01020304")
    if refresh(vm, image, pixels, address, 2) != 0:
        raise RuntimeError("Changed pixmap pages were not uploaded")
    pitch = WIDTH * 4 + 32
    padded = bytearray(b"\xa5" * (pitch * HEIGHT))
    for row in range(HEIGHT):
        padded[row * pitch:row * pitch + WIDTH * 4] = pixels[
            row * WIDTH * 4:(row + 1) * WIDTH * 4]
    vm.request(f"write {address:#x} {len(padded)} 0x{padded.hex()}")
    if refresh(vm, image, pixels, address, 2, pitch=pitch) != 3:
        raise RuntimeError("Layout-only change uploaded identical pixels")
    padded[WIDTH * 4] ^= 0xff
    vm.request(f"write {address:#x} {len(padded)} 0x{padded.hex()}")
    if refresh(vm, image, pixels, address, 2, pitch=pitch) != 3:
        raise RuntimeError("Padding-only change uploaded identical pixels")
    if refresh(vm, image, pixels, address, 2, pitch=pitch) != 3:
        raise RuntimeError("Updated padding did not prime the native cache")
    pixels[0] ^= 0xff
    padded[0] = pixels[0]
    vm.request(f"write {address:#x} {len(padded)} 0x{padded.hex()}")
    if refresh(vm, image, pixels, address, 2, pitch=pitch) != 0:
        raise RuntimeError("Reused cache skipped changed padded pixels")
    vm.send(146, struct.pack("<5I", 4, image, 0, WIDTH, HEIGHT))
    if receive(vm, 4 + len(pixels)) != bytes(4) + pixels:
        raise RuntimeError("Renderer pixmap differs from guest pages")
    check_device_sync(vm, image, bytes(pixels))
    vm.send(145, struct.pack("<5I", 3, image, 0, WIDTH, HEIGHT))
    if receive(vm, 4) != bytes(4):
        raise RuntimeError("Renderer image deletion failed")
    smaller = bytes(pixels[:32 * 32 * 4])
    vm.request(f"write {address:#x} {len(smaller)} 0x{smaller.hex()}")
    if refresh(vm, image, smaller, address, 0,
               pitch=128, width=32, height=32) != 0:
        raise RuntimeError("Resized image cache entry was not replaced")
    if refresh(vm, image, smaller, address, 2,
               pitch=128, width=32, height=32) != 3:
        raise RuntimeError("Resized image cache did not retain its pixels")
    vm.send(146, struct.pack("<5I", 4, image, 0, 32, 32))
    if receive(vm, 4 + len(smaller)) != bytes(4) + smaller:
        raise RuntimeError("Resized renderer image pixels differ")


def check_refresh_cache_budget(vm):
    """Keep live native+linear storage within the existing 256 MiB budget."""
    address, width, height, pitch = 0x81000000, 64, 2048, 16384
    pixels = bytes(width * height * 4)
    vm.request(f"memset {address:#x} {pitch * height} 0")
    for index in range(8):
        if refresh(vm, 0x100 + index, pixels, address, 0,
                   pitch=pitch, width=width, height=height) != 0:
            raise RuntimeError("Cache-budget fixture upload failed")
        if index == 6 and refresh(
                vm, 0x100, pixels, address, 2,
                pitch=pitch, width=width, height=height) != 3:
            raise RuntimeError("Native cache accounting caused premature eviction")
    if refresh(vm, 0x100, pixels, address, 2,
               pitch=pitch, width=width, height=height) != 0:
        raise RuntimeError("Cache did not evict when its storage budget was exceeded")


def device_sync(vm, image, address, length):
    if vm.read32(BASE + 16) != 0:
        raise RuntimeError("Bridge record lock unavailable")
    record = struct.pack("<II5I", 146, 20, 4, image, 0, WIDTH, HEIGHT)
    vm.request(f"write {BULK:#x} {len(record)} 0x{record.hex()}")
    vm.write32(BASE + 28, len(record))
    pitch = WIDTH * 4
    descriptor = (struct.pack("<10I", 0x31414d44, length, WIDTH, HEIGHT, pitch,
                              1, 0, 12, length, 1) +
                  struct.pack("<2I", 1234, image) +
                  struct.pack("<III", address, 0, length))
    vm.request(f"write {BULK:#x} {len(descriptor)} 0x{descriptor.hex()}")
    vm.write32(BASE + 40, len(descriptor))
    status = vm.read32(BASE + 44)
    vm.write32(BASE + 16, 0)
    if vm.read32(BASE + 32) != 0:
        raise RuntimeError("Device pixmap readback left reply bytes behind")
    return status


def check_device_sync(vm, image, pixels):
    """Place a status-prefixed pixmap readback and prime the refresh cache."""
    address = 0x80400000
    vm.request(f"write {address:#x} {len(pixels)} 0x{'00' * len(pixels)}")
    if device_sync(vm, image, address, len(pixels)) != 0:
        raise RuntimeError("Device pixmap readback was not placed")
    placed = vm.request(f"read {address:#x} {len(pixels)}")[0]
    if bytes.fromhex(placed.removeprefix("0x")) != pixels:
        raise RuntimeError("Device pixmap readback pixels differ")
    if refresh(vm, image, pixels, address, 2) != 3:
        raise RuntimeError("Placed readback did not prime the refresh cache")
    if device_sync(vm, 0x77, address, len(pixels)) != 2:
        raise RuntimeError("Failed renderer readback status was not reported")


def draw_test_frame(vm):
    vm.send(3, struct.pack("<4f", 1, 0, 0, 1))
    vm.send(4, struct.pack("<I", 0x4000))
    vm.send(33, struct.pack("<I", 0x0c11))
    vm.send(65, struct.pack("<4I", WIDTH // 2, 0, WIDTH // 2, HEIGHT))
    vm.send(3, struct.pack("<4f", 0, 1, 0, 1))
    vm.send(4, struct.pack("<I", 0x4000))
    vm.send(34, struct.pack("<I", 0x0c11))


def check(qemu, renderer, output, machine="mh2p"):
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="mh2p-graphics-") as temporary:
        scratch = Path(temporary)
        if machine == "mh2p":
            kernel = scratch / "startup.bin"
            kernel.write_bytes(bytes.fromhex("feffffea"))
            args = ["-M", machine, "-m", "256M", "-kernel", str(kernel)]
        else:
            nor = scratch / "nor.bin"
            with nor.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            iram = scratch / "iram.bin"
            iram.write_bytes(bytes(0x40000))
            args = ["-M", f"{machine},iram={iram}", "-m", "256M", "-bios", str(nor)]
        args += ["-display", "none", "-serial", "null", "-serial", "null",
                 "-nic", "none"]
        with BridgeVM(str(qemu), args=args, base_temp_dir=temporary,
                      qmp_timer=10) as vm:
            vm.launch()
            if vm.read32(BASE) != 0:
                raise RuntimeError("Graphics service is exposed without opt-in")

        (scratch / "tmp").mkdir()
        stress = subprocess.run([
            sys.executable, str(ROOT / "tools/mhi2/check_gl_transport.py"),
            str(scratch), "--machine", machine, "--qemu", str(qemu),
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        (output / "transport.log").write_bytes(stress.stdout)
        if stress.returncode:
            raise RuntimeError(f"Transport regression failed; see {output / 'transport.log'}")

        pipe = scratch / "bridge"
        os.mkfifo(str(pipe) + ".in")
        os.mkfifo(str(pipe) + ".out")
        frame = output / "frame.ppm"
        env = dict(os.environ, MHI2_GL_FRAME=str(frame),
                   EGL_PLATFORM="surfaceless", LIBGL_ALWAYS_SOFTWARE="1")
        for key in ("MHI2_GL_DUMP_CLIENTS", "MHI2_GL_CAPTURE", "MHI2_GL_TRACE"):
            env.pop(key, None)
        with open(str(pipe) + ".out", "r+b", buffering=0) as tx, \
                open(str(pipe) + ".in", "r+b", buffering=0) as rx, \
                (output / "renderer.log").open("wb") as errors:
            host = subprocess.Popen([str(renderer)], stdin=tx, stdout=rx,
                                    stderr=errors, env=env)
            vm = BridgeVM(str(qemu), args=args + [
                "-chardev", f"pipe,id=glbridge,path={pipe}",
                "-trace", "glbridge_surface_mapping",
                "-trace", "glbridge_image_cache",
                "-trace", "glbridge_image_reuse",
            ], base_temp_dir=temporary, qmp_timer=10)
            try:
                with vm:
                    vm.launch()
                    if vm.read32(BASE) != 0x474c4252 or vm.read32(BASE + 24) != 1 << 20:
                        raise RuntimeError("Wrong graphics transport identity or bulk size")
                    if vm.read32(BASE + 16) != 0 or vm.read32(BASE + 16) != 1:
                        raise RuntimeError("Record lock does not serialize clients")
                    vm.write32(BASE + 16, 0)
                    vm.send(1, struct.pack("<2I", WIDTH, HEIGHT))
                    draw_test_frame(vm)
                    vm.send(5)
                    vm.send(120, struct.pack("<6I", 0, 0, WIDTH, HEIGHT, 0x1908, 0x1401))
                    size = WIDTH * HEIGHT * 4
                    deadline = time.monotonic() + 15
                    while vm.read32(BASE + 32) != size or not frame.exists():
                        if host.poll() is not None:
                            raise RuntimeError(f"Renderer exited: {host.returncode}")
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Rendered frame/readback did not arrive")
                        time.sleep(0.01)
                    raw = vm.request(f"read {BULK + (1 << 20):#x} {size}")[0]
                    if bytes.fromhex(raw.removeprefix("0x")) != expected_pixels(alpha=True):
                        raise RuntimeError("Bridge bulk readback pixels differ")
                    if vm.read32(BASE + 8) != 4096 or vm.read32(BASE + 0x2000) != 0xff0000ff:
                        raise RuntimeError("Legacy RX window differs from bulk data")
                    vm.write32(BASE + 20, 4097)
                    vm.write32(BASE + 36, size + 1)
                    if vm.read32(BASE + 32) != size:
                        raise RuntimeError("Oversized acknowledgement consumed data")
                    vm.write32(BASE + 36, size)
                    if vm.read32(BASE + 32) != 0:
                        raise RuntimeError("Readback acknowledgement failed")
                    frame_hash = verify_frame(frame.read_bytes())
                    check_direct_surface(vm, size)
                    check_device_refresh(vm)
                    check_refresh_cache_budget(vm)
                    vm.send(60, struct.pack("<II", 0x8d40, 0))
                    draw_test_frame(vm)
                    check_direct_surface(vm, size)
                    vm.read32(BASE + 16)
                    vm.cmd("system_reset")
                    if vm.read32(BASE + 32) != 0 or vm.read32(BASE + 16) != 0:
                        raise RuntimeError("Reset retained RX data or record lock")
                    vm.write32(BASE + 16, 0)
                mapping_log = vm.get_log() or ""
                for runs, mode in ((1, 1), (2, 2), (2, 0), (3, 0)):
                    if f"glbridge_surface_mapping runs={runs} mode={mode}" not in mapping_log:
                        raise RuntimeError(f"Surface mapping path {mode} was not exercised")
                for native, total in ((8192, 16384), (9216, 17408), (0, 8192)):
                    record = (f"glbridge_image_cache pixels=8192 native={native} "
                              f"total={total} entries=1")
                    if record not in mapping_log:
                        raise RuntimeError(f"Incorrect image-cache accounting: {record}")
                if ("glbridge_image_cache pixels=4096 native=4096 "
                        "total=8192 entries=1") not in mapping_log:
                    raise RuntimeError("Resized cache replacement accounting differs")
                if mapping_log.count("glbridge_image_reuse bytes=8192") < 3:
                    raise RuntimeError("Same-size image storage was not reused")
                if ("glbridge_image_cache pixels=524288 native=33554432 "
                        "total=34078720 entries=1") not in mapping_log:
                    raise RuntimeError("Cache eviction accounting was not exercised")
            finally:
                (output / "qemu.log").write_text(vm.get_log() or "")
                if host.poll() is None:
                    host.terminate()
                    try:
                        host.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        host.kill()
                        host.wait()
    result = {
        "machine": machine, "width": WIDTH, "height": HEIGHT,
        "frame_sha256": frame_hash, "exact_pixels_verified": True,
        "bulk_readback_verified": True, "idle_transport_reset_verified": True,
        "disabled_by_default_verified": True,
        "transport_backpressure_verified": True,
        "direct_surface_placement_verified": True,
        "scattered_page_mapping_verified": True,
        "irregular_surface_fallback_verified": True,
        "aliased_surface_fallback_verified": True,
        "device_pixmap_refresh_verified": True,
        "device_pixmap_readback_verified": True,
        "readback_buffer_reuse_verified": True,
        "refresh_buffer_reuse_verified": True,
        "image_cache_accounting_verified": True,
        "resized_image_cache_verified": True,
        "padding_only_refresh_verified": True,
        "image_cache_budget_verified": True,
        "note": "qtest-injected protocol records; no QNX graphics client or HMI verified",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine", choices=("mh2p", "mhi2-harman"), default="mh2p")
    args = parser.parse_args()
    print(json.dumps(check(args.qemu.resolve(), args.renderer.resolve(),
                           args.output.resolve(), args.machine), indent=2))


if __name__ == "__main__":
    main()
