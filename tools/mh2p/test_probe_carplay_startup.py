# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

from hmi_diagnosis import DiagnosisError, is_read_only_command
from probe_carplay_startup import (
    CONTROLLER_SIZE, CONFIGURATION_OFFSET, READS, START_COMMAND, SUCCESS,
    assess_class_loading, assess_domains, capture_native_readiness, decode_native_readiness,
    parse_domains, parse_memory_dump, request_start, terminalmode_registered)
from prepare_carplay_java_overlay import OVERLAY_ENTRIES


def controller_data(base=0x788cd000):
    data = bytearray(CONTROLLER_SIZE)
    struct.pack_into("<I", data, 0, base + 0x106268)
    struct.pack_into("<I", data, CONFIGURATION_OFFSET, base + 0x107320)
    data[0x114:0x118] = b"\0\1\1\1"
    struct.pack_into("<I", data, 0x11c, 8)
    return data


def memory_reply(data):
    lines = [f"{at:07X} " + " ".join(f"{value:02x}" for value in data[at:at + 16])
             for at in range(0, len(data), 16)]
    return ("echo CARPLAY_MEMORY_BEGIN; command; echo CARPLAY_MEMORY_END\r\n"
            "CARPLAY_MEMORY_BEGIN\r\n" + "\r\n".join(lines) +
            f"\r\n{len(data):07X}\r\nCARPLAY_MEMORY_END\r\n1+0 records in\r\n")


class ClassLoadingTests(unittest.TestCase):
    def test_exact_load_lines_verify_classes_not_activation_or_delivery(self):
        names = [name.removesuffix(".class") for name in OVERLAY_ENTRIES]
        reply = "\r\n".join(f"class load: {name}" for name in names) + "\r\n"
        report = assess_class_loading(reply)
        self.assertEqual(report["loaded_target_classes"], names)
        self.assertEqual(report["missing_target_classes"], [])
        self.assertTrue(report["target_classes_loaded_verified"])
        self.assertTrue(report["contact_slot_helper_loaded"])
        for field in ("component_activation_verified", "live_hmi_dsi_delivery_verified",
                      "iphone_session_verified"):
            self.assertFalse(report[field])

    def test_installed_classpath_or_similar_names_do_not_verify_loading(self):
        names = [name.removesuffix(".class") for name in OVERLAY_ENTRIES]
        reply = "-Xbootclasspath:/tmp/mh2p-carplay-contacts.jar\n"
        reply += "\n".join(
            line for name in names for line in (
                f"echo class load: {name}", f"class load: {name}Extra",
                f"class load: {name}.class", f"class load: {name} extra"))
        report = assess_class_loading(reply)
        self.assertEqual(report["missing_target_classes"], names)
        self.assertFalse(report["target_classes_loaded_verified"])
        self.assertFalse(report["contact_slot_helper_loaded"])

    def test_partial_loading_keeps_missing_classes_explicit(self):
        report = assess_class_loading("class load: mh2p/lab/CarplayContactSlots\n" * 2)
        self.assertTrue(report["contact_slot_helper_loaded"])
        self.assertFalse(report["target_classes_loaded_verified"])
        self.assertEqual(len(report["missing_target_classes"]), len(OVERLAY_ENTRIES) - 1)
        self.assertEqual(report["loaded_target_classes"], ["mh2p/lab/CarplayContactSlots"])
        self.assertEqual(len(assess_class_loading("")["missing_target_classes"]),
                         len(OVERLAY_ENTRIES))

    def test_existing_targets_without_helper_are_not_full_overlay_execution(self):
        names = [name.removesuffix(".class") for name in OVERLAY_ENTRIES[:-1]]
        report = assess_class_loading(
            "\n".join(f"class load: {name}" for name in names) + "\n")
        self.assertEqual(report["loaded_target_classes"], names)
        self.assertEqual(report["missing_target_classes"], ["mh2p/lab/CarplayContactSlots"])
        self.assertFalse(report["target_classes_loaded_verified"])
        self.assertFalse(report["contact_slot_helper_loaded"])


class NativeReadinessTests(unittest.TestCase):
    def test_exact_dump_offsets_and_lengths_required(self):
        data = bytes(range(32))
        reply = memory_reply(data)
        self.assertEqual(parse_memory_dump(reply, 32), data)
        for bad in (reply.replace("0000010 ", "0000014 "),
                    reply.replace("0000020\r", "0000030\r"),
                    memory_reply(data[:16]),
                    "CARPLAY_MEMORY_BEGIN\nNo such file\nCARPLAY_MEMORY_END\n",
                    reply.replace("00 01", "gg 01"), reply + reply):
            with self.subTest(reply=bad), self.assertRaises(DiagnosisError):
                parse_memory_dump(bad, 32)

    def test_unknown_metadata_is_not_zero_or_success(self):
        report = decode_native_readiness(controller_data(), 0x788cd000)
        self.assertFalse(report["USBInfoValid"])
        self.assertTrue(report["PersistenceRead"])
        self.assertEqual(report["pending_runmode"], 8)
        self.assertEqual(report["missing_usb_metadata"], ["coding", "ports", "hub_support"])
        self.assertIsNone(report["usb_metadata"]["coding"]["value"])
        self.assertFalse(report["component_activation_verified"])

    def test_separate_process_notifications_are_logged_not_memory(self):
        data = bytes(range(32))
        notifications = (
            '[pfsn] Gone PID=2195519 PPID=3 "sleep" "10"',
            '[pfsn] New PID=2261055 PPID=3 Prio=10 "dd" "if" "bs" "skip" "count"',
            '[pfsn] New PID=2261056 PPID=3 Prio=10 "/armle/usr/bin/hd" "-v" ...',
        )
        reply = memory_reply(data).replace(
            "0000010 ", "\r\n".join(notifications) + "\r\n0000010 ")
        with self.assertLogs("probe_carplay_startup", level="WARNING") as logged:
            self.assertEqual(parse_memory_dump(reply, len(data)), data)
        self.assertEqual(len(logged.output), len(notifications))
        for bad in (
                reply.replace("0000010 ", "0000014 "),
                reply.replace("0000010 ", ""),
                reply.replace("0000010 ", "[pfsn] unknown notification\r\n0000010 "),
                reply.replace("0000010 ", notifications[0] + "0000010 ")):
            with self.subTest(reply=bad):
                with self.assertLogs("probe_carplay_startup", level="WARNING"):
                    with self.assertRaises(DiagnosisError):
                        parse_memory_dump(bad, len(data))

    def test_valid_zero_metadata_and_specific_missing_field(self):
        data = controller_data()
        data[CONFIGURATION_OFFSET + 0x264] = 1
        data[CONFIGURATION_OFFSET + 0x278] = 1
        report = decode_native_readiness(data, 0x788cd000)
        self.assertEqual(report["missing_usb_metadata"], ["ports"])
        self.assertEqual(report["usb_metadata"]["coding"], {"valid": True, "value": 0})
        self.assertEqual(report["usb_metadata"]["hub_support"]["value"], 0)

    def test_wrong_objects_and_invalid_flags_rejected(self):
        for offset in (0, CONFIGURATION_OFFSET, 0x114,
                       CONFIGURATION_OFFSET + 0x270):
            data = controller_data()
            data[offset] = 2
            with self.subTest(offset=offset), self.assertRaises(DiagnosisError):
                decode_native_readiness(data, 0x788cd000)
        with self.assertRaises(DiagnosisError):
            decode_native_readiness(b"", 0x788cd000)

    def test_capture_resolves_pid_and_base_and_reads_only(self):
        shell = Mock(side_effect=[
            " 4231215 1 rtphone_integrator 10r CONDVAR\n"
            " rtphone_integrator @788cd000 1028K 4096\n",
            memory_reply(struct.pack("<I", 0x78c8aa88)),
            memory_reply(controller_data()),
        ])
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "binary"
            binary.write_bytes(b"fixture")
            with patch("probe_carplay_startup.ORIGINAL_SHA256",
                       hashlib.sha256(b"fixture").hexdigest()):
                report = capture_native_readiness(shell, binary, root / "capture")
            self.assertTrue((root / "capture/result.json").exists())
        self.assertEqual(report["pid"], 4231215)
        self.assertEqual(report["controller_address"], 0x78c8aa88)
        self.assertTrue(all("of=/proc" not in call.args[0]
                            for call in shell.call_args_list))

    def test_exited_process_never_produces_result(self):
        shell = Mock(return_value="     pid Arguments\n")
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "binary"
            binary.write_bytes(b"fixture")
            with patch("probe_carplay_startup.ORIGINAL_SHA256",
                       hashlib.sha256(b"fixture").hexdigest()):
                with self.assertRaises(DiagnosisError):
                    capture_native_readiness(shell, binary, root / "capture")
            self.assertFalse((root / "capture/result.json").exists())
        self.assertEqual(shell.call_count, 1)

    def test_unknown_binary_and_existing_output_never_query_guest(self):
        shell = Mock()
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "binary"
            binary.write_bytes(b"fixture")
            with self.assertRaisesRegex(DiagnosisError, "original P2865"):
                capture_native_readiness(shell, binary, root / "capture")
            self.assertFalse((root / "capture").exists())
            with patch("probe_carplay_startup.ORIGINAL_SHA256",
                       hashlib.sha256(b"fixture").hexdigest()):
                with self.assertRaises(FileExistsError):
                    capture_native_readiness(shell, binary, root)
        shell.assert_not_called()

    def test_process_exit_during_memory_capture_fails_explicitly(self):
        shell = Mock(side_effect=[
            " 4231215 1 rtphone_integrator 10r CONDVAR\n"
            " rtphone_integrator @788cd000 1028K 4096\n",
            "CARPLAY_MEMORY_BEGIN\nCARPLAY_MEMORY_END\n"
            "dd: /proc/4231215/as: No such file or directory\n",
        ])
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "binary"
            binary.write_bytes(b"fixture")
            with patch("probe_carplay_startup.ORIGINAL_SHA256",
                       hashlib.sha256(b"fixture").hexdigest()):
                with self.assertRaises(DiagnosisError):
                    capture_native_readiness(shell, binary, root / "capture")
            self.assertFalse((root / "capture/result.json").exists())
            self.assertIn("No such file", (root / "capture/singleton.log").read_text())

    def test_ambiguous_live_processes_are_not_selected(self):
        shell = Mock(return_value=(
            " 4231215 1 rtphone_integrator 10r CONDVAR\n"
            " 4231216 1 rtphone_integrator 10r CONDVAR\n"
            " rtphone_integrator @788cd000 1028K 4096\n"))
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "binary"
            binary.write_bytes(b"fixture")
            with patch("probe_carplay_startup.ORIGINAL_SHA256",
                       hashlib.sha256(b"fixture").hexdigest()):
                with self.assertRaises(DiagnosisError):
                    capture_native_readiness(shell, binary, root / "capture")
        self.assertEqual(shell.call_count, 1)


def domain_reply(media="100 update(0x1ff7ff00)", smartphone="100 update(0x1)"):
    return ("response_getvalueDomain Startup Times:<newline>"
            "Domain: 3 = Media<newline>" + media + "<newline>"
            "Domain: 37 = SmartphoneIntegration<newline>" + smartphone)


def snapshot_replies():
    return [("response_getcommandscmdStartApp(java.lang.String),"
             if name == "startup_commands" else expected + " original value")
            for name, (_, expected) in READS.items()]


class CarplayStartupTests(unittest.TestCase):
    def test_all_startup_snapshot_commands_are_read_only(self):
        for command, _ in READS.values():
            self.assertTrue(is_read_only_command(command), command)
        self.assertFalse(is_read_only_command(START_COMMAND))

    def test_application_registration_is_exact_not_class_loading(self):
        self.assertFalse(terminalmode_registered("response_getapps101,FwStartUpMgr,"))
        self.assertTrue(terminalmode_registered(
            "response_getapps101,FwStartUpMgr,32,AppTerminalMode,"))
        self.assertTrue(terminalmode_registered(
            "response_getapps32,AppTerminalMode [MAIN]"))
        self.assertFalse(terminalmode_registered("response_getapps"))
        for reply in ("response_getapps32", "class load: AppTerminalMode",
                      "response_getapps32,Other,", "response_getappsx,AppTerminalMode,",
                      "response_getapps32,AppTerminalMode,32,AppTerminalMode,",
                      "response_getapps32,,"):
            with self.subTest(reply=reply), self.assertRaises(DiagnosisError):
                terminalmode_registered(reply)

    def test_unrelated_original_ids_can_be_reused_without_ambiguating_target(self):
        unrelated = "11,AppSettings,11,OnlineApiImplementation,"
        for entries in (unrelated + "32,AppTerminalMode,",
                        "32,AppTerminalMode," + unrelated):
            self.assertTrue(terminalmode_registered("response_getapps" + entries))
        self.assertFalse(terminalmode_registered("response_getapps" + unrelated))
        with self.assertRaises(DiagnosisError):
            terminalmode_registered(
                "response_getapps" + unrelated + "32,AppTerminalMode,032,AppTerminalMode,")

    def test_domain_timeout_does_not_count_as_readiness(self):
        report = assess_domains(domain_reply(), domain_reply(smartphone=(
            "100 update(0x1)<newline>110 request(0x4)<newline>115 update(0x10)")))
        self.assertTrue(report["fresh_smartphone_domain_request_observed"])
        self.assertFalse(report["smartphone_required_state_observed_after_request"])
        self.assertEqual(report["last_observed_smartphone_state"], 0x10)
        self.assertTrue(report["media_dependency_bits_currently_satisfied"])
        self.assertFalse(report["component_activation_verified"])

    def test_prior_ready_state_cannot_satisfy_new_request(self):
        before = domain_reply(smartphone="100 update(0x4)")
        after = domain_reply(smartphone=(
            "100 update(0x4)<newline>110 request(0x4)<newline>115 update(0x10)"))
        self.assertFalse(assess_domains(before, after)[
            "smartphone_required_state_observed_after_request"])
        self.assertFalse(assess_domains(before, before)[
            "fresh_smartphone_domain_request_observed"])
        self.assertTrue(assess_domains(before, before)[
            "smartphone_dependency_bits_currently_satisfied"])
        self.assertFalse(assess_domains(before, after)[
            "smartphone_dependency_bits_currently_satisfied"])

    def test_required_bits_not_exact_equality_and_no_activation_claim(self):
        report = assess_domains(domain_reply(), domain_reply(smartphone=(
            "100 update(0x1)<newline>110 request(0x4)<newline>111 response(0xc)")))
        self.assertTrue(report["smartphone_required_state_observed_after_request"])
        self.assertFalse(report["target_classes_loaded_verified"])
        self.assertFalse(report["component_activation_verified"])

    def test_missing_domain_states_stay_explicitly_unknown(self):
        report = assess_domains(domain_reply(media="", smartphone=""),
                                domain_reply(media="", smartphone=""))
        self.assertIsNone(report["last_observed_smartphone_state"])
        self.assertIsNone(report["smartphone_dependency_bits_currently_satisfied"])
        self.assertIsNone(report["media_dependency_bits_currently_satisfied"])

    def test_malformed_unknown_and_rewritten_histories_rejected(self):
        for reply in ("response_getvaluefailed", domain_reply().replace(
                "37 = SmartphoneIntegration", "37 = Media"),
                domain_reply(smartphone="100 update(0x1)<newline>99 update(0x4)"),
                domain_reply(smartphone="not a domain event"),
                domain_reply() + "<newline>Domain: 37 = SmartphoneIntegration"):
            with self.subTest(reply=reply), self.assertRaises(DiagnosisError):
                parse_domains(reply)
        with self.assertRaisesRegex(DiagnosisError, "history"):
            assess_domains(domain_reply(), domain_reply(smartphone="101 update(0x4)"))

    def test_reads_before_enqueuing_original_component_request(self):
        diagnosis = Mock(side_effect=snapshot_replies() + [SUCCESS])
        with tempfile.TemporaryDirectory() as name:
            report = request_start(diagnosis, Path(name) / "probe")
        self.assertEqual([call.args for call in diagnosis.call_args_list],
                         list(READS.values()) + [(START_COMMAND, "response_processcommand")])
        self.assertTrue(report["original_diagnostic_method_returned"])
        self.assertFalse(report["startup_work_enqueued_verified"])
        self.assertTrue(report["original_component_dependencies_preserved"])
        self.assertFalse(report["component_activation_verified"])
        self.assertFalse(report["target_classes_loaded_verified"])
        self.assertFalse(report["iphone_session_verified"])

    def test_failed_or_success_shaped_reply_never_reports_activation(self):
        for reply in ("response_processcommand No such method", SUCCESS + " failed"):
            diagnosis = Mock(side_effect=snapshot_replies() + [reply])
            with tempfile.TemporaryDirectory() as name:
                output = Path(name) / "probe"
                with self.assertRaises(DiagnosisError):
                    request_start(diagnosis, output)
                self.assertFalse((output / "result.json").exists())
                self.assertTrue((output / "request-response.json").exists())

    def test_unadvertised_original_command_is_not_invoked(self):
        replies = snapshot_replies()
        replies[list(READS).index("startup_commands")] = (
            "response_getcommandscmdStartApp(java.lang.String)Extra,")
        diagnosis = Mock(side_effect=replies)
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaisesRegex(DiagnosisError, "advertise"):
                request_start(diagnosis, Path(name) / "probe")
        self.assertEqual(diagnosis.call_count, len(READS))

    def test_failed_read_does_not_send_start_command(self):
        diagnosis = Mock(side_effect=DiagnosisError("Original getter failed"))
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(DiagnosisError):
                request_start(diagnosis, Path(name) / "probe")
        self.assertEqual(diagnosis.call_count, 1)

    def test_existing_output_rejected_before_any_guest_request(self):
        diagnosis = Mock()
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(FileExistsError):
                request_start(diagnosis, Path(name))
        diagnosis.assert_not_called()


if __name__ == "__main__":
    unittest.main()
