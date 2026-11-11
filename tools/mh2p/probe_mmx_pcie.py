#!/usr/bin/env python3
"""Verify original MMX PCIe setup and no-peer retries, not a connected RCC."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import uuid

from check_boot import verify_runtime_services
from check_guest_graphics import wait_console
from prepare_mnand import digest
from probe_rcc import QEMUMachine, ROOT
from probe_rcc_pcie import qtest_command
from qemu.machine.qtest import QEMUQtestProtocol


PREREQUISITES = (
    "devg-nvsku && waitfor /dev/nvsku/sku && "
    "devg-nvos && waitfor /dev/nvos && devg-nvgpio && waitfor /dev/nvgpio",
    "devb-sdmmc-tegra3 blk cache=10m "
    "sdio addr=0x700b0600,irq=63,emmc,bw=8,clk=52000000,timing=hs disk name=mnanda; "
    "waitfor /dev/mnanda0t177.1 10 && "
    "mount -tqnx6 -oro /dev/mnanda0t177.1 /mnt/app && "
    "res_init -d 0,31 && ifsloader -s 32505856 -a 18087936 "
    "-r 0x92000000,0x0B400000 -m / -v -n0 "
    "-d /mnt/app/img_restore/main_stage2.ifs.lzo",
    "nvmemctrl && devg-nvclock && waitfor /dev/nvclock 5 && "
    "devg-nvpowergate && waitfor /dev/nvpowergate 5 && powergate_init && clock_init && "
    "devg-nvsys && waitfor /dev/nvsys/tegra_chip_id 5 && "
    "devg-nvrm && waitfor /dev/nvmap 5 && waitfor /dev/nvhost-ctrl 5",
)
CONTROLLER_ENABLED = b"PCIE: Controller Enabled"
CHECKING_LINK = b"PCIE: checking link 0: 1000, 110"
LINK_RETRY = b"PCIE: PCIE: port 0: link down, retrying"
BANNER = b"MH2P diagnostic shell: normal services are NOT started."
REGISTERS = {
    "pex_dpd_request": (0x7000e5b8, 0x40),
    "pex_dpd_status": (0x7000e5bc, 0),
    "pad_pll": (0x7009f040, 0x00080002),
    "pad_pll_reference": (0x7009f044, 0x70),
    "pcie_lane_mux": (0x7009f134, 0x38),
    "pad_clamps_released": (0x7009f01c, 0),
    "mselect_config": (0x50060000, 0x20),
    "reference_pads": (0x010030c8, 0xd7acd8ac),
    "reference_bias": (0x010030d0, 0x30),
    "fpci_enabled": (0x010038ac, 1),
    "legacy_interrupt_mask": (0x010038b4, 1),
    "sideband_interrupt_enables": (0x010038c4, 0xff),
    "afi_interrupt_enables": (0x010038c8, 0x17f),
    "lane_crossbar": (0x010038f8, 0),
    "gen2_disable_fuse": (0x01003904, 0),
    # Original retries pulse reset low for 2 ms; either sampled phase is valid.
    "port0_control": (0x01003910, (0x1a, 0x1b)),
    "port1_control": (0x01003918, 0x1b),
    "pads_plle_control": (0x01003960, 2),
    "root1_device_control": (0x01001088, 0xf),
    "root1_target_speed": (0x010010b0, 2),
    "root1_presence_override": (0x01001fe0, 0xe),
    "root0_data_link_down": (0x01000f00, 0),
    "root1_data_link_down": (0x01001f00, 0),
    "root0_link_down": (0x01000090, 0),
    "root1_link_down": (0x01001090, 0),
}
SERVER_COMMAND = (
    "PCI_HW_MODULE=pci_hw-nv_tk1-vcm30-mib.so "
    "PCI_DEBUG_MODULE=pci_debug.so PCI_SLOG_MODULE=pci_slog.so "
    "PCI_BASE_VERBOSITY=5 PCI_BKWD_COMPAT_MODULE=pci_bkwd_compat.so "
    "/sbin/pci-server -v -c --bus-scan-limit=3"
)
SERVER_REGISTERS = {
    **REGISTERS,
    "sideband_interrupt_enables": (0x010038c4, 0),
    "afi_interrupt_enables": (0x010038c8, 0),
    "root0_identity": (0x01000000, 0x0e1210de),
    "root1_identity": (0x01001000, 0x0e1310de),
    "root0_class_revision": (0x01000008, 0x060400a1),
    "root1_class_revision": (0x01001008, 0x060400a1),
    **{f"msi_pending_{bank}": (0x0100386c + bank * 4, 0)
       for bank in range(8)},
}


def execute(console, captured, output, command, timeout, background=False):
    token = ("MMX_PCIE_" + uuid.uuid4().hex).encode()
    start = len(captured)
    separator = b" & echo " if background else b"; echo "
    console.sendall(command.encode() + separator + token + b":$?\n")
    match = wait_console(console, captured, output,
                         token + rb":(\d+)\r?\n", start, timeout)
    if match[1] != b"0":
        raise RuntimeError(f"Original MMX command failed ({match[1]!r}): {command}")
    return bytes(captured[start:])


def verify_native_setup(log, processes):
    normalized = log.replace(b"\r", b"")
    stages = (CONTROLLER_ENABLED, CHECKING_LINK, LINK_RETRY)
    positions = [
        re.search(rb"(?m)^(?:mh2p# )?" + re.escape(stage) + rb"\n", normalized)
        for stage in stages
    ]
    if (any(match is None for match in positions) or
        any(left.start() >= right.start()
            for left, right in zip(positions, positions[1:]))):
        raise RuntimeError("Missing ordered original MMX controller/link-retry evidence")
    if b"SIGBUS" in normalized or re.search(rb"PCIE: port \d+ up", normalized):
        raise RuntimeError("Unexpected MMX fault or link-up without an RCC peer")
    verify_runtime_services(processes)
    if not re.search(
        rb"(?m)^[ \t]*\d+[ \t]+/sbin/pcie_init -v -p0 -r25[ \t]*\r?$",
        processes,
    ):
        raise RuntimeError("Original MMX initializer is not alive in its retry loop")


def verify_media(media):
    manifest = json.loads((media / "manifest.json").read_text())
    if not manifest["diagnostic_shell"]:
        raise ValueError("This isolated initializer probe requires diagnostic-shell media")
    for name, field in (("startup.bin", "startup_sha256"),
                        ("board.dtb", "dtb_sha256")):
        if hashlib.sha256((media / name).read_bytes()).hexdigest() != manifest[field]:
            raise ValueError(f"Media hash mismatch: {name}")
    return manifest


def verify_native_server(slog, processes, device, *, endpoint=False):
    normalized = slog.replace(b"\r", b"")
    stages = (
        b"+ PCI Server v1.6 init start +",
        b"PCIe 0 link is not active",
        b"tk1_pcie_init() [PCIe 0] end, OK [PCI_ERR_OK]",
        (b"++++ tk1_pcie_init() [PCIe 1] start ++++" if endpoint else
         b"PCIe 1 link is not active"),
        b"tk1_pcie_init() [PCIe 1] end, OK [PCI_ERR_OK]",
        b"Found device B0:D0:F0",
        b"Found device B2:D0:F0",
        b"enumeration and configuration complete, status OK [PCI_ERR_OK]",
        b"+ PCI Server v1.6 init complete, OK [PCI_ERR_OK] +",
    )
    position = 0
    for stage in stages:
        match = re.search(
            rb"(?m)^Jan 01 [^\n]*\]: [^\n]*" + re.escape(stage) + rb"[^\n]*\n",
            normalized[position:],
        )
        if match is None:
            raise RuntimeError(
                f"Missing ordered original PCI server evidence: {stage!r}")
        position += match.end()
    topology = [b"B0:D0:F0", b"B2:D0:F0"]
    if endpoint:
        topology.append(b"B3:D0:F0")
    if re.findall(rb"Found device (B\d+:D\d+:F\d+)", normalized) != topology:
        raise RuntimeError(
            f"Unexpected PCI topology: expected {topology!r}")
    if endpoint:
        for bar, base, length in ((0, 0x10000, 0x4000),
                                  (1, 0x14000, 0x1000), (2, 0, 0x10000)):
            marker = (f"Setting MEM BAR {bar} for B3:D0:F0 to "
                      f"{base:x} ({length:x})").encode()
            if marker not in normalized:
                raise RuntimeError(
                    f"Missing original BAR assignment {marker!r}")
    if not re.search(
        rb"(?m)^[ \t]*\d+[ \t]+/sbin/pci-server "
        rb"-v -c --bus-scan-limit=3[ \t]*\r?$",
        processes,
    ):
        raise RuntimeError("Original PCI server is not alive")
    if not re.search(rb"(?m)^d[^\r\n]*[ \t]/dev/pci\r?$", device):
        raise RuntimeError(
            "Original PCI resource manager did not expose /dev/pci")


def read_registers(qtest, pci_server=False, *, link=False):
    registers = {}
    for name, (address, expected) in (
        SERVER_REGISTERS if pci_server else REGISTERS
    ).items():
        if link and address in (0x01001f00, 0x01001090):
            expected = 0x40000000 if address == 0x01001f00 else 0x20110000
            name = name.replace("_down", "_up")
        value = int(qtest_command(qtest, f"readl 0x{address:x}").split()[1], 0)
        accepted = expected if isinstance(expected, tuple) else (expected,)
        if value not in accepted:
            raise RuntimeError(
                f"{name} readback {value:#x} not in {accepted!r}")
        registers[name] = hex(value)
    return registers


def read_config_apertures(qtest):
    apertures = []
    windows = [
        (0x3800 + i * 4, 0x3818 + i * 4, 0x3830 + i * 4)
        for i in range(6)
    ] + [
        (0x3934 + i * 4, 0x3940 + i * 4, 0x394c + i * 4)
        for i in range(3)
    ]
    for index, offsets in enumerate(windows):
        pages, base, target = (
            int(qtest_command(qtest, f"readl 0x{0x01000000 + off:x}")
                .split()[1], 0)
            for off in offsets
        )
        if not pages or target & 1:
            continue
        fpci = target << 8
        if not 0xfdfe000000 <= fpci < 0xfe20000000:
            continue
        length = pages << 12
        if not 0x01004000 <= base < base + length <= 0x40000000:
            raise RuntimeError(
                "Native PCI configuration window outside aperture")
        reads = {}
        for operation, offset, expected in (
            ("readb", 0, 0xff), ("readw", 2, 0xffff),
            ("readl", 4, 0xffffffff), ("readl", length - 4, 0xffffffff),
        ):
            address = base + offset
            value = int(qtest_command(
                qtest, f"{operation} 0x{address:x}").split()[1], 0)
            if value != expected:
                raise RuntimeError(
                    f"Native config aperture {index} absent read: "
                    f"{value:#x} != {expected:#x}")
            reads[f"{operation}_{address:#x}"] = hex(value)
        apertures.append({
            "window": index, "axi_base": hex(base),
            "fpci_base": hex(fpci), "size_bytes": length,
            "diagnostic_absent_reads": reads,
        })
    if not apertures:
        raise RuntimeError("Original PCI server programmed no config aperture")
    return apertures


def probe(qemu, media, nor, mnand, output, timeout=30, *, pci_server=False):
    if not 0 < timeout <= 60:
        raise ValueError("Console timeout must be between 0 and 60 seconds")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    manifest = verify_media(media)
    source_hashes = {"nor": digest(nor), "mnand": digest(mnand)}
    output = output.resolve()
    output.mkdir(parents=True)
    boots = []
    with tempfile.TemporaryDirectory(prefix="mh2p-mmx-pcie-") as directory:
        path = str(Path(directory) / "qtest.sock")
        qtest = QEMUQtestProtocol(path, server=True)
        listener = qtest._sock
        args = [
            "-M", "mh2p", "-S", "-kernel", str((media / "startup.bin").resolve()),
            "-dtb", str((media / "board.dtb").resolve()), "-bios", str(nor.resolve()),
            "-drive", "if=sd,index=0,format=raw,snapshot=on,file=" +
            str(mnand.resolve()).replace(",", ",,"),
            "-display", "none", "-nic", "none",
            "-icount", "shift=0,align=off,sleep=on",
            "-qtest", f"unix:{path}", "-qtest-log", "/dev/null",
            "-d", "unimp,guest_errors", "-D", str(output / "unimplemented.log"),
        ]
        try:
            qtest.settimeout(5)
            with QEMUMachine(str(qemu.resolve()), args=args) as vm:
                vm.set_console(console_index=1)
                vm.launch()
                command = list(vm._qemu_full_args)
                vm._qmp.settimeout(5)
                try:
                    qtest.accept()
                    qtest.settimeout(5)
                    for boot in range(2):
                        if boot:
                            vm.cmd("system_reset")
                        captured = bytearray()
                        vm.cmd("cont")
                        with (output / f"boot-{boot}.log").open("wb") as log:
                            banner = wait_console(
                                vm.console_socket, captured, log,
                                re.escape(BANNER), 0, timeout)
                            wait_console(vm.console_socket, captured, log,
                                         rb"mh2p# ", banner.end(), timeout)
                            for prerequisite in PREREQUISITES:
                                execute(vm.console_socket, captured, log,
                                        prerequisite, timeout)
                            if captured.count(b"ifs image checksum is valid") != 6:
                                raise RuntimeError("Expected six verified original stage-two volumes")
                            if pci_server:
                                execute(vm.console_socket, captured, log,
                                        "slogger -s 256 && waitfor /dev/slog 5",
                                        timeout)
                            start = len(captured)
                            execute(vm.console_socket, captured, log,
                                    "/sbin/pcie_init -v -p0 -r25", timeout,
                                    background=True)
                            wait_console(vm.console_socket, captured, log,
                                         re.escape(LINK_RETRY) + rb"\r?\n",
                                         start, timeout)
                            processes = execute(vm.console_socket, captured, log,
                                                "pidin ar", timeout)
                            verify_native_setup(bytes(captured[start:]), processes)
                            if pci_server:
                                execute(vm.console_socket, captured, log,
                                        SERVER_COMMAND, timeout,
                                        background=True)
                                execute(vm.console_socket, captured, log,
                                        "waitfor /dev/pci 50", 90)
                                processes = execute(
                                    vm.console_socket, captured, log,
                                    "pidin ar", timeout)
                                device = execute(
                                    vm.console_socket, captured, log,
                                    "ls -ld /dev/pci", timeout)
                                slog = execute(vm.console_socket, captured, log,
                                               "sloginfo", timeout)
                                verify_native_server(slog, processes, device)
                                verify_native_setup(
                                    bytes(captured[start:]), processes)
                            vm.cmd("stop")
                            registers = read_registers(qtest, pci_server)
                            boots.append({
                                "native_controller_enabled": True,
                                "original_initializer_alive_retrying": True,
                                "native_pci_server_initialized": pci_server,
                                "registers": registers,
                                "config_apertures": (
                                    read_config_apertures(qtest)
                                    if pci_server else []
                                ),
                            })
                except TimeoutError:
                    print("MMX PCIe probe timed out; terminating the probe VM",
                          file=sys.stderr)
                    vm.kill()
                    raise
        finally:
            qtest.close()
            listener.close()
    if pci_server and (output / "unimplemented.log").stat().st_size:
        raise RuntimeError(
            "Unexpected unsupported hardware access; inspect unimplemented.log")
    result = {
        "command": command, "source_hashes": source_hashes,
        "startup_sha256": manifest["startup_sha256"],
        "dtb_sha256": manifest["dtb_sha256"],
        "diagnostic_shell": True, "original_pcie_executable_unmodified": True,
        "native_controller_setup_verified_across_reset": True,
        "native_root_enumeration_verified_across_reset": pci_server,
        "boots": boots, "rcc_peer_connected": False, "mmx_link_verified": False,
        "endpoint_enumeration_verified": False,
        "note": "Isolated diagnostic, not normal complete startup. "
                "The optional "
                "original PCI server is started alongside the retrying initializer; "
                "normal startup waits for that initializer to exit first. "
                "No MHI2 synthetic endpoint, RCC clock/reset wiring, downstream transactions "
                "or link-up are supplied. The probe stops during native link retries; "
                "shutdown/PME completion is not verified.",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--nor", type=Path, required=True)
    parser.add_argument("--mnand", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--pci-server", action="store_true",
                        help="verify original PCI server and two local roots, "
                             "not an RCC endpoint")
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.media, args.nor, args.mnand,
                           args.output, args.timeout,
                           pci_server=args.pci_server), indent=2))


if __name__ == "__main__":
    main()
