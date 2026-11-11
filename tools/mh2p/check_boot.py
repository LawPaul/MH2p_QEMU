#!/usr/bin/env python3
"""Verify stock QNX userspace and diagnostic shell execution, including reset."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
import uuid

from prepare_mnand import DISK_SIZE, digest


CLOCK_INIT_REGISTERS = {
    0x6000652c: 0x03000043,
    0x60006530: 0x03000040,
    0x600060c0: 0x4803c00c,
    0x600060ec: 0x00000b3c,
    0x6000648c: 0x07000008,
    0x6000651c: 0x03000040,
    0x7009f040: 0x00080000,
}

SMMU_INIT_REGISTERS = {
    0x70019010: 0x00000001,
    0x70019014: 0x30000020,
    0x70019018: 0x2800003f,
    0x70019228: 0xffffffff,
    0x7001922c: 0xffffffff,
    0x70019230: 0xffffffff,
    0x70019234: 0xffffffff,
}


GPU_EARLY_INIT_REGISTERS = {
    0x57000000: 0x0ea000a1,
    0x57000200: 0x00000020,
    0x57001140: 0x0000000e,
    0x57137250: 0x00000000,
    0x7000e6d4: 0x00000000,
}

GPU_RING_INIT_REGISTERS = {
    0x5712004c: 0x00000000,
    0x57120050: 0x00000001,
    0x57120070: 0x00000001,
    0x57120074: 0x00000001,
    0x57120078: 0x00000001,
    0x57122204: 0x00000002,
    0x57009080: 0x800186a0,
}


def verify_hmp_registers(log, expected, label):
    rows = re.findall(r"(?m)^([0-9a-f]{8}): 0x([0-9a-f]{8})\r?$", log)
    actual = {int(address, 16): int(value, 16) for address, value in rows}
    if len(rows) != len(expected) or actual != expected:
        raise RuntimeError(f"Unexpected native {label} register evidence: {actual}")
    return {f"0x{address:08x}": f"0x{value:08x}" for address, value in actual.items()}


def verify_smmu_init(log):
    return verify_hmp_registers(log, SMMU_INIT_REGISTERS, "SMMU-init")


def verify_gpu_early_init(log):
    return verify_hmp_registers(log, GPU_EARLY_INIT_REGISTERS, "GPU-early-init")


def verify_gpu_ring_init(log):
    return verify_hmp_registers(log, GPU_RING_INIT_REGISTERS, "GPU-ring-init")


def verify_clock_init(log):
    rows = re.findall(rb"(?m)^0x([0-9a-f]{8}) : 0x([0-9a-f]{8})\r?$", log)
    actual = {int(address, 16): int(value, 16) for address, value in rows}
    if len(rows) != len(CLOCK_INIT_REGISTERS) or actual != CLOCK_INIT_REGISTERS:
        raise RuntimeError(f"Unexpected native clock-init register evidence: {actual}")
    return {f"0x{address:08x}": f"0x{value:08x}" for address, value in actual.items()}


def verify_normal_mnand(log):
    if log.count(b"ifs image checksum is valid") != 6:
        raise RuntimeError("Expected exactly six checksum-valid normal-startup volumes")
    ready = re.search(rb"time (\d+) ms, \[\[\d+\] wait for MNAND done\]", log)
    if not ready or int(ready[1]) >= 5000:
        raise RuntimeError("Stock app file was not ready within the five-second deadline")
    reads = re.findall(rb"mnandCounter = (\d+), readsize: (\d+) MiB", log)
    if not any(int(blocks) > 0 and int(size) > 0 for blocks, size in reads):
        raise RuntimeError("No successful normal-startup mNAND reads")
    return int(ready[1])


def verify_runtime_services(log):
    for service in (b"nvmemctrl", b"devg-nvclock", b"devg-nvpowergate",
                    b"devg-nvsys", b"devg-nvrm"):
        if not re.search(rb"(?m)^[ \t]*\d+[ \t]+(?:/sbin/)?" +
                         service + rb"(?:[ \t\r\n]|$)", log):
            raise RuntimeError(f"Missing live service {service!r}")


def verify_hmi_startup(log):
    version = re.findall(rb"(?m)^# HMI[ \t]+: (HMI_PAG_[A-Za-z0-9_]+)\r?$", log)
    if len(version) != 1:
        raise RuntimeError("Missing unique original Porsche HMI version")
    slog = (rb"^[A-Z][a-z]{2}[ \t]+\d{1,2} \d{2}:\d{2}:\d{2}"
            rb"[ \t]+\d+[ \t]+\d+[ \t]+\d+[ \t]+")
    for pattern in (
        rb"^Starting 'LSD\.main' thread with prio: \d+\r?$",
        slog + rb"[^\r\n]*MMX BENCH_Startup: StartupActivator done\r?$",
        slog + rb"#FWP: de\.esolutions\.fw\.dsi\.AdapterActivator started\r?$",
        rb"^[ \t]*\d+[ \t]+/ifs/jre/bin/j9[ \t]+[^\r\n]* de\.dreisoft\.lsd\.LSD\r?$",
    ):
        if len(re.findall(pattern, log, re.MULTILINE)) != 1:
            raise RuntimeError(f"Missing unique HMI startup evidence: {pattern!r}")
    return {
        "version": version[0].decode(),
        "original_hmi_entrypoint_verified": True,
        "framework_activation_verified": True,
        "live_hmi_process_verified": True,
        "note": "Framework startup only; Screen, UI readiness and MOST video are not verified",
    }


def check_one(qemu, media, output, shell, nor=None, mnand=None, icount_shift=None,
              hmi_startup=False):
    if hmi_startup and (not shell or not mnand or icount_shift != 0):
        raise ValueError("HMI startup requires diagnostic media, mNAND and icount shift 0")
    manifest = json.loads((media / "manifest.json").read_text())
    if manifest["diagnostic_shell"] != shell:
        raise ValueError(f"Incorrect media mode: {media}")
    for name, field in [("startup.bin", "startup_sha256"), ("board.dtb", "dtb_sha256")]:
        if hashlib.sha256((media / name).read_bytes()).hexdigest() != manifest[field]:
            raise ValueError(f"Media hash mismatch: {media / name}")
    output.mkdir()
    mnand_ready_ms = []
    clock_init_registers = []
    smmu_init_registers = []
    gpu_early_init_registers = []
    gpu_ring_init_registers = []
    hmi_startups = []
    with tempfile.TemporaryDirectory(prefix="mh2p-serial-") as temporary:
        console_path = Path(temporary) / "uart"
        cmd = [
            str(qemu.resolve()), "-M", "mh2p", "-S",
            "-kernel", str((media / "startup.bin").resolve()),
            "-dtb", str((media / "board.dtb").resolve()),
            "-display", "none", "-monitor", "none", "-qmp", "stdio", "-nic", "none",
            "-serial", "null", "-serial", f"unix:{console_path},server=on,wait=off",
        ]
        if icount_shift is not None:
            cmd += ["-icount", f"shift={icount_shift},align=off,sleep=off"]
        if nor:
            cmd += ["-bios", str(nor.resolve())]
        if mnand:
            filename = str(mnand.resolve()).replace(",", ",,")
            cmd += ["-drive", f"if=sd,index=0,format=raw,snapshot=on,file={filename}"]
        with (output / "host.log").open("wb") as errors, (output / "uart.log").open("wb") as log:
            process = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0
            )
            pending = bytearray()

            def receive():
                deadline = time.monotonic() + 10
                while b"\n" not in pending:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
                        raise TimeoutError("QMP response timed out")
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError(f"QEMU exited; see {output / 'host.log'}")
                    pending.extend(chunk)
                line, _, rest = pending.partition(b"\n")
                pending[:] = rest
                return json.loads(line)

            def qmp(command, arguments=None):
                process.stdin.write((json.dumps({
                    "execute": command, "arguments": arguments or {}
                }) + "\n").encode())
                while True:
                    response = receive()
                    if "error" in response:
                        raise RuntimeError(f"QMP {command}: {response['error']}")
                    if "return" in response:
                        return response["return"]

            try:
                receive()
                qmp("qmp_capabilities")
                with socket.socket(socket.AF_UNIX) as console:
                    console.settimeout(5)
                    console.connect(str(console_path))
                    captured = bytearray()

                    def wait_marker(marker, start, timeout=15, count=1):
                        deadline = time.monotonic() + timeout
                        while captured[start:].count(marker) < count:
                            if process.poll() is not None:
                                raise RuntimeError("QEMU exited before the serial marker")
                            if time.monotonic() >= deadline:
                                raise TimeoutError(f"Missing guest marker {marker!r}")
                            if select.select([console], [], [], 0.1)[0]:
                                chunk = console.recv(65536)
                                if not chunk:
                                    raise RuntimeError("Guest serial connection closed")
                                captured.extend(chunk)
                                log.write(chunk)
                                log.flush()

                    for boot in range(2):
                        start = len(captured)
                        qmp("cont")
                        if shell:
                            banner = b"MH2P diagnostic shell: normal services are NOT started."
                            wait_marker(banner, start)
                            prompt_start = captured.index(banner, start) + len(banner)
                            wait_marker(b"mh2p# ", prompt_start)
                            marker = f"MH2P_CHECK_{uuid.uuid4().hex}"
                            console.sendall(f"echo {marker}:$((6 * 7))\n".encode())
                            wait_marker(f"{marker}:42".encode(), start)
                            marker = f"MH2P_GPIO_{uuid.uuid4().hex}"
                            console.sendall((
                                "devg-nvsku && waitfor /dev/nvsku/sku && "
                                "devg-nvos && waitfor /dev/nvos && "
                                "devg-nvgpio && waitfor /dev/nvgpio && "
                                f"test -e /dev/nvgpio && echo {marker}:$((6 * 7))\n"
                            ).encode())
                            wait_marker(f"{marker}:42".encode(), start)
                            if mnand:
                                marker = f"MH2P_MNAND_{uuid.uuid4().hex}"
                                console.sendall((
                                    "devb-sdmmc-tegra3 blk cache=10m "
                                    "sdio addr=0x700b0600,irq=63,emmc,bw=8,"
                                    "clk=52000000,timing=hs disk name=mnanda; "
                                    "waitfor /dev/mnanda0t177.1 10; "
                                    "echo /dev/mnanda*; "
                                    "mount -tqnx6 -oro /dev/mnanda0t177.1 /mnt/app && "
                                    "test -f /mnt/app/img_restore/main_stage2.ifs.lzo && "
                                    f"echo {marker}:$((6 * 7))\n"
                                ).encode())
                                wait_marker(f"{marker}:42".encode(), start)
                                marker = f"MH2P_IFS_{uuid.uuid4().hex}"
                                console.sendall((
                                    "res_init -d 0,31 && "
                                    "ifsloader -s 32505856 -a 18087936 "
                                    "-r 0x92000000,0x0B400000 -m / -v -n0 "
                                    "-d /mnt/app/img_restore/main_stage2.ifs.lzo && "
                                    f"echo {marker}:$((6 * 7))\n"
                                ).encode())
                                wait_marker(f"{marker}:42".encode(), start, timeout=45)
                                if captured[start:].count(b"ifs image checksum is valid") != 6:
                                    raise RuntimeError("Expected six checksum-valid app volumes")
                                marker = f"MH2P_I2C_{uuid.uuid4().hex}"
                                commands = []
                                for controller in range(1, 7):
                                    commands.append(
                                        f"/sbin/i2c-tegra2 -c{controller} "
                                        f"--u{controller} --b100000"
                                    )
                                    commands.append(f"waitfor /dev/i2c{controller} 5")
                                    commands.append(f"test -e /dev/i2c{controller}")
                                commands.append(f"echo {marker}:$((6 * 7))")
                                console.sendall((" && ".join(commands) + "\n").encode())
                                wait_marker(f"{marker}:42".encode(), start, timeout=40)
                                marker = f"MH2P_RUNTIME_{uuid.uuid4().hex}"
                                service_start = len(captured)
                                clock_reads = " && ".join(
                                    f"in32 0x{address:08x}" for address in CLOCK_INIT_REGISTERS
                                )
                                console.sendall((
                                    "nvmemctrl && devg-nvclock && "
                                    "waitfor /dev/nvclock 5 && "
                                    "devg-nvpowergate && waitfor /dev/nvpowergate 5 && "
                                    "powergate_init && clock_init && "
                                    "test -e /dev/nvclock && test -e /dev/nvpowergate && "
                                    "devg-nvsys && waitfor /dev/nvsys/tegra_chip_id 5 && "
                                    "test -r /dev/nvsys/tegra_chip_rev && "
                                    "test -r /dev/nvsys/tegra_fuse_vp8_enable && "
                                    f"{clock_reads} && "
                                    "devg-nvrm && waitfor /dev/nvmap 5 && "
                                    "waitfor /dev/nvhost-ctrl 5 && "
                                    "waitfor /dev/nvdisp-ctrl 5 && "
                                    "test -e /dev/nvmap && test -e /dev/nvhost-ctrl && "
                                    "test -e /dev/nvdisp-ctrl && "
                                    f"pidin ar && echo {marker}:$((6 * 7))\n"
                                ).encode())
                                wait_marker(f"{marker}:42".encode(), start, timeout=40)
                                verify_runtime_services(captured[service_start:])
                                clock_init_registers.append(
                                    verify_clock_init(captured[service_start:])
                                )
                                if hmi_startup:
                                    hmi_start = len(captured)
                                    marker = f"MH2P_HMI_{uuid.uuid4().hex}"
                                    console.sendall((
                                        "pipe && slogger && . /etc/global.env && "
                                        "export RUN_MODE=normal IPL_CONFIG_DIR=/etc/eso/production "
                                        "LD_LIBRARY_PATH=/lib:/usr/lib:/eso/lib:/armle/lib:"
                                        "/armle/usr/lib && "
                                        "/eso/bin/runHMI.sh >/tmp/mh2p-hmi.log 2>&1 && "
                                        "sleep 5 && cat /tmp/mh2p-hmi.log && sloginfo && "
                                        f"pidin ar && echo {marker}:$((6 * 7))\n"
                                    ).encode())
                                    wait_marker(f"{marker}:42".encode(), hmi_start, timeout=60)
                                    hmi_startups.append(verify_hmi_startup(captured[hmi_start:]))
                            response = f"{marker}:42".encode()
                            prompt_start = captured.index(response, start) + len(response)
                            wait_marker(b"mh2p# ", prompt_start)
                        else:
                            wait_marker(b"Starting /usr/sbin/main_stage1.sh", start)
                            wait_marker(b"PROJ: 61857, REV: 202, SKU: 5017", start)
                            wait_marker(b"Starting res_init...", start)
                            if (b"waitfor timeout /dev/nvgpio" in captured[start:]
                                    or b"(devg-nvgpio) terminated" in captured[start:]):
                                raise RuntimeError("Stock GPIO initialization failed")
                            if nor:
                                wait_marker(b"ifs image checksum is valid", start,
                                            timeout=90, count=6 if mnand else 5)
                                wait_marker(b"RESET SOURCE: 0x7000e5b4 : 0x00000000", start)
                                wait_marker(b"Starting /usr/sbin/main_stage2.2.sh", start)
                                if mnand:
                                    mnand_ready_ms.append(verify_normal_mnand(captured[start:]))
                                    wait_marker(b"Starting NvPmu server...", start, timeout=60)
                                    wait_marker(b"Starting display-starter.sh...", start, timeout=60)
                                    for service in (b"nvmemctrl", b"devg-nvclock",
                                                    b"devg-nvpowergate", b"clock_init",
                                                    b"powergate_init", b"devg-nvsys"):
                                        if b"(" + service + b") terminated" in captured[start:]:
                                            raise RuntimeError(f"Stock runtime crash: {service!r}")
                                elif captured[start:].count(b"ifs image checksum is valid") != 5:
                                    raise RuntimeError("Expected five checksum-valid NOR volumes")
                        qmp("stop")
                        if not shell and nor and mnand:
                            for name, expected, verify, snapshots in (
                                ("smmu", SMMU_INIT_REGISTERS,
                                 verify_smmu_init, smmu_init_registers),
                                ("gpu-early", GPU_EARLY_INIT_REGISTERS,
                                 verify_gpu_early_init, gpu_early_init_registers),
                                ("gpu-ring", GPU_RING_INIT_REGISTERS,
                                 verify_gpu_ring_init, gpu_ring_init_registers),
                            ):
                                snapshot = "\n".join(
                                    qmp("human-monitor-command", {
                                        "command-line": f"xp /1wx 0x{address:08x}"
                                    }) for address in expected
                                )
                                (output / f"{name}-{boot}.log").write_text(snapshot)
                                snapshots.append(verify(snapshot))
                        registers = qmp("human-monitor-command", {"command-line": "info registers"})
                        (output / f"registers-{boot}.log").write_text(registers)
                        if boot == 0:
                            qmp("system_reset")
                    qmp("quit")
                process.wait(timeout=5)
                if process.returncode:
                    raise RuntimeError(f"QEMU exit status {process.returncode}")
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
    return {"mode": "diagnostic-shell" if shell else "stock",
            "boot_count": 2, "reset_verified": True,
            "gpio_device_verified": shell,
            "reset_source_verified": bool(nor and not shell),
            "verified_nor_volumes": 5 if nor and not mnand and not shell else 0,
            "verified_normal_volumes": 6 if nor and mnand and not shell else 0,
            "normal_mnand_ready_ms": mnand_ready_ms,
            "icount_shift": icount_shift,
            "mnand_app_verified": bool(mnand and (shell or nor)),
            "verified_app_volumes": 6 if mnand and shell else 0,
            "verified_i2c_controllers": 6 if mnand and shell else 0,
            "runtime_services_verified": bool(mnand and shell),
            "system_service_verified": bool(mnand and shell),
            "resource_manager_without_gpu_open_verified": bool(mnand and shell),
            "clock_init_verified": bool(mnand and shell),
            "clock_init_registers": clock_init_registers,
            "normal_clock_init_reached": bool(mnand and nor and not shell),
            "normal_smmu_init_verified": bool(smmu_init_registers),
            "smmu_init_registers": smmu_init_registers,
            "normal_gpu_early_init_verified": bool(gpu_early_init_registers),
            "gpu_early_init_registers": gpu_early_init_registers,
            "normal_gpu_ring_init_verified": bool(gpu_ring_init_registers),
            "gpu_ring_init_registers": gpu_ring_init_registers,
            "hmi_startups": hmi_startups,
            "startup_sha256": manifest["startup_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path,
                        default=Path(__file__).resolve().parents[2] / "build/qemu-system-arm")
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--shell", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nor", type=Path, help="optional prepared NOR image for stage-two checks")
    parser.add_argument("--mnand", type=Path, help="optional prepared mNAND for app mount checks")
    parser.add_argument("--hmi-startup", action="store_true",
                        help="also verify original Porsche HMI framework startup in the "
                             "diagnostic guest, not rendering; requires mNAND and icount 0")
    parser.add_argument("--icount-shift", type=int, choices=range(11),
                        help="instruction-counted virtual time; use 0 for normal mNAND startup")
    args = parser.parse_args()
    if args.hmi_startup and (not args.mnand or args.icount_shift != 0):
        parser.error("--hmi-startup requires --mnand and --icount-shift 0")
    try:
        nor_digest = None
        mnand_digest = None
        if args.mnand:
            if args.mnand.stat().st_size != DISK_SIZE:
                raise ValueError("Expected a prepared 4 GiB mNAND image")
            mnand_digest = digest(args.mnand)
            metadata = json.loads((args.mnand.parent / "manifest.json").read_text())
            if mnand_digest != metadata["mnand_sha256"]:
                raise ValueError("mNAND image differs from its manifest")
        if args.nor:
            if args.nor.stat().st_size != 64 * 1024 * 1024:
                raise ValueError("Expected a 64 MiB NOR image")
            nor_digest = hashlib.sha256(args.nor.read_bytes()).hexdigest()
            nor_manifest = json.loads((args.nor.parent / "manifest.json").read_text())
            if nor_digest != nor_manifest["nor_sha256"]:
                raise ValueError("NOR image differs from its manifest")
        args.output.mkdir(parents=True, exist_ok=False)
        result = [
            check_one(args.qemu, args.stock, args.output / "stock", False,
                      args.nor, args.mnand, args.icount_shift),
            check_one(args.qemu, args.shell, args.output / "shell", True,
                      args.nor, args.mnand, args.icount_shift, args.hmi_startup),
        ]
        if args.nor:
            if hashlib.sha256(args.nor.read_bytes()).hexdigest() != nor_digest:
                raise RuntimeError("Guest modified the source NOR image")
            for item in result:
                item["nor_sha256"] = nor_digest
                item["nor_source_unchanged"] = True
        if args.mnand:
            if digest(args.mnand) != mnand_digest:
                raise RuntimeError("Guest modified the source mNAND image")
            for item in result:
                item["mnand_sha256"] = mnand_digest
                item["mnand_source_unchanged"] = True
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f"MH2P boot check FAILED: {exc}\n")
    (args.output / "checks.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS: stock QNX userspace, diagnostic shell and GPIO device, across reset")
    if args.nor and args.mnand:
        print("PASS: normal mNAND startup, app ready before 5s and all six volumes, across reset")
        print("PASS: native SMMU initialization registers, across reset (not graphics/DMA)")
        print("PASS: native GPU identification and early controls, across reset (not rendering)")
        print("PASS: native GPU register-ring setup, across reset (not command execution)")
    elif args.nor:
        print("PASS: five checksum-valid NOR volumes and stage-two scripts, across reset")
    if args.mnand:
        print("PASS: diagnostic app mount and six checksum-valid volumes, across reset")
        print("PASS: six native I2C controller devices, across reset")
        print("PASS: native services, powergate_init and clock_init, across reset")
        print("PASS: native resource manager without eager GPU open, across reset")
    if args.hmi_startup:
        print("PASS: original Porsche HMI entry point and framework, across reset "
              "(not UI readiness, Screen rendering or MOST video)")


if __name__ == "__main__":
    main()
