#!/usr/bin/env python3
"""Firmware-free tests for MMX native PCIe evidence and probe failure handling."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import probe_mmx_pcie as probe


TOKEN = b"MMX_PCIE_TEST:0\r\n"
PROCESSES = (
    b" 1 procnto-smp-instr\n 2 devc-sertegra\n 3 /bin/ksh\n"
    b" 4 nvmemctrl\n 5 devg-nvclock\n 6 devg-nvpowergate\n"
    b" 7 devg-nvsys\n 8 devg-nvrm\n 9 /sbin/pcie_init -v -p0 -r25\n"
)
NATIVE = (b"\nmh2p# " + probe.CONTROLLER_ENABLED + b"\r\n" +
          probe.CHECKING_LINK + b"\r\n" + probe.LINK_RETRY + b"\r\n")
SERVER_PROCESSES = (
    PROCESSES + b" 10 /sbin/pci-server -v -c --bus-scan-limit=3\n"
)
DEVICE = b"drwxrwxrwx 1 root 0 0 Jan 01 00:00 /dev/pci\r\n"
SLOG = b"".join(
    b"Jan 01 00:00:56 5 17 0 PCI INFO [10:1]: " + stage + b"\r\n"
    for stage in (
        b"+ PCI Server v1.6 init start +",
        b"PCIe 0 link is not active",
        b"tk1_pcie_init() [PCIe 0] end, OK [PCI_ERR_OK]",
        b"PCIe 1 link is not active",
        b"tk1_pcie_init() [PCIe 1] end, OK [PCI_ERR_OK]",
        b"Found device B0:D0:F0", b"Found device B2:D0:F0",
        b"enumeration and configuration complete, status OK [PCI_ERR_OK]",
        b"+ PCI Server v1.6 init complete, OK [PCI_ERR_OK] +",
    )
)


class MMXPCIeTests(unittest.TestCase):
    def test_live_endpoint_topology_and_exact_bar_assignments_required(self):
        native = SLOG.replace(
            b"PCIe 1 link is not active",
            b"++++ tk1_pcie_init() [PCIe 1] start ++++")
        native += (
            b"Found device B3:D0:F0\n"
            b"Setting MEM BAR 0 for B3:D0:F0 to 10000 (4000)\n"
            b"Setting MEM BAR 1 for B3:D0:F0 to 14000 (1000)\n"
            b"Setting MEM BAR 2 for B3:D0:F0 to 0 (10000)\n")
        probe.verify_native_server(
            native, SERVER_PROCESSES, DEVICE, endpoint=True)
        for missing in (b"Found device B3:D0:F0",
                        b"Setting MEM BAR 2 for B3:D0:F0 to 0 (10000)"):
            with self.subTest(missing=missing), self.assertRaises(RuntimeError):
                probe.verify_native_server(
                    native.replace(missing, b""), SERVER_PROCESSES,
                    DEVICE, endpoint=True)

    def test_requires_ordered_native_messages_and_live_initializer(self):
        probe.verify_native_setup(NATIVE, PROCESSES)
        for invalid in (
            NATIVE.replace(probe.CONTROLLER_ENABLED, b""),
            NATIVE.replace(probe.CHECKING_LINK, b""),
            NATIVE.replace(probe.LINK_RETRY, b""),
            NATIVE.replace(probe.CHECKING_LINK + b"\r\n" + probe.LINK_RETRY,
                           probe.LINK_RETRY + b"\r\n" + probe.CHECKING_LINK),
            NATIVE.replace(b"mh2p# PCIE", b"echo PCIE"),
            NATIVE + b"\nSIGBUS\n",
            NATIVE + b"\nPCIE: PCIE: port 0 up\n",
        ):
            with self.subTest(invalid=invalid), self.assertRaises(RuntimeError):
                probe.verify_native_setup(invalid, PROCESSES)
        for missing in (b" 9 /sbin/pcie_init -v -p0 -r25\n", b" 5 devg-nvclock\n"):
            with self.subTest(missing=missing), self.assertRaises(RuntimeError):
                probe.verify_native_setup(NATIVE, PROCESSES.replace(missing, b""))

    def test_shell_status_requires_complete_line_and_valid_background_separator(self):
        console = MagicMock()
        console.recv.side_effect = [b"\nMMX_PCIE_TEST:13", b"8\r\n"]
        with patch.object(probe.uuid, "uuid4", return_value=MagicMock(hex="TEST")), \
             patch("check_guest_graphics.select.select",
                   return_value=([console], [], [])):
            with self.assertRaisesRegex(RuntimeError, "138"):
                probe.execute(console, bytearray(), io.BytesIO(), "pcie_init", 1,
                              background=True)
        self.assertEqual(console.recv.call_count, 2)
        self.assertEqual(console.sendall.call_args.args[0],
                         b"pcie_init & echo MMX_PCIE_TEST:$?\n")

    def test_shell_status_accepts_interleaved_log_prefix_not_echo(self):
        console = MagicMock()
        console.recv.side_effect = [
            b"# true; echo MMX_PCIE_TEST:$?\r\n",
            b"asynchronous guest log jMMX_PCIE_TEST:0\r\n",
        ]
        with patch.object(probe.uuid, "uuid4", return_value=MagicMock(hex="TEST")), \
             patch("check_guest_graphics.select.select",
                   return_value=([console], [], [])):
            response = probe.execute(console, bytearray(), io.BytesIO(), "true", 1)
        self.assertEqual(console.recv.call_count, 2)
        self.assertIn(b"asynchronous guest log jMMX_PCIE_TEST:0", response)

    def test_shell_prefixed_nonzero_status_still_fails(self):
        console = MagicMock()
        console.recv.return_value = b"asynchronous logMMX_PCIE_TEST:7\r\n"
        with patch.object(probe.uuid, "uuid4", return_value=MagicMock(hex="TEST")), \
             patch("check_guest_graphics.select.select",
                   return_value=([console], [], [])):
            with self.assertRaisesRegex(RuntimeError, "7"):
                probe.execute(console, bytearray(), io.BytesIO(), "false", 1)

    def run_probe(self, directory, bad_register=None, timeout=False,
                  missing_volumes=False, reset_asserted=False, pci_server=False,
                  bad_slog=False, error_log=False, bad_aperture=False,
                  missing_aperture=False):
        media = Path(directory) / "media"
        media.mkdir()
        manifest = {"diagnostic_shell": True}
        for name, key in (("startup.bin", "startup_sha256"),
                          ("board.dtb", "dtb_sha256")):
            (media / name).write_bytes(name.encode())
            manifest[key] = hashlib.sha256(name.encode()).hexdigest()
        (media / "manifest.json").write_text(json.dumps(manifest))
        output = Path(directory) / "probe"
        vm = MagicMock()
        vm.__enter__.return_value = vm
        vm._qemu_full_args = ["qemu", "-M", "mh2p"]
        def launch():
            (output / "unimplemented.log").write_bytes(
                b"unsupported access\n" if error_log else b"")

        vm.launch.side_effect = launch
        vm.console_socket.recv.side_effect = [
            chunk for _ in range(2) for chunk in [
                # A stale previous prompt must not release the new boot's gate.
                b"mh2p# ", probe.BANNER + b"\n", b"mh2p# ",
                b"\n" + TOKEN,
                b"\n" + (b"ifs image checksum is valid\n" *
                          (5 if missing_volumes else 6)) + TOKEN,
                b"\n" + TOKEN,
                *([b"\n" + TOKEN] if pci_server else []),
                b"\n" + TOKEN + NATIVE,
                b"\n" + PROCESSES + TOKEN,
                *([
                    b"\n" + TOKEN, b"\n" + TOKEN,
                    b"\n" + SERVER_PROCESSES + TOKEN,
                    b"\n" + DEVICE + TOKEN,
                    b"\n" + (SLOG.replace(b"PCIe 1 link is not active", b"")
                             if bad_slog else SLOG) + TOKEN,
                ] if pci_server else []),
            ]
        ]

        def send(data):
            if (not pci_server and
                data.startswith(probe.PREREQUISITES[0].encode())):
                boot = vm.console_socket.sendall.call_count // 5
                self.assertEqual(vm.console_socket.recv.call_count, 3 + boot * 8)

        vm.console_socket.sendall.side_effect = send
        qtest = MagicMock()

        def read(command):
            address = int(command.split()[1], 0)
            if pci_server and (
                0x01003800 <= address <= 0x01003844 or
                0x01003934 <= address <= 0x01003954
            ):
                fields = {
                    0x01003804: 0x10000, 0x0100381c: 0x01100000,
                    0x01003834: 0xfe100000,
                }
                value = 0 if missing_aperture else fields.get(address, 0)
                return f"OK {value:#x}\n"
            if pci_server and 0x01100000 <= address < 0x11100000:
                value = {
                    "readb": 0xff, "readw": 0xffff, "readl": 0xffffffff,
                }[command.split()[0]]
                return f"OK {0 if bad_aperture else value:#x}\n"
            registers = (probe.SERVER_REGISTERS if pci_server
                         else probe.REGISTERS)
            expected = next(value for addr, value in registers.values()
                            if addr == address)
            if isinstance(expected, tuple):
                expected = expected[0 if reset_asserted else -1]
            if address == bad_register:
                expected ^= 2
            return f"OK {expected:#x}\n"

        qtest.cmd.side_effect = TimeoutError if timeout else read
        with patch.object(probe, "QEMUMachine", return_value=vm) as machine, \
             patch.object(probe, "QEMUQtestProtocol", return_value=qtest), \
             patch.object(probe, "digest", return_value="disk-digest"), \
             patch.object(probe.uuid, "uuid4", return_value=MagicMock(hex="TEST")), \
             patch("check_guest_graphics.select.select",
                   return_value=([vm.console_socket], [], [])):
            try:
                result = probe.probe(Path("qemu"), media, Path("nor"),
                                     Path("mnand"), output, 1,
                                     pci_server=pci_server)
            except (RuntimeError, TimeoutError):
                self.assertFalse((output / "result.json").exists())
                if timeout:
                    vm.kill.assert_called_once()
                raise
            finally:
                qtest.close.assert_called_once()
        return result, vm, machine, qtest

    def test_two_boots_verify_exact_registers_but_never_claim_connected_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            result, vm, machine, qtest = self.run_probe(directory)
        self.assertTrue(result["native_controller_setup_verified_across_reset"])
        self.assertEqual(len(result["boots"]), 2)
        for flag in ("rcc_peer_connected", "mmx_link_verified",
                     "endpoint_enumeration_verified"):
            self.assertFalse(result[flag])
        self.assertEqual(qtest.cmd.call_count, 2 * len(probe.REGISTERS))
        self.assertEqual([call.args for call in vm.cmd.call_args_list],
                         [("cont",), ("stop",), ("system_reset",),
                          ("cont",), ("stop",)])
        args = machine.call_args.kwargs["args"]
        self.assertIn("shift=0,align=off,sleep=on", args)
        self.assertIn("snapshot=on", " ".join(args))
        self.assertNotIn("tegra30-pcie", " ".join(args))
        self.assertEqual(vm.console_socket.sendall.call_count, 10)

    def test_native_reset_asserted_phase_is_also_valid(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _, _, _ = self.run_probe(directory, reset_asserted=True)
        self.assertTrue(all(boot["registers"]["port0_control"] == "0x1a"
                            for boot in result["boots"]))

    def test_every_bad_register_fails_without_success_report(self):
        for name, (address, _) in probe.REGISTERS.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(RuntimeError, "readback"):
                    self.run_probe(directory, bad_register=address)

    def test_timeout_terminates_only_probe_vm_and_preserves_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TimeoutError):
                self.run_probe(directory, timeout=True)

    def test_missing_stage_two_volume_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "six verified"):
                self.run_probe(directory, missing_volumes=True)

    def test_original_server_across_reset_is_not_endpoint_enumeration(self):
        with tempfile.TemporaryDirectory() as directory:
            result, vm, _, qtest = self.run_probe(directory, pci_server=True)
        self.assertTrue(result["native_root_enumeration_verified_across_reset"])
        self.assertFalse(result["endpoint_enumeration_verified"])
        self.assertFalse(result["mmx_link_verified"])
        self.assertTrue(all(boot["native_pci_server_initialized"]
                            for boot in result["boots"]))
        self.assertEqual(
            qtest.cmd.call_count, 2 * (len(probe.SERVER_REGISTERS) + 27 + 4))
        for boot in result["boots"]:
            self.assertEqual(boot["config_apertures"][0]["window"], 1)
            self.assertEqual(boot["config_apertures"][0]["axi_base"],
                             "0x1100000")
            self.assertEqual(boot["config_apertures"][0]["fpci_base"],
                             "0xfe10000000")
            self.assertEqual(boot["config_apertures"][0]["size_bytes"],
                             0x10000000)
        commands = [
            call.args[0] for call in vm.console_socket.sendall.call_args_list
        ]
        self.assertEqual(
            sum(b"waitfor /dev/pci 50;" in cmd for cmd in commands), 2)
        self.assertEqual(sum(b"PCI_HW_MODULE=" in cmd for cmd in commands), 2)
        self.assertFalse(any(b"&;" in cmd for cmd in commands))

    def test_server_requires_ordered_slog_live_process_and_real_directory(self):
        probe.verify_native_server(SLOG, SERVER_PROCESSES, DEVICE)
        for slog in (
            SLOG.replace(b"PCIe 0 link is not active", b""),
            SLOG.replace(b"Found device B2:D0:F0", b"Found device B1:D0:F0"),
            SLOG + b"Found device B1:D0:F0\n",
            SLOG.replace(b"Jan 01", b"echo Jan 01"),
            b"\n".join(reversed(SLOG.split(b"\n"))),
        ):
            with self.subTest(slog=slog), self.assertRaises(RuntimeError):
                probe.verify_native_server(slog, SERVER_PROCESSES, DEVICE)
        with self.assertRaisesRegex(RuntimeError, "not alive"):
            probe.verify_native_server(SLOG, PROCESSES, DEVICE)
        with self.assertRaisesRegex(RuntimeError, "/dev/pci"):
            probe.verify_native_server(
                SLOG, SERVER_PROCESSES, b"ls -ld /dev/pci\n")

    def test_server_failure_does_not_write_success_report(self):
        for kwargs in ({"bad_slog": True}, {"error_log": True},
                       {"bad_register": 0x0100386c}, {"bad_aperture": True},
                       {"missing_aperture": True}):
            with self.subTest(kwargs=kwargs), \
                 tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(RuntimeError):
                    self.run_probe(directory, pci_server=True, **kwargs)

    def test_config_aperture_diagnostic_uses_read_only_native_windows(self):
        qtest = MagicMock()

        def read(command):
            operation, address = command.split()
            address = int(address, 0)
            fields = {
                0x01003800: 1, 0x01003818: 0x3ffff000,
                0x01003830: 0xfe1ffff0,
            }
            value = ({"readb": 0xff, "readw": 0xffff, "readl": 0xffffffff}
                     [operation] if address >= 0x3ffff000
                     else fields.get(address, 0))
            return f"OK {value:#x}\n"

        qtest.cmd.side_effect = read
        result = probe.read_config_apertures(qtest)
        self.assertEqual(result[0]["axi_base"], "0x3ffff000")
        self.assertEqual(len(result[0]["diagnostic_absent_reads"]), 4)
        self.assertTrue(all(call.args[0].startswith("read")
                            for call in qtest.cmd.call_args_list))

    def test_invalid_timeout_precedes_filesystem_access(self):
        for timeout in (0, -1, 61):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                probe.probe(Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                            Path("unused"), timeout)


if __name__ == "__main__":
    unittest.main()
