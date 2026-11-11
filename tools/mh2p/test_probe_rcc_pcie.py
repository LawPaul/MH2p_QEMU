#!/usr/bin/env python3
"""Firmware-free RCC endpoint evidence and diagnostic-provenance tests."""
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import probe_rcc_pcie as probe


PROCESSES = (
    b" 1 procnto-instr\n 2 devc-seromap\n 3 slogger\n"
    b" 4 ProcessManager\n 5 sh\n"
)


def inventory(processes):
    return (b"\nRCC_PCIE_TEST:BEGIN\n" + processes +
            b"RCC_PCIE_TEST:END:42\n# ")


NATIVE = (
    b"# Find PEX_RST01 to High - 12[ms]\n"
    b"Find PCIe memory: pciemem_base=0x80010000, pciemem_size=0x10000\n"
    b"PCIe TRC start dra7xx_PCIESS1_Clock_Enable\n" +
    probe.CLOCK_END + probe.PLL_START + b"native " + probe.APLL_REQUEST
)
INTERNAL = (NATIVE + probe.PLL_END + probe.CTRL_END + probe.PHY_START +
            probe.PHY_END + probe.RESET_START + probe.RESET_END +
            probe.EP_END + probe.LINK_CONFIG_END + probe.INBOUND_END +
            probe.EP_REG_END + probe.LTSSM_START)


class RCCPCIeTests(unittest.TestCase):
    def endpoint(self, native=NATIVE, waiting=True, premature=False,
                 bad_clock=False, internal=False, bad_register=None,
                 after_alive=True, ltssm_pending=False, connected=False,
                 linked=False):
        complete = internal or connected
        release = MagicMock() if connected else None
        console = MagicMock()
        processes = PROCESSES + (b" 6 rcc2-pcie-init -F0x4\n"
                                 if waiting else b"")
        console.recv.side_effect = [
            probe.BANNER + b"\n# ", b"\nWait for PEX_RST01\n",
            inventory(processes) +
            (b"\nFind PEX_RST01 to High - 0[ms]\n" if premature else b""),
            native,
            inventory(processes if complete and after_alive else PROCESSES),
        ]
        qtest = MagicMock()

        def command(text):
            if text.startswith("set_irq_in"):
                self.assertEqual(console.recv.call_count, 3)
                return "OK\n"
            address = int(text.split()[1], 0)
            clocks = {**probe.CLOCKS, **probe.REFERENCE_CLOCKS,
                      "apll_mode": (0x4a00821c, 0x101 if internal else
                                    0x181 if connected else 0x180),
                      "apll_status": (0x4a008220, 1 if complete else 0)}
            if complete:
                clocks.update(probe.PHY_REGISTERS)
                clocks.update(probe.CONTROLLER_REGISTERS)
                if linked:
                    clocks["physical_link_down"] = (0x5100210c, 1 << 16)
                if ltssm_pending:
                    return "OK 0x0\n"
                if qtest.cmd.call_count == 2:
                    return "OK 0x1\n"
            expected = next(value for addr, value in clocks.values()
                            if addr == address)
            value = expected ^ 1 if address == bad_register else expected
            return f"OK {0 if bad_clock else value:#x}\n"

        qtest.cmd.side_effect = command
        with patch.object(probe.uuid, "uuid4",
                          return_value=MagicMock(hex="TEST")), \
             patch("check_rcc_boot.select.select",
                   return_value=([console], [], [])):
            evidence = probe.check_endpoint(console, qtest, io.BytesIO(), 1,
                                            internal, release_from_mmx=release,
                                            link_peer=linked)
        self.assertEqual(qtest.cmd.call_count,
                         22 - int(connected) + len(probe.CONTROLLER_REGISTERS)
                         if complete else 12)
        if connected:
            release.assert_called_once_with()
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))
        self.assertEqual(console.sendall.call_count, 3)
        self.assertEqual(console.sendall.call_args_list[0].args[0],
                         b"rcc2-pcie-init -c" +
                         (b"0" if internal else b"1") + b" -F0x4 &\n")
        return evidence

    def test_connected_external_reference_requires_complete_configuration(self):
        result = self.endpoint(native=INTERNAL, connected=True)
        self.assertFalse(result["injected_reset_release_observed"])
        self.assertTrue(result["mmx_reset_release_observed"])
        self.assertTrue(result["native_endpoint_configuration_completed"])
        self.assertEqual(result["clock_registers"]["apll_mode"]["value"],
                         "0x181")
        for invalid in (
            {"native": INTERNAL.replace(probe.PLL_END, b"")},
            {"bad_register": 0x4a008220},
            {"bad_register": 0x51002104},
            {"after_alive": False},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(RuntimeError):
                self.endpoint(native=invalid.get("native", INTERNAL),
                              connected=True,
                              **{key: value for key, value in invalid.items()
                                 if key != "native"})

    def test_connected_probe_cannot_select_internal_reference(self):
        with self.assertRaises(ValueError):
            probe.check_endpoint(MagicMock(), MagicMock(), io.BytesIO(), 1,
                                 internal_reference=True,
                                 release_from_mmx=MagicMock())

    def test_connected_link_requires_exact_physical_status(self):
        result = self.endpoint(native=INTERNAL, connected=True, linked=True,
                               after_alive=False)
        self.assertEqual(
            result["clock_registers"]["physical_link_up"]["value"], "0x10000")
        self.assertFalse(result["initializer_present_in_final_inventory"])
        with self.assertRaises(RuntimeError):
            self.endpoint(native=INTERNAL, connected=True, linked=True,
                          bad_register=0x5100210c)
        with self.assertRaises(ValueError):
            probe.check_endpoint(MagicMock(), MagicMock(), io.BytesIO(), 1,
                                 link_peer=True)

    def test_native_clock_evidence_requires_live_wait_and_ordered_output(self):
        result = self.endpoint()
        self.assertTrue(result["native_clock_enable_completed"])
        self.assertTrue(result["injected_reset_release_observed"])
        self.assertTrue(result["native_reference_dpll_locked"])
        self.assertFalse(result["native_pll_configuration_completed"])
        self.assertFalse(result["initializer_present_in_final_inventory"])
        self.assertEqual(result["reserved_memory"]["size"], "0x10000")
        for invalid in (
            {"waiting": False}, {"premature": True}, {"bad_clock": True},
            {"native": NATIVE.replace(b"0x80010000", b"0x80020000")},
            {"native": NATIVE.replace(probe.CLOCK_END, b"")},
            {"native": NATIVE.replace(probe.CLOCK_END + probe.PLL_START,
                                     probe.PLL_START + probe.CLOCK_END)},
            {"native": NATIVE.replace(b"# Find PEX", b"echo Find PEX")},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(RuntimeError):
                self.endpoint(**invalid)

    def test_internal_reference_requires_native_pll_completion(self):
        result = self.endpoint(native=INTERNAL, internal=True)
        self.assertTrue(result["native_pll_configuration_completed"])
        self.assertTrue(result["native_phy_configuration_completed"])
        self.assertTrue(result["native_local_reset_completed"])
        self.assertTrue(result["native_endpoint_configuration_completed"])
        self.assertTrue(result["native_ltssm_enable_observed"])
        self.assertTrue(result["initializer_present_in_final_inventory"])
        self.assertEqual(result["clock_registers"]["apll_status"]["value"], "0x1")
        with self.assertRaises(RuntimeError):
            self.endpoint(native=INTERNAL.replace(probe.PLL_START, b""),
                          internal=True)
        for missing in (probe.PLL_END, probe.CTRL_END, probe.PHY_START,
                        probe.PHY_END, probe.RESET_START, probe.RESET_END,
                        probe.EP_END, probe.LINK_CONFIG_END,
                        probe.INBOUND_END, probe.EP_REG_END):
            with self.subTest(missing=missing), self.assertRaises(RuntimeError):
                self.endpoint(native=INTERNAL.replace(missing, b""), internal=True)
        with self.assertRaises(RuntimeError):
            self.endpoint(native=INTERNAL.replace(
                probe.PHY_START + probe.PHY_END,
                probe.PHY_END + probe.PHY_START), internal=True)
        for name, (address, _) in {
                **probe.PHY_REGISTERS, **probe.CONTROLLER_REGISTERS}.items():
            with self.subTest(register=name), self.assertRaises(RuntimeError):
                self.endpoint(native=INTERNAL, internal=True,
                              bad_register=address)
        with self.assertRaises(RuntimeError):
            self.endpoint(native=INTERNAL, internal=True, after_alive=False)
        with self.assertRaises(RuntimeError):
            self.endpoint(native=INTERNAL.replace(
                probe.INBOUND_END + probe.EP_REG_END,
                probe.EP_REG_END + probe.INBOUND_END), internal=True)

    def test_ltssm_enable_wait_is_bounded(self):
        with patch.object(probe, "time") as clock:
            clock.monotonic.side_effect = [0, 2]
            with self.assertRaises(TimeoutError):
                self.endpoint(native=INTERNAL, internal=True, ltssm_pending=True)

    def test_qtest_error_or_eof_is_not_acknowledgement(self):
        qtest = MagicMock()
        for response in ("FAIL Unknown GPIO\n", "", "NOT OK\n"):
            qtest.cmd.return_value = response
            with self.subTest(response=response), self.assertRaises(RuntimeError):
                probe.qtest_command(qtest, "set_irq_in bad unnamed-gpio-in 27 1")
        qtest.cmd.return_value = "OK\n"
        self.assertEqual(probe.qtest_command(qtest, "command"), "OK")
        qtest.cmd.return_value = "OK 0x702\n"
        self.assertEqual(probe.qtest_command(qtest, "readl"), "OK 0x702")

    def test_reset_probe_records_fixtures_and_never_claims_packet_link(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "probe"
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm._qemu_full_args = ["qemu", "-M", "mh2p-rcc"]
            vm.launch.side_effect = lambda: (output / "unimplemented.log").write_text(
                "mh2p-rcc: unsupported read at 0x4a008200 size=4\n")
            with patch.object(probe, "verify_image", return_value="digest"), \
                 patch.object(probe, "QEMUMachine", return_value=vm) as machine, \
                 patch.object(probe, "QEMUQtestProtocol") as protocol, \
                 patch.object(probe, "check_endpoint", return_value={"clock": True}):
                protocol.return_value.cmd.return_value = "OK\n"
                result = probe.probe(Path("qemu"), Path("image"), output)
            self.assertEqual(len(result["boots"]), 2)
            self.assertFalse(result["mmx_link_verified"])
            self.assertFalse(result["endpoint_initialization_verified"])
            self.assertFalse(result["diagnostic_flash_handoff"])
            self.assertFalse(result["diagnostic_internal_reference"])
            self.assertFalse(result["native_pll_configuration_verified_across_reset"])
            self.assertFalse(result["native_phy_configuration_verified_across_reset"])
            self.assertFalse(result["native_local_reset_verified_across_reset"])
            self.assertFalse(
                result["native_endpoint_configuration_verified_across_reset"])
            self.assertEqual(result["initializer_reference_option"], "-c1")
            self.assertEqual(result["diagnostic_reset_fixture"]["levels_each_boot"],
                             [0, 1])
            self.assertEqual(len(result["unsupported_operations"]), 1)
            self.assertEqual(protocol.return_value.cmd.call_count, 2)
            self.assertEqual(
                [call.args for call in vm.cmd.call_args_list],
                [("cont",), ("stop",), ("system_reset",), ("cont",), ("stop",)])
            args = machine.call_args.kwargs["args"]
            self.assertNotIn("loader", " ".join(args))
            self.assertTrue((output / "result.json").exists())
            protocol.return_value.close.assert_called_once()

    def test_timeout_leaves_evidence_but_no_success_report(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "probe"
            vm = MagicMock()
            vm.__enter__.return_value = vm
            with patch.object(probe, "verify_image", return_value="digest"), \
                 patch.object(probe, "QEMUMachine", return_value=vm), \
                 patch.object(probe, "QEMUQtestProtocol") as protocol, \
                 patch.object(probe, "check_endpoint", side_effect=TimeoutError):
                protocol.return_value.cmd.return_value = "OK\n"
                with self.assertRaises(TimeoutError):
                    probe.probe(Path("qemu"), Path("image"), output)
            vm.kill.assert_called_once()
            self.assertFalse((output / "result.json").exists())
            protocol.return_value.close.assert_called_once()

    def test_internal_probe_records_alternate_source_for_both_boots(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "probe"
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm._qemu_full_args = ["qemu", "-M", "mh2p-rcc"]
            vm.launch.side_effect = lambda: (output / "unimplemented.log").write_text(
                "mh2p-rcc: unsupported write at 0x4a003c40 size=4\n")
            with patch.object(probe, "verify_image", return_value="digest"), \
                 patch.object(probe, "QEMUMachine", return_value=vm), \
                 patch.object(probe, "QEMUQtestProtocol") as protocol, \
                 patch.object(probe, "check_endpoint",
                              return_value={"native_pll_configuration_completed": True}
                              ) as endpoint:
                protocol.return_value.cmd.return_value = "OK\n"
                result = probe.probe(Path("qemu"), Path("image"), output,
                                     internal_reference=True)
            self.assertEqual(endpoint.call_count, 2)
            self.assertTrue(all(call.args[-1] for call in endpoint.call_args_list))
            self.assertTrue(result["diagnostic_internal_reference"])
            self.assertEqual(result["initializer_reference_option"], "-c0")
            self.assertTrue(result["native_pll_configuration_verified_across_reset"])
            self.assertTrue(result["native_phy_configuration_verified_across_reset"])
            self.assertTrue(result["native_local_reset_verified_across_reset"])
            self.assertTrue(
                result["native_endpoint_configuration_verified_across_reset"])
            self.assertFalse(result["endpoint_initialization_verified"])
            self.assertFalse(result["mmx_link_verified"])

    def test_invalid_timeout_precedes_image_or_output_access(self):
        for timeout in (0, -1, 31):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                probe.probe(Path("qemu"), Path("image"), Path("unused"), timeout)


if __name__ == "__main__":
    unittest.main()
