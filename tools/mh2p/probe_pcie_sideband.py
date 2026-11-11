#!/usr/bin/env python3
"""Verify native MMX/RCC clock/reset, PCIe, networking and optional ASI."""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile
import time

from check_guest_graphics import wait_console
from prepare_mnand import digest
from native_persistence import verify_qdb_ready
from probe_mmx_pcie import (
    BANNER, LINK_RETRY, PREREQUISITES, execute, read_registers,
    SERVER_COMMAND, verify_media, verify_native_setup, verify_native_server,
)
from probe_rcc import QEMUMachine, ROOT, verify_image
from probe_rcc_pcie import check_endpoint, qtest_command
from qemu.machine.qtest import QEMUQtestProtocol


CROSSBOARD_COMMAND = (
    "PCI_HW_MODULE=pci_hw-nv_tk1-vcm30-mib.so "
    "PCI_BKWD_COMPAT_MODULE=pci_bkwd_compat.so /sbin/mmx-pcie-init-rcc2"
)
RCC_IPU1_SHA256 = (
    "3df0a92d7dc1ef69655536152d9086c963a7e715d033e6641cb61bc6f6243b8f")
CROSSBOARD_REGISTERS = {
    0x2c: 0xdeadbeef, 0x900: 0, 0x904: 0, 0x908: 0x80000000,
    0x90c: 0x08000000, 0x910: 0, 0x914: 0x09ffffff,
    0x918: 0x90000000, 0x91c: 0,
}
MMX_NETWORK_COMMANDS = (
    "random -U 103:103 -pt -i2 && "
    "waitfor /dev/urandom 10 && ln -sP /dev/shmem /var/run",
    "PCI_HW_MODULE=pci_hw-nv_tk1-vcm30-mib.so "
    "PCI_BKWD_COMPAT_MODULE=pci_bkwd_compat.so "
    "/sbin/io-pkt-v6-hc -S -ptcpip threads_max=1000,enmap=0,"
    "stacksize=16384,stackguard,pkt_typed_mem=/ram/dma,random -ppf-v6",
    "waitfor /dev/socket 5",
    "/bin/mount -Tio-pkt /lib/dll/devnp-mib-mmx.so",
    "ifconfig mmx0 10.0.0.15/24",
)
RCC_DEFAULTS_NOR_SHA256 = (
    "06aa9606c95e7679eea86cc428cb338f8e285631613db18c10b216faf6549273")
IPL_SPI_CLOCKS = (0x4a0097f0, 0x4a0097f8, 0x4a009800, 0x4a009808)
RCC_GPIO6_DATAIN = 0x4805d138
HID_STARTUP_JOBS = [
    {"cmd": "init", "app": "hidcontroller", "apprunmode": 0,
     "ps_access": "PS_ACC_READ_WRITE", "timeout": 20000, "blocking": False},
    {"cmd": "run", "app": "hidcontroller", "timeout": 20000, "blocking": False},
]


def connect_native_uart(mmx, rcc, directory, output, input_peer=False):
    """Replace unused UART0 backends while both original CPUs are stopped.

    With input_peer, the MMX UARTA is served by QEMU's mh2p-rstp chardev
    (RCC side of RSTP carrying host pointer/keyboard keypanel input), and
    the RCC UART0 stays disconnected.
    """
    path = str(directory / "native-uart.sock")
    for name, vm in (("mmx", mmx), ("rcc", rcc)):
        devices = vm.cmd("query-chardev")
        if not any(device["label"] == "serial0" and
                   device["filename"] == "null" for device in devices):
            raise RuntimeError(f"Expected unused {name} UART0 null backend")
    if input_peer:
        mmx.cmd("chardev-change", id="serial0", backend={
            "type": "mh2p-rstp",
            "data": {"logfile": str(output / "mmx-uart-wire.bin")}})
        (output / "rcc-uart-wire.bin").touch()
        if not any(device["label"] == "serial0" and device["frontend-open"]
                   for device in mmx.cmd("query-chardev")):
            raise RuntimeError("Cannot connect MMX UART0 RSTP input peer")
        return
    for name, vm in (("mmx", mmx), ("rcc", rcc)):
        options = {
            "addr": {"type": "unix", "data": {"path": path}},
            "server": name == "mmx",
            "logfile": str(output / f"{name}-uart-wire.bin"),
        }
        if name == "mmx":
            options["wait"] = False
        vm.cmd(
            "chardev-change", id="serial0",
            backend={"type": "socket", "data": options})
    for name, vm in (("mmx", mmx), ("rcc", rcc)):
        devices = vm.cmd("query-chardev")
        if not any(device["label"] == "serial0" and
                   path in device["filename"] and device["frontend-open"]
                   for device in devices):
            raise RuntimeError(f"Cannot connect original {name} UART0 backend")
        if not (output / f"{name}-uart-wire.bin").is_file():
            raise RuntimeError(f"Missing original {name} UART capture")


def observe_native_rstp(mmx, rcc, captured, log, timeout, output, boot,
                        offsets):
    """Observe original OOC/RSTP, without promoting byte counts to readiness."""
    paths = {name: output / f"{name}-uart-wire.bin"
             for name in ("mmx", "rcc")}
    mmx.cmd("cont")
    rcc.cmd("cont")
    try:
        response = execute(
            mmx.console_socket, captured, log,
            "LP_GPIO_ST=0 sh /usr/sbin/ooc-starter.sh "
            ">/tmp/ooc-starter.log 2>&1 & OOC_STARTER_PID=$!",
            max(timeout, 120))
        response += execute(
            mmx.console_socket, captured, log,
            "waitfor /dev/rstp/persistence 10", max(timeout, 30))
        response += execute(
            mmx.console_socket, captured, log,
            "LD_LIBRARY_PATH=/lib:/eso/lib:/usr/lib "
            "IPL_CONFIG_DIR=/etc/eso/production "
            "/usr/bin/rccpersistencebackend -r /dev/rstp/persistence "
            "-m /etc/persistence_mapping.json "
            ">/tmp/rccpersistencebackend.log 2>&1 & "
            "RCC_PERSISTENCE_BACKEND_PID=$!", max(timeout, 30))
        response += execute(
            mmx.console_socket, captured, log,
            "sleep 15; ls -l /dev/ser2 /dev/rstp; pidin ar; "
            "pidin -p$RCC_PERSISTENCE_BACKEND_PID; "
            "pidin -p$RCC_PERSISTENCE_BACKEND_PID fd; "
            "cat /tmp/ooc-starter.log; "
            "dd if=/tmp/rccpersistencebackend.log bs=4096 count=1", 600)
    finally:
        mmx.cmd("stop")
        rcc.cmd("stop")
    (output / f"rstp-boot-{boot}.log").write_bytes(response)
    counts = {}
    for name, path in paths.items():
        size = path.stat().st_size
        if size < offsets[name]:
            raise RuntimeError(f"Original {name} UART log shrank across reset")
        counts[name] = size - offsets[name]
    result = {
        "mmx_uart": "UARTA 0x70006000 /dev/ser2",
        "rcc_uart": "UART1 0x4806a000 original M4 /uart0",
        "transport": "UNIX chardev byte stream",
        "tx_file_offsets": offsets,
        "tx_bytes": counts,
        "bidirectional_serial_bytes_observed": all(counts.values()),
        "host_protocol_responder": False,
        "electrical_flow_control_or_baud_verified": False,
        "diagnostic_ooc_environment": {"LP_GPIO_ST": "0"},
        "original_rcc_persistence_backend_invoked": True,
        "persistence_transactions_verified": False,
        "rstp_handshake_verified": False,
        "full_hmi_or_touch_verified": False,
        "note": (
            "Original ooc-starter.sh and the main_stage2.3.sh RCC persistence "
            "backend command were invoked. Backend startup and descriptor "
            "output do not verify completed persistence transactions. "
            "Serial bytes and guest "
            "output are observations, not verification of startup, packet "
            "acceptance, RSTP handshake, power/runmode or touch readiness."),
    }
    (output / f"rstp-boot-{boot}.json").write_text(
        json.dumps(result, indent=2) + "\n")
    return result


def observe_native_hid(mmx, rcc, captured, log, timeout, output, boot):
    """Let the original manager launch HID using explicit diagnostic jobs."""
    def section(name):
        match = re.search(
            rb"(?ms)^" + name + rb"_BEGIN\r?\n(.*?)^" + name + rb"_END\r?$",
            response)
        if match is None:
            raise RuntimeError(f"Missing native HID evidence: {name.decode()}")
        return match[1]

    jobs = json.dumps(HID_STARTUP_JOBS, separators=(",", ":"))[1:-1]
    response = b""
    start = len(captured)
    mmx.cmd("cont")
    rcc.cmd("cont")
    try:
        response += execute(
            mmx.console_socket, captured, log,
            "cp /etc/eso/production/*.json /tmp/ && "
            "sed 's/\"startup\":[[:space:]]*\\[/\"startup\":["
            + jobs + "/' /etc/eso/production/servicemgr.json "
            ">/tmp/servicemgr.json && "
            "{ echo HID_ORIGINAL_CONFIG_BEGIN; "
            "cat /etc/eso/production/servicemgr.json && "
            "echo HID_ORIGINAL_CONFIG_END; "
            "echo HID_DIAGNOSTIC_CONFIG_BEGIN; "
            "cat /tmp/servicemgr.json && echo HID_DIAGNOSTIC_CONFIG_END; }",
            max(timeout, 120))
        original = json.loads(section(b"HID_ORIGINAL_CONFIG"))
        diagnostic = json.loads(section(b"HID_DIAGNOSTIC_CONFIG"))
        if (not isinstance(original, dict) or
                original.get("startup") != [] or
                diagnostic != dict(original, startup=HID_STARTUP_JOBS)):
            raise RuntimeError("Unexpected native HID manager configuration")
        response += execute(
            mmx.console_socket, captured, log,
            "cd /eso && { IPL_CONFIG_DIR=/tmp ./bin/servicemgrmibhigh "
            ">/tmp/hid-servicemgr.log 2>&1 & HID_SM_PID=$!; }",
            max(timeout, 120))
        response += execute(
            mmx.console_socket, captured, log,
            "sleep 30; kill -0 $HID_SM_PID; echo HID_MANAGER_ALIVE:$?; "
            "echo HID_MANAGER_PID:$HID_SM_PID; "
            "echo HID_PROCESS_INVENTORY_BEGIN; pidin ar; "
            "echo HID_PROCESS_INVENTORY_END; "
            "pidin -p hidcontroller; echo HID_DESCRIPTOR_INVENTORY_BEGIN; "
            "pidin -p hidcontroller fd; echo HID_DESCRIPTOR_INVENTORY_END; "
            "cat /tmp/hid-servicemgr.log; "
            "ls -l /dev/rstp/keypanel /dev/spi5; "
            "sloginfo; echo HID_GRAPHICS_CONFIGURATION; cat /etc/graphics.conf",
            600)
    finally:
        mmx.cmd("stop")
        rcc.cmd("stop")
        (output / f"hid-boot-{boot}.log").write_bytes(
            bytes(captured[start:]) or response)
    inventory = section(b"HID_PROCESS_INVENTORY")
    pids = re.findall(
        rb"(?m)^[ \t]*([1-9][0-9]*)[ \t]+/eso/bin/apps/hidcontroller"
        rb"(?:[ \t]+[^\r\n]*)?\r?$", inventory)
    manager_pid = re.search(rb"(?m)^HID_MANAGER_PID:([1-9][0-9]*)\r?$",
                            response)
    manager_alive = bool(
        manager_pid and
        re.search(rb"(?m)^HID_MANAGER_ALIVE:0\r?$", response) and
        re.search(rb"(?m)^[ \t]*" + manager_pid[1] +
                  rb"[ \t]+\./bin/servicemgrmibhigh\r?$", inventory))
    alive = len(pids) == 1
    descriptors = []
    fd_inventory = section(b"HID_DESCRIPTOR_INVENTORY")
    owner = None
    for line in fd_inventory.splitlines():
        fields = line.split()
        if (len(fields) == 2 and fields[0].isdigit() and
                not fields[1].isdigit()):
            owner = fields[0] if fields[1] == b"apps/hidcontroller" else None
        elif alive and owner == pids[0]:
            descriptor = re.fullmatch(
                rb"[ \t]*([0-9]+)[ \t]+([1-9][0-9]*)[ \t]+"
                rb"([r-][w-])[ \t]+[0-9]+[ \t]+/dev/rstp/keypanel[ \t]*",
                line)
            if descriptor:
                fd, server, mode = descriptor.groups()
                descriptors.append({
                    "fd": int(fd), "server_pid": int(server),
                    "mode": mode.decode()})
    open_message = b"Open RSTP device was successful" in response
    result = {
        "original_process": "/eso/bin/apps/hidcontroller",
        "original_service_manager": "/eso/bin/servicemgrmibhigh",
        "service_manager_process_alive_observed": manager_alive,
        "diagnostic_config_dir": "/tmp",
        "diagnostic_service_manager_startup": HID_STARTUP_JOBS,
        "other_service_manager_settings_preserved": True,
        "process_alive_observed": alive,
        "process_ids_observed": [int(pid) for pid in pids],
        "hid_touch_service_registration_observed":
            b"HidTouchMMXImpl: service registered" in response,
        "remote_keypanel_service_registration_observed":
            b"RemKeypanelMMXImpl: service registered" in response,
        "rstp_device_open_message_observed": open_message,
        "rstp_device_open_observed": open_message or bool(descriptors),
        "rstp_keypanel_descriptors_observed": descriptors,
        "rstp_keypanel_read_descriptor_observed":
            any(item["mode"][0] == "r" for item in descriptors),
        "rstp_keypanel_write_descriptor_observed":
            any(item["mode"][1] == "w" for item in descriptors),
        "rstp_device": "/dev/rstp/keypanel",
        "screen_input_none_observed":
            bool(re.search(rb"(?m)^\s*input\s*=\s*none\s*\r?$", response)),
        "host_input_packets_injected": False,
        "native_touch_delivery_verified": False,
        "full_hmi_or_touch_verified": False,
        "note": (
            "The unchanged original service manager launches HID through "
            "the original enforcer and supplies its own launch arguments. "
            "An explicit temporary configuration copies only HID's init/run "
            "jobs from the original DSI startup into the otherwise empty "
            "static startup list. This bypasses original DSI startup gating "
            "and does not complete the original service-manager graph. "
            "Process liveness, registration, device descriptors and logs are "
            "separate observations, not input-panel readiness, RSTP peer "
            "handshake, delivered touch events or HMI interaction."),
    }
    (output / f"hid-boot-{boot}.json").write_text(
        json.dumps(result, indent=2) + "\n")
    if not manager_alive or not alive:
        raise RuntimeError(
            "Original HID manager/controller is not alive or unique; "
            "see saved HID observation")
    return result


def diagnostic_ipl_spi_handoff(qtest):
    """Explicit diagnostic subset, not execution of the recovered bootloader."""
    for address in IPL_SPI_CLOCKS:
        qtest_command(qtest, f"writel {address:#x} 2")
        actual = int(qtest_command(qtest, f"readl {address:#x}").split()[1], 0)
        if actual != 2:
            raise RuntimeError(
                f"Diagnostic IPL SPI clock {address:#x}: expected 2, "
                f"observed {actual:#x}")


def stage_native_ipu_nor(nor, firmware, destination):
    """Transport the unchanged OTA ELF, not a preloaded remote CPU image."""
    raw = bytearray(nor.read_bytes())
    elf = firmware.read_bytes()
    offset = 2 * 1024 * 1024
    if len(raw) != 64 * 1024 * 1024 or len(elf) != offset:
        raise ValueError("Expected 64 MiB RCC NOR and 2 MiB padded OTA ELF")
    if raw[offset:offset * 2] != b"\xff" * offset:
        raise ValueError("Native IPU transport would overwrite non-erased NOR")
    raw[offset:offset * 2] = elf
    with destination.open("xb") as stream:
        stream.write(raw)


def start_native_ipu1(rcc, captured, log, timeout):
    execute(rcc.console_socket, captured, log,
            "dd if=/dev/fs0 of=/tmp/ipu1-original.xem4 "
            "bs=131072 skip=16 count=16", timeout)
    loaded = execute(
        rcc.console_socket, captured, log,
        "ipc IPU1 /tmp/ipu1-original.xem4; echo IPC_NATIVE_EXIT:$?; "
        "ls -l /dev/ipc /dev/ipc-trace/IPU1; sloginfo", timeout)
    if not re.search(rb"(?m)^IPC_NATIVE_EXIT:0\r?$", loaded):
        raise RuntimeError("Original native IPU1 loader did not exit zero")
    if b"RST1 & RST2 released!" not in loaded:
        raise RuntimeError(
            "Original native IPU1 loader did not release both cores")


def capture_native_ipu1(rcc, qtest, output, boot, state):
    trace = output / f"ipu1-trace-boot-{boot}.bin"
    rcc.cmd("human-monitor-command",
            command_line=f"memsave 0x80800000 0x8000 "
            f"{json.dumps(str(trace))}", **{"cpu-index": 1})
    if not trace.is_file() or trace.stat().st_size != 0x8000:
        raise RuntimeError("Cannot capture original IPU1 trace")
    content = trace.read_bytes()
    control, ttb = (
        int(qtest_command(qtest, f"readl {address:#x}").split()[1], 0)
        for address in (0x58882044, 0x5888204c))
    state["mmu_control"] = f"{control:#x}"
    state["mmu_ttb"] = f"{ttb:#x}"
    state["sysbios_startup_observed"] = (
        b"sys/bios startup is done..." in content)
    state["firmware_exception_observed"] = bool(re.search(
        rb"E_hardFault|E_busFault|Exception occurred|assertion failure",
        content))
    state["native_remoteproc_mmu_verified"] = (
        control & 6 == 6 and ttb != 0 and state["sysbios_startup_observed"])
    (output / f"ipu1-boot-{boot}.json").write_text(
        json.dumps(state, indent=2) + "\n")


def inspect_subcpu_tx(stream, size):
    """Match original RL78 framing and additive checksum, not peer execution."""
    counts = dict.fromkeys((
        "frames", "checksum_valid_frames", "checksum_invalid_frames",
        "discarded_bytes", "invalid_lengths"), 0)
    packet = bytearray()
    expected = None
    remaining = size
    while remaining:
        chunk = stream.read(min(4096, remaining))
        if not chunk:
            raise RuntimeError("Short SubCPU UART capture")
        remaining -= len(chunk)
        for byte in chunk:
            if not packet:
                if byte == 0xf7:
                    packet.append(byte)
                else:
                    counts["discarded_bytes"] += 1
                continue
            packet.append(byte)
            if len(packet) == 2 and byte != 0xc1:
                counts["discarded_bytes"] += 2
                packet.clear()
            elif len(packet) == 5:
                length = int.from_bytes(packet[3:5], "little")
                if not 10 <= length <= 266:
                    counts["invalid_lengths"] += 1
                    counts["discarded_bytes"] += len(packet)
                    packet.clear()
                else:
                    expected = length + 4
            elif expected is not None and len(packet) == expected:
                counts["frames"] += 1
                valid = sum(packet[3:-1]) & 0xff == packet[-1]
                counts["checksum_valid_frames" if valid else
                       "checksum_invalid_frames"] += 1
                packet.clear()
                expected = None
    counts["incomplete_frame_bytes"] = len(packet)
    return counts


def capture_subcpu_observation(rcc, output, boot, tx_offset):
    """Read original M4 power state and UART5 output; supply no peer input."""
    path = (output / f"ipu1-power-boot-{boot}.bin").resolve()
    rcc.cmd("human-monitor-command",
            command_line=f"memsave 0x80567900 0xd0 {json.dumps(str(path))}",
            **{"cpu-index": 1})
    if not path.is_file() or path.stat().st_size != 0xd0:
        raise RuntimeError("Cannot capture IPU1 power state")
    state = path.read_bytes()
    wire = output / "rcc-subcpu-uart-wire.bin"
    size = wire.stat().st_size
    if size < tx_offset:
        raise RuntimeError("Original RCC SubCPU UART log shrank across reset")
    with wire.open("rb") as stream:
        stream.seek(tx_offset)
        prefix = stream.read(min(64, size - tx_offset))
        stream.seek(tx_offset)
        framing = inspect_subcpu_tx(stream, size - tx_offset)
    return {
        "rcc_uart": "UART5 0x48066000 original M4 /uart4",
        "tx_file": wire.name,
        "tx_file_offset": tx_offset,
        "tx_bytes": size - tx_offset,
        "tx_prefix_hex": prefix.hex(),
        "tx_framing": framing,
        "power_state_snapshot": path.name,
        "m4_virtual_state_bytes": {
            "0x80567907": state[7],
            "0x80567991": state[0x91],
            "0x805679cc": state[0xcc],
        },
        "subcpu_peer_attached": False,
        "host_protocol_responder": False,
        "subcpu_exchange_verified": False,
        "note": (
            "Firmware-specific read-only observations. Local M4 state, "
            "handler-enable flag and decoded power-state bytes are not "
            "board identity or readiness. UART5 output is captured without "
            "an attached SubCPU; no received bytes or replies are supplied."),
    }


def verify_asi_registration(rcc_log, mmx_log):
    started = re.search(
        rb"(?m)^\[PER_CYC\] \d+ call AgentStart clientRCC_persistence\.\r?$",
        rcc_log)
    if not started or not re.search(
            rb"(?m)^\[PER_CYC\] \d+ StartAgent success\.\r?$",
            rcc_log[started.end():]):
        raise RuntimeError("Original RCC persistence agent did not start")
    connections = re.findall(
        rb"(?m)^tcp\s+\d+\s+\d+\s+10\.0\.0\.16\.(\d+)"
        rb"\s+10\.0\.0\.15\.21100\s+ESTABLISHED\s*\r?$", rcc_log)
    for port in connections:
        if re.search(
                rb"(?m)^tcp\s+\d+\s+\d+\s+10\.0\.0\.15\.21100"
                rb"\s+10\.0\.0\.16\." + port +
                rb"\s+ESTABLISHED\s*\r?$", mmx_log):
            return {
                "agent": "clientRCC_persistence",
                "configured_process_id": 425,
                "agent_start_verified": True,
                "broker": "10.0.0.15:21100",
                "rcc_peer": "10.0.0.16:" + port.decode(),
                "matching_native_tcp_peers_verified": True,
                "persistence_transactions_verified": False,
                "m4_rstp_verified": False,
            }
    raise RuntimeError("Original MMX/RCC TCP peer states do not match")


def isolate_manual_rcc_startup(rcc, captured, log, timeout):
    """Stop the stock manager before explicitly launching diagnostic services."""
    inventory = execute(rcc.console_socket, captured, log,
                        "pidin -p ProcessManager", timeout)
    pattern = rb"(?m)^\s*(\d+)\s+\d+\s+\S*ProcessManager\s+\S+\s+(\S+)"
    threads = re.findall(pattern, inventory)
    pids = {pid for pid, _ in threads}
    if len(pids) != 1:
        raise RuntimeError("Cannot identify one original RCC ProcessManager")
    pid = next(iter(pids))
    stopped = execute(rcc.console_socket, captured, log,
                      "kill -s STOP " + pid.decode() +
                      " && pidin -p " + pid.decode(), timeout)
    states = re.findall(pattern, stopped)
    if (len(states) != len(threads) or
            any(found != pid or state != b"STOPPED" for found, state in states)):
        raise RuntimeError("Original RCC ProcessManager did not stop completely")
    return {
        "pid": int(pid),
        "signal": "SIGSTOP",
        "process_stopped_verified": True,
        "original_startup_verified": False,
        "reason": "Prevent delayed stock startup from duplicating explicit "
                  "manual diagnostic services",
        "inventory": inventory.decode(errors="replace"),
        "stopped": stopped.decode(errors="replace"),
    }


def start_mmx_asi(mmx, captured, log, command_timeout):
    for command in (
        "waitfor /dev/mnanda0t177.3 10 && "
        "mount -tqnx6 -oro /dev/mnanda0t177.3 /mnt/swup && "
        # The OTA installs the same swup.img as SWUP_RECOVERY, which
        # sentinel requires; disks with a recovery copy carry it as .12.
        "if test -e /dev/mnanda0t177.12; then "
        "mount -tqnx6 -oro /dev/mnanda0t177.12 /mnt/swup_recovery; fi",
        ". /etc/boot/common.sh && set_environment_variables_basic && "
        "test -r \"$LIBIMG_CFGFILE\"",
        "pipe && ifconfig lo0 127.0.0.1 up && "
        "setconf _CS_HOSTNAME mmx && setconf _CS_DOMAIN mib2p && /eso/bin/mapper",
        ". /etc/global.env; export RUN_MODE=normal "
        "IPL_CONFIG_DIR=/etc/eso/production LOGFILES_DIR=/tmp "
        "LD_LIBRARY_PATH=/lib:/usr/lib:/eso/lib:/armle/lib:/armle/usr/lib; "
        "cd /eso; ./bin/broker --node mmx >/tmp/broker.log 2>&1 & BROKER_PID=$!",
    ):
        execute(mmx.console_socket, captured, log, command, command_timeout)


def verify_native_asi(mmx, rcc, captured, log, rcc_log, timeout, *,
                      wait_timeout=None, native_ipu1=False,
                      manual_startup_isolation=False):
    """Start original applications, not a host ASI or persistence responder."""
    mmx.cmd("cont")
    rcc.cmd("cont")
    command_timeout = max(timeout, 120)
    isolation = None
    if manual_startup_isolation:
        with rcc_log.open("ab") as rlog:
            isolation = isolate_manual_rcc_startup(
                rcc, bytearray(), rlog, command_timeout)
    start_mmx_asi(mmx, captured, log, command_timeout)
    rcaptured = bytearray()
    with rcc_log.open("ab") as rlog:
        for command in (
            "setconf _CS_HOSTNAME rcc && setconf _CS_DOMAIN mibhigh.net && "
            "devf-generic -s0x08000000,64m -r -b9 && inflator /App",
            "ls -l /Persis1/index_per1.bin /Persis2/index_per2.bin",
            "devb-ram disk name=ram ram capacity=1,nodinit "
            "blk ramdisk=3m,cache=0,vnode=256 >/tmp/ram.log 2>&1 & RAM_PID=$!",
            "waitfor /dev/ram0 10 && dinit -h -q /dev/ram0 && "
            "mount -tqnx4 /dev/ram0 /ram0",
            "pps -m /mypps && mkdir -p /mypps/qdb1/config && "
            "cp /usr/persis/persis_qdb_BAT.cfg /mypps/qdb1/config/persisDB_BAT && "
            "cp /usr/persis/persis_qdb_IMM.cfg /mypps/qdb1/config/persisDB_IMM && "
            "cp /usr/persis/persis_qdb_TMP.cfg /mypps/qdb1/config/persisDB_TMP && "
            "cp /usr/persis/persis_qdb_DB.cfg /mypps/qdb1/config/persisDB && "
            "qdb -c /mypps/qdb1 -o tempstore=/ram0",
            "shmemallocator >/tmp/shmemallocator.log 2>&1 & ALLOCATOR_PID=$!",
            "LD_PRELOAD=/App/libExceptionHandler.so "
            "/App/IPC_APP >/tmp/ipc-app.log 2>&1 & IPC_PID=$!",
            # Keep stdout on the TTY so native progress is not file-buffered.
            "LD_PRELOAD=/App/libExceptionHandler.so "
            "/App/PERSISTENCE & PERSISTENCE_PID=$!",
            "sleep 25; netstat -an; pidin -p$PERSISTENCE_PID; "
            "pidin -p$IPC_PID; ls -l /ram0; "
            "cat /mypps/qdb1/status/*; sloginfo",
        ):
            if native_ipu1 and command.startswith("shmemallocator "):
                start_native_ipu1(rcc, rcaptured, rlog, command_timeout)
            reply = execute(rcc.console_socket, rcaptured, rlog,
                            command, max(command_timeout, wait_timeout or 0)
                            if command.startswith("sleep 25;") else command_timeout)
            if command.startswith("sleep 25;"):
                qdb = verify_qdb_ready(reply.decode("latin1"))
    peer = execute(mmx.console_socket, captured, log,
                   "netstat -an; pidin -p$BROKER_PID; cat /tmp/broker.log",
                   command_timeout)
    result = verify_asi_registration(bytes(rcaptured), peer)
    result["qdb"] = qdb
    result["ipc_app_startup_observed"] = bool(re.search(
        rb"BENCH_Startup J6\(A15\) IPC_APP end of startup\r?\n", rcaptured))
    if isolation is not None:
        result["manual_startup_isolation"] = isolation
    if native_ipu1:
        result["original_native_ipu1_loader_verified"] = True
    mmx.cmd("stop")
    rcc.cmd("stop")
    return result


def verify_interface(log, interface, address):
    headers = list(re.finditer(
        rb"(?m)(?<![A-Za-z0-9_.-])([A-Za-z0-9_.-]+): "
        rb"flags=[0-9a-f]+<([^>\r\n]+)> mtu ([0-9]+)\r*$", log))
    for index, header in enumerate(headers):
        if (header[1] != interface.encode() or header[3] != b"1500" or
                not {b"UP", b"RUNNING"}.issubset(header[2].split(b","))):
            continue
        end = headers[index + 1].start() if index + 1 < len(headers) else len(log)
        # Native serial diagnostics can precede an intact ifconfig field.
        if re.search(rb"(?m)(?:^|[ \t])inet " + re.escape(address.encode()) +
                     rb" netmask 0xffffff00 broadcast 10\.0\.0\.255\r*$",
                     log[header.end():end]):
            return
    raise RuntimeError(f"Native {interface} is not running at {address}")


def verify_ping(log, address):
    # Other native services can leave a partial line before ping's output.
    replies = re.findall(
        rb"(?m)(?<![0-9])64 bytes from " + re.escape(address.encode()) +
        rb": icmp_seq=(\d+) ttl=\d+ time=[0-9.]+ ms\r?$", log)
    if (replies != [b"0", b"1"] or
        not re.search(rb"(?m)(?<![0-9])2 packets transmitted, 2 packets received, "
                      rb"0% packet loss\r?$", log)):
        raise RuntimeError(
            f"Native ping to {address} did not receive both replies")


def verify_network(mmx, rcc, captured, log, rcc_log, timeout):
    """Run original drivers without synthetic packets or descriptors."""
    mmx.cmd("cont")
    rcc.cmd("cont")
    rcaptured = bytearray()
    with rcc_log.open("ab") as rlog:
        for command in (
            "io-pkt-v6-hc -S -ptcpip stacksize=16384,stackguard,random",
            "waitfor /dev/socket 5",
        ):
            execute(rcc.console_socket, rcaptured, rlog, command, timeout)
        execute(rcc.console_socket, rcaptured, rlog,
                "mount -Tio-pkt /lib/dll/devnp-mib-rcc.so", timeout,
                background=True)
        for command in MMX_NETWORK_COMMANDS:
            execute(mmx.console_socket, captured, log, command, timeout)
        execute(rcc.console_socket, rcaptured, rlog, "wait $!", timeout)
        execute(rcc.console_socket, rcaptured, rlog,
                "ifconfig rcc0 10.0.0.16/24", timeout)
        interfaces = {}
        pings = {}
        for name, vm, capture, stream, interface, address, peer in (
            ("mmx", mmx, captured, log, "mmx0", "10.0.0.15", "10.0.0.16"),
            ("rcc", rcc, rcaptured, rlog, "rcc0", "10.0.0.16", "10.0.0.15"),
        ):
            status = execute(vm.console_socket, capture, stream,
                             f"ifconfig {interface}", timeout)
            verify_interface(status, interface, address)
            interfaces[name] = status.decode(errors="replace")
            ping = execute(vm.console_socket, capture, stream,
                           f"ping -c 2 {peer}", timeout)
            verify_ping(ping, peer)
            pings[name] = ping.decode(errors="replace")
    mmx.cmd("stop")
    rcc.cmd("stop")
    return {"interfaces": interfaces, "pings": pings,
            "bidirectional_icmp_verified": True}


def verify_crossboard(qtest):
    values = {}
    for offset, expected in CROSSBOARD_REGISTERS.items():
        value = int(qtest_command(
            qtest, f"readl {0x51000000 + offset:#x}").split()[1], 0)
        if value != expected:
            raise RuntimeError(
                f"Native crossboard register {offset:#x}: "
                f"{value:#x} != {expected:#x}")
        values[hex(offset)] = hex(value)
    return values


def verify_disconnected(qtest, timeout):
    deadline = time.monotonic() + timeout
    while True:
        values = {
            address: int(qtest_command(
                qtest, f"readl {address:#x}").split()[1], 0)
            for address in (0x48051138, 0x4a008220)
        }
        if values == {0x48051138: 0, 0x4a008220: 0}:
            return {"reset_input": "0x0", "external_apll_status": "0x0"}
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"RCC retained disconnected MMX inputs: {values!r}")
        time.sleep(0.01)


def stop_after_reset_release(mmx, qtest, timeout):
    """Pause at the original initializer's released, not pulsed-low, phase."""
    deadline = time.monotonic() + timeout
    while True:
        mmx.cmd("stop")
        level = int(qtest_command(qtest, "readl 0x01003910").split()[1], 0)
        if level & 1:
            return
        if time.monotonic() >= deadline:
            raise TimeoutError(
                "Original MMX initializer kept endpoint reset low")
        mmx.cmd("cont")
        time.sleep(0.001)


def verify_configuration(mmx, rcc):
    """Explicit stopped-guest diagnostic window, not native enumeration."""
    setup = {
        0x01001018: 0x030302, 0x01003818: 0x02000000,
        0x01003830: 0xfe100300, 0x01003800: 1,
    }
    saved = {
        address: int(qtest_command(mmx, f"readl {address:#x}").split()[1], 0)
        for address in setup
    }
    values = {}
    try:
        for address, value in setup.items():
            qtest_command(mmx, f"writel {address:#x} {value:#x}")
        for offset, expected in (
            (0, 0xb801104c), (8, 0x07800000), (0x10, 0),
            (0x14, 0), (0x18, 0),
        ):
            local = int(qtest_command(
                rcc, f"readl {0x51000000 + offset:#x}").split()[1], 0)
            remote = int(qtest_command(
                mmx, f"readl {0x02000000 + offset:#x}").split()[1], 0)
            if local != expected or remote != local:
                raise RuntimeError(
                    f"Live config {offset:#x}: local={local:#x}, "
                    f"remote={remote:#x}, expected={expected:#x}")
            values[hex(offset)] = hex(remote)
        physical = int(qtest_command(rcc, "readl 0x5100210c").split()[1], 0)
        if physical != 1 << 16:
            raise RuntimeError(f"RCC physical link status {physical:#x}")
        return {"external_config": values, "rcc_link_status": hex(physical)}
    finally:
        for address, value in saved.items():
            qtest_command(mmx, f"writel {address:#x} {value:#x}")


POST_LINK_REGISTERS = {
    0x01001494: 0x3fe,
    0x01001e00: 0xe00,
    0x01001f00: 0x5bfc0000,
    0x01001f04: 0x200000,
    0x01001f48: 0x2000,
    0x01001f4c: 0x10000000,
    0x01001fd4: 0x10000000,
    0x01001fd8: 0x10000d00,
    0x01001fe0: 0x8f8f000e,
}


def verify_post_link(log, processes, qtest):
    normalized = log.replace(b"\r", b"")
    if not re.search(rb"(?m)^(?:mh2p# )?PCIE: PCIE: port 0 up\n", normalized):
        raise RuntimeError("Missing original MMX link-up message")
    if b"SIGBUS" in normalized or b"Memory fault" in normalized:
        raise RuntimeError("Original MMX initializer faulted after link-up")
    if re.search(rb"(?m)^[ \t]*\d+[ \t]+/sbin/pcie_init ", processes):
        raise RuntimeError("Original MMX initializer has not exited")
    values = {}
    for address, expected in POST_LINK_REGISTERS.items():
        value = int(qtest_command(qtest, f"readl {address:#x}").split()[1], 0)
        if value != expected:
            raise RuntimeError(
                f"Post-link register {address:#x}: {value:#x} != {expected:#x}")
        values[hex(address)] = hex(value)
    return values


def probe(qemu, media, nor, mnand, image, output, timeout=30, *,
          config_transport=False, native_link=False, pci_server=False,
          crossboard_init=False, network=False, asi=False, rcc_nor=None,
          experimental_rcc_ipu1_fixture=None,
          experimental_rcc_ipu1_native=None,
          experimental_rcc_ipu1_sensor_fixture=False,
          experimental_native_rstp=False, experimental_native_hid=False,
          rcc_input_peer=False,
          experimental_rcc_manual_startup_isolation=False,
          experimental_rcc_icount_shift=0, mnand_overlay=None):
    if not 0 < timeout <= 60:
        raise ValueError("Console timeout must be between 0 and 60 seconds")
    if (type(experimental_rcc_icount_shift) is not int or
            experimental_rcc_icount_shift not in range(5)):
        raise ValueError("RCC icount shift must be an integer from 0 to 4")
    if native_link and not config_transport:
        raise ValueError(
            "Native link check requires the configuration transport")
    if pci_server and not native_link:
        raise ValueError("PCI server check requires the native link check")
    if crossboard_init and not pci_server:
        raise ValueError("Crossboard check requires the PCI server check")
    if network and not crossboard_init:
        raise ValueError("Network check requires native crossboard setup")
    if asi and (not network or rcc_nor is None):
        raise ValueError("ASI check requires native networking and --rcc-nor")
    if rcc_nor is not None and not asi:
        raise ValueError("RCC App NOR option requires the ASI check")
    if experimental_rcc_manual_startup_isolation and not asi:
        raise ValueError("Manual RCC startup isolation requires --asi")
    if experimental_rcc_ipu1_fixture is not None and not asi:
        raise ValueError("Experimental RCC IPU1 fixture requires --asi")
    if experimental_rcc_ipu1_native is not None:
        if not asi:
            raise ValueError("Experimental native RCC IPU1 requires --asi")
        if experimental_rcc_ipu1_fixture is not None:
            raise ValueError("Native and diagnostic IPU1 modes are exclusive")
    if (experimental_rcc_ipu1_sensor_fixture and
            experimental_rcc_ipu1_native is None):
        raise ValueError("IPU1 sensor fixture requires native IPU1 loading")
    if experimental_native_rstp and experimental_rcc_ipu1_native is None:
        raise ValueError("Native RSTP observation requires native IPU1 loading")
    if experimental_native_hid and not experimental_native_rstp:
        raise ValueError(
            "Native HID observation requires native RSTP observation")
    if rcc_input_peer and not experimental_native_rstp:
        raise ValueError("The RSTP input peer replaces the native UART link")
    ipu_firmware = (experimental_rcc_ipu1_native
                    if experimental_rcc_ipu1_native is not None else
                    experimental_rcc_ipu1_fixture)
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    manifest = verify_media(media)
    rcc_hash = verify_image(image)
    source_hashes = {"nor": digest(nor), "mnand": digest(mnand)}
    if mnand_overlay is not None:
        from mnand_overlay import validate
        writable_mnand = validate(mnand, mnand_overlay, source_hashes["mnand"])
        mnand_drive = ("if=sd,index=0,format=qcow2,file=" +
                       str(writable_mnand).replace(",", ",,"))
    else:
        mnand_drive = ("if=sd,index=0,format=raw,snapshot=on,file=" +
                       str(mnand.resolve()).replace(",", ",,"))
    if rcc_nor is not None:
        source_hashes["rcc_nor"] = digest(rcc_nor)
        if source_hashes["rcc_nor"] != RCC_DEFAULTS_NOR_SHA256:
            raise ValueError("Expected P2865 App/Per1/Per2 diagnostic NOR SHA-256")
    if ipu_firmware is not None:
        source_hashes["rcc_ipu1"] = digest(ipu_firmware)
        if source_hashes["rcc_ipu1"] != RCC_IPU1_SHA256:
            raise ValueError("Expected original P2865 IPU1 firmware SHA-256")
    output = output.resolve()
    output.mkdir(parents=True)
    boots = []
    with tempfile.TemporaryDirectory(prefix="mh2p-pcie-wire-") as directory:
        directory = Path(directory)
        flash = rcc_nor
        if experimental_rcc_ipu1_native is not None:
            flash = directory / "native-ipu-transport-nor.bin"
            stage_native_ipu_nor(rcc_nor, ipu_firmware, flash)
        sideband = directory / "wire.sock"
        tests = {
            name: QEMUQtestProtocol(str(directory / f"{name}.sock"),
                                   server=True)
            for name in ("mmx", "rcc")
        }
        common = [
            "-S", "-display", "none", "-nic", "none",
            "-icount", "shift=0,align=off,sleep=on",
            "-qtest-log", "/dev/null", "-d", "unimp,guest_errors",
        ]
        mmx_args = common + [
            "-M", "mh2p", "-kernel", str((media / "startup.bin").resolve()),
            "-dtb", str((media / "board.dtb").resolve()),
            "-bios", str(nor.resolve()),
            "-drive", mnand_drive,
            "-qtest", f"unix:{directory / 'mmx.sock'}",
            "-D", str(output / "mmx-unimplemented.log"),
            "-chardev", f"socket,id=mh2p-pcie-sideband,path={sideband},"
            "server=on,wait=off",
        ]
        rcc_args = common + [
            "-M", "mh2p-rcc", "-kernel", str(image.resolve()),
            "-qtest", f"unix:{directory / 'rcc.sock'}",
            "-D", str(output / "rcc-unimplemented.log"),
            "-chardev", f"socket,id=mh2p-pcie-sideband,path={sideband}",
        ]
        rcc_args[rcc_args.index("-icount") + 1] = (
            f"shift={experimental_rcc_icount_shift},align=off,sleep=on")
        if flash is not None:
            rcc_args += [
                "-drive", "if=pflash,format=raw,snapshot=on,file=" +
                str(flash.resolve()).replace(",", ",,")]
        transport = (
            socket.socketpair() + socket.socketpair()
            if config_transport else ())
        if transport:
            mmx_args[mmx_args.index("mh2p")] += (
                f",pcie-fd={transport[0].fileno()},"
                f"pcie-outbound-fd={transport[2].fileno()}")
            rcc_args[rcc_args.index("mh2p-rcc")] += (
                f",pcie-fd={transport[1].fileno()},"
                f"pcie-outbound-fd={transport[3].fileno()}")
        if ipu_firmware is not None:
            index = rcc_args.index("-M") + 1
            if experimental_rcc_ipu1_native is not None:
                rcc_args[index] += ",experimental-ipu1-native=on"
            else:
                rcc_args[index] += (
                    ",experimental-ipu1-firmware=" +
                    str(ipu_firmware.resolve()).replace(",", ",,") +
                    ",experimental-ipu1-smp=on")
            eeprom = output / "diagnostic-erased-eeprom.bin"
            eeprom.write_bytes(b"\xff" * 8192)
            rcc_args += [
                "-device", "tja1145,bus=spi4,cs=0",
                "-drive", "if=none,id=ipu_eeprom,format=raw,snapshot=on,file=" +
                str(eeprom).replace(",", ",,"),
                "-device", "at24c-eeprom,bus=i2c2,address=0x50,"
                "rom-size=8192,drive=ipu_eeprom",
                "-serial", "null",
                "-chardev", "file,id=subcpu_uart,path=" +
                str((output / "rcc-subcpu-uart-wire.bin").resolve())
                .replace(",", ",,"),
                "-serial", "chardev:subcpu_uart",
                "-serial", "null",
                "-chardev", "file,id=m4_uart7,path=" +
                str((output / "rcc-m4-uart7.log").resolve()).replace(",", ",,"),
                "-serial", "chardev:m4_uart7",
            ]
        if experimental_rcc_ipu1_sensor_fixture:
            rcc_args += ["-device", "lis331dlh,bus=spi3,cs=1"]
        try:
            for qtest in tests.values():
                qtest.settimeout(5)
            with QEMUMachine(str(qemu.resolve()), args=mmx_args) as mmx, \
                 QEMUMachine(str(qemu.resolve()), args=rcc_args) as rcc:
                mmx.set_console(console_index=1)
                rcc.set_console(console_index=2)
                if transport:
                    transport[0].set_inheritable(True)
                    transport[2].set_inheritable(True)
                mmx.launch()
                if transport:
                    transport[0].close()
                    transport[2].close()
                    transport[1].set_inheritable(True)
                    transport[3].set_inheritable(True)
                rcc.launch()
                if experimental_native_rstp:
                    connect_native_uart(mmx, rcc, directory, output,
                                        input_peer=rcc_input_peer)
                if transport:
                    transport[1].close()
                    transport[3].close()
                commands = {"mmx": list(mmx._qemu_full_args),
                            "rcc": list(rcc._qemu_full_args)}
                for vm in (mmx, rcc):
                    vm._qmp.settimeout(5)
                for qtest in tests.values():
                    listener = qtest._sock
                    qtest.accept()
                    listener.close()
                    qtest.settimeout(5)
                try:
                    for boot in range(2):
                        subcpu_tx_offset = (
                            (output / "rcc-subcpu-uart-wire.bin")
                            .stat().st_size if ipu_firmware is not None else 0)
                        uart_offsets = {
                            name: (output /
                                   f"{name}-uart-wire.bin").stat().st_size
                            for name in ("mmx", "rcc")
                        } if experimental_native_rstp else {}
                        if boot:
                            mmx.cmd("system_reset")
                            rcc.cmd("system_reset")
                        verify_disconnected(tests["rcc"], timeout)
                        if experimental_rcc_ipu1_sensor_fixture:
                            diagnostic_ipl_spi_handoff(tests["rcc"])
                        mmx.cmd("cont")
                        captured = bytearray()
                        mmx_log = output / f"mmx-boot-{boot}.log"
                        rcc_log = output / f"rcc-boot-{boot}.log"
                        with mmx_log.open("wb") as log:
                            banner = wait_console(
                                mmx.console_socket, captured, log,
                                re.escape(BANNER), 0, timeout)
                            wait_console(mmx.console_socket, captured, log,
                                         rb"mh2p# ", banner.end(), timeout)
                            for prerequisite in PREREQUISITES:
                                execute(mmx.console_socket, captured, log,
                                        prerequisite, timeout)
                            volumes = captured.count(
                                b"ifs image checksum is valid")
                            if volumes != 6:
                                raise RuntimeError(
                                    "Expected six verified stage-two volumes")
                            clock_profile = {}

                            def release_from_mmx():
                                if config_transport:
                                    # Preserve a deliberate pre-peer checkpoint.
                                    rcc.cmd("stop")
                                start = len(captured)
                                execute(mmx.console_socket, captured, log,
                                        "/sbin/pcie_init -v -p0 -r25", timeout,
                                        background=True)
                                wait_console(mmx.console_socket, captured, log,
                                             re.escape(LINK_RETRY) + rb"\r?\n",
                                             start, timeout)
                                processes = execute(
                                    mmx.console_socket, captured, log,
                                    "pidin ar", timeout)
                                verify_native_setup(bytes(captured[start:]),
                                                    processes)
                                if config_transport:
                                    stop_after_reset_release(
                                        mmx, tests["mmx"], timeout)
                                else:
                                    mmx.cmd("stop")
                                if config_transport:
                                    rcc.cmd("cont")
                                for name, address in (
                                    ("plle_base", 0x600060e8),
                                    ("plle_misc", 0x600060ec),
                                    ("plle_aux", 0x6000648c),
                                    ("pllre_base", 0x600064c4),
                                    ("pllre_misc", 0x600064c8),
                                ):
                                    clock_profile[name] = hex(int(qtest_command(
                                        tests["mmx"], f"readl {address:#x}")
                                        .split()[1], 0))
                                (output / f"mmx-clock-{boot}.json").write_text(
                                    json.dumps(clock_profile, indent=2) + "\n")

                            rcc.cmd("cont")
                            with rcc_log.open("wb") as rlog:
                                endpoint = check_endpoint(
                                    rcc.console_socket, tests["rcc"], rlog,
                                    timeout, release_from_mmx=release_from_mmx,
                                    link_peer=config_transport)
                            rcc.cmd("stop")
                            boots.append({
                                "mmx_native_controller_enabled": True,
                                "mmx_original_initializer_alive_retrying": True,
                                "mmx_registers": read_registers(
                                    tests["mmx"], link=config_transport),
                                "mmx_clock_profile": clock_profile,
                                "rcc_endpoint": endpoint,
                            })
                            if native_link:
                                mmx.cmd("cont")
                                wait_console(
                                    mmx.console_socket, captured, log,
                                    rb"PCIE: PCIE: port 0 up\r?\n",
                                    0, timeout)
                                execute(mmx.console_socket, captured, log,
                                        "wait $!", timeout)
                                processes = execute(
                                    mmx.console_socket, captured, log,
                                    "pidin ar", timeout)
                                mmx.cmd("stop")
                                boots[-1]["native_post_link_registers"] = (
                                    verify_post_link(
                                        bytes(captured), processes,
                                        tests["mmx"]))
                                boots[-1][
                                    "mmx_original_initializer_alive_retrying"
                                ] = False
                                boots[-1][
                                    "mmx_original_initializer_exited_ok"
                                ] = True
                            elif config_transport:
                                boots[-1]["live_configuration"] = (
                                    verify_configuration(
                                        tests["mmx"], tests["rcc"]))
                            if pci_server:
                                mmx.cmd("cont")
                                execute(mmx.console_socket, captured, log,
                                        "slogger -s 256 && waitfor /dev/slog 5",
                                        timeout)
                                execute(mmx.console_socket, captured, log,
                                        SERVER_COMMAND, timeout,
                                        background=True)
                                execute(mmx.console_socket, captured, log,
                                        "waitfor /dev/pci 50", 90)
                                processes = execute(
                                    mmx.console_socket, captured, log,
                                    "pidin ar", timeout)
                                device = execute(
                                    mmx.console_socket, captured, log,
                                    "ls -ld /dev/pci", timeout)
                                slog = execute(
                                    mmx.console_socket, captured, log,
                                    "sloginfo", timeout)
                                verify_native_server(
                                    slog, processes, device, endpoint=True)
                                mmx.cmd("stop")
                                endpoint_registers = {}
                                for offset, expected in (
                                    (0, 0xb801104c),
                                    (0x10, 0x10000), (0x14, 0x14000),
                                    (0x18, 0),
                                ):
                                    value = int(qtest_command(
                                        tests["rcc"],
                                        f"readl {0x51000000 + offset:#x}")
                                        .split()[1], 0)
                                    if value != expected:
                                        raise RuntimeError(
                                            f"Enumerated RCC {offset:#x}: "
                                            f"{value:#x} != {expected:#x}")
                                    endpoint_registers[hex(offset)] = hex(value)
                                boots[-1]["native_endpoint_registers"] = (
                                    endpoint_registers)
                                boots[-1]["native_afi_registers"] = {
                                    hex(off): qtest_command(
                                        tests["mmx"],
                                        f"readl {0x1000000 + off:#x}")
                                        .split()[1]
                                    for off in range(0x3800, 0x3848, 4)
                                }
                            if crossboard_init:
                                mmx.cmd("cont")
                                execute(
                                    mmx.console_socket, captured, log,
                                    "setconf _CS_LIBPATH /proc/boot:/lib:"
                                    "/usr/lib:/lib/dll:/lib/dll/pci", timeout)
                                execute(mmx.console_socket, captured, log,
                                        CROSSBOARD_COMMAND, timeout)
                                mmx.cmd("stop")
                                boots[-1]["native_crossboard_registers"] = (
                                    verify_crossboard(tests["rcc"]))
                            if network:
                                boots[-1]["native_network"] = verify_network(
                                    mmx, rcc, captured, log, rcc_log, timeout)
                            if asi:
                                boots[-1]["native_asi"] = verify_native_asi(
                                    mmx, rcc, captured, log, rcc_log,
                                    timeout, wait_timeout=600
                                    if ipu_firmware is not None else None,
                                    native_ipu1=experimental_rcc_ipu1_native
                                    is not None,
                                    manual_startup_isolation=
                                    experimental_rcc_manual_startup_isolation)
                            if experimental_native_rstp:
                                boots[-1]["native_rstp_observation"] = (
                                    observe_native_rstp(
                                        mmx, rcc, captured, log, timeout,
                                        output, boot, uart_offsets))
                            if experimental_native_hid:
                                boots[-1]["native_hid_observation"] = (
                                    observe_native_hid(
                                        mmx, rcc, captured, log, timeout,
                                        output, boot))
                            if ipu_firmware is not None:
                                cpus = rcc.cmd("query-cpus-fast")
                                if len(cpus) != 3:
                                    raise RuntimeError(
                                        "Expected A15 and two diagnostic M4s")
                                program_counters = []
                                fault_status = []
                                for index in (1, 2):
                                    registers = rcc.cmd(
                                        "human-monitor-command",
                                        command_line="info registers",
                                        **{"cpu-index": index})
                                    (output / f"ipu1-core-{index - 1}-"
                                     f"boot-{boot}.log").write_text(registers)
                                    pc = re.search(
                                        r"\bR15=([0-9a-fA-F]{8})\b", registers)
                                    if pc is None:
                                        raise RuntimeError(
                                            "Cannot read diagnostic M4 PC")
                                    program_counters.append(int(pc[1], 16))
                                    path = (output / f"ipu1-core-{index - 1}-"
                                            f"fault-boot-{boot}.bin").resolve()
                                    rcc.cmd(
                                        "human-monitor-command",
                                        command_line=(
                                            "memsave 0xe000ed28 0x14 "
                                            f"{json.dumps(str(path))}"),
                                        **{"cpu-index": index})
                                    if (not path.is_file() or
                                            path.stat().st_size != 0x14):
                                        raise RuntimeError(
                                            "Cannot capture IPU1 core "
                                            f"{index - 1} fault status")
                                    status = dict(zip(
                                        ("cfsr", "hfsr", "dfsr", "mmfar", "bfar"),
                                        struct.unpack("<5I", path.read_bytes())))
                                    fault_status.append({
                                        name: f"{value:#x}"
                                        for name, value in status.items()
                                    })
                                    if status["cfsr"] or status["hfsr"]:
                                        print(
                                            f"Boot {boot}: original M4 core "
                                            f"{index - 1} fault status observed: "
                                            f"{fault_status[-1]}",
                                            file=sys.stderr)
                                for name, address, size, physical in (
                                    ("monitor", 0x8cbc5000, 0x2000, True),
                                    ("tasks", 0x80597f90, 0x1000, False),
                                ):
                                    path = (output / f"ipu1-{name}-"
                                            f"boot-{boot}.bin").resolve()
                                    rcc.cmd(
                                        "human-monitor-command",
                                        command_line=(
                                            f"{'pmemsave' if physical else 'memsave'} "
                                            f"{address:#x} {size:#x} "
                                            f"{json.dumps(str(path))}"),
                                        **{"cpu-index": 1})
                                    if (not path.is_file() or
                                            path.stat().st_size != size):
                                        raise RuntimeError(
                                            f"Cannot capture IPU1 {name} data")
                                fatal = 0x68644 in program_counters
                                if fatal:
                                    print(
                                        f"Boot {boot}: original M4 MAWD fatal "
                                        "loop observed; M4 startup failed",
                                        file=sys.stderr)
                                key = ("native_ipu1"
                                       if experimental_rcc_ipu1_native
                                       is not None else "diagnostic_ipu1")
                                shared_words = [
                                    int(qtest_command(
                                        tests["rcc"],
                                        f"readl {address:#x}").split()[1], 0)
                                    for address in range(
                                        0x8cbff000, 0x8cbff014, 4)
                                ]
                                gpio6_data_in = int(qtest_command(
                                    tests["rcc"],
                                    f"readl {RCC_GPIO6_DATAIN:#x}").split()[1],
                                    0)
                                boots[-1][key] = {
                                    "m4_cores_present": 2,
                                    "program_counters": [
                                        f"{pc:#x}" for pc in program_counters],
                                    "fault_status": fault_status,
                                    "known_mawd_fatal_loop_observed": fatal,
                                    "shared_startup_words": shared_words,
                                    "processmanager_wakeup_token":
                                        shared_words[2],
                                    "ipc_startup_state": shared_words[4],
                                    "gpio6_data_in":
                                        f"{gpio6_data_in:#010x}",
                                    "wakeup_predicate_gpio6_21_22": [
                                        (gpio6_data_in >> pin) & 1
                                        for pin in (21, 22)
                                    ],
                                    "native_remoteproc_mmu_verified": False,
                                    "uart7_log": "rcc-m4-uart7.log",
                                    "subcpu_observation":
                                        capture_subcpu_observation(
                                            rcc, output, boot,
                                            subcpu_tx_offset),
                                    "m4_rstp_verified": False,
                                    "diagnostic_sensor_fixture":
                                        experimental_rcc_ipu1_sensor_fixture,
                                }
                                (output / f"ipu1-boot-{boot}.json").write_text(
                                    json.dumps(boots[-1][key],
                                               indent=2) + "\n")
                                if experimental_rcc_ipu1_native is not None:
                                    capture_native_ipu1(
                                        rcc, tests["rcc"], output, boot,
                                        boots[-1][key])
                                    path = output / f"native-boot-{boot}.json"
                                    path.write_text(
                                        json.dumps(boots[-1], indent=2) + "\n")
                    mmx.cmd("quit")
                    disconnected = verify_disconnected(tests["rcc"], timeout)
                except TimeoutError:
                    print("Paired PCIe probe timed out; "
                          "terminating both probe VMs",
                          file=sys.stderr)
                    rcc.kill()
                    mmx.kill()
                    raise
        finally:
            for connection in transport:
                connection.close()
            for qtest in tests.values():
                qtest.close()
    unsupported = {
        name: [line for line in (output / f"{name}-unimplemented.log")
               .read_text().splitlines() if "unsupported" in line]
        for name in ("mmx", "rcc")
    }
    if any(unsupported.values()):
        raise RuntimeError(
            f"Unsupported operations in paired probe: {unsupported!r}")
    if experimental_rcc_ipu1_native is not None:
        uart = (output / "rcc-m4-uart7.log").read_bytes()
        if (b"M4 Application WatchDog Failed" in uart or
                b"[MAWD] Enter infinity loop" in uart):
            raise RuntimeError(
                "Native IPU1 application watchdog failed; see UART7 log")
        for boot in boots:
            state = boot["native_ipu1"]
            if (not state["native_remoteproc_mmu_verified"] or
                    state["firmware_exception_observed"] or
                    state["known_mawd_fatal_loop_observed"] or
                    any(int(status[name], 0) for status in state["fault_status"]
                        for name in ("cfsr", "hfsr"))):
                raise RuntimeError(
                    "Native IPU1 startup failed; "
                    "see saved per-boot diagnostics")
    report = {
        "commands": commands, "source_hashes": source_hashes,
        "rcc_source_sha256": rcc_hash,
        "mmx_startup_sha256": manifest["startup_sha256"],
        "mmx_dtb_sha256": manifest["dtb_sha256"],
        "original_executables_unmodified": True,
        "diagnostic_shell": True, "diagnostic_flash_handoff": False,
        "injected_reset_line": False, "diagnostic_internal_reference": False,
        "initializer_reference_option": "-c1", "boots": boots,
        "mmx_clock_reset_verified_across_reset": True,
        "rcc_external_reference_endpoint_configuration_verified": True,
        "rcc_inputs_after_mmx_disconnect": disconnected,
        "unsupported_operations": unsupported,
        "mmx_link_verified": config_transport,
        "rcc_link_verified": config_transport,
        "configuration_transport_verified": config_transport,
        "diagnostic_configuration_window": config_transport and not native_link,
        "mmx_native_post_link_verified": native_link,
        "endpoint_enumeration_verified": pci_server,
        "native_crossboard_configuration_verified": crossboard_init,
        "native_bidirectional_network_verified": network,
        "native_rcc_agent_registration_verified": asi,
        "full_rcc_services_verified": False,
        "icount": {
            "mmx_shift": 0, "rcc_shift": experimental_rcc_icount_shift,
            "align": False, "sleep": True,
            "hardware_frequency_calibration_verified": False,
        },
        "note": "Two native QNX guests, isolated unchanged initializers. Only "
                "MMX-derived reference-clock/reset levels cross this socket. "
                "No qtest writes or GPIO injection, flash token, TLP transport, "
                "fake link-up, BAR mapping, interrupts, DMA or network packets.",
    }
    if config_transport:
        report["note"] = (
            "Two unchanged native initializers program their controllers. "
            "Functional link readiness and live RCC header reads cross a "
            "separate bounded configuration socket on both boots. With both "
            "CPUs stopped, qtest temporarily programs and restores an MMX "
            "configuration window and root bus numbers; no endpoint state, "
            "GPIO, clock or flash token is injected. This is diagnostic "
            "configuration access, NOT native PCI-server enumeration, analog "
            "link training, BAR memory, interrupts, DMA or service readiness.")
    if native_link:
        report["note"] = (
            "Two unchanged native initializers reach functional link readiness. "
            "The original MMX initializer completes its post-link register "
            "programming and exits with status zero on both boots. Qtest reads "
            "only: no diagnostic window, bus-number, endpoint, GPIO, clock or "
            "flash-token writes. Native PCI-server endpoint enumeration, BAR "
            "memory, DMA, interrupts and full RCC services are NOT verified.")
    if pci_server:
        report["note"] = (
            "Original MMX PCI server enumerates both roots and live RCC "
            "B3:D0:F0, assigns its three BARs, completes initialization and "
            "exposes /dev/pci on both boots. RCC DBI readbacks confirm the "
            "native BAR assignments. Qtest reads only. BAR memory, interrupts, "
            "DMA and full RCC services remain unverified.")
    if crossboard_init:
        report["note"] = (
            "Original MMX PCI server enumerates live RCC and assigns its BARs. "
            "Unmodified mmx-pcie-init-rcc2 exits successfully on both boots; "
            "its BAR0 accesses program RCC outbound region 0 for MMX reserved "
            "RAM and write the genuine 0xdeadbeef subsystem handoff. RCC DBI "
            "readbacks verify every outbound field. Qtest reads only. "
            "Outbound transactions, interrupts, DMA, network communication "
            "and full RCC services remain unverified.")
    if network:
        report["note"] = (
            "Unmodified MMX/RCC initializers and PCI server configure live "
            "BARs and outbound memory. Original devnp-mib-mmx/rcc drivers "
            "bring up 10.0.0.15/24 and 10.0.0.16/24, with two ICMP replies "
            "and zero packet loss in each direction on both boots. Actual "
            "RAM, QDMA packet transfers, mailbox and MSI notifications are "
            "used; no host virtual NIC, descriptor injection or fake replies. "
            "Qtest reads only. Full RCC services and HMI remain unverified.")
    if asi:
        report["note"] += (
            " The original RCC persistence agent additionally reports "
            "successful startup against the original MMX broker, with "
            "matching established TCP peers on both boots. Original App and "
            "empty Per1/Per2 templates are mounted from snapshot NOR. "
            "Persistence transactions, M4/RSTP and touch remain unverified.")
    if experimental_rcc_manual_startup_isolation:
        report["note"] += (
            " Explicit manual-startup isolation sends the original RCC "
            "ProcessManager SIGSTOP and verifies every observed thread is "
            "STOPPED before launching diagnostic services on each boot. "
            "This prevents duplicate delayed startup, but is a guest-process "
            "intervention, not native startup completion or a wakeup fix.")
    if experimental_rcc_ipu1_fixture is not None:
        report["diagnostic_ipu1_fixture"] = {
            "two_m4_cores": True,
            "spi_peer": "opt-in TJA1145 control plane; "
                        "exact board part unverified",
            "eeprom_initial_bytes": 8192,
            "eeprom_initial_value": 255,
            "eeprom_snapshot_writes": True,
            "factory_eeprom_recovered": False,
            "native_remoteproc_mmu_verified": False,
        }
        report["note"] += (
            " Original dual-M4 firmware executes in the explicitly diagnostic "
            "shared-address fixture with opt-in TJA1145 and erased EEPROM. "
            "Shared startup words are observed, never written by the probe. "
            "Neither CPU presence nor those words establish M4/RSTP or HMI "
            "readiness. The ASI observation allows up to 600 host seconds "
            "without changing the 25-second guest wait.")
    if experimental_rcc_ipu1_native is not None:
        report["native_ipu1_boot"] = {
            "original_guest_loader": True, "host_preloaded_m4": False,
            "transport_nor_offset": "0x200000",
            "transport_elf_bytes": 2097152,
            "factory_eeprom_recovered": False,
            "spi_peer": "opt-in TJA1145 control plane; "
                        "exact board part unverified",
        }
        report["note"] += (
            " The native IPU1 option uses the original guest ipc loader and "
            "unchanged padded OTA ELF transported through verified erased "
            "space in a temporary snapshot NOR copy. Guest-selected MMU "
            "tables and original SYS/BIOS trace are checked on both boots. "
            "No private DDR aliases or host-loaded M4. "
            "This still does not verify MessageQ/RSTP, full RCC or touch UI.")
    if experimental_rcc_ipu1_sensor_fixture:
        report["note"] = report["note"].replace(
            "Qtest reads only.", "PCIe validation uses qtest reads only.")
        report["diagnostic_ipu1_sensor_fixture"] = {
            "sensor": "LIS331DLH-compatible SPI3 CS1; "
                      "exact board part unverified",
            "stationary_acceleration_mg": [0, 0, 1000],
            "sample_clock": "QEMU virtual time",
            "diagnostic_spi_clock_writes": {
                hex(address): 2 for address in IPL_SPI_CLOCKS},
            "original_ipl_executed": False,
        }
        report["note"] += (
            " The opt-in sensor fixture supplies timed LIS331DLH samples "
            "and explicitly writes the four SPI clock enables observed in "
            "recovered K9829 BTR code before each boot. This is a diagnostic "
            "handoff subset, not execution of the original IPL boot chain "
            "or recovery of real unit acceleration/calibration.")
    if experimental_native_rstp:
        report["note"] += (
            " The experimental native RSTP observation connects the original "
            "MMX UARTA and RCC UART1 byte streams and invokes original OOC "
            "startup with explicit LP_GPIO_ST=0. Per-boot transmit counts "
            "and original output are retained; neither bytes nor local "
            "RSTP device nodes establish a completed handshake or HMI.")
    if experimental_native_hid:
        report["note"] += (
            " The explicit native HID observation launches the unchanged "
            "MMX hidcontroller and records process/service/device state. "
            "No input packet is injected and no touch readiness is inferred.")
    if experimental_rcc_icount_shift:
        report["note"] += (
            f" Explicit RCC instruction-count shift "
            f"{experimental_rcc_icount_shift} changes virtual nanoseconds per "
            "RCC instruction only; MMX stays at shift zero. This is an "
            "experimental timing calibration, not measured target CPU "
            "frequency, faster translated execution or verified real-time "
            "operation. Hardware counters and watchdog intervals are unchanged.")
    report["persistent_mnand_overlay"] = (
        str(writable_mnand) if mnand_overlay is not None else None)
    report["durable_rcc_persistence_verified"] = False
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path,
                        default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--nor", type=Path, required=True)
    parser.add_argument("--mnand", type=Path, required=True)
    parser.add_argument("--mnand-overlay", type=Path,
                        help="Explicit prevalidated persistent development disk directory; "
                             "default remains disposable snapshot writes")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--experimental-rcc-icount-shift", type=int,
                        choices=range(5), default=0,
                        help="Experimental RCC-only virtual nanoseconds per "
                             "instruction (2**shift); MMX stays at shift zero. "
                             "Not a CPU-frequency or real-time guarantee")
    parser.add_argument("--config-transport", action="store_true",
                        help="Also check live configuration using an explicit "
                             "temporary diagnostic MMX window")
    parser.add_argument("--native-link", action="store_true",
                        help="Resume the original MMX post-link path; implies "
                             "--config-transport and uses qtest reads only")
    parser.add_argument("--pci-server", action="store_true",
                        help="Verify native RCC enumeration; implies "
                             "--native-link")
    parser.add_argument("--crossboard-init", action="store_true",
                        help="Verify original MMX RCC outbound configuration "
                             "through BAR0; implies --pci-server")
    parser.add_argument("--network", action="store_true",
                        help="Verify original drivers and two-way ICMP; "
                             "implies --crossboard-init")
    parser.add_argument("--asi", action="store_true",
                        help="Verify original RCC agent/MMX broker "
                             "registration; implies --network")
    parser.add_argument("--rcc-nor", type=Path,
                        help="Hash-pinned App/Per1/Per2 fixture for --asi")
    parser.add_argument("--experimental-rcc-manual-startup-isolation",
                        action="store_true",
                        help="With --asi, stop original RCC ProcessManager "
                             "before manual diagnostic services to avoid "
                             "duplicate delayed startup; not native boot")
    parser.add_argument("--experimental-rcc-ipu1-fixture", type=Path,
                        metavar="M4_ELF",
                        help="With --asi, run hash-pinned original dual-M4 "
                             "firmware with opt-in TJA1145 and an erased "
                             "snapshot EEPROM; not native remoteproc/MMU boot")
    parser.add_argument("--experimental-rcc-ipu1-native", type=Path,
                        metavar="M4_ELF",
                        help="With --asi, load the original padded OTA M4 "
                             "through guest ipc with native reset/MMU control "
                             "and the explicit SPI/erased-EEPROM fixtures")
    parser.add_argument("--experimental-rcc-ipu1-sensor-fixture",
                        action="store_true",
                        help="With native IPU1, simulate a stationary "
                             "LIS331DLH and diagnostic recovered IPL SPI clock "
                             "writes; not an original IPL boot")
    parser.add_argument("--experimental-native-rstp", action="store_true",
                        help="With native IPU1, connect MMX UARTA to RCC UART1 "
                             "and observe original OOC/RSTP on both boots; "
                             "does not verify a handshake or HMI readiness")
    parser.add_argument("--experimental-native-hid", action="store_true",
                        help="With --experimental-native-rstp, launch original "
                             "MMX hidcontroller and capture native input "
                             "service state; no injected input or touch claim")
    args = parser.parse_args()
    network = args.network or args.asi
    print(json.dumps(probe(args.qemu, args.media, args.nor, args.mnand,
                           args.image, args.output, args.timeout,
                           config_transport=(args.config_transport or
                                             args.native_link or
                                             args.pci_server or
                                             args.crossboard_init or
                                             network),
                           native_link=(args.native_link or args.pci_server or
                                        args.crossboard_init or network),
                           pci_server=(args.pci_server or args.crossboard_init
                                       or network),
                           crossboard_init=(args.crossboard_init or
                                            network),
                           network=network, asi=args.asi,
                           rcc_nor=args.rcc_nor,
                           experimental_rcc_ipu1_fixture=
                           args.experimental_rcc_ipu1_fixture,
                           experimental_rcc_ipu1_native=
                           args.experimental_rcc_ipu1_native,
                           experimental_rcc_ipu1_sensor_fixture=
                           args.experimental_rcc_ipu1_sensor_fixture,
                           experimental_native_rstp=
                           args.experimental_native_rstp,
                           experimental_native_hid=
                           args.experimental_native_hid,
                           experimental_rcc_manual_startup_isolation=
                           args.experimental_rcc_manual_startup_isolation,
                           experimental_rcc_icount_shift=
                           args.experimental_rcc_icount_shift,
                           mnand_overlay=args.mnand_overlay),
                     indent=2))


if __name__ == "__main__":
    main()
