#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run original NVTOSP entry code in an isolated, explicitly secure CPU probe."""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import select
import socket
import struct
import tempfile
import time

from probe_rcc import QEMUMachine, ROOT


LOAD_ADDRESS = 0x88000000
SECURE_SIZE = 16 * 1024 * 1024
CONTEXT_ADDRESS = 0x80010000
QNX_CONTEXT_ADDRESS = 0x81010000
QNX_SHIM_ADDRESS = 0x81000000


def trusted_payload(data):
    if len(data) < 0x300 or data[:7] != b"NVTOSP\0":
        raise ValueError("Expected an original NVTOSP image")
    size_text = data[7:16].split(b"\0", 1)[0]
    if not size_text.isdigit():
        raise ValueError("Invalid NVTOSP decimal payload length")
    size = int(size_text)
    if size != struct.unpack_from("<I", data, 28)[0]:
        raise ValueError("NVTOSP payload lengths disagree")
    if not 0 < size < SECURE_SIZE or size % 4:
        raise ValueError("NVTOSP payload must fit the probe secure region")
    if len(data) != 0x200 + size + 0x100:
        raise ValueError("Expected NVTOSP header, payload and signature trailer")
    # Container validation is not cryptographic signature authentication.
    return data[0x200:0x200 + size]


def boot_context(config, warmboot=b""):
    """Serialize config and raw TZRAM data (legacy ``warmboot`` argument)."""
    if b"\0" in config:
        raise ValueError("Boot configuration must not contain embedded NULs")
    config.decode("ascii")
    config += b"\0"
    if len(config) > 65536 or len(warmboot) > 65536:
        raise ValueError("Probe configuration and TZRAM sections must fit 64 KiB")
    padding = bytes(-len(config) % 4)
    return (struct.pack("<I", len(config)) + config + padding +
            struct.pack("<I", len(warmboot)) + warmboot)


def bootstrap(context_address=0):
    # A probe ABI, not a reconstruction of Quickboot's full config structure.
    # The original entry checks r0 (secure-region size) and derives its load PA.
    # Original fuse initialization requires the bootloader to enable its clock.
    return struct.pack(
        "<17I",
        0xe59f3038, 0xe3a00080, 0xe5830000, 0xf57ff04f,
        0xe59f0018, 0xe59f1020, 0xe3a02000, 0xe59fe010,
        0xe59f3010, 0xe12fff13, 0xeafffffe, 0xeafffffe,
        SECURE_SIZE, 0x80008028, LOAD_ADDRESS, context_address,
        0x60006328,
    )


def qnx_boot_tags(memory_mib):
    """Keep the top 16 MiB outside the original QNX startup's normal RAM."""
    if not 256 <= memory_mib <= 2048:
        raise ValueError("Probe memory must be between 256 and 2048 MiB")
    normal_mib = memory_mib - SECURE_SIZE // (1024 * 1024)
    command = (
        "nvsku=699-61857-5017-202 SkuVer=00 "
        f"mem={normal_mib}M@2048M nr_cpus=1 lp=0"
    ).encode() + b"\0"
    command += bytes(-len(command) % 4)
    return (struct.pack(
        "<11I", 5, 0x54410001, 0, 0, 0,
        4, 0x54410002, normal_mib * 1024 * 1024, 0x80000000,
        2 + len(command) // 4, 0x54410009) + command + bytes(8))


def qnx_bootstrap(context_address, parameters_address, memory_mib=256):
    """Enter the unchanged TOS, then its actual nonsecure QNX return path."""
    tags = qnx_boot_tags(memory_mib)
    words, literals = [], []

    def load(register, value):
        literals.append((len(words), register, value))
        words.append(0)

    load(3, 0x60006328)
    words += [0xe3a00080, 0xe5830000, 0xf57ff04f]
    load(0, SECURE_SIZE)
    load(1, context_address)
    words.append(0xe3a02000)
    load(14, QNX_SHIM_ADDRESS + 40)
    load(3, 0x80000000 + memory_mib * 1024 * 1024 - SECURE_SIZE)
    words += [0xe12fff13, 0xe3e01000]
    # Restore the direct-QNX profile's legacy routing after TOS masks it.
    load(0, 0x60004024)
    words += [0xe3a02005, 0xe4801100, 0xe2522001, 0x1afffffc]
    load(0, 0x80000100)
    tags_source = len(literals)
    load(1, 0)
    load(2, len(tags) // 4)
    words += [0xe4913004, 0xe4803004, 0xe2522001, 0x1afffffb,
              0xe3a00000, 0xe3a01000]
    load(2, parameters_address)
    load(3, 0x80008000)
    words += [0xe12fff13, 0xeafffffe]
    tags_address = QNX_SHIM_ADDRESS + (len(words) + len(literals)) * 4
    index, register, _ = literals[tags_source]
    literals[tags_source] = (index, register, tags_address)
    for index, register, value in literals:
        words[index] = (0xe59f0000 | register << 12 |
                        (len(words) - index - 2) * 4)
        words.append(value)
    return struct.pack(f"<{len(words)}I", *words) + tags


def trusted_driver_command(command=None, nor_storage=False):
    """Use original executables with diagnostic RAM or explicit NOR storage."""
    from probe_mmx_pcie import PREREQUISITES

    setup = " &&\n".join(PREREQUISITES[:2])
    setup = setup.replace("; ", ";\n").replace(" && ", " &&\n")
    if nor_storage:
        setup += (
            " &&\n/sbin/devf-generic-tegra2 "
            "-s0x48000000,64M,0,0,0,2,1 -r -b5 -w0 -g -x 1 -e0 &&\n"
            "waitfor /dev/fs0p3 5 &&\n"
            "mount -o noatime,nosuid,noexec /dev/fs0p3 /mnt/ss &&\n"
            "echo MH2P_TLK_NOR_STORAGE_MOUNTED")
    else:
        setup += (
            " &&\n/sbin/devb-ram mem name=/memory/below4G "
            "blk memory=/memory/below4G,cache=0,vnode=512 "
            "ram capacity=8192 disk name=diagss &&\n"
            "waitfor /dev/diagss0t77 5 &&\n"
            "mount -t qnx4 /dev/diagss0t77 /mnt/ss")
    setup += (
        " &&\n"
        "( /sbin/devg-nvtlkdr /mnt/ss &\n"
        "waitfor /dev/tlkdr 5 &&\n"
        "echo MH2P_TLK_DRIVER_EVIDENCE_BEGIN &&\n"
        "pidin ar &&\npidin syspage=asinfo &&\n"
        "ls -ld /dev/tlkdr /mnt/ss &&\n"
        "echo MH2P_TLK_DRIVER_EVIDENCE_END )")
    if command is not None:
        setup += " &&\n(\n" + command + "\n)"
    return "(\n" + setup + "\n)"


def verify_trusted_driver(console, load_address, nor_storage=False):
    text = console.decode("utf-8", errors="replace").replace("\r", "")
    if nor_storage and not re.search(
            r"(?m)^MH2P_TLK_NOR_STORAGE_MOUNTED$", text):
        raise ValueError("Missing successful original NOR secure-storage mount")
    if text.count("ifs image checksum is valid") != 6:
        raise ValueError("Expected six checksum-valid original stage-two volumes")
    evidence = re.search(
        r"(?ms)^MH2P_TLK_DRIVER_EVIDENCE_BEGIN\n(.*?)"
        r"^MH2P_TLK_DRIVER_EVIDENCE_END$", text)
    if evidence is None:
        raise ValueError("Missing complete trusted-driver evidence")
    text = evidence[1]
    if not re.search(r"(?m)^\s*\d+ /sbin/devg-nvtlkdr /mnt/ss$", text):
        raise ValueError("Original trusted-driver process is not running")
    if not re.search(r"(?m)^crw-rw----\s+.* /dev/tlkdr$", text):
        raise ValueError("Original trusted-driver character device is absent")
    ranges = []
    for first, last, name in re.findall(
            r"(?m)^\s*[0-9a-f]{4}\) ([0-9a-f]+)-([0-9a-f]+)"
            r".* n:(\S+)$", text):
        if name.endswith(("/ram", "/sysram")):
            low, high = int(first, 16), int(last, 16)
            if not 0x80000000 <= low <= high < load_address:
                raise ValueError("QNX normal RAM overlaps or exceeds trusted DRAM")
            ranges.append((name, low, high))
    for suffix in ("/ram", "/sysram"):
        if not any(name.endswith(suffix) and high == load_address - 1
                   for name, _, high in ranges):
            raise ValueError(f"Missing QNX allocator boundary for {suffix}")
    return {
        "original_driver_running": True,
        "driver_character_device_observed": True,
        "normal_ram_exclusion_verified": True,
        "normal_ram_ranges_checked": len(ranges),
        "storage_is_empty_ephemeral_fixture": not nor_storage,
        "original_nor_storage_mount_observed": nor_storage,
        "functional_trusted_client_verified": False,
    }


def run(qemu, image, output, seconds, config=None, warmboot=b"",
        fuse_security_mode="unknown", boot_slot14_file=None, boot_slot15_file=None,
        normal_os_startup=None, normal_os_dtb=None, normal_os_command=None,
        normal_os_mnand=None, memory_mib=256, normal_os_driver_probe=False,
        normal_os_nor=None):
    if not 256 <= memory_mib <= 2048:
        raise ValueError("Probe memory must be between 256 and 2048 MiB")
    if not 0 < seconds <= 60:
        raise ValueError("Probe duration must be greater than zero and at most 60")
    if fuse_security_mode not in ("unknown", "production", "development"):
        raise ValueError("Invalid experimental fuse security mode")
    if warmboot and config is None:
        raise ValueError("TZRAM boot data requires a boot configuration")
    if normal_os_dtb is not None and normal_os_startup is None:
        raise ValueError("Normal-OS DTB requires a startup image")
    if normal_os_command is not None and normal_os_startup is None:
        raise ValueError("Normal-OS command requires a startup image")
    if normal_os_mnand is not None and normal_os_startup is None:
        raise ValueError("Normal-OS mNAND requires a startup image")
    if normal_os_nor is not None:
        if normal_os_startup is None:
            raise ValueError("Normal-OS NOR requires a startup image")
        if normal_os_nor.stat().st_size != 64 * 1024 * 1024:
            raise ValueError("Normal-OS NOR must be a complete 64 MiB image")
    if normal_os_driver_probe:
        if normal_os_startup is None or normal_os_mnand is None or memory_mib < 512:
            raise ValueError("Trusted-driver probe requires startup, mNAND and "
                             "at least 512 MiB RAM")
        normal_os_command = trusted_driver_command(
            normal_os_command, nor_storage=normal_os_nor is not None)
    if normal_os_startup is not None and not (
            0 < normal_os_startup.stat().st_size <= QNX_SHIM_ADDRESS - 0x80008000):
        raise ValueError("Normal-OS startup must fit below the trusted-entry shim")
    image_data = image.read_bytes()
    payload = trusted_payload(image_data)
    load_address = (0x80000000 + memory_mib * 1024 * 1024 - SECURE_SIZE
                    if normal_os_startup is not None else LOAD_ADDRESS)
    context = boot_context(config, warmboot) if config is not None else None
    context_address = (QNX_CONTEXT_ADDRESS if normal_os_startup is not None
                       else CONTEXT_ADDRESS)
    boot_keys = {slot: path for slot, path in (
        (14, boot_slot14_file), (15, boot_slot15_file)) if path is not None}
    sensitive_boot_context = bool(boot_keys or warmboot or normal_os_nor is not None)
    output.mkdir(parents=True, exist_ok=False,
                 mode=0o700 if sensitive_boot_context else 0o777)
    result = {
        "image_sha256": hashlib.sha256(image_data).hexdigest(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "payload_bytes": len(payload),
        "load_address": hex(load_address),
        "secure_region_bytes": SECURE_SIZE,
        "original_firmware_changed": False,
        "signature_authenticated": False,
        "full_boot_context_supplied": False,
        "probe_boot_context_supplied": context is not None,
        "warmboot_bytes": len(warmboot),  # Legacy result field.
        "tzram_data_bytes": len(warmboot),
        "bootloader_clocks_enabled": ["FUSE"],
        "trusted_services_ready": False,
        "experimental_fuse_security_mode": fuse_security_mode,
        "fuse_security_mode_hardware_verified": False,
        "boot_key_slots_supplied": list(boot_keys),
        "boot_key_material_hardware_verified": False,
        "sensitive_cpu_snapshots_omitted": sensitive_boot_context,
        "normal_os_startup_supplied": normal_os_startup is not None,
        "normal_os_command_requested": normal_os_command is not None,
        "normal_os_legacy_irq_handoff": normal_os_startup is not None,
        "normal_os_mnand_snapshot": normal_os_mnand is not None,
        "normal_os_driver_probe_requested": normal_os_driver_probe,
        "normal_os_nor_supplied": normal_os_nor is not None,
        "normal_os_secure_dram_excluded_from_boot_tags": normal_os_startup is not None,
        "memory_mib": memory_mib,
    }
    with tempfile.TemporaryDirectory(prefix="mh2p-tlk-probe-") as directory:
        temporary = Path(directory)
        kernel = temporary / "entry.bin"
        if normal_os_startup is not None:
            kernel.write_bytes(qnx_bootstrap(
                context_address if context is not None else 0,
                0x82000000 if normal_os_dtb is not None else 0x80000100,
                memory_mib))
            result["normal_os_startup_sha256"] = hashlib.sha256(
                normal_os_startup.read_bytes()).hexdigest()
        else:
            kernel.write_bytes(bootstrap(context_address if context is not None else 0))
        tos = temporary / "tos.bin"
        tos.write_bytes(payload)
        args = [
            "-M", "mh2p,experimental-el3=on,"
            f"experimental-fuse-security-mode={fuse_security_mode}",
            "-m", f"{memory_mib}M",
            "-kernel", str(normal_os_startup or kernel),
            "-S", "-display", "none", "-nic", "none",
            "-serial", f"file:{output / 'uart0.log'}",
            "-accel", "tcg,thread=single",
            "-device", f"loader,file={tos},addr={load_address:#x},force-raw=on",
            "-d", "unimp,guest_errors" + ("" if sensitive_boot_context else ",int"),
            "-D", str(output / "hardware.log"),
        ]
        if normal_os_startup is not None:
            args += [
                "-chardev",
                f"socket,id=qnx-console,path={temporary / 'uart1'},"
                "server=on,wait=off",
                "-serial", "chardev:qnx-console",
                "-device",
                f"loader,file={kernel},addr={QNX_SHIM_ADDRESS:#x},"
                "cpu-num=0,force-raw=on",
            ]
        else:
            args += ["-serial", "null"]
        if normal_os_dtb is not None:
            args += ["-dtb", str(normal_os_dtb)]
            result["normal_os_dtb_sha256"] = hashlib.sha256(
                normal_os_dtb.read_bytes()).hexdigest()
        if normal_os_mnand is not None:
            args += [
                "-drive", "if=sd,index=0,format=raw,snapshot=on,file=" +
                str(normal_os_mnand.resolve()).replace(",", ",,"),
            ]
        if normal_os_nor is not None:
            args += ["-bios", str(normal_os_nor.resolve())]
            result["normal_os_nor_sha256"] = hashlib.sha256(
                normal_os_nor.read_bytes()).hexdigest()
        for slot, path in boot_keys.items():
            args += [
                "-object",
                json.dumps({"qom-type": "secret", "id": f"bootkey{slot}",
                            "file": str(Path(path).resolve())}),
                "-global",
                f"tegra124-se.boot-slot{slot}-secret=bootkey{slot}",
            ]
        if context is not None:
            parameters = temporary / "parameters.bin"
            parameters.write_bytes(context)
            args += [
                "-device",
                f"loader,file={parameters},addr={context_address:#x},force-raw=on",
            ]
            result["config_sha256"] = hashlib.sha256(config).hexdigest()
        with ExitStack() as stack:
            vm = stack.enter_context(QEMUMachine(str(qemu), args=args))
            vm.launch()
            if normal_os_startup is not None:
                console = stack.enter_context(socket.socket(socket.AF_UNIX))
                console.connect(str(temporary / "uart1"))
            vm.cmd("cont")
            if normal_os_startup is None:
                time.sleep(seconds)
            else:
                captured = bytearray()
                sent = False
                deadline = time.monotonic() + seconds
                with (output / "uart1.log").open("wb") as log:
                    while time.monotonic() < deadline:
                        if not select.select(
                                [console], [], [],
                                max(0, deadline - time.monotonic()))[0]:
                            break
                        chunk = console.recv(65536)
                        if not chunk:
                            raise RuntimeError("QNX console closed during trusted probe")
                        captured.extend(chunk)
                        log.write(chunk)
                        if normal_os_command is not None and not sent and (
                                b"mh2p# " in captured):
                            console.sendall(
                                normal_os_command.encode() +
                                b"; echo MH2P_TLK_COMMAND_RC:$?\n")
                            sent = True
                        status = re.search(
                            rb"(?m)^MH2P_TLK_COMMAND_RC:(\d+)\r?$", captured)
                        if sent and status:
                            result["normal_os_command_exit_status"] = int(status[1])
                            break
                result["normal_os_shell_observed"] = b"mh2p# " in captured
                if normal_os_command is not None:
                    result["normal_os_command_completed"] = (
                        "normal_os_command_exit_status" in result)
                    if not result["normal_os_command_completed"]:
                        result["normal_os_command_error"] = (
                            "Command timed out" if sent else "QNX shell was not reached")
                if normal_os_driver_probe:
                    try:
                        result["normal_os_driver_probe"] = verify_trusted_driver(
                            captured, load_address,
                            nor_storage=normal_os_nor is not None)
                    except ValueError as error:
                        result["normal_os_driver_probe_error"] = str(error)
            vm.cmd("stop")
            registers = vm.cmd(
                "human-monitor-command", command_line="info registers")
            result["cpu_pc"] = "0x" + re.search(
                r"R15=([0-9a-fA-F]+)", registers).group(1).lower()
            result["cpu_status"] = registers.split("PSR=", 1)[1].splitlines()[0]
            result["nonsecure_execution_observed"] = " NS " in result["cpu_status"]
            result["trusted_entry_return_observed"] = (
                result["nonsecure_execution_observed"] and
                ((normal_os_startup is None and
                  result["cpu_pc"] == "0x80008028") or
                 result.get("normal_os_shell_observed", False)))
            result["mc_security_config"] = vm.cmd(
                "human-monitor-command", command_line="xp /2wx 0x70019070")
            if not sensitive_boot_context:
                result["registers"] = registers
                result["pc_bytes"] = vm.cmd(
                    "human-monitor-command", command_line="x /32bx $pc")
                result["stack_words"] = vm.cmd(
                    "human-monitor-command", command_line="x /128wx $sp")
        (output / "qemu.log").write_text(vm.get_log())
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=0.5)
    parser.add_argument("--memory-mib", type=int, default=256)
    parser.add_argument("--boot-config", type=Path,
                        help="ASCII boot configuration; no identity or keys invented")
    parser.add_argument("--tzram-data", "--warmboot", dest="tzram_data", type=Path,
                        help="Raw boot data copied to TZRAM; requires --boot-config. "
                             "--warmboot is a legacy alias, not a content type.")
    parser.add_argument("--normal-os-startup", type=Path,
                        help="Optional unchanged QNX startup to enter after TOS returns")
    parser.add_argument("--normal-os-dtb", type=Path,
                        help="QNX DTB; requires --normal-os-startup")
    parser.add_argument("--normal-os-command",
                        help="One explicit shell command after diagnostic QNX boots; "
                             "output may be sensitive, requires --normal-os-startup")
    parser.add_argument("--normal-os-mnand", type=Path,
                        help="Read original QNX volumes using snapshot-on mNAND")
    parser.add_argument("--normal-os-driver-probe", action="store_true",
                        help="Load original stage two, start original trusted driver "
                             "with empty RAM storage unless --normal-os-nor is given, "
                             "verify node and RAM map; "
                             "requires startup, mNAND and at least 512 MiB RAM")
    parser.add_argument("--normal-os-nor", type=Path,
                        help="private full 64 MiB NOR copy; use original NOR "
                             "storage for --normal-os-driver-probe, or inspect "
                             "with --normal-os-command; requires startup; "
                             "CPU dumps omitted")
    parser.add_argument("--experimental-fuse-security-mode",
                        choices=("unknown", "production", "development"),
                        default="unknown",
                        help="Explicit unverified hardware input; not credentials")
    for slot in (14, 15):
        parser.add_argument(
            f"--boot-slot{slot}-file", type=Path,
            help="Explicit raw boot-slot state: 16-byte key or 64-byte key/IVs. "
                 "Firmware UART diagnostics may still contain sensitive data.")
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be greater than zero and at most 60")
    if not 256 <= args.memory_mib <= 2048:
        parser.error("--memory-mib must be between 256 and 2048")
    if args.tzram_data and not args.boot_config:
        parser.error("--tzram-data requires --boot-config")
    if args.normal_os_dtb and not args.normal_os_startup:
        parser.error("--normal-os-dtb requires --normal-os-startup")
    if args.normal_os_command and not args.normal_os_startup:
        parser.error("--normal-os-command requires --normal-os-startup")
    if args.normal_os_mnand and not args.normal_os_startup:
        parser.error("--normal-os-mnand requires --normal-os-startup")
    config = args.boot_config.read_bytes() if args.boot_config else None
    warmboot = args.tzram_data.read_bytes() if args.tzram_data else b""
    result = run(args.qemu.resolve(), args.image, args.output.resolve(),
                 args.seconds, config, warmboot,
                 args.experimental_fuse_security_mode,
                 args.boot_slot14_file, args.boot_slot15_file,
                 args.normal_os_startup, args.normal_os_dtb, args.normal_os_command,
                 args.normal_os_mnand, args.memory_mib, args.normal_os_driver_probe,
                 args.normal_os_nor)
    print(json.dumps(result, indent=2))
    if (args.normal_os_command is not None or args.normal_os_driver_probe) and (
            not result["normal_os_command_completed"] or
            result["normal_os_command_exit_status"] != 0 or
            "normal_os_driver_probe_error" in result):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
