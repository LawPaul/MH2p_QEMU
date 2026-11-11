#!/usr/bin/env python3
"""Firmware-free checks of paired-probe provenance and failure handling."""
from pathlib import Path
import io
import json
import shlex
import struct
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import probe_pcie_sideband as probe


class PairedProbeTests(unittest.TestCase):
    QDB_READY = b"".join(
        b"[n]@" + name + b"\nStatus::Valid\n"
        for name in (b"persisDB", b"persisDB_BAT", b"persisDB_IMM", b"persisDB_TMP"))
    def run_probe(self, directory, timeout=False, unsupported=False,
                  config_transport=False, native_link=False, pci_server=False,
                  crossboard_init=False, network=False, asi=False,
                  asi_failure=False, ipu_fixture=False, ipu_bad_cpus=False,
                  ipu_fatal=False, ipu_capture_failure=False,
                  ipu_fault=False, ipu_fault_capture_failure=False,
                  ipu_native=False, ipu_no_trace=False, ipu_no_mmu=False,
                  ipu_trace_exception=False, ipu_trace_capture_failure=False,
                  ipu_sensor=False, ipu_uart_watchdog=False,
                  native_rstp=False, native_hid=False,
                  manual_startup_isolation=False,
                  rcc_icount_shift=0,
                  mnand_overlay=None,
                  ipu_shared_words=(0, 0, 0, 0, 0),
                  ipu_gpio6_data=0,
                  ipu_subcpu_tx=b""):
        output = Path(directory) / "probe"
        machines = [MagicMock(), MagicMock()]
        tests = [MagicMock(), MagicMock()]
        for vm in machines:
            vm.__enter__.return_value = vm
            vm._qemu_full_args = ["qemu", "paired"]
        if ipu_fixture or ipu_native:
            def monitor(command, **kwargs):
                if command == "query-cpus-fast":
                    wire = output / "rcc-subcpu-uart-wire.bin"
                    with wire.open("ab") as log:
                        log.write(ipu_subcpu_tx)
                    return [{}] * (2 if ipu_bad_cpus else 3)
                if command == "human-monitor-command":
                    parts = shlex.split(kwargs["command_line"])
                    if parts[0] in ("pmemsave", "memsave"):
                        fault = int(parts[1], 0) == 0xe000ed28
                        if not ((not fault and ipu_capture_failure) or
                                (fault and ipu_fault_capture_failure) or
                                (int(parts[1], 0) == 0x80800000 and
                                 ipu_trace_capture_failure)):
                            content = bytes(int(parts[2], 0))
                            if (int(parts[1], 0) == 0x80800000 and
                                    not ipu_no_trace):
                                text = b"sys/bios startup is done..."
                                if ipu_trace_exception:
                                    text += (
                                        b"\nassertion failure: A_bufOverflow")
                                content = text.ljust(len(content), b"\0")
                            if fault and ipu_fault and kwargs["cpu-index"] == 2:
                                content = struct.pack(
                                    "<5I", 0x8200, 0x40000000, 0, 0, 0x480b8110)
                            Path(parts[3]).write_bytes(content)
                        return ""
                return "R15=00068644" if ipu_fatal else "R15=0009c74c"
            machines[1].cmd.side_effect = monitor
        def launch(name):
            (output / f"{name}-unimplemented.log").write_text(
                "unsupported test access\n" if unsupported else "")
            if name == "rcc" and ipu_native:
                (output / "rcc-m4-uart7.log").write_bytes(
                    b"[MAWD] >>> M4 Application WatchDog Failed!!!!!"
                    if ipu_uart_watchdog else b"")
            if name == "rcc" and (ipu_native or ipu_fixture):
                (output / "rcc-subcpu-uart-wire.bin").write_bytes(b"")

        for name, vm in zip(("mmx", "rcc"), machines):
            vm.launch.side_effect = lambda name=name: launch(name)
        from probe_mmx_pcie import REGISTERS
        registers = {
            address: (expected[0] if isinstance(expected, tuple) else expected)
            for address, expected in REGISTERS.values()
        }
        if config_transport:
            registers.update({0x01001f00: 0x40000000, 0x01001090: 0x20110000,
                              0x01003910: 0x1b})
        tests[0].cmd.side_effect = lambda command: (
            f"OK {registers.get(int(command.split()[1], 0), 0):#x}\n")
        tests[1].cmd.return_value = "OK 0x0\n"
        if pci_server:
            endpoint_registers = {
                0x51000000: 0xb801104c, 0x51000010: 0x10000,
                0x51000014: 0x14000,
            }
            endpoint_registers.update(zip(
                range(0x8cbff000, 0x8cbff014, 4), ipu_shared_words))
            endpoint_registers[probe.RCC_GPIO6_DATAIN] = ipu_gpio6_data
            if ipu_native and not ipu_no_mmu:
                endpoint_registers.update({
                    0x58882044: 6, 0x5888204c: 0x8ea00000})
            def read_endpoint(command):
                parts = command.split()
                address = int(parts[1], 0)
                if parts[0] == "writel":
                    endpoint_registers[address] = int(parts[2], 0)
                    return "OK"
                return f"OK {endpoint_registers.get(address, 0):#x}"
            tests[1].cmd.side_effect = read_endpoint

        def execute(console, captured, log, command, limit, background=False):
            if command == probe.PREREQUISITES[1]:
                captured.extend(b"ifs image checksum is valid\n" * 6)
            if command == "/sbin/pcie_init -v -p0 -r25" and native_link:
                registers.update({
                    address: (expected[0] if isinstance(expected, tuple)
                              else expected)
                    for address, expected in REGISTERS.values()
                })
                registers[0x01001f00] = 0x40000000
                registers[0x01001090] = 0x20110000
                registers[0x01003910] = 0x1b
            if command == "wait $!":
                captured.extend(b"\nPCIE: PCIE: port 0 up\n")
                registers.update(probe.POST_LINK_REGISTERS)
            if command == probe.CROSSBOARD_COMMAND:
                endpoint_registers.update({
                    0x51000000 + offset: value
                    for offset, value in probe.CROSSBOARD_REGISTERS.items()
                })
            return b"original process inventory"

        def endpoint(console, qtest, log, limit, release_from_mmx, *,
                     link_peer=False):
            self.assertEqual(link_peer, config_transport)
            if timeout:
                raise TimeoutError("bounded native failure")
            release_from_mmx()
            return {"mmx_reset_release_observed": True}

        def connect_uart(mmx, rcc, directory, destination, input_peer=False):
            for name in ("mmx", "rcc"):
                (destination / f"{name}-uart-wire.bin").write_bytes(b"")

        with patch.object(probe, "verify_media", return_value={
                "startup_sha256": "startup", "dtb_sha256": "dtb"}), \
             patch.object(probe, "verify_image", return_value="rcc-hash"), \
             patch.object(probe, "digest", side_effect=lambda path:
                          probe.RCC_DEFAULTS_NOR_SHA256
                          if path.name == "rcc-nor" else
                          probe.RCC_IPU1_SHA256 if path.name == "ipu1"
                          else "source-hash"), \
             patch.object(probe, "stage_native_ipu_nor") as stage, \
             patch.object(probe, "QEMUMachine",
                          side_effect=machines) as create, \
             patch.object(probe, "QEMUQtestProtocol", side_effect=tests), \
             patch.object(probe, "wait_console"), \
             patch.object(probe, "execute", side_effect=execute), \
             patch.object(probe, "verify_native_setup"), \
             patch.object(probe, "verify_native_server"), \
             patch.object(probe, "verify_network", return_value={
                 "bidirectional_icmp_verified": True}) as check_network, \
             patch.object(probe, "verify_native_asi", return_value={
                 "agent_start_verified": True},
                 side_effect=RuntimeError("native ASI failed")
                 if asi_failure else None) as check_asi, \
             patch.object(probe, "connect_native_uart",
                          side_effect=connect_uart) as connect, \
             patch.object(probe, "observe_native_rstp", return_value={
                 "rstp_handshake_verified": False}) as observe, \
             patch.object(probe, "observe_native_hid", return_value={
                 "native_touch_delivery_verified": False}) as observe_hid, \
             patch.object(probe, "check_endpoint", side_effect=endpoint), \
             patch.object(probe, "verify_configuration",
                          return_value={"external_config": "diagnostic"}):
            try:
                result = probe.probe(
                    Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                    Path("image"), output, config_transport=config_transport,
                    native_link=native_link, pci_server=pci_server,
                    crossboard_init=crossboard_init, network=network,
                    asi=asi, rcc_nor=Path("rcc-nor") if asi else None,
                    experimental_rcc_ipu1_fixture=Path("ipu1")
                    if ipu_fixture else None,
                    experimental_rcc_ipu1_native=Path("ipu1")
                    if ipu_native else None,
                    experimental_rcc_ipu1_sensor_fixture=ipu_sensor,
                    experimental_native_rstp=native_rstp,
                    experimental_native_hid=native_hid,
                    experimental_rcc_manual_startup_isolation=
                    manual_startup_isolation,
                    experimental_rcc_icount_shift=rcc_icount_shift,
                    mnand_overlay=mnand_overlay)
                self.assertEqual(stage.call_count, 1 if ipu_native else 0)
                self.assertEqual(check_network.call_count, 2 if network else 0)
                self.assertEqual(check_asi.call_count, 2 if asi else 0)
                self.assertEqual(connect.call_count, 1 if native_rstp else 0)
                self.assertEqual(observe.call_count, 2 if native_rstp else 0)
                self.assertEqual(observe_hid.call_count, 2 if native_hid else 0)
                for call in check_asi.call_args_list:
                    self.assertEqual(call.kwargs["wait_timeout"],
                                     600 if ipu_fixture or ipu_native else None)
                    self.assertEqual(call.kwargs["native_ipu1"], ipu_native)
                    self.assertEqual(call.kwargs["manual_startup_isolation"],
                                     manual_startup_isolation)
            finally:
                if (timeout or unsupported or asi_failure or ipu_bad_cpus or
                        ipu_capture_failure or ipu_fault_capture_failure or
                        ipu_uart_watchdog):
                    self.assertFalse((output / "result.json").exists())
                if timeout:
                    for vm in machines:
                        vm.kill.assert_called_once_with()
                for qtest in tests:
                    qtest.close.assert_called_once_with()
        return result, create, machines, tests

    def test_persistent_mnand_is_opt_in_validated_and_not_snapshotted(self):
        for persistent in (False, True):
            with self.subTest(persistent=persistent), \
                    tempfile.TemporaryDirectory() as directory, \
                    patch("mnand_overlay.validate",
                          return_value=Path("/private/disk/mnand.qcow2")) as validate:
                report, create, _, _ = self.run_probe(
                    directory, mnand_overlay=Path("/private/disk") if persistent else None)
                args = create.call_args_list[0].kwargs["args"]
                drive = args[args.index("-drive") + 1]
                if persistent:
                    validate.assert_called_once_with(
                        Path("mnand"), Path("/private/disk"), "source-hash")
                    self.assertEqual(drive,
                                     "if=sd,index=0,format=qcow2,file=/private/disk/mnand.qcow2")
                    self.assertEqual(report["persistent_mnand_overlay"],
                                     "/private/disk/mnand.qcow2")
                else:
                    validate.assert_not_called()
                    self.assertIn("format=raw,snapshot=on", drive)
                    self.assertIsNone(report["persistent_mnand_overlay"])
                self.assertFalse(report["durable_rcc_persistence_verified"])

    def test_rcc_icount_shift_is_board_local_and_recorded(self):
        for shift in range(5):
            with self.subTest(shift=shift), \
                    tempfile.TemporaryDirectory() as directory:
                report, create, machines, _ = self.run_probe(
                    directory, rcc_icount_shift=shift)
                for call, expected in zip(create.call_args_list, (0, shift)):
                    args = call.kwargs["args"]
                    self.assertEqual(args[args.index("-icount") + 1],
                                     f"shift={expected},align=off,sleep=on")
                for machine in machines:
                    self.assertEqual(machine.launch.call_count, 1)
                    machine.cmd.assert_any_call("system_reset")
                self.assertEqual(report["icount"], {
                    "mmx_shift": 0, "rcc_shift": shift,
                    "align": False, "sleep": True,
                    "hardware_frequency_calibration_verified": False,
                })
                self.assertFalse(report["full_rcc_services_verified"])
                if shift:
                    self.assertIn("experimental timing calibration",
                                  report["note"])

    def test_rcc_icount_shift_rejects_invalid_values_before_media_access(self):
        paths = [Path(name) for name in
                 ("qemu", "media", "nor", "mnand", "image", "output")]
        for shift in (-1, 5, "3", 1.0, True, None):
            with self.subTest(shift=shift), \
                    patch.object(probe, "verify_media") as verify, \
                    self.assertRaisesRegex(ValueError, "RCC icount shift"):
                probe.probe(*paths, experimental_rcc_icount_shift=shift)
            verify.assert_not_called()

    def test_rcc_icount_shift_cli_defaults_to_zero_and_forwards_opt_in(self):
        arguments = [
            "probe", "--media", "media", "--nor", "nor", "--mnand", "mnand",
            "--image", "image", "--output", "output",
        ]
        for extra, shift in (([], 0),
                             (["--experimental-rcc-icount-shift", "3"], 3)):
            with patch.object(probe.sys, "argv", arguments + extra), \
                    patch.object(probe, "probe", return_value={}) as run, \
                    patch("builtins.print"):
                probe.main()
            self.assertEqual(
                run.call_args.kwargs["experimental_rcc_icount_shift"], shift)
        with patch.object(probe.sys, "argv", arguments + [
                "--experimental-rcc-icount-shift", "5"]), \
                patch.object(probe, "probe") as run, \
                patch("sys.stderr", new_callable=io.StringIO), \
                self.assertRaises(SystemExit):
            probe.main()
        run.assert_not_called()

    def test_native_rstp_requires_native_ipu1(self):
        with self.assertRaisesRegex(ValueError, "requires native IPU1"):
            probe.probe(
                Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                Path("image"), Path("output"), experimental_native_rstp=True)

    def test_native_rstp_observes_both_boots_without_promoting_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, _ = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_sensor=True, native_rstp=True)
        for boot in report["boots"]:
            self.assertFalse(boot["native_rstp_observation"]
                             ["rstp_handshake_verified"])
        self.assertFalse(report["full_rcc_services_verified"])
        self.assertIn("LP_GPIO_ST=0", report["note"])

    def test_native_hid_requires_native_rstp(self):
        with self.assertRaisesRegex(ValueError, "requires native RSTP"):
            probe.probe(
                Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                Path("image"), Path("output"), experimental_native_hid=True)

    def test_native_hid_runs_on_both_boots_without_promoting_touch(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, _ = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_sensor=True, native_rstp=True,
                native_hid=True)
        for boot in report["boots"]:
            self.assertFalse(boot["native_hid_observation"]
                             ["native_touch_delivery_verified"])
        self.assertFalse(report["full_rcc_services_verified"])
        self.assertIn("No input packet is injected", report["note"])

    @staticmethod
    def hid_config(original=None, diagnostic=None):
        if original is None:
            original = {"startup": [], "restart_enabled": True,
                        "spawnfactory": {"poolsize": 4}}
        if diagnostic is None:
            diagnostic = dict(original, startup=probe.HID_STARTUP_JOBS)
        return (
            b"HID_ORIGINAL_CONFIG_BEGIN\r\n" +
            json.dumps(original).encode() +
            b"\r\nHID_ORIGINAL_CONFIG_END\r\n"
            b"HID_DIAGNOSTIC_CONFIG_BEGIN\r\n" +
            json.dumps(diagnostic).encode() +
            b"\r\nHID_DIAGNOSTIC_CONFIG_END\r\n")

    @staticmethod
    def hid_inventory(processes=b"123 /eso/bin/apps/hidcontroller "
                      b"-sm-name hidcontroller -sm-id 366\r\n",
                      manager=b"42 ./bin/servicemgrmibhigh\r\n",
                      descriptors=b""):
        return (
            b"HID_MANAGER_ALIVE:0\r\nHID_MANAGER_PID:42\r\n"
            b"HID_PROCESS_INVENTORY_BEGIN\r\n" + manager + processes +
            b"HID_PROCESS_INVENTORY_END\r\n"
            b"HID_DESCRIPTOR_INVENTORY_BEGIN\r\n" + descriptors +
            b"HID_DESCRIPTOR_INVENTORY_END\r\n")

    def test_native_hid_separates_liveness_registration_and_device_open(self):
        for registered, opened in ((False, False), (True, False), (True, True)):
            with self.subTest(registered=registered, opened=opened), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                vm, rcc = MagicMock(), MagicMock()
                text = self.hid_inventory() + b"    input = none\r\n"
                if registered:
                    text += (b"HidTouchMMXImpl: service registered\r\n"
                             b"RemKeypanelMMXImpl: service registered\r\n")
                if opened:
                    text += b"Open RSTP device was successful\r\n"
                with patch.object(probe, "execute",
                                  side_effect=[self.hid_config(),
                                               b"launch\n", text]) as run:
                    result = probe.observe_native_hid(
                        vm, rcc, bytearray(), io.BytesIO(), 30, output, 1)
                self.assertTrue(result["process_alive_observed"])
                self.assertTrue(
                    result["service_manager_process_alive_observed"])
                self.assertEqual(result["process_ids_observed"], [123])
                self.assertEqual(result["diagnostic_service_manager_startup"],
                                 probe.HID_STARTUP_JOBS)
                self.assertTrue(
                    result["other_service_manager_settings_preserved"])
                self.assertTrue(result["screen_input_none_observed"])
                self.assertEqual(
                    result["hid_touch_service_registration_observed"],
                    registered)
                self.assertEqual(
                    result["remote_keypanel_service_registration_observed"],
                    registered)
                self.assertEqual(result["rstp_device_open_observed"], opened)
                self.assertEqual(
                    result["rstp_device_open_message_observed"], opened)
                self.assertEqual(
                    result["rstp_keypanel_descriptors_observed"], [])
                self.assertFalse(result["host_input_packets_injected"])
                self.assertFalse(result["native_touch_delivery_verified"])
                self.assertFalse(result["full_hmi_or_touch_verified"])
                self.assertEqual(
                    json.loads((output / "hid-boot-1.json").read_text()),
                    result)
                self.assertEqual((output / "hid-boot-1.log").read_bytes(),
                                 self.hid_config() + b"launch\n" + text)
                configuration = run.call_args_list[0].args[3]
                self.assertIn("cp /etc/eso/production/*.json /tmp/",
                              configuration)
                self.assertIn('"startup":[' + json.dumps(
                    probe.HID_STARTUP_JOBS, separators=(",", ":"))[1:-1],
                    configuration)
                self.assertIn("IPL_CONFIG_DIR=/tmp ./bin/servicemgrmibhigh",
                              run.call_args_list[1].args[3])
                for call in run.call_args_list:
                    self.assertNotIn("-sm-id", call.args[3])
                    self.assertNotIn("mkdir", call.args[3])
                for machine in (vm, rcc):
                    self.assertEqual(
                        [call.args[0] for call in machine.cmd.call_args_list],
                        ["cont", "stop"])

    def test_native_hid_detects_real_rstp_descriptors_without_log_message(self):
        for modes in ((b"-w", b"r-"), (b"rw",), (b"r-",), (b"-w",)):
            with self.subTest(modes=modes), \
                    tempfile.TemporaryDirectory() as directory:
                listing = b"     pid name\r\n  123 apps/hidcontroller\r\n"
                for index, mode in enumerate(modes, 7):
                    listing += (f"\t{index} 438294 ".encode() + mode +
                                b" 0 /dev/rstp/keypanel\r\n")
                with patch.object(probe, "execute", side_effect=[
                        self.hid_config(), b"launch\n",
                        self.hid_inventory(descriptors=listing)]):
                    result = probe.observe_native_hid(
                        MagicMock(), MagicMock(), bytearray(), io.BytesIO(),
                        30, Path(directory), 0)
                self.assertTrue(result["rstp_device_open_observed"])
                self.assertFalse(result["rstp_device_open_message_observed"])
                self.assertEqual(
                    result["rstp_keypanel_descriptors_observed"],
                    [{"fd": index, "server_pid": 438294, "mode": mode.decode()}
                     for index, mode in enumerate(modes, 7)])
                self.assertEqual(
                    result["rstp_keypanel_read_descriptor_observed"],
                    any(mode[:1] == b"r" for mode in modes))
                self.assertEqual(
                    result["rstp_keypanel_write_descriptor_observed"],
                    any(mode[1:] == b"w" for mode in modes))
                self.assertFalse(result["native_touch_delivery_verified"])
                self.assertFalse(result["full_hmi_or_touch_verified"])

    def test_native_hid_requires_exact_descriptor_owner_and_path(self):
        listing = (b"123 apps/hidcontroller\r\n"
                   b"7 438294 rw 0 /dev/rstp/keypanel\r\n")
        for invalid in (
                listing.replace(b"123", b"456"),
                listing.replace(b"apps/hidcontroller", b"apps/other"),
                listing.replace(b"/keypanel", b"/keypanel-other"),
                listing.replace(b"7 438294", b"echo 7 438294"),
                listing.replace(b"7 438294", b"7s 438294"),
                b"123 apps/hidcontroller\r\n" +
                listing.replace(b"123", b"456"),
                b"123 apps/hidcontroller\r\n" +
                listing.replace(b"123 apps/hidcontroller", b"456 apps/other")):
            with self.subTest(invalid=invalid), \
                    tempfile.TemporaryDirectory() as directory:
                with patch.object(probe, "execute", side_effect=[
                        self.hid_config(), b"launch\n",
                        self.hid_inventory(descriptors=invalid)]):
                    result = probe.observe_native_hid(
                        MagicMock(), MagicMock(), bytearray(), io.BytesIO(),
                        30, Path(directory), 0)
                self.assertFalse(result["rstp_device_open_observed"])
                self.assertFalse(
                    result["rstp_keypanel_read_descriptor_observed"])
                self.assertFalse(
                    result["rstp_keypanel_write_descriptor_observed"])

    def test_native_hid_dead_process_retains_failure_evidence(self):
        for processes in (
                b"", b"123 /eso/bin/apps/not-hidcontroller\r\n",
                b"echo 123 /eso/bin/apps/hidcontroller\r\n",
                b"123 /eso/bin/apps/hidcontroller-other\r\n",
                b"123 /eso/bin/apps/hidcontroller\r\n"
                b"456 /eso/bin/apps/hidcontroller\r\n"):
            with self.subTest(processes=processes), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                with patch.object(probe, "execute", side_effect=[
                        self.hid_config(), b"launch\n",
                        self.hid_inventory(processes) +
                        b"echo HID_PROCESS_ALIVE:0\r\n"
                        b"HID_PROCESS_ALIVE:0\r\n"]):
                    with self.assertRaisesRegex(RuntimeError, "not alive"):
                        probe.observe_native_hid(
                            MagicMock(), MagicMock(), bytearray(),
                            io.BytesIO(), 30, output, 0)
                result = json.loads((output / "hid-boot-0.json").read_text())
                self.assertFalse(result["process_alive_observed"])
                self.assertTrue((output / "hid-boot-0.log").exists())

    def test_native_hid_requires_observed_manager_identity(self):
        for manager in (b"", b"43 ./bin/servicemgrmibhigh\r\n",
                        b"42 ./bin/not-servicemgrmibhigh\r\n"):
            with self.subTest(manager=manager), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                with patch.object(probe, "execute", side_effect=[
                        self.hid_config(), b"launch\n",
                        self.hid_inventory(manager=manager)]):
                    with self.assertRaisesRegex(RuntimeError, "not alive"):
                        probe.observe_native_hid(
                            MagicMock(), MagicMock(), bytearray(),
                            io.BytesIO(), 30, output, 0)
                result = json.loads((output / "hid-boot-0.json").read_text())
                self.assertTrue(result["process_alive_observed"])
                self.assertFalse(
                    result["service_manager_process_alive_observed"])

    def test_native_hid_rejects_changed_manager_settings_before_launch(self):
        original = {"startup": [], "restart_enabled": True}
        for before, after in (
                (original, dict(original, startup=[])),
                (original, dict(original, startup=probe.HID_STARTUP_JOBS,
                                restart_enabled=False)),
                ({"startup": [{"cmd": "other"}]},
                 {"startup": probe.HID_STARTUP_JOBS}),
                ([], {"startup": probe.HID_STARTUP_JOBS})):
            with self.subTest(before=before, after=after), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                response = self.hid_config(before, after)
                with patch.object(probe, "execute",
                                  return_value=response) as run:
                    with self.assertRaisesRegex(RuntimeError,
                                                "manager configuration"):
                        probe.observe_native_hid(
                            MagicMock(), MagicMock(), bytearray(),
                            io.BytesIO(), 30, output, 0)
                self.assertEqual(run.call_count, 1)
                self.assertEqual((output / "hid-boot-0.log").read_bytes(),
                                 response)
                self.assertFalse((output / "hid-boot-0.json").exists())

    def test_native_hid_requires_framed_configuration_evidence(self):
        for response in (b"", b"echo HID_ORIGINAL_CONFIG_BEGIN\r\n",
                         self.hid_config().replace(b'"startup"', b'"bad')):
            with self.subTest(response=response), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                with patch.object(probe, "execute",
                                  return_value=response) as run:
                    with self.assertRaises((RuntimeError, ValueError)):
                        probe.observe_native_hid(
                            MagicMock(), MagicMock(), bytearray(),
                            io.BytesIO(), 30, output, 0)
                self.assertEqual(run.call_count, 1)
                self.assertTrue((output / "hid-boot-0.log").exists())

    def test_native_hid_stops_both_vms_on_console_failure(self):
        vm, rcc = MagicMock(), MagicMock()
        captured = bytearray()
        def fail(*args):
            captured.extend(b"partial native console output")
            raise TimeoutError("console")
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(probe, "execute", side_effect=fail):
            output = Path(directory)
            with self.assertRaises(TimeoutError):
                probe.observe_native_hid(
                    vm, rcc, captured, io.BytesIO(), 30, output, 0)
            self.assertEqual((output / "hid-boot-0.log").read_bytes(),
                             b"partial native console output")
        for machine in (vm, rcc):
            commands = [call.args[0] for call in machine.cmd.call_args_list]
            self.assertEqual(commands, ["cont", "stop"])

    def test_connect_uart_uses_unused_serial0_and_verifies_frontends(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = str(root / "native-uart.sock")
            machines = [MagicMock(), MagicMock()]
            for name, vm in zip(("mmx", "rcc"), machines):
                (root / f"{name}-uart-wire.bin").write_bytes(b"")
                vm.cmd.side_effect = [
                    [{"label": "serial0", "filename": "null"}], {},
                    [{"label": "serial0", "filename": f"unix:{path}",
                      "frontend-open": True}]]
            probe.connect_native_uart(*machines, root, root)
            for name, vm in zip(("mmx", "rcc"), machines):
                call = vm.cmd.call_args_list[1]
                self.assertEqual(call.args, ("chardev-change",))
                self.assertEqual(call.kwargs["id"], "serial0")
                backend = call.kwargs["backend"]
                self.assertEqual(backend["type"], "socket")
                options = backend["data"]
                self.assertEqual(options["addr"],
                                 {"type": "unix", "data": {"path": path}})
                self.assertEqual(options["server"], name == "mmx")
                self.assertEqual("wait" in options, name == "mmx")
                self.assertEqual(options.get("wait", False), False)
                self.assertEqual(options["logfile"],
                                 str(root / f"{name}-uart-wire.bin"))

    def test_connect_uart_input_peer_serves_mmx_from_qemu(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mmx, rcc = MagicMock(), MagicMock()
            mmx.cmd.side_effect = [
                [{"label": "serial0", "filename": "null"}], {},
                [{"label": "serial0", "filename": "mh2p-rstp",
                  "frontend-open": True}]]
            rcc.cmd.side_effect = [[{"label": "serial0", "filename": "null"}]]
            probe.connect_native_uart(mmx, rcc, root, root, input_peer=True)
            call = mmx.cmd.call_args_list[1]
            self.assertEqual(call.args, ("chardev-change",))
            self.assertEqual(call.kwargs["backend"], {
                "type": "mh2p-rstp",
                "data": {"logfile": str(root / "mmx-uart-wire.bin")}})
            self.assertEqual(rcc.cmd.call_count, 1)
            self.assertTrue((root / "rcc-uart-wire.bin").is_file())

    def test_connect_uart_rejects_in_use_port_before_changing_either_vm(self):
        mmx, rcc = MagicMock(), MagicMock()
        mmx.cmd.return_value = [{"label": "serial0", "filename": "null"}]
        rcc.cmd.return_value = [{"label": "serial0", "filename": "console"}]
        with self.assertRaisesRegex(RuntimeError, "unused rcc UART0"):
            probe.connect_native_uart(mmx, rcc, Path("socket"), Path("output"))
        mmx.cmd.assert_called_once_with("query-chardev")
        rcc.cmd.assert_called_once_with("query-chardev")

    def test_connect_uart_rejects_detached_frontend_or_missing_capture(self):
        for frontend, capture in ((False, True), (True, False)):
            with self.subTest(frontend=frontend, capture=capture), \
                    tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                mmx, rcc = MagicMock(), MagicMock()
                mmx.cmd.side_effect = [
                    [{"label": "serial0", "filename": "null"}], {},
                    [{"label": "serial0",
                      "filename": str(root / "native-uart.sock"),
                      "frontend-open": frontend}]]
                rcc.cmd.side_effect = [
                    [{"label": "serial0", "filename": "null"}], {}]
                if capture:
                    (root / "mmx-uart-wire.bin").write_bytes(b"")
                with self.assertRaisesRegex(
                        RuntimeError, "Cannot connect|Missing original"):
                    probe.connect_native_uart(mmx, rcc, root, root)

    def test_native_rstp_counts_current_boot_without_claiming_handshake(self):
        for returned in (b"", b"\x7e\x01"):
            with self.subTest(returned=returned), \
                    tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                mmx, rcc = MagicMock(), MagicMock()
                for name, data in (("mmx", b"old" + b"\x7e\x12"),
                                   ("rcc", b"old" + returned)):
                    (root / f"{name}-uart-wire.bin").write_bytes(data)
                with patch.object(probe, "execute", side_effect=[
                        b"started\n", b"RSTP node ready\n",
                        b"backend invoked\n",
                        b"original OOC output\n"]) as execute:
                    result = probe.observe_native_rstp(
                        mmx, rcc, bytearray(), io.BytesIO(), 30, root, 1,
                        {"mmx": 3, "rcc": 3})
                self.assertEqual(result["tx_bytes"],
                                 {"mmx": 2, "rcc": len(returned)})
                self.assertEqual(result["bidirectional_serial_bytes_observed"],
                                 bool(returned))
                self.assertFalse(result["rstp_handshake_verified"])
                self.assertFalse(result["host_protocol_responder"])
                self.assertTrue(
                    result["original_rcc_persistence_backend_invoked"])
                self.assertFalse(result["persistence_transactions_verified"])
                self.assertFalse(result["full_hmi_or_touch_verified"])
                self.assertEqual(
                    json.loads((root / "rstp-boot-1.json").read_text()), result)
                self.assertEqual((root / "rstp-boot-1.log").read_bytes(),
                                 b"started\nRSTP node ready\n"
                                 b"backend invoked\noriginal OOC output\n")
                self.assertIn("/usr/sbin/ooc-starter.sh",
                              execute.call_args_list[0].args[3])
                self.assertFalse(
                    execute.call_args_list[0].args[3].endswith("&"))
                self.assertEqual(execute.call_args_list[1].args[3],
                                 "waitfor /dev/rstp/persistence 10")
                self.assertEqual(
                    execute.call_args_list[2].args[3],
                    "LD_LIBRARY_PATH=/lib:/eso/lib:/usr/lib "
                    "IPL_CONFIG_DIR=/etc/eso/production "
                    "/usr/bin/rccpersistencebackend -r /dev/rstp/persistence "
                    "-m /etc/persistence_mapping.json "
                    ">/tmp/rccpersistencebackend.log 2>&1 & "
                    "RCC_PERSISTENCE_BACKEND_PID=$!")
                self.assertIn("pidin -p$RCC_PERSISTENCE_BACKEND_PID fd",
                              execute.call_args_list[3].args[3])
                self.assertIn(
                    "dd if=/tmp/rccpersistencebackend.log bs=4096 count=1",
                    execute.call_args_list[3].args[3])
                self.assertNotIn("cat /tmp/rccpersistencebackend.log",
                                 execute.call_args_list[3].args[3])
                for vm in (mmx, rcc):
                    self.assertEqual(
                        [call.args for call in vm.cmd.call_args_list],
                        [("cont",), ("stop",)])

    def test_native_rstp_stops_both_vms_on_console_failure(self):
        mmx, rcc = MagicMock(), MagicMock()
        with patch.object(probe, "execute",
                          side_effect=TimeoutError("console")):
            with self.assertRaises(TimeoutError):
                probe.observe_native_rstp(
                    mmx, rcc, bytearray(), io.BytesIO(), 30, Path("output"), 0,
                    {"mmx": 0, "rcc": 0})
        for vm in (mmx, rcc):
            self.assertEqual([call.args for call in vm.cmd.call_args_list],
                             [("cont",), ("stop",)])

    def test_native_rstp_does_not_launch_backend_without_its_device(self):
        mmx, rcc = MagicMock(), MagicMock()
        with patch.object(probe, "execute", side_effect=[
                b"started\n", RuntimeError("waitfor failed")]) as execute:
            with self.assertRaisesRegex(RuntimeError, "waitfor failed"):
                probe.observe_native_rstp(
                    mmx, rcc, bytearray(), io.BytesIO(), 30, Path("output"), 0,
                    {"mmx": 0, "rcc": 0})
        self.assertEqual(execute.call_count, 2)
        for vm in (mmx, rcc):
            self.assertEqual([call.args for call in vm.cmd.call_args_list],
                             [("cont",), ("stop",)])

    def test_native_rstp_rejects_truncated_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "mmx-uart-wire.bin").write_bytes(b"")
            with patch.object(probe, "execute", return_value=b""), \
                    self.assertRaisesRegex(RuntimeError, "log shrank"):
                probe.observe_native_rstp(
                    MagicMock(), MagicMock(), bytearray(), io.BytesIO(),
                    30, root, 1, {"mmx": 1, "rcc": 0})

    def test_sensor_fixture_requires_native_and_checks_clock_readback(self):
        with self.assertRaisesRegex(ValueError, "requires native IPU1"):
            probe.probe(
                Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                Path("image"), Path("output"),
                experimental_rcc_ipu1_sensor_fixture=True)
        qtest = MagicMock()
        qtest.cmd.side_effect = ["OK", "OK 0x30000"]
        with self.assertRaisesRegex(RuntimeError, "Diagnostic IPL SPI clock"):
            probe.diagnostic_ipl_spi_handoff(qtest)

    def test_native_watchdog_failure_rejects_healthy_register_snapshot(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(RuntimeError, "watchdog failed"):
            self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_uart_watchdog=True)

    def test_sensor_fixture_is_explicit_and_applied_on_both_boots(self):
        with tempfile.TemporaryDirectory() as directory:
            report, create, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_sensor=True)
        self.assertIn("lis331dlh,bus=spi3,cs=1",
                      create.call_args_list[1].kwargs["args"])
        fixture = report["diagnostic_ipu1_sensor_fixture"]
        self.assertFalse(fixture["original_ipl_executed"])
        self.assertEqual(fixture["stationary_acceleration_mg"], [0, 0, 1000])
        self.assertNotIn("Qtest reads only.", report["note"])
        self.assertIn("diagnostic", report["note"])
        commands = [call.args[0] for call in tests[1].cmd.call_args_list]
        for address in probe.IPL_SPI_CLOCKS:
            self.assertEqual(commands.count(f"writel {address:#x} 2"), 2)
            self.assertEqual(commands.count(f"readl {address:#x}"), 2)
        self.assertFalse(report["full_rcc_services_verified"])

    def test_two_boots_use_socket_and_read_only_qtest_without_fake_link(self):
        with tempfile.TemporaryDirectory() as directory:
            result, create, machines, tests = self.run_probe(directory)
        self.assertEqual(len(result["boots"]), 2)
        self.assertTrue(result["mmx_clock_reset_verified_across_reset"])
        self.assertFalse(result["injected_reset_line"])
        self.assertFalse(result["diagnostic_internal_reference"])
        self.assertNotIn("diagnostic_ipu1_fixture", result)
        self.assertEqual(result["initializer_reference_option"], "-c1")
        for flag in ("mmx_link_verified", "rcc_link_verified",
                     "endpoint_enumeration_verified",
                     "full_rcc_services_verified"):
            self.assertFalse(result[flag])
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))
        for call in create.call_args_list:
            args = call.kwargs["args"]
            self.assertIn("-chardev", args)
            self.assertIn("mh2p-pcie-sideband", " ".join(args))
            self.assertNotIn("loader,addr=", " ".join(args))
            self.assertNotIn("experimental-ipu1", " ".join(args))
            self.assertNotIn("tja1145", " ".join(args))
            self.assertNotIn("at24c-eeprom", " ".join(args))
            self.assertNotIn("lis331dlh", " ".join(args))
        for vm in machines:
            vm.kill.assert_not_called()
            self.assertEqual(sum(call.args == ("system_reset",)
                                 for call in vm.cmd.call_args_list), 1)

    def test_native_transport_preserves_sources_and_other_nor_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nor, elf, target = (
                root / name for name in ("nor", "elf", "target"))
            original = (b"\xff" * (32 * 1024 * 1024) +
                        b"\x5a" * (32 * 1024 * 1024))
            firmware = b"\x7fELF" + bytes(2 * 1024 * 1024 - 4)
            nor.write_bytes(original)
            elf.write_bytes(firmware)
            probe.stage_native_ipu_nor(nor, elf, target)
            staged = target.read_bytes()
            self.assertEqual(staged[:0x200000], original[:0x200000])
            self.assertEqual(staged[0x200000:0x400000], firmware)
            self.assertEqual(staged[0x400000:], original[0x400000:])
            self.assertEqual(nor.read_bytes(), original)
            self.assertEqual(elf.read_bytes(), firmware)
            with self.assertRaises(FileExistsError):
                probe.stage_native_ipu_nor(nor, elf, target)
            with nor.open("r+b") as stream:
                stream.seek(0x200000)
                stream.write(b"\0")
            with self.assertRaisesRegex(ValueError, "non-erased"):
                probe.stage_native_ipu_nor(nor, elf, root / "bad")
            elf.write_bytes(b"truncated")
            with self.assertRaisesRegex(ValueError, "2 MiB"):
                probe.stage_native_ipu_nor(nor, elf, root / "bad")

    def test_native_loader_requires_real_exit_and_core_release(self):
        vm = MagicMock()
        for loaded in (
                b"IPC_NATIVE_EXIT:0\nRST1 & RST2 released!\n",
                b"IPC_NATIVE_EXIT:1\nRST1 & RST2 released!\n",
                b"IPC_NATIVE_EXIT:0\n",
                b"echo IPC_NATIVE_EXIT:0\nRST1 & RST2 released!\n"):
            with self.subTest(loaded=loaded), \
                 patch.object(probe, "execute",
                              side_effect=[b"copied", loaded]) as execute:
                if loaded.startswith(b"IPC_NATIVE_EXIT:0\nRST1"):
                    probe.start_native_ipu1(vm, bytearray(), MagicMock(), 120)
                else:
                    with self.assertRaises(RuntimeError):
                        probe.start_native_ipu1(
                            vm, bytearray(), MagicMock(), 120)
                self.assertIn("skip=16 count=16",
                              execute.call_args_list[0].args[3])

    def test_native_ipu_records_guest_mmu_and_original_trace(self):
        transmitted = b"synthetic UART output"
        with tempfile.TemporaryDirectory() as directory:
            report, create, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_subcpu_tx=transmitted)
            self.assertEqual(
                (Path(directory) / "probe/rcc-subcpu-uart-wire.bin")
                .read_bytes(), transmitted * 2)
        arguments = create.call_args_list[1].kwargs["args"]
        self.assertIn("experimental-ipu1-native=on", " ".join(arguments))
        self.assertNotIn("experimental-ipu1-firmware=", " ".join(arguments))
        self.assertNotIn("diagnostic_ipu1_fixture", report)
        self.assertFalse(report["native_ipu1_boot"]["host_preloaded_m4"])
        self.assertFalse(report["full_rcc_services_verified"])
        for index, boot in enumerate(report["boots"]):
            state = boot["native_ipu1"]
            self.assertTrue(state["native_remoteproc_mmu_verified"])
            self.assertEqual(state["mmu_ttb"], "0x8ea00000")
            self.assertTrue(state["sysbios_startup_observed"])
            self.assertEqual(state["shared_startup_words"], [0] * 5)
            self.assertEqual(state["processmanager_wakeup_token"], 0)
            self.assertEqual(state["ipc_startup_state"], 0)
            self.assertEqual(state["gpio6_data_in"], "0x00000000")
            self.assertEqual(state["wakeup_predicate_gpio6_21_22"], [0, 0])
            subcpu = state["subcpu_observation"]
            self.assertEqual(subcpu["tx_file_offset"], index * len(transmitted))
            self.assertEqual(subcpu["tx_bytes"], len(transmitted))
            self.assertEqual(subcpu["tx_prefix_hex"], transmitted.hex())
            self.assertFalse(subcpu["subcpu_exchange_verified"])
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_native_ipu_reports_distinct_wakeup_and_ipc_state_read_only(self):
        words = (0xa5, 0, 0x5a, 0, 4)
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_shared_words=words)
            for index, boot in enumerate(report["boots"]):
                state = boot["native_ipu1"]
                self.assertEqual(state["shared_startup_words"], list(words))
                self.assertEqual(state["processmanager_wakeup_token"], 0x5a)
                self.assertEqual(state["ipc_startup_state"], 4)
                self.assertFalse(state["m4_rstp_verified"])
                saved = json.loads((Path(directory) / "probe" /
                                    f"native-boot-{index}.json")
                                   .read_text())
                self.assertEqual(saved["native_ipu1"]["ipc_startup_state"], 4)
            self.assertFalse(report["full_rcc_services_verified"])
        self.assertEqual(sum(call.args[0] == "readl 0x8cbff010"
                             for call in tests[1].cmd.call_args_list), 2)
        self.assertTrue(all(call.args[0].startswith("readl ")
                            for call in tests[1].cmd.call_args_list))

    def test_native_ipu_records_gpio6_wakeup_predicate_inputs_read_only(self):
        gpio6_data = (1 << 21) | (1 << 22)
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_native=True, ipu_gpio6_data=gpio6_data)
        for boot in report["boots"]:
            state = boot["native_ipu1"]
            self.assertEqual(state["gpio6_data_in"], f"{gpio6_data:#010x}")
            self.assertEqual(state["wakeup_predicate_gpio6_21_22"], [1, 1])
        self.assertEqual(sum(
            call.args[0] == f"readl {probe.RCC_GPIO6_DATAIN:#x}"
            for call in tests[1].cmd.call_args_list), 2)
        self.assertTrue(all(call.args[0].startswith("readl ")
                            for call in tests[1].cmd.call_args_list))

    def test_native_ipu_rejects_faults_and_missing_trace_or_mmu(self):
        for kwargs in ({"ipu_fault": True}, {"ipu_fatal": True},
                       {"ipu_no_trace": True}, {"ipu_no_mmu": True},
                       {"ipu_trace_exception": True}):
            with self.subTest(kwargs=kwargs), \
                 tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(
                        RuntimeError, "Native IPU1 startup failed"):
                    self.run_probe(
                        directory, config_transport=True, native_link=True,
                        pci_server=True, crossboard_init=True, network=True,
                        asi=True, ipu_native=True, **kwargs)
                output = Path(directory) / "probe"
                self.assertFalse((output / "result.json").exists())
                for boot in (0, 1):
                    self.assertTrue(
                        (output / f"ipu1-boot-{boot}.json").is_file())
                    saved = json.loads(
                        (output / f"native-boot-{boot}.json").read_text())
                    self.assertTrue(
                        saved["native_asi"]["agent_start_verified"])
                    self.assertTrue(saved["native_network"][
                        "bidirectional_icmp_verified"])

    def test_native_ipu_capture_failure_never_creates_success(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "IPU1 trace"):
                self.run_probe(
                    directory, config_transport=True, native_link=True,
                    pci_server=True, crossboard_init=True, network=True,
                    asi=True, ipu_native=True, ipu_trace_capture_failure=True)
            self.assertFalse(
                (Path(directory) / "probe/result.json").exists())

    def test_native_ipu_requires_asi_and_excludes_diagnostic_preload(self):
        for kwargs in ({}, {"asi": True, "network": True,
                            "config_transport": True, "native_link": True,
                            "pci_server": True, "crossboard_init": True,
                            "rcc_nor": Path("rcc-nor"),
                            "experimental_rcc_ipu1_fixture": Path("ipu1")}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(
                    ValueError, "exclusive" if kwargs else "requires --asi"):
                probe.probe(
                    Path("qemu"), Path("media"), Path("nor"), Path("mnand"),
                    Path("image"), Path("output"),
                    experimental_rcc_ipu1_native=Path("ipu1"), **kwargs)

    def test_timeout_kills_both_probe_vms_without_success_report(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.assertRaises(TimeoutError):
            self.run_probe(directory, timeout=True)

    def test_transport_mode_labels_diagnostic_not_native_enumeration(self):
        with tempfile.TemporaryDirectory() as directory:
            result, create, _, _ = self.run_probe(
                directory, config_transport=True)
        for flag in ("mmx_link_verified", "rcc_link_verified",
                     "configuration_transport_verified",
                     "diagnostic_configuration_window"):
            self.assertTrue(result[flag])
        self.assertFalse(result["endpoint_enumeration_verified"])
        self.assertFalse(result["full_rcc_services_verified"])
        self.assertIn("NOT native PCI-server enumeration", result["note"])
        for call in create.call_args_list:
            self.assertIn("pcie-fd=", " ".join(call.kwargs["args"]))
        for boot in result["boots"]:
            self.assertIn("live_configuration", boot)
            self.assertEqual(
                boot["mmx_registers"]["root1_data_link_up"], "0x40000000")

    def test_diagnostic_window_is_restored_even_on_mismatched_peer(self):
        mmx, rcc = MagicMock(), MagicMock()
        mmx.cmd.return_value = "OK 0x0"
        rcc.cmd.return_value = "OK 0xdeadbeef"
        with self.assertRaisesRegex(RuntimeError, "Live config"):
            probe.verify_configuration(mmx, rcc)
        commands = [call.args[0] for call in mmx.cmd.call_args_list]
        self.assertEqual(commands[-4:], [
            "writel 0x1001018 0x0", "writel 0x1003818 0x0",
            "writel 0x1003830 0x0", "writel 0x1003800 0x0",
        ])

    def test_native_link_mode_requires_completion_and_no_qtest_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True)
        self.assertTrue(result["mmx_native_post_link_verified"])
        self.assertFalse(result["diagnostic_configuration_window"])
        self.assertFalse(result["endpoint_enumeration_verified"])
        for boot in result["boots"]:
            self.assertTrue(boot["mmx_original_initializer_exited_ok"])
            self.assertFalse(boot["mmx_original_initializer_alive_retrying"])
            self.assertNotIn("live_configuration", boot)
            self.assertEqual(boot["native_post_link_registers"]["0x1001fd8"],
                             "0x10000d00")
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_native_post_link_rejects_incomplete_or_bad_evidence(self):
        qtest = MagicMock()
        qtest.cmd.side_effect = lambda command: (
            f"OK {probe.POST_LINK_REGISTERS[int(command.split()[1], 0)]:#x}")
        up = b"\nPCIE: PCIE: port 0 up\n"
        self.assertEqual(len(probe.verify_post_link(up, b"", qtest)), 9)
        for log, processes in (
            (b"echo PCIE: PCIE: port 0 up\n", b""),
            (up + b"SIGBUS", b""),
            (up, b" 123 /sbin/pcie_init -v -p0 -r25\n"),
        ):
            with self.subTest(log=log, processes=processes), \
                 self.assertRaises(RuntimeError):
                probe.verify_post_link(log, processes, qtest)
        qtest.cmd.side_effect = None
        qtest.cmd.return_value = "OK 0x0"
        with self.assertRaisesRegex(RuntimeError, "Post-link register"):
            probe.verify_post_link(up, b"", qtest)

    def test_unsupported_access_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.assertRaisesRegex(RuntimeError, "Unsupported operations"):
            self.run_probe(directory, unsupported=True)

    def test_disconnect_requires_reset_and_clock_both_low(self):
        qtest = MagicMock()
        qtest.cmd.side_effect = ["OK 0x0", "OK 0x1"]
        with patch.object(probe, "time") as clock:
            clock.monotonic.side_effect = [0, 2]
            with self.assertRaises(TimeoutError):
                probe.verify_disconnected(qtest, 1)

    def test_invalid_timeout_precedes_filesystem_access(self):
        for timeout in (0, -1, 61):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                probe.probe(Path("qemu"), Path("media"), Path("nor"),
                            Path("mnand"), Path("image"), Path("output"),
                            timeout)

    def test_native_link_requires_transport_before_filesystem_access(self):
        with self.assertRaisesRegex(ValueError, "requires"):
            probe.probe(Path("qemu"), Path("media"), Path("nor"),
                        Path("mnand"), Path("image"), Path("output"),
                        native_link=True)

    def test_native_enumeration_records_assignments_without_injection(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True)
        self.assertTrue(report["endpoint_enumeration_verified"])
        self.assertFalse(report["diagnostic_configuration_window"])
        for boot in report["boots"]:
            self.assertEqual(
                boot["native_endpoint_registers"]["0x10"], "0x10000")
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_crossboard_checks_real_handoff_and_outbound_registers(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True)
        self.assertTrue(report["native_crossboard_configuration_verified"])
        for boot in report["boots"]:
            self.assertEqual(
                boot["native_crossboard_registers"]["0x2c"], "0xdeadbeef")
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_missing_native_handoff_fails(self):
        qtest = MagicMock()
        qtest.cmd.return_value = "OK 0x10000"
        with self.assertRaisesRegex(RuntimeError, "crossboard register"):
            probe.verify_crossboard(qtest)

    def test_network_check_records_two_boots_without_qtest_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True)
        self.assertTrue(report["native_bidirectional_network_verified"])
        self.assertFalse(report["full_rcc_services_verified"])
        for boot in report["boots"]:
            self.assertTrue(
                boot["native_network"]["bidirectional_icmp_verified"])
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_network_evidence_rejects_missing_flags_address_or_replies(self):
        interface = (
            b"mmx0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX> mtu 1500\n"
            b"\tinet 10.0.0.15 netmask 0xffffff00 broadcast 10.0.0.255\n")
        probe.verify_interface(interface, "mmx0", "10.0.0.15")
        interleaved_interface = b"\x00\x00\x10\x00\x10\x02\xff\x00" + interface
        probe.verify_interface(interleaved_interface, "mmx0", "10.0.0.15")
        for newline in (b"\n", b"\r\n", b"\r\r\n"):
            interleaved_fields = interface.replace(
                b"\tinet", b"status 10,\tinet").replace(b"\n", newline)
            probe.verify_interface(interleaved_fields, "mmx0", "10.0.0.15")
        other_interface = (
            b"other0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX> mtu 9000\n"
            b"\tinet 10.0.0.15 netmask 0xffffff00 broadcast 10.0.0.255\n")
        for invalid in (interface.replace(b"UP,", b""),
                        interface.replace(b"RUNNING,", b""),
                        interface.replace(b"10.0.0.15 ", b"10.0.0.16 "),
                        interface.replace(b"mmx0:", b"notmmx0:"),
                        interface.replace(b"mmx0:", b"not-mmx0:"),
                        interface.replace(b"mmx0:", b"not.mmx0:"),
                        interface.replace(b"mmx0:", b"mmx01:"),
                        interface.splitlines(keepends=True)[0] + other_interface,
                        interface.replace(b"inet 10.", b"notinet 10."),
                        interface.replace(b"10.0.0.15 ", b"10.0.\n0.15 ")):
            with self.assertRaises(RuntimeError):
                probe.verify_interface(invalid, "mmx0", "10.0.0.15")
            with self.assertRaises(RuntimeError):
                probe.verify_interface(b"\x00\xff" + invalid, "mmx0", "10.0.0.15")
        ping = (
            b"64 bytes from 10.0.0.16: icmp_seq=0 ttl=255 time=10 ms\n"
            b"64 bytes from 10.0.0.16: icmp_seq=1 ttl=255 time=0 ms\n"
            b"2 packets transmitted, 2 packets received, 0% packet loss\n")
        probe.verify_ping(ping, "10.0.0.16")
        interleaved = (b"t\\fa" + ping.replace(
            b"\n64 bytes", b"\ncaused64 bytes").replace(
            b"\n2 packets", b"\nby2 packets"))
        probe.verify_ping(interleaved, "10.0.0.16")
        for invalid in (ping.replace(b"icmp_seq=1", b"icmp_seq=0"),
                        ping.replace(b"0% packet loss", b"50% packet loss"),
                        ping.replace(b"10.0.0.16", b"10.0.0.15"),
                        ping.splitlines()[-1],
                        ping.replace(b"64 bytes", b"164 bytes"),
                        ping.replace(b"2 packets transmitted",
                                     b"12 packets transmitted")):
            with self.assertRaises(RuntimeError):
                probe.verify_ping(invalid, "10.0.0.16")

    def test_asi_check_records_both_boots_and_snapshot_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            report, create, _, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True, asi=True)
        self.assertTrue(report["native_rcc_agent_registration_verified"])
        self.assertFalse(report["full_rcc_services_verified"])
        self.assertEqual(report["source_hashes"]["rcc_nor"],
                         probe.RCC_DEFAULTS_NOR_SHA256)
        for boot in report["boots"]:
            self.assertTrue(boot["native_asi"]["agent_start_verified"])
        rcc_args = create.call_args_list[1].kwargs["args"]
        self.assertIn("if=pflash,format=raw,snapshot=on,file=",
                      " ".join(rcc_args))
        for qtest in tests:
            self.assertTrue(all(call.args[0].startswith("readl ")
                                for call in qtest.cmd.call_args_list))

    def test_diagnostic_ipu_fixture_provenance_and_read_only_observations(self):
        with tempfile.TemporaryDirectory() as directory:
            report, create, machines, tests = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True)
            eeprom = Path(directory) / "probe/diagnostic-erased-eeprom.bin"
            self.assertEqual(eeprom.read_bytes(), b"\xff" * 8192)
            for boot in (0, 1):
                for name in ("core-0", "core-1"):
                    self.assertIn("R15=", (
                        Path(directory) / f"probe/ipu1-{name}-"
                        f"boot-{boot}.log").read_text())
        arguments = create.call_args_list[1].kwargs["args"]
        machine = arguments[arguments.index("-M") + 1]
        self.assertIn(",experimental-ipu1-smp=on", machine)
        self.assertIn(",experimental-ipu1-firmware=", machine)
        self.assertIn("tja1145,bus=spi4,cs=0", arguments)
        self.assertIn("chardev:m4_uart7", arguments)
        self.assertIn("chardev:subcpu_uart", arguments)
        # set_console(console_index=2) prepends serial backends 0, 1 and 2.
        serials = [arguments[index + 1]
                   for index, arg in enumerate(arguments) if arg == "-serial"]
        self.assertEqual(serials, ["null", "chardev:subcpu_uart", "null",
                                  "chardev:m4_uart7"])
        self.assertTrue(any(
            arg.startswith("file,id=m4_uart7,path=") for arg in arguments))
        self.assertTrue(any("id=ipu_eeprom" in arg and "snapshot=on" in arg
                            for arg in arguments))
        self.assertEqual(report["source_hashes"]["rcc_ipu1"],
                         probe.RCC_IPU1_SHA256)
        self.assertFalse(report["full_rcc_services_verified"])
        fixture = report["diagnostic_ipu1_fixture"]
        self.assertFalse(fixture["factory_eeprom_recovered"])
        self.assertFalse(fixture["native_remoteproc_mmu_verified"])
        for boot in report["boots"]:
            self.assertEqual(boot["diagnostic_ipu1"]["m4_cores_present"], 2)
            self.assertEqual(boot["diagnostic_ipu1"]["shared_startup_words"],
                             [0, 0, 0, 0, 0])
            subcpu = boot["diagnostic_ipu1"]["subcpu_observation"]
            self.assertEqual(subcpu["tx_bytes"], 0)
            self.assertEqual(subcpu["m4_virtual_state_bytes"],
                             {"0x80567907": 0, "0x80567991": 0,
                              "0x805679cc": 0})
            self.assertFalse(subcpu["subcpu_exchange_verified"])
            self.assertFalse(boot["diagnostic_ipu1"]["m4_rstp_verified"])
            self.assertFalse(boot["diagnostic_ipu1"][
                "known_mawd_fatal_loop_observed"])
            self.assertEqual(boot["diagnostic_ipu1"]["fault_status"], [
                dict.fromkeys(("cfsr", "hfsr", "dfsr", "mmfar", "bfar"), "0x0")
            ] * 2)
        self.assertTrue(all(call.args[0].startswith("readl ")
                            for call in tests[1].cmd.call_args_list))

    def test_diagnostic_ipu_monitor_failure_is_reported_not_readiness(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch("sys.stderr") as stderr:
            report, _, _, _ = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True, ipu_fatal=True)
            self.assertEqual(
                (Path(directory) / "probe/ipu1-monitor-boot-0.bin").stat().st_size,
                0x2000)
        self.assertTrue(stderr.write.called)
        self.assertFalse(report["full_rcc_services_verified"])
        self.assertTrue(all(boot["diagnostic_ipu1"][
            "known_mawd_fatal_loop_observed"] for boot in report["boots"]))

    def test_diagnostic_ipu_capture_failure_does_not_write_success_report(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(RuntimeError, "capture IPU1 monitor"):
            self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True, ipu_capture_failure=True)

    def test_diagnostic_ipu_fault_status_is_core_local_and_persistent(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch("sys.stderr") as stderr:
            report, _, _, _ = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True, ipu_fault=True)
            for boot in report["boots"]:
                faults = boot["diagnostic_ipu1"]["fault_status"]
                self.assertEqual(faults[0]["cfsr"], "0x0")
                self.assertEqual(faults[1]["cfsr"], "0x8200")
                self.assertEqual(faults[1]["hfsr"], "0x40000000")
                self.assertEqual(faults[1]["bfar"], "0x480b8110")
            for boot in (0, 1):
                saved = json.loads((
                    Path(directory) / f"probe/ipu1-boot-{boot}.json").read_text())
                self.assertEqual(
                    saved, report["boots"][boot]["diagnostic_ipu1"])
        self.assertTrue(stderr.write.called)
        self.assertFalse(report["full_rcc_services_verified"])

    def test_diagnostic_ipu_fault_capture_failure_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(RuntimeError, "core 0 fault status"):
            self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True, ipu_fault_capture_failure=True)

    def test_diagnostic_ipu_failure_retains_observations_not_success_report(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch("sys.stderr"), \
                self.assertRaisesRegex(RuntimeError, "Unsupported operations"):
            try:
                self.run_probe(
                    directory, config_transport=True, native_link=True,
                    pci_server=True, crossboard_init=True, network=True,
                    asi=True, ipu_fixture=True, ipu_fault=True, unsupported=True)
            finally:
                for boot in (0, 1):
                    saved = json.loads((
                        Path(directory) /
                        f"probe/ipu1-boot-{boot}.json").read_text())
                    self.assertEqual(
                        saved["fault_status"][1]["bfar"], "0x480b8110")
                    self.assertFalse(saved["m4_rstp_verified"])

    def test_diagnostic_ipu_fixture_missing_core_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(RuntimeError, "two diagnostic M4s"):
            self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, ipu_fixture=True, ipu_bad_cpus=True)

    def test_diagnostic_ipu_fixture_requires_asi_and_original_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(name) for name in
                     ("qemu", "media", "nor", "mnand", "image")]
            output = Path(directory) / "probe"
            with self.assertRaisesRegex(ValueError, "requires --asi"):
                probe.probe(*paths, output,
                            experimental_rcc_ipu1_fixture=Path("ipu1"))
            with patch.object(probe, "verify_media", return_value={}), \
                    patch.object(probe, "verify_image", return_value="rcc"), \
                    patch.object(probe, "digest", side_effect=lambda path:
                                 probe.RCC_DEFAULTS_NOR_SHA256
                                 if path.name == "rcc-nor" else "wrong"), \
                    patch.object(probe, "QEMUMachine") as create, \
                    self.assertRaisesRegex(ValueError, "IPU1 firmware"):
                probe.probe(
                    *paths, output, config_transport=True, native_link=True,
                    pci_server=True, crossboard_init=True, network=True,
                    asi=True, rcc_nor=Path("rcc-nor"),
                    experimental_rcc_ipu1_fixture=Path("ipu1"))
            create.assert_not_called()
            self.assertFalse(output.exists())

    def test_asi_failure_does_not_write_success_report(self):
        with tempfile.TemporaryDirectory() as directory, \
                self.assertRaisesRegex(RuntimeError, "native ASI failed"):
            self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, asi_failure=True)

    def test_asi_requires_network_and_explicit_validated_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(name) for name in
                     ("qemu", "media", "nor", "mnand", "image")]
            output = Path(directory) / "output"
            with self.assertRaisesRegex(ValueError, "native networking"):
                probe.probe(*paths, output, asi=True)
            with self.assertRaisesRegex(ValueError, "requires the ASI"):
                probe.probe(*paths, output, rcc_nor=Path("rcc-nor"))
            with self.assertRaisesRegex(ValueError, "--rcc-nor"):
                probe.probe(
                    *paths, output, config_transport=True, native_link=True,
                    pci_server=True, crossboard_init=True, network=True,
                    asi=True)
            with patch.object(probe, "verify_media", return_value={}), \
                    patch.object(probe, "verify_image"), \
                    patch.object(probe, "digest", return_value="bad"), \
                    self.assertRaisesRegex(ValueError, "diagnostic NOR"):
                probe.probe(
                    *paths, output, config_transport=True, native_link=True,
                    pci_server=True, crossboard_init=True, network=True,
                    asi=True, rcc_nor=Path("rcc-nor"))
            self.assertFalse(output.exists())

    def test_asi_cli_implies_every_native_network_prerequisite(self):
        arguments = [
            "probe", "--media", "media", "--nor", "nor", "--mnand", "mnand",
            "--image", "image", "--output", "output", "--asi",
            "--rcc-nor", "rcc-nor",
        ]
        for mode in (None, "fixture", "native"):
            extra = ([f"--experimental-rcc-ipu1-{mode}", "ipu1"]
                     if mode else [])
            with patch.object(probe.sys, "argv", arguments + extra), \
                    patch.object(probe, "probe", return_value={}) as run, \
                    patch("builtins.print"):
                probe.main()
            for flag in ("config_transport", "native_link", "pci_server",
                         "crossboard_init", "network", "asi"):
                self.assertTrue(run.call_args.kwargs[flag])
            self.assertEqual(run.call_args.kwargs["rcc_nor"], Path("rcc-nor"))
            self.assertEqual(
                run.call_args.kwargs["experimental_rcc_ipu1_fixture"],
                Path("ipu1") if mode == "fixture" else None)
            self.assertEqual(
                run.call_args.kwargs["experimental_rcc_ipu1_native"],
                Path("ipu1") if mode == "native" else None)

    def test_manual_startup_isolation_requires_asi_and_is_explicit_in_cli(self):
        paths = [Path(name) for name in
                 ("qemu", "media", "nor", "mnand", "image", "output")]
        with patch.object(probe, "verify_media") as verify, \
                self.assertRaisesRegex(ValueError, "isolation requires --asi"):
            probe.probe(
                *paths, experimental_rcc_manual_startup_isolation=True)
        verify.assert_not_called()
        arguments = [
            "probe", "--media", "media", "--nor", "nor", "--mnand", "mnand",
            "--image", "image", "--output", "output", "--asi",
            "--rcc-nor", "rcc-nor",
        ]
        for enabled in (False, True):
            extra = (["--experimental-rcc-manual-startup-isolation"]
                     if enabled else [])
            with patch.object(probe.sys, "argv", arguments + extra), \
                    patch.object(probe, "probe", return_value={}) as run, \
                    patch("builtins.print"):
                probe.main()
            self.assertEqual(
                run.call_args.kwargs[
                    "experimental_rcc_manual_startup_isolation"], enabled)

    def test_manual_startup_isolation_is_forwarded_on_both_boots(self):
        with tempfile.TemporaryDirectory() as directory:
            report, _, _, _ = self.run_probe(
                directory, config_transport=True, native_link=True,
                pci_server=True, crossboard_init=True, network=True,
                asi=True, manual_startup_isolation=True)
        self.assertIn("SIGSTOP", report["note"])
        self.assertIn("not native startup completion", report["note"])
        self.assertFalse(report["full_rcc_services_verified"])

    def test_manual_startup_isolation_checks_all_original_manager_threads(self):
        inventory = (
            b" pid tid name prio STATE Blocked\r\n"
            b" 4100 1 bin/ProcessManager 12r NANOSLEEP\r\n"
            b" 4100 2 bin/ProcessManager 12r RECEIVE 1\r\n")
        stopped = inventory.replace(b"NANOSLEEP", b"STOPPED").replace(
            b"RECEIVE 1", b"STOPPED")
        rcc = MagicMock()
        with patch.object(probe, "execute",
                          side_effect=[inventory, stopped]) as run:
            result = probe.isolate_manual_rcc_startup(
                rcc, bytearray(), MagicMock(), 120)
        self.assertEqual(result["pid"], 4100)
        self.assertTrue(result["process_stopped_verified"])
        self.assertFalse(result["original_startup_verified"])
        self.assertEqual([call.args[3] for call in run.call_args_list],
                         ["pidin -p ProcessManager",
                          "kill -s STOP 4100 && pidin -p 4100"])
        for bad_inventory in (
                b"", b" 4100 1 bin/OtherManager 12r NANOSLEEP\n",
                inventory + b" 5000 1 bin/ProcessManager 12r NANOSLEEP\n"):
            with self.subTest(inventory=bad_inventory), \
                    patch.object(probe, "execute",
                                 return_value=bad_inventory) as run, \
                    self.assertRaisesRegex(RuntimeError, "identify one"):
                probe.isolate_manual_rcc_startup(
                    rcc, bytearray(), MagicMock(), 120)
            self.assertEqual(run.call_count, 1)
        for bad_stop in (
                b"", inventory, stopped.replace(b"4100", b"4101"),
                stopped.replace(b" 4100 2 bin/ProcessManager 12r STOPPED\r\n",
                                b""),
                stopped.replace(b" 4100 2", b" 4100 3").replace(
                    b"STOPPED\r\n", b"RECEIVE\r\n", 1)):
            with self.subTest(stopped=bad_stop), \
                    patch.object(probe, "execute",
                                 side_effect=[inventory, bad_stop]), \
                    self.assertRaisesRegex(RuntimeError, "stop completely"):
                probe.isolate_manual_rcc_startup(
                    rcc, bytearray(), MagicMock(), 120)

    def test_manual_startup_isolation_precedes_all_asi_services_and_fails_closed(self):
        for failure in (False, True):
            commands = []

            def execute(console, captured, log, command, timeout):
                commands.append(command)
                return self.QDB_READY if command.startswith("sleep 25;") else b""

            def isolate(*args):
                commands.append("isolate original manager")
                if failure:
                    raise RuntimeError("Manager is not stopped")
                return {"process_stopped_verified": True,
                        "original_startup_verified": False}

            with tempfile.TemporaryDirectory() as directory, \
                    patch.object(probe, "execute", side_effect=execute), \
                    patch.object(probe, "isolate_manual_rcc_startup",
                                 side_effect=isolate), \
                    patch.object(probe, "verify_asi_registration",
                                 return_value={"agent_start_verified": True}):
                args = (MagicMock(), MagicMock(), bytearray(), MagicMock(),
                        Path(directory) / "rcc.log", 30)
                if failure:
                    with self.assertRaisesRegex(RuntimeError, "not stopped"):
                        probe.verify_native_asi(
                            *args, manual_startup_isolation=True)
                    self.assertEqual(commands, ["isolate original manager"])
                else:
                    result = probe.verify_native_asi(
                        *args, manual_startup_isolation=True)
                    self.assertEqual(commands[0], "isolate original manager")
                    self.assertTrue(result["manual_startup_isolation"][
                        "process_stopped_verified"])
                    self.assertFalse(result["manual_startup_isolation"][
                        "original_startup_verified"])

    def test_asi_evidence_requires_agent_success_and_matching_tcp_peers(self):
        start = (
            b"[PER_CYC] 11073 call AgentStart clientRCC_persistence.\r\n"
            b"[PER_CYC] 11203 StartAgent success.\r\n")
        rcc = (
            b"tcp 0 0 10.0.0.16.65535 10.0.0.15.21100 ESTABLISHED\r\n")
        mmx = (
            b"tcp 0 0 10.0.0.15.21100 10.0.0.16.65535 ESTABLISHED\r\n")
        result = probe.verify_asi_registration(start + rcc, mmx)
        self.assertEqual(result["agent"], "clientRCC_persistence")
        self.assertEqual(result["rcc_peer"], "10.0.0.16:65535")
        self.assertFalse(result["persistence_transactions_verified"])
        self.assertFalse(result["m4_rstp_verified"])
        for invalid_rcc, invalid_mmx in (
            (rcc, mmx),
            (start.replace(b"success", b"timeout") + rcc, mmx),
            (start.replace(b"clientRCC_persistence", b"other") + rcc, mmx),
            (start + rcc.replace(b"ESTABLISHED", b"LISTEN"), mmx),
            (start + rcc, mmx.replace(b"65535", b"65534")),
            (start + rcc, mmx.replace(b"21100", b"21101")),
            (start + rcc, mmx.replace(b"10.0.0.16", b"10.0.0.17")),
            (start + rcc, b""),
        ):
            with self.assertRaises(RuntimeError):
                probe.verify_asi_registration(invalid_rcc, invalid_mmx)

    def test_asi_starts_original_applications_without_redirecting_progress(self):
        mmx, rcc = MagicMock(), MagicMock()
        agent = (
            b"[PER_CYC] 1 call AgentStart clientRCC_persistence.\n"
            b"[PER_CYC] 2 StartAgent success.\n"
            b"tcp 0 0 10.0.0.16.65535 10.0.0.15.21100 ESTABLISHED\n")
        peer = b"tcp 0 0 10.0.0.15.21100 10.0.0.16.65535 ESTABLISHED\n"

        def execute(console, captured, stream, command, timeout):
            if console is rcc.console_socket and command.startswith("sleep 25"):
                captured.extend(agent)
                if timeout == 600:
                    captured.extend(
                        b"BENCH_Startup J6(A15) IPC_APP end of startup\r\n")
                return self.QDB_READY
            return peer if console is mmx.console_socket else b""

        for wait_timeout in (None, 600):
            with tempfile.TemporaryDirectory() as directory, \
                    patch.object(probe, "execute", side_effect=execute) as run:
                result = probe.verify_native_asi(
                    mmx, rcc, bytearray(), MagicMock(),
                    Path(directory) / "rcc.log", 30,
                    wait_timeout=wait_timeout)
            self.assertEqual(result["ipc_app_startup_observed"],
                             wait_timeout == 600)
            for call in run.call_args_list:
                self.assertEqual(
                    call.args[4], 600 if wait_timeout and
                    call.args[3].startswith("sleep 25;") else 120)
            for vm in (mmx, rcc):
                self.assertEqual([call.args[0] for call in vm.cmd.call_args_list],
                                 ["cont", "stop"])
                vm.cmd.reset_mock()
        self.assertTrue(result["agent_start_verified"])
        self.assertEqual(result["qdb"]["aggregate_database"], "Valid")
        commands = [call.args[3] for call in run.call_args_list]
        self.assertIn("LD_PRELOAD=/App/libExceptionHandler.so "
                      "/App/PERSISTENCE & PERSISTENCE_PID=$!", commands)
        environment = commands.index(
            ". /etc/boot/common.sh && set_environment_variables_basic && "
            "test -r \"$LIBIMG_CFGFILE\"")
        broker = next(i for i, cmd in enumerate(commands)
                      if "./bin/broker --node" in cmd)
        self.assertLess(environment, broker)
        self.assertTrue(any("/eso/bin/mapper" in cmd for cmd in commands))
        self.assertFalse(any("loader,addr=" in cmd for cmd in commands))
        qdb_command = next(cmd for cmd in commands if "qdb -c /mypps" in cmd)
        for name in ("BAT", "IMM", "TMP"):
            self.assertIn(
                f"cp /usr/persis/persis_qdb_{name}.cfg /mypps/qdb1/config/persisDB_{name} &&",
                qdb_command)
        self.assertIn("cp /usr/persis/persis_qdb_DB.cfg /mypps/qdb1/config/persisDB &&",
                      qdb_command)

    def test_asi_does_not_start_applications_after_environment_failure(self):
        def execute(console, captured, stream, command, timeout):
            if "set_environment_variables_basic" in command:
                raise RuntimeError("Original basic environment unavailable")
            return b""

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(probe, "execute", side_effect=execute) as run:
            with self.assertRaisesRegex(RuntimeError, "environment unavailable"):
                probe.verify_native_asi(
                    MagicMock(), MagicMock(), bytearray(), MagicMock(),
                    Path(directory) / "rcc.log", 30)
        commands = [call.args[3] for call in run.call_args_list]
        self.assertFalse(any("./bin/broker" in cmd or "/eso/bin/mapper" in cmd
                             for cmd in commands))

    def test_asi_rejects_pending_qdb_before_promoting_agent_registration(self):
        def execute(console, captured, log, command, timeout):
            if command.startswith("sleep 25;"):
                return self.QDB_READY.replace(b"Status::Valid", b"Status::AttachWait", 1)
            return b""

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(probe, "execute", side_effect=execute), \
                patch.object(probe, "verify_asi_registration") as registration:
            with self.assertRaisesRegex(ValueError, "not valid"):
                probe.verify_native_asi(
                    MagicMock(), MagicMock(), bytearray(), MagicMock(),
                    Path(directory) / "rcc.log", 30)
            registration.assert_not_called()

    def test_native_loader_runs_after_mount_before_original_ipc_app(self):
        commands = []

        def execute(console, captured, log, command, timeout):
            commands.append(command)
            return self.QDB_READY if command.startswith("sleep 25;") else b""

        def load(*args):
            commands.append("original native loader")

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(probe, "execute", side_effect=execute), \
                patch.object(probe, "start_native_ipu1", side_effect=load), \
                patch.object(probe, "verify_asi_registration",
                             return_value={"agent_start_verified": True}):
            result = probe.verify_native_asi(
                MagicMock(), MagicMock(), bytearray(), MagicMock(),
                Path(directory) / "rcc.log", 30, native_ipu1=True)
        mounted = next(i for i, command in enumerate(commands)
                       if "inflator /App" in command)
        ipc_app = next(i for i, command in enumerate(commands)
                       if "/App/IPC_APP" in command)
        self.assertLess(mounted, commands.index("original native loader"))
        self.assertLess(commands.index("original native loader"), ipc_app)
        self.assertTrue(result["original_native_ipu1_loader_verified"])

    def test_pause_samples_original_reset_release_without_writes(self):
        vm, qtest = MagicMock(), MagicMock()
        qtest.cmd.side_effect = ["OK 0x1a", "OK 0x1b"]
        probe.stop_after_reset_release(vm, qtest, 1)
        self.assertEqual(
            [call.args[0] for call in vm.cmd.call_args_list],
            ["stop", "cont", "stop"])
        self.assertTrue(all(call.args[0] == "readl 0x01003910"
                            for call in qtest.cmd.call_args_list))

    def test_subcpu_capture_separates_boot_bytes_and_preserves_raw_state(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            wire = output / "rcc-subcpu-uart-wire.bin"
            old = b"previous boot"
            transmitted = bytes(range(80))
            wire.write_bytes(old + transmitted)
            vm = MagicMock()

            def save(command, **kwargs):
                self.assertEqual(command, "human-monitor-command")
                self.assertEqual(kwargs["cpu-index"], 1)
                parts = shlex.split(kwargs["command_line"])
                self.assertEqual(parts[:3],
                                 ["memsave", "0x80567900", "0xd0"])
                state = bytearray(0xd0)
                state[7], state[0x91], state[0xcc] = 1, 2, 0xff
                Path(parts[3]).write_bytes(state)

            vm.cmd.side_effect = save
            result = probe.capture_subcpu_observation(
                vm, output, 1, len(old))
            self.assertEqual(result["tx_file_offset"], len(old))
            self.assertEqual(result["tx_bytes"], 80)
            self.assertEqual(result["tx_prefix_hex"], transmitted[:64].hex())
            self.assertEqual(result["m4_virtual_state_bytes"],
                             {"0x80567907": 1, "0x80567991": 2,
                              "0x805679cc": 0xff})
            for name in ("subcpu_peer_attached", "host_protocol_responder",
                         "subcpu_exchange_verified"):
                self.assertFalse(result[name])
            self.assertEqual(vm.cmd.call_count, 1)
            with self.assertRaisesRegex(RuntimeError, "shrank"):
                probe.capture_subcpu_observation(
                    vm, output, 2, wire.stat().st_size + 1)
            wire.unlink()
            with self.assertRaises(FileNotFoundError):
                probe.capture_subcpu_observation(vm, output, 3, 0)

    def test_subcpu_capture_requires_complete_memory_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            vm = MagicMock()
            with self.assertRaisesRegex(RuntimeError, "power state"):
                probe.capture_subcpu_observation(vm, output, 0, 0)
            (output / "ipu1-power-boot-1.bin").write_bytes(bytes(0xcf))
            with self.assertRaisesRegex(RuntimeError, "power state"):
                probe.capture_subcpu_observation(vm, output, 1, 0)

    def test_subcpu_framing_and_checksum_use_original_length_contract(self):
        for length in (10, 266):
            with self.subTest(length=length):
                body = length.to_bytes(2, "little") + bytes(length - 2)
                packet = b"\xf7\xc1\x42" + body + bytes([sum(body) & 0xff])
                self.assertEqual(len(packet), length + 4)
                stream = io.BytesIO(packet * 2 + b"not part of this boot")
                result = probe.inspect_subcpu_tx(stream, len(packet) * 2)
                self.assertEqual(result, {
                    "frames": 2, "checksum_valid_frames": 2,
                    "checksum_invalid_frames": 0, "discarded_bytes": 0,
                    "invalid_lengths": 0, "incomplete_frame_bytes": 0})
                self.assertEqual(stream.tell(), len(packet) * 2)
                corrupt = packet[:-1] + bytes([packet[-1] ^ 1])
                result = probe.inspect_subcpu_tx(io.BytesIO(corrupt),
                                                len(corrupt))
                self.assertEqual(result["checksum_valid_frames"], 0)
                self.assertEqual(result["checksum_invalid_frames"], 1)

    def test_subcpu_framing_reports_invalid_lengths_noise_and_truncation(self):
        for length in (0, 9, 267, 65535):
            with self.subTest(length=length):
                packet = b"\xf7\xc1\x00" + length.to_bytes(2, "little")
                result = probe.inspect_subcpu_tx(io.BytesIO(packet), 5)
                self.assertEqual(result["invalid_lengths"], 1)
                self.assertEqual(result["discarded_bytes"], 5)
                self.assertEqual(result["frames"], 0)
        for partial in (b"\xf7", b"\xf7\xc1\x00\x0a\x00\x12"):
            result = probe.inspect_subcpu_tx(io.BytesIO(partial), len(partial))
            self.assertEqual(result["incomplete_frame_bytes"], len(partial))
            self.assertEqual(result["frames"], 0)
        result = probe.inspect_subcpu_tx(io.BytesIO(b"\xf7\xf7\xc1"), 3)
        self.assertEqual(result["discarded_bytes"], 3)
        self.assertEqual(result["incomplete_frame_bytes"], 0)
        with self.assertRaisesRegex(RuntimeError, "Short"):
            probe.inspect_subcpu_tx(io.BytesIO(b""), 1)

    def test_subcpu_framing_preserves_state_across_stream_chunks(self):
        body = b"\x0a\x00" + bytes(8)
        packet = b"\xf7\xc1\x42" + body + bytes([sum(body) & 0xff])
        for prefix_size in (4095, 4094, 4092, 4091, 4090):
            data = bytes(prefix_size) + packet
            result = probe.inspect_subcpu_tx(io.BytesIO(data), len(data))
            self.assertEqual(result["discarded_bytes"], prefix_size)
            self.assertEqual(result["checksum_valid_frames"], 1)
            self.assertEqual(result["incomplete_frame_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
