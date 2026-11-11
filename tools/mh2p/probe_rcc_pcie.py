#!/usr/bin/env python3
"""Diagnose original RCC PCIe startup with an explicit injected reset signal."""
import argparse
import json
from pathlib import Path
import re
import sys
import tempfile
import time
import uuid

from check_rcc_boot import BANNER, process_inventory, receive_until
from probe_rcc import QEMUMachine, ROOT, verify_image
from qemu.machine.qtest import QEMUQtestProtocol


RESET_INPUT = "/machine/gpio7 unnamed-gpio-in 27"
CLOCKS = {
    "ocp2scp1": (0x4a0093e0, 1),
    "ocp2scp3": (0x4a0093e8, 1),
    "pciess1": (0x4a0093b0, 0x702),
    "pcie_domain": (0x4a0093a0, 2),
}
CLOCK_END = b"\nPCIe TRC end dra7xx_PCIESS1_Clock_Enable\n"
PLL_START = b"\nPCIe TRC start dra7xx_PCIESS1_Pll_Config\n"
PLL_END = b"\nPCIe TRC end dra7xx_PCIESS1_Pll_Config\n"
APLL_REQUEST = b"CM_CLKMODE_APLL_PCIE__MODE_SELECT__FORCE_LOCK\n"
CTRL_END = b"\nPCIe TRC end dra7xx_PCIESS1_Ctrl_Config\n"
PHY_START = b"\nPCIe TRC start dra7xx_PCIESS1_Phy_Config\n"
PHY_END = b"\nPCIe TRC end dra7xx_PCIESS1_Phy_Config\n"
RESET_START = b"\nPCIe TRC start dra7xx_PCIESS1_Reset\n"
RESET_END = b"\nPCIe TRC end dra7xx_PCIESS1_Reset\n"
EP_END = b"\nPCIe TRC end dra7xx_PCIESS1_Set_EP_Type\n"
LINK_CONFIG_END = b"\nPCIe TRC end dra7xx_PCIE_EP_Link_Config\n"
INBOUND_END = b"\nPCIe TRC end dra7xx_PCIE_InBound_Config\n"
EP_REG_END = b"\nPCIe TRC end dra7xx_PCIe_Ep_Reg_Config\n"
LTSSM_START = b"\nPCIe TRC start dra7xx_PCIE_Ltssm_Enable\n"
REFERENCE_CLOCKS = {
    "reference_dpll_mode": (0x4a008200, 7),
    "reference_dpll_status": (0x4a008204, 0x11),
    "reference_dpll_select": (0x4a00820c, 0x0602ee09),
    "reference_dpll_m2": (0x4a008210, 0x60f),
    "acs_control": (0x4a003c14, 0x20000),
}
PHY_REGISTERS = {
    "phy_power_command": (0x4a003c40, 0x0500c000),
    "pcs_receiver_delay": (0x4a003c34, 0x00960000),
    "rx_analog": (0x4a09400c, 0x08028000),
    "rx_trim": (0x4a09401c, 0x80000000),
    "rx_dll": (0x4a094024, 0xc0000000),
    "rx_digital": (0x4a094028, 0x02e33000),
    "rx_equalizer": (0x4a094038, 0x0000f880),
    "local_reset_control": (0x4ae07310, 2),
    "local_reset_status": (0x4ae07314, 1),
}
CONTROLLER_REGISTERS = {
    "endpoint_type": (0x51002100, 0),
    "ltssm_enabled": (0x51002104, 1),
    "physical_link_down": (0x5100210c, 0),
    "endpoint_identity": (0x51000000, 0xb801104c),
    "endpoint_command": (0x51000004, 0x00100002),
    "endpoint_class": (0x51000008, 0x07800000),
    "endpoint_link_capabilities": (0x5100007c, 0x00433c12),
    "bar0": (0x51000010, 0),
    "bar1": (0x51000014, 0),
    "bar2": (0x51000018, 0),
    "bar0_mask": (0x51001010, 0x3fff),
    "bar1_mask": (0x51001014, 0xfff),
    "bar2_mask": (0x51001018, 0xffff),
    "bar3_disabled": (0x5100101c, 0),
    "bar4_disabled": (0x51001020, 0),
    "bar5_disabled": (0x51001024, 0),
    # The original initializer ORs the index: 0, 1, then 1|2 == 3.
    "inbound_viewport": (0x51000900, 0x80000003),
    "inbound_memory_type": (0x51000904, 0),
    "inbound_bar2_control": (0x51000908, 0xc0000200),
    "inbound_shared_memory_target": (0x51000918, 0x80010000),
    "inbound_target_upper": (0x5100091c, 0),
}


def qtest_command(qtest, command):
    response = qtest.cmd(command).strip()
    if response != "OK" and not response.startswith("OK "):
        raise RuntimeError(f"qtest {command!r} failed: {response!r}")
    return response


def inventory(console, log, output, timeout):
    marker = ("RCC_PCIE_" + uuid.uuid4().hex).encode()
    start = len(log)
    console.sendall(b"echo " + marker + b":BEGIN; pidin ar; echo " +
                    marker + b":END:$((6 * 7))\n")
    receive_until(console, log, output,
                  lambda data: b"\n" + marker + b":END:42\n" in
                  data[start:].replace(b"\r", b""), timeout)
    return process_inventory(bytes(log[start:]), marker)


def check_endpoint(console, qtest, output, timeout, internal_reference=False,
                   release_from_mmx=None, *, link_peer=False):
    if internal_reference and release_from_mmx is not None:
        raise ValueError(
            "Connected MMX probe must use the normal external reference")
    if link_peer and release_from_mmx is None:
        raise ValueError("Link peer requires the connected MMX probe")
    complete = internal_reference or release_from_mmx is not None
    log = bytearray()
    receive_until(console, log, output,
                  lambda data: BANNER in data and
                  b"\n# " in data.split(BANNER, 1)[1], timeout)
    # Start only the original endpoint initializer, not the entire flash path.
    console.sendall(b"rcc2-pcie-init -c" +
                    (b"0" if internal_reference else b"1") + b" -F0x4 &\n")
    receive_until(console, log, output,
                  lambda data: b"\nWait for PEX_RST01\n" in
                  data.replace(b"\r", b""), timeout)
    waiting = inventory(console, log, output, timeout)
    if not any(Path(row["arguments"].split()[0]).name == "rcc2-pcie-init"
               for row in waiting):
        raise RuntimeError("Original endpoint initializer did not remain alive")
    if b"Find PEX_RST01 to High" in log:
        raise RuntimeError("Endpoint passed its reset gate before signal injection")
    before_release = len(log)
    if release_from_mmx is None:
        qtest_command(qtest, f"set_irq_in {RESET_INPUT} 1")
    else:
        release_from_mmx()
    target = LTSSM_START if complete else APLL_REQUEST
    receive_until(console, log, output,
                  lambda data: target in
                  data[before_release:].replace(b"\r", b""), timeout)
    normalized = bytes(log[before_release:]).replace(b"\r", b"")
    memory = re.search(
        rb"(?m)^Find PCIe memory: pciemem_base=0x([0-9a-f]+), "
        rb"pciemem_size=0x([0-9a-f]+)$", normalized)
    if (not memory or
        tuple(int(value, 16) for value in memory.groups()) != (0x80010000, 0x10000)
        or not re.search(rb"(?m)^(?:# )?Find PEX_RST01 to High - \d+\[ms\]$",
                         normalized) or CLOCK_END not in normalized or
        PLL_START not in normalized or
        normalized.index(CLOCK_END) >= normalized.index(PLL_START) or
        normalized.index(PLL_START) >= normalized.index(target)):
        raise RuntimeError("Missing ordered native PCIe reset/memory/clock evidence")
    if complete:
        stages = (PLL_START, PLL_END, CTRL_END, PHY_START, PHY_END,
                  RESET_START, RESET_END, EP_END, LINK_CONFIG_END,
                  INBOUND_END, EP_REG_END, LTSSM_START)
        if (any(stage not in normalized for stage in stages) or
            any(normalized.index(left) >= normalized.index(right)
                for left, right in zip(stages, stages[1:]))):
            raise RuntimeError("Missing ordered native PCIe endpoint evidence")
        deadline = time.monotonic() + timeout
        while int(qtest_command(qtest, "readl 0x51002104").split()[1], 0) != 1:
            if time.monotonic() >= deadline:
                raise TimeoutError("Original initializer did not enable LTSSM")
            time.sleep(0.01)
    clocks = {}
    expected_clocks = {
        **CLOCKS, **REFERENCE_CLOCKS,
        "apll_mode": (0x4a00821c, 0x101 if internal_reference else
                      0x181 if complete else 0x180),
        "apll_status": (0x4a008220, 1 if complete else 0),
    }
    if complete:
        expected_clocks.update(PHY_REGISTERS)
        expected_clocks.update(CONTROLLER_REGISTERS)
        if link_peer:
            del expected_clocks["physical_link_down"]
            expected_clocks["physical_link_up"] = (0x5100210c, 1 << 16)
    for name, (address, expected) in expected_clocks.items():
        response = qtest_command(qtest, f"readl 0x{address:x}")
        value = int(response.split()[1], 0)
        if value != expected:
            raise RuntimeError(f"{name} readback {value:#x} != {expected:#x}")
        clocks[name] = {"address": hex(address), "value": hex(value)}
    after = inventory(console, log, output, timeout)
    initializer_present = any(
        Path(row["arguments"].split()[0]).name == "rcc2-pcie-init"
        for row in after
    )
    if complete and not initializer_present and not link_peer:
        raise RuntimeError("Original initializer exited instead of waiting for peer")
    return {
        "initializer_waited_for_reset": True,
        "injected_reset_release_observed": release_from_mmx is None,
        "mmx_reset_release_observed": release_from_mmx is not None,
        "native_clock_enable_completed": True,
        "native_pll_configuration_entered": True,
        "native_reference_dpll_locked": True,
        "native_pll_configuration_completed": complete,
        "native_phy_configuration_completed": complete,
        "native_local_reset_completed": complete,
        "native_endpoint_configuration_completed": complete,
        "native_ltssm_enable_observed": complete,
        "reserved_memory": {"address": "0x80010000", "size": "0x10000"},
        "clock_registers": clocks,
        "processes_before_release": waiting,
        "processes_after_clock_enable": after,
        "initializer_present_in_final_inventory": initializer_present,
    }


def probe(qemu, image, output, timeout=15, internal_reference=False):
    if not 0 < timeout <= 30:
        raise ValueError("Console timeout must be between 0 and 30 seconds")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    digest = verify_image(image)
    output = output.resolve()
    output.mkdir(parents=True)
    boots = []
    with tempfile.TemporaryDirectory(prefix="mh2p-rcc-pcie-") as directory:
        path = str(Path(directory) / "qtest.sock")
        qtest = QEMUQtestProtocol(path, server=True)
        args = [
            "-M", "mh2p-rcc", "-kernel", str(image.resolve()),
            "-display", "none", "-nic", "none", "-S",
            "-icount", "shift=0,align=off,sleep=on",
            "-qtest", f"unix:{path}", "-qtest-log", "/dev/null",
            "-d", "unimp", "-D", str(output / "unimplemented.log"),
        ]
        try:
            qtest.settimeout(5)
            with QEMUMachine(str(qemu.resolve()), args=args) as vm:
                vm.set_console(console_index=2)
                vm.launch()
                command = list(vm._qemu_full_args)
                vm._qmp.settimeout(5)
                try:
                    qtest.accept()
                    qtest.settimeout(5)
                    for boot in range(2):
                        if boot:
                            vm.cmd("system_reset")
                        qtest_command(qtest, f"set_irq_in {RESET_INPUT} 0")
                        vm.cmd("cont")
                        with (output / f"boot-{boot}.log").open("wb") as log:
                            boots.append(check_endpoint(
                                vm.console_socket, qtest, log, timeout,
                                internal_reference))
                        vm.cmd("stop")
                except TimeoutError:
                    print("RCC PCIe probe timed out; terminating the probe VM",
                          file=sys.stderr)
                    vm.kill()
                    raise
        finally:
            qtest.close()
    report = {
        "command": command,
        "source_sha256": digest,
        "original_executable_unmodified": True,
        "diagnostic_reset_fixture": {
            "gpio": "GPIO7_27", "input_address": "0x48051138", "bit": 27,
            "levels_each_boot": [0, 1], "source": "qtest, NOT a running MMX",
        },
        "diagnostic_flash_handoff": False,
        "diagnostic_internal_reference": internal_reference,
        "initializer_reference_option": "-c0" if internal_reference else "-c1",
        "boots": boots,
        "native_clock_enable_verified_across_reset": True,
        "native_reference_dpll_verified_across_reset": True,
        "native_pll_configuration_verified_across_reset": internal_reference,
        "native_phy_configuration_verified_across_reset": internal_reference,
        "native_local_reset_verified_across_reset": internal_reference,
        "native_endpoint_configuration_verified_across_reset": internal_reference,
        "unsupported_operations": [
            line for line in (output / "unimplemented.log").read_text().splitlines()
            if "unsupported" in line
        ],
        "mmx_link_verified": False,
        "endpoint_initialization_verified": False,
        "full_rcc_services_verified": False,
        "note": (
            "Original endpoint reset-gate and clock-configuration evidence only. "
            "The reset input is driven explicitly by qtest, not MMX. No flash "
            "wakeup token, fake link-up, peer driver, packets or services are "
            "supplied. The optional -c0 diagnostic uses the original "
            "initializer's internal reference instead of normal -c1 external "
            "ACSPCIE. Internal-reference runs verify original PHY register "
            "and endpoint register configuration, not analog operation or "
            "transactions. LTSSM is enabled but physical link remains down. "
            "External reference, endpoint transactions "
            "and root-complex work remain; configuration "
            "completion is not PCIe link readiness."
        ),
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/qemu-system-arm")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument(
        "--diagnostic-reset-line", action="store_true", required=True,
        help="Explicitly permit qtest to drive GPIO7_27; NOT a native MMX link",
    )
    parser.add_argument(
        "--diagnostic-internal-reference", action="store_true",
        help="Use original -c0 internal reference, NOT normal -c1 MMX clock",
    )
    args = parser.parse_args()
    print(json.dumps(probe(args.qemu, args.image, args.output, args.timeout,
                           args.diagnostic_internal_reference),
                     indent=2))


if __name__ == "__main__":
    main()
