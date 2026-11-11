#!/usr/bin/env python3
"""Firmware-free original RCC console evidence and failure-path checks."""
import io
import json
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import check_rcc_boot as check


MARKER = b"RCC_TEST"
PROCESSES = (
    b"       1 procnto-instr\n"
    b"       2 /proc/boot/devc-seromap -u3\n"
    b"    4099 /proc/boot/slogger -s 512k\n"
    b"    4100 ./usr/bin/ProcessManager\n"
    b"   86026 /bin/sh\n"
)
DIAGNOSTIC_PROCESSES = PROCESSES + (
    b" 36875 devf-generic -s0x08000000,64m -r -b9\n"
    b" 45068 inflator /App\n"
    b" 98318 io-pkt-v6-hc -ptcpip random\n"
    b" 516123 devc-seromap -u6 0x48068000^2,138\n"
)


def inventory_log(processes=PROCESSES):
    return (b"# echo RCC_TEST:BEGIN; pidin ar; echo RCC_TEST:END:$((6 * 7))\r\n"
            b"RCC_TEST:BEGIN\r\n" + processes.replace(b"\n", b"\r\n") +
            b"RCC_TEST:END:42\r\n# ")


class RCCConsoleTests(unittest.TestCase):
    def test_inventory_requires_executed_markers_and_original_processes(self):
        rows = check.process_inventory(inventory_log(), MARKER)
        self.assertEqual(rows[0], {"pid": 1, "arguments": "procnto-instr"})
        self.assertEqual(len(rows), 5)
        for invalid in (
            b"# echo RCC_TEST:BEGIN; pidin ar; echo RCC_TEST:END:42\n",
            inventory_log().replace(b"RCC_TEST:END:42", b"RCC_TEST:END:41"),
            inventory_log() + b"\nRCC_TEST:END:42\n",
            b"\nRCC_TEST:END:42\nRCC_TEST:BEGIN\n",
            inventory_log(PROCESSES.replace(b"./usr/bin/ProcessManager", b"other")),
        ):
            with self.subTest(log=invalid), self.assertRaises(RuntimeError):
                check.process_inventory(invalid, MARKER)

    def test_diagnostic_inventory_requires_live_flash_and_tcp_processes(self):
        with self.assertRaisesRegex(RuntimeError, "devf-generic"):
            check.process_inventory(inventory_log(), MARKER, True)
        with self.assertRaisesRegex(RuntimeError, "UART6"):
            check.process_inventory(
                inventory_log(DIAGNOSTIC_PROCESSES.rsplit(b" 516123", 1)[0]),
                MARKER, True)
        rows = check.process_inventory(inventory_log(DIAGNOSTIC_PROCESSES),
                                       MARKER, True)
        self.assertEqual(len(rows), 9)

    def test_console_roundtrip_requires_uart6_node_and_host_transmission(self):
        for node, transmitted in ((True, True), (False, True), (True, False)):
            with self.subTest(node=node, transmitted=transmitted), \
                 tempfile.TemporaryDirectory() as directory:
                uart6 = Path(directory) / "uart-5.log"
                uart6.write_bytes(b"RCC_AUDIT_TEST:UART6_TX:42\n"
                                  if transmitted else b"")
                console = MagicMock()
                audit = (b"\nRCC_AUDIT_TEST:UART6:42\n" if node else b"")
                console.recv.side_effect = [
                    b"\n# " + check.BANNER + b"\n",
                    b"# ",
                    inventory_log(DIAGNOSTIC_PROCESSES),
                    audit + b"\nRCC_AUDIT_TEST:42\n",
                ]

                def sendall(data):
                    if data.startswith(b"echo RCC_TEST:BEGIN"):
                        self.assertEqual(console.recv.call_count, 2)

                console.sendall.side_effect = sendall
                with patch.object(check.uuid, "uuid4",
                                  return_value=MagicMock(hex="TEST")), \
                     patch.object(check.select, "select",
                                  return_value=([console], [], [])):
                    if not node or not transmitted:
                        with self.assertRaises((RuntimeError, TimeoutError)):
                            check.check_console(console, io.BytesIO(), 0.01,
                                                True, uart6)
                    else:
                        evidence = check.check_console(console, io.BytesIO(),
                                                       1, True, uart6)
                        self.assertTrue(evidence["uart6_transmit_verified"])
                        self.assertEqual(console.sendall.call_count, 2)

    def test_console_receive_records_fragments_and_rejects_eof_or_timeout(self):
        reader, writer = socket.socketpair()
        try:
            writer.sendall(b"original console")
            log, output = bytearray(), io.BytesIO()
            check.receive_until(reader, log, output,
                                lambda data: b"console" in data, 1)
            self.assertEqual(output.getvalue(), b"original console")
            with self.assertRaises(TimeoutError):
                check.receive_until(reader, log, output,
                                    lambda data: False, 0.01)
            writer.close()
            with self.assertRaisesRegex(RuntimeError, "closed"):
                check.receive_until(reader, log, output,
                                    lambda data: False, 1)
        finally:
            reader.close()
            writer.close()

    def test_input_guards_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for timeout in (0, -1, 31, float("nan"), float("inf")):
                with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                    check.check(root / "qemu", root / "missing", root / "out",
                                timeout)
            with self.assertRaises(FileExistsError):
                check.check(root / "qemu", root / "missing", root)
            with self.assertRaises(FileNotFoundError):
                check.check(root / "qemu", root / "missing", root / "out")
            self.assertFalse((root / "out").exists())
            for count in (0, 11, True, 1.5, "3"):
                with self.subTest(cold_boots=count), self.assertRaises(ValueError):
                    check.check(root / "qemu", root / "missing", root / "out",
                                cold_boots=count)

    def test_reset_and_explicit_diagnostic_provenance(self):
        for diagnostic in (False, True):
            with self.subTest(diagnostic=diagnostic), \
                 tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vm = MagicMock()
                vm.__enter__.return_value = vm
                vm.get_pid.return_value = 2147483646
                vm._qemu_full_args = ("qemu", "-serial", "managed-console")
                vm.cmd.return_value = "stopped registers"
                with patch.object(check, "verify_image", return_value="verified"), \
                     patch.object(check, "QEMUMachine", return_value=vm) as factory, \
                     patch.object(check, "check_console",
                                  return_value={"console_roundtrip_verified": True}):
                    report = check.check(root / "qemu", root / "image",
                                         root / "out", 1, diagnostic)
                args = factory.call_args.kwargs["args"]
                self.assertEqual(check.FLASH_HANDOFF in args, diagnostic)
                self.assertEqual([call.args[0] for call in vm.cmd.call_args_list],
                                 ["cont", "stop", "human-monitor-command",
                                  "system_reset", "cont", "stop",
                                  "human-monitor-command"])
                vm.set_console.assert_called_once_with(console_index=2)
                vm._qmp.settimeout.assert_called_once_with(5)
                self.assertTrue(report["console_verified_across_reset"])
                self.assertEqual(report["command"], list(vm._qemu_full_args))
                self.assertFalse(report["full_rcc_services_verified"])
                if diagnostic:
                    self.assertFalse(report["handoff_fixture"]["producer_verified"])
                else:
                    self.assertIsNone(report["handoff_fixture"])
                self.assertEqual(json.loads((root / "out/result.json").read_text()),
                                 report)

    def test_distinct_cold_hosts_each_reset_and_incomplete_repeat_rejected(self):
        for reuse_pid in (False, True):
            with self.subTest(reuse_pid=reuse_pid), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                vms = [MagicMock() for _ in range(3)]
                for index, vm in enumerate(vms):
                    vm.__enter__.return_value = vm
                    vm.get_pid.return_value = 2147483646 if reuse_pid else 2147483646 - index
                    vm._qemu_full_args = ("qemu",)
                    vm.cmd.return_value = "original registers"
                with patch.object(check, "verify_image", return_value="verified"), \
                     patch.object(check, "QEMUMachine", side_effect=vms) as factory, \
                     patch.object(check, "check_console",
                                  return_value={"console_roundtrip_verified": True}) as console:
                    if reuse_pid:
                        with self.assertRaisesRegex(RuntimeError, "reused"):
                            check.check(root / "qemu", root / "image", root / "out",
                                        cold_boots=3)
                        self.assertFalse((root / "out/result.json").exists())
                    else:
                        result = check.check(root / "qemu", root / "image", root / "out",
                                             cold_boots=3)
                        self.assertEqual(console.call_count, 6)
                        self.assertEqual(result["cold_boot_count"], 3)
                        self.assertEqual([boot["cold_boot"] for boot in result["boots"]],
                                         [0, 0, 1, 1, 2, 2])
                        self.assertEqual([boot["system_reset"] for boot in result["boots"]],
                                         [False, True, False, True, False, True])
                        self.assertTrue(result["all_owned_hosts_absent"])
                        trace_paths = [
                            call.kwargs["args"][call.kwargs["args"].index("-D") + 1]
                            for call in factory.call_args_list
                        ]
                        self.assertEqual(len(set(trace_paths)), 3)
                        for vm in vms:
                            vm.cmd.assert_any_call("system_reset")
                            vm.__exit__.assert_called_once()
                        for index, vm in enumerate(vms):
                            owned = json.loads(
                                (root / f"out/owned-host-{index}.json").read_text())
                            self.assertEqual(owned["qemu_pid"], vm.get_pid.return_value)
                            self.assertEqual(owned["command"], list(vm._qemu_full_args))

    def test_invalid_owned_pid_and_later_cold_failure_close_hosts(self):
        for pid in (None, 0, -1, True, 1.5):
            with self.subTest(pid=pid), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                vm = MagicMock()
                vm.__enter__.return_value = vm
                vm.get_pid.return_value = pid
                with patch.object(check, "verify_image", return_value="verified"), \
                     patch.object(check, "QEMUMachine", return_value=vm):
                    with self.assertRaisesRegex(RuntimeError, "Missing"):
                        check.check(root / "qemu", root / "image", root / "out")
                vm.__exit__.assert_called_once()
                self.assertFalse((root / "out/result.json").exists())
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            vms = [MagicMock(), MagicMock()]
            for index, vm in enumerate(vms):
                vm.__enter__.return_value = vm
                vm.get_pid.return_value = 2147483646 - index
                vm._qemu_full_args = ("qemu",)
                vm.cmd.return_value = "original registers"
            with patch.object(check, "verify_image", return_value="verified"), \
                 patch.object(check, "QEMUMachine", side_effect=vms), \
                 patch.object(check, "check_console",
                              side_effect=[{}, {}, RuntimeError("later cold failure")]):
                with self.assertRaisesRegex(RuntimeError, "later cold failure"):
                    check.check(root / "qemu", root / "image", root / "out", cold_boots=2)
            for vm in vms:
                vm.__exit__.assert_called_once()
            self.assertFalse((root / "out/result.json").exists())

    def test_owned_host_liveness_failure_does_not_report_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm.get_pid.return_value = 2147483646
            vm._qemu_full_args = ("qemu",)
            vm.cmd.return_value = "original registers"
            with patch.object(check, "verify_image", return_value="verified"), \
                 patch.object(check, "QEMUMachine", return_value=vm), \
                 patch.object(check, "check_console", return_value={}), \
                 patch.object(check.Path, "exists", side_effect=[False, True]):
                with self.assertRaisesRegex(RuntimeError, "remains"):
                    check.check(root / "qemu", root / "image", root / "out")
            self.assertFalse((root / "out/result.json").exists())

    def test_failed_second_boot_does_not_write_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vm = MagicMock()
            vm.__enter__.return_value = vm
            vm.get_pid.return_value = 2147483646
            vm.cmd.return_value = "stopped registers"
            with patch.object(check, "verify_image", return_value="verified"), \
                 patch.object(check, "QEMUMachine", return_value=vm), \
                 patch.object(check, "check_console",
                              side_effect=[{}, TimeoutError("reset stalled")]), \
                 self.assertRaisesRegex(TimeoutError, "reset stalled"):
                check.check(root / "qemu", root / "image", root / "out")
            vm.kill.assert_called_once()
            self.assertFalse((root / "out/result.json").exists())
            self.assertTrue((root / "out/registers-0.log").exists())


if __name__ == "__main__":
    unittest.main()
