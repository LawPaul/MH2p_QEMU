#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute original P2865 radio reset up to, but not through, its first MMIO read."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import tempfile
import time

from probe_rcc import QEMUMachine, ROOT


FIRMWARE_SIZE = 601441
FIRMWARE_SHA256 = "249d47237b5e33a17cea1563c608079adc82e55b4c2aee1bc7cba813c4730181"
RAM_BASE = 0x180000
FIRST_MMIO_PC = 0x183086
CHIPCOMMON_BASE = 0x18000000


def verify_firmware(image):
    if image.stat().st_size != FIRMWARE_SIZE:
        raise ValueError("Expected original P2865 BCM4359 firmware size")
    data = image.read_bytes()
    if hashlib.sha256(data).hexdigest() != FIRMWARE_SHA256:
        raise ValueError("Expected original P2865 BCM4359 firmware SHA-256")
    return data


def read_packet(sock):
    def receive(count):
        data = bytearray()
        while len(data) < count:
            part = sock.recv(count - len(data))
            if not part:
                raise RuntimeError("Radio debugger disconnected")
            data.extend(part)
        return bytes(data)

    while True:
        byte = receive(1)
        if byte == b"$":
            break
        if byte == b"-":
            raise RuntimeError("Radio debugger rejected request checksum")
        if byte != b"+":
            raise RuntimeError(f"Unexpected radio debugger prefix: {byte!r}")
    payload = bytearray()
    while True:
        byte = receive(1)
        if byte == b"#":
            break
        payload.extend(byte)
        if len(payload) > 65536:
            raise RuntimeError("Radio debugger reply exceeds probe limit")
    if int(receive(2), 16) != sum(payload) & 255:
        raise RuntimeError("Radio debugger reply checksum mismatch")
    sock.sendall(b"+")
    return payload.decode("ascii")


def request(sock, command):
    payload = command.encode("ascii")
    sock.sendall(b"$" + payload + b"#" +
                 f"{sum(payload) & 255:02x}".encode("ascii"))
    return read_packet(sock)


def require_ok(sock, command):
    reply = request(sock, command)
    if reply != "OK":
        raise RuntimeError(f"Radio debugger rejected {command!r}: {reply!r}")


def verify_state(registers, vectors, source):
    # ARM GDB's first sixteen registers precede any optional register banks.
    raw = bytes.fromhex(registers)
    if len(raw) < 64:
        raise RuntimeError("Radio debugger returned incomplete ARM registers")
    words = [int.from_bytes(raw[i:i + 4], "little") for i in range(0, 64, 4)]
    if words[15] != FIRST_MMIO_PC:
        raise RuntimeError(f"Radio stopped at unexpected PC {words[15]:#x}")
    if words[0] != CHIPCOMMON_BASE:
        raise RuntimeError(f"Unexpected first radio MMIO address {words[0]:#x}")
    if bytes.fromhex(vectors) != source[0x20:0x40]:
        raise RuntimeError("Original radio reset did not copy its exception vectors")
    return words


def probe(qemu, image, output, timeout=5):
    if not 0 < timeout <= 30:
        raise ValueError("Debugger timeout must be between 0 and 30 seconds")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    source = verify_firmware(image)
    image = image.resolve()
    if "," in str(image):
        raise ValueError("Firmware path cannot contain a QEMU loader delimiter")
    output = output.resolve()
    output.mkdir(parents=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-wifi-reset-") as name:
        gdb_path = Path(name) / "gdb.sock"
        args = [
            "-M", "none", "-cpu", "cortex-r5", "-m", "4M", "-S",
            "-display", "none", "-nic", "none",
            "-device", f"loader,file={image},addr={RAM_BASE:#x},force-raw=on",
            "-device", (f"loader,addr=0,data={int.from_bytes(source[:4], 'little'):#x},"
                        "data-len=4"),
            "-device", "loader,addr=1,cpu-num=0",
            "-gdb", f"unix:{gdb_path},server=on,wait=off",
            "-d", "in_asm,unimp,guest_errors",
            "-D", str(output / "hardware.log"),
        ]
        with QEMUMachine(str(qemu.resolve()), args=args) as vm:
            vm.launch()
            vm._qmp.settimeout(timeout)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(timeout)
                sock.connect(str(gdb_path))
                request(sock, "qSupported:multiprocess+")
                require_ok(sock, "Hgp1.1")
                require_ok(sock, f"Z0,{FIRST_MMIO_PC:x},2")
                try:
                    stopped = request(sock, "vCont;c")
                    if not stopped.startswith("T05"):
                        raise RuntimeError(f"Unexpected radio stop reply: {stopped!r}")
                    registers = request(sock, "g")
                    vectors = request(sock, "m0,20")
                    words = verify_state(registers, vectors, source)
                finally:
                    vm.cmd("stop")
                    state = vm.cmd("human-monitor-command",
                                   command_line="info registers")
                    (output / "registers.log").write_text(state)
    if image.read_bytes() != source:
        raise RuntimeError("Radio firmware changed during probe")
    report = {
        "source_sha256": FIRMWARE_SHA256,
        "source_size": len(source),
        "original_firmware_unmodified": True,
        "cpu_fixture": "cortex-r5",
        "actual_broadcom_cpu_emulated": False,
        "ram_fixture_bytes": 4 * 1024 * 1024,
        "firmware_load_address": RAM_BASE,
        "reset_pc_fixture": 1,
        "reset_word_supplied_by_loader": True,
        "original_reset_vector_copy_verified": True,
        "original_reset_and_mpu_instructions_executed": True,
        "first_mmio_pc": words[15],
        "first_mmio_address": words[0],
        "stopped_before_first_mmio_read": True,
        "hardware_register_returns_stubbed": False,
        "pcie_endpoint_emulated": False,
        "radio_firmware_boot_verified": False,
        "wifi_ap_verified": False,
        "wireless_carplay_verified": False,
        "owned_vm_exited": True,
        "elapsed_seconds": time.monotonic() - started,
        "command": [str(qemu.resolve())] + args,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.image, args.output, args.timeout),
                     indent=2))


if __name__ == "__main__":
    main()
