# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

from prepare_carplay_usb_capture import (
    EMPTY_LAB_RECORD, FORMATS, empty_lab_mapping, install_empty_lab_ports,
    prepare, restore_capture, validate_capture,
    verify_restored_ports)


def provenance(raw, source_format="raw"):
    spec = FORMATS[source_format]
    return {
        "firmware": "synthetic test fixture, not unit data",
        "hardware": "synthetic host test",
        "capture_method": "fixture",
        "captured_at": "2026-10-29",
        "namespace": spec["namespace"], "key": spec["key"],
        "type": "BLOB", "length": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
    }


def fixture_shell(reply):
    text = reply.decode() if isinstance(reply, bytes) else reply
    values = re.findall(r"(?m)^[0-9a-f]{8}\r?$", text)
    value = values[0].strip() if values else "ff8003fe"
    return Mock(side_effect=lambda command: (
        f"BLOB\t{value}\nCARPLAY_USB_UPDATE_OBSERVED\n"
        if "dumb_persistence_subscriber" in command else reply))


class UsbCaptureTests(unittest.TestCase):
    def test_lab_mapping_preserves_original_entries_exactly(self):
        source = b'[\n{"ns":1,"key":2,"preload":true # original comment\n}]\n'
        with patch("prepare_carplay_usb_capture.MAPPING_SHA256",
                   hashlib.sha256(source).hexdigest()):
            observed = empty_lab_mapping(source)
            entry, remainder = observed[2:].split(b",\n", 1)
            self.assertEqual(json.loads(entry), EMPTY_LAB_RECORD)
            self.assertEqual(remainder, source[2:])
            self.assertEqual(EMPTY_LAB_RECORD["stonebackup"]["default"], [0] * 4)
            self.assertEqual(EMPTY_LAB_RECORD["key"], 2433)
            self.assertEqual(EMPTY_LAB_RECORD["ns"], 24354)
        with self.assertRaises(ValueError):
            empty_lab_mapping(source)

    def test_explicit_empty_fixture_cannot_be_mistaken_for_unit_capture(self):
        with tempfile.TemporaryDirectory() as name:
            output = Path(name) / "fixture"
            shell = fixture_shell(
                b"CARPLAY_USB_WRITE_STATUS:0\nCARPLAY_USB_READ_BEGIN\n00000000\n"
                b"CARPLAY_USB_READ_END\nCARPLAY_USB_READ_STATUS:0\n")
            result = install_empty_lab_ports(shell, output, mapping_default=True)
            self.assertTrue(result["synthetic"])
            self.assertTrue(result["initial_mapping_default"])
            self.assertFalse(result["authentic_vehicle_configuration"])
            self.assertFalse(result["coding_or_fec_supplied"])
            self.assertTrue(result["exact_guest_readback_verified"])
            self.assertEqual(result["decoded_port_mask"], 0)
            self.assertEqual((output / "usb-ports.blob").read_bytes(), bytes(4))
            self.assertNotIn("provenance", json.loads(
                (output / "manifest.json").read_text()))
            self.assertIn("24354 2433 00000000", shell.call_args_list[0].args[0])
            with self.assertRaises(FileExistsError):
                install_empty_lab_ports(shell, output)
            self.assertEqual(shell.call_count, 3)

    def test_empty_fixture_failure_is_not_saved_as_success(self):
        with tempfile.TemporaryDirectory() as name:
            output = Path(name) / "fixture"
            with self.assertRaises(ValueError):
                install_empty_lab_ports(
                    fixture_shell(b"CARPLAY_USB_WRITE_STATUS:1\n"), output)
            self.assertTrue(json.loads(
                (output / "manifest.json").read_text())["synthetic"])
            self.assertFalse((output / "restore-result.json").exists())

    def test_notification_barrier_requires_exact_value_and_cleanup_marker(self):
        for update in (
                "", "CARPLAY_USB_UPDATE_OBSERVED\n",
                "BLOB\t00000001\nCARPLAY_USB_UPDATE_OBSERVED\n",
                "BLOB\t00000000\n",
                "BLOB\t00000000\n" +
                "CARPLAY_USB_UPDATE_OBSERVED\n" * 2):
            with self.subTest(update=update), tempfile.TemporaryDirectory() as name:
                root = Path(name)
                calls = []

                def shell(command):
                    calls.append(command)
                    if "dumb_persistence_writer" in command:
                        return "CARPLAY_USB_WRITE_STATUS:0\n"
                    if "dumb_persistence_subscriber" in command:
                        return update
                    return "failure diagnostics\n"

                with self.assertRaisesRegex(ValueError, "notification"):
                    install_empty_lab_ports(shell, root / "fixture")
                self.assertFalse(any("dumb_persistence_reader" in command for command in calls))
                self.assertFalse((root / "fixture/restore-result.json").exists())

    def test_integrator_must_independently_accept_exact_port_mask(self):
        restored = {"decoded_port_mask": 5}
        verify_restored_ports({"usb_metadata": {"ports": {"valid": True, "value": 5}}},
                              restored)
        for readiness in (
                {}, {"usb_metadata": {}},
                {"usb_metadata": {"ports": {"valid": False, "value": None}}},
                {"usb_metadata": {"ports": {"valid": True, "value": 4}}},
                {"usb_metadata": {"ports": {"valid": True, "value": True}}}):
            with self.subTest(readiness=readiness), self.assertRaises(ValueError):
                verify_restored_ports(readiness, restored)
        verify_restored_ports({"usb_metadata": {"ports": {"valid": True, "value": 0}}},
                              {"decoded_port_mask": 0})

    def restore(self, root, reply, raw=b"\xff\x80\x03\xfe"):
        source, meta = root / "source", root / "provenance"
        source.write_bytes(raw)
        meta.write_text(json.dumps(provenance(raw)))
        shell = fixture_shell(reply)
        result = restore_capture(shell, source, meta, root / "prepared", "raw")
        return shell, result

    def test_explicit_restore_requires_original_write_and_exact_readback(self):
        reply = (
            "echo CARPLAY_USB_WRITE_STATUS:$USB_WRITE_STATUS; command\r\n"
            "CARPLAY_USB_WRITE_STATUS:0\r\n"
            "CARPLAY_USB_READ_BEGIN\r\nff8003fe\r\nCARPLAY_USB_READ_END\r\n"
            "CARPLAY_USB_READ_STATUS:0\r\n")
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            shell, result = self.restore(root, reply)
            self.assertTrue(result["exact_guest_readback_verified"])
            self.assertFalse(result["component_activation_verified"])
            self.assertEqual(shell.call_count, 3)
            command = shell.call_args_list[0].args[0]
            self.assertIn("/eso/bin/dumb_persistence_writer --flush 24354 2433 ff8003fe", command)
            self.assertIn("/eso/bin/dumb_persistence_reader 24354 2433",
                          shell.call_args_list[2].args[0])
            for call in shell.call_args_list:
                self.assertLess(len(call.args[0]), 800)
                parsed = subprocess.run(
                    ["sh", "-n"], input=call.args[0], text=True, capture_output=True)
                self.assertEqual(parsed.returncode, 0, parsed.stderr)
            self.assertTrue(result["original_value_notification_verified"])
            self.assertTrue(result["owned_subscriber_stopped"])
            self.assertEqual((root / "prepared/guest-restore.log").read_text(),
                             reply.replace("\r\n", "\n"))
            self.assertTrue((root / "prepared/restore-result.json").exists())

    def test_restore_failure_never_saves_success(self):
        reply = (
            "CARPLAY_USB_WRITE_STATUS:0\n"
            "CARPLAY_USB_READ_BEGIN\nff8003fe\nCARPLAY_USB_READ_END\n"
            "CARPLAY_USB_READ_STATUS:0\n")
        for bad in (
                "", reply.replace("WRITE_STATUS:0", "WRITE_STATUS:1"),
                reply.replace("READ_STATUS:0", "READ_STATUS:2"),
                reply.replace("ff8003fe", "ff8003ff"),
                reply.replace("ff8003fe", "ff8003"),
                reply.replace("ff8003fe", "PERS_STATUS_DOES_NOT_EXIST"),
                reply.replace("ff8003fe", "[pfsn] unknown\nff8003fe"),
                reply + reply, reply.replace("CARPLAY_USB_READ_STATUS:0", "")):
            with self.subTest(reply=bad), tempfile.TemporaryDirectory() as name:
                root = Path(name)
                with self.assertRaises(ValueError):
                    self.restore(root, bad)
                self.assertEqual((root / "prepared/guest-restore.log").read_text(), bad)
                self.assertEqual((root / "prepared/failure-diagnostic.log").read_text(), bad)
                self.assertFalse((root / "prepared/restore-result.json").exists())

    def test_exact_process_notifications_are_logged_not_treated_as_record_bytes(self):
        reply = (
            "CARPLAY_USB_WRITE_STATUS:0\nCARPLAY_USB_READ_BEGIN\n"
            '[pfsn] Gone PID=2125872 PPID=2109482 "sh" "-c" "sh"\n'
            "ff8003fe\n"
            '[pfsn] New PID=2183218 PPID=2097191 Prio=10 "/eso/bin/apps/mmxfilestorage" ...\n'
            "CARPLAY_USB_READ_END\nCARPLAY_USB_READ_STATUS:0\n")
        with tempfile.TemporaryDirectory() as name, self.assertLogs(
                "prepare_carplay_usb_capture", level="WARNING") as logs:
            root = Path(name)
            _, result = self.restore(root, reply)
            self.assertEqual(len(result["separate_process_notifications"]), 2)
            self.assertEqual(len(logs.output), 2)
            self.assertTrue(result["exact_guest_readback_verified"])
            self.assertEqual((root / "prepared/guest-restore.log").read_text(), reply)

    def test_invalid_input_never_contacts_restore_guest(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, meta = root / "source", root / "provenance"
            source.write_bytes(bytes(4))
            meta.write_text(json.dumps(provenance(b"different")))
            shell = Mock()
            with self.assertRaises(ValueError):
                restore_capture(shell, source, meta, root / "prepared", "raw")
            shell.assert_not_called()
            self.assertFalse((root / "prepared").exists())

    def test_raw_bytes_are_preserved_including_high_bits(self):
        raw = bytes((0xff, 0x80, 0x03, 0xfe))
        self.assertEqual(validate_capture(raw, provenance(raw), "raw"), raw)

    def test_zero_is_valid_data_not_a_missing_record(self):
        raw = bytes(4)
        self.assertEqual(validate_capture(raw, provenance(raw), "raw"), raw)

    def test_composite_extracts_only_mapped_bytes(self):
        raw = bytes(range(90))
        self.assertEqual(validate_capture(raw, provenance(raw, "composite"),
                                          "composite"), raw[30:34])

    def test_no_padding_truncation_or_cross_format_guessing(self):
        for source_format, length in (("raw", 4), ("composite", 90)):
            for size in (0, length - 1, length + 1):
                raw = bytes(size)
                meta = provenance(raw, source_format)
                meta["length"] = length
                with self.subTest(format=source_format, size=size):
                    with self.assertRaisesRegex(ValueError, "captured bytes"):
                        validate_capture(raw, meta, source_format)
        with self.assertRaisesRegex(ValueError, "namespace"):
            validate_capture(bytes(90), provenance(bytes(90), "composite"), "raw")

    def test_wrong_identity_type_and_boolean_fields_rejected(self):
        raw = bytes(4)
        for field, value in (("namespace", 1), ("key", 1), ("length", True),
                             ("type", "INT"), ("sha256", "0" * 64)):
            meta = provenance(raw)
            meta[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_capture(raw, meta, "raw")

    def test_missing_extra_or_invalid_provenance_rejected(self):
        raw = bytes(4)
        for field in ("firmware", "hardware", "capture_method", "captured_at"):
            for value in ("", " ", "line\nbreak", "line\x00break", 42, None):
                meta = provenance(raw)
                meta[field] = value
                with self.subTest(field=field, value=value):
                    with self.assertRaises(ValueError):
                        validate_capture(raw, meta, "raw")
        for meta in (None, [], {}, dict(provenance(raw), unexpected="value")):
            with self.subTest(meta=meta), self.assertRaises(ValueError):
                validate_capture(raw, meta, "raw")
        with self.assertRaises(ValueError):
            validate_capture(raw, provenance(raw), "auto")

    def test_preparation_preserves_inputs_and_makes_no_activation_claim(self):
        for source_format in FORMATS:
            with self.subTest(format=source_format), tempfile.TemporaryDirectory() as name:
                root = Path(name)
                raw = bytes(range(FORMATS[source_format]["length"]))
                source, meta, output = root / "source", root / "provenance", root / "prepared"
                source.write_bytes(raw)
                meta.write_text(json.dumps(provenance(raw, source_format)))
                original_meta = meta.read_bytes()
                report = prepare(source, meta, output, source_format)
                offset = FORMATS[source_format]["offset"]
                self.assertEqual((output / "usb-ports.blob").read_bytes(),
                                 raw[offset:offset + 4])
                self.assertEqual(report["decoded_port_mask"], 10)
                self.assertEqual(report["record"]["sha256"],
                                 hashlib.sha256(raw[offset:offset + 4]).hexdigest())
                for field in ("installed", "source_authenticity_independently_verified",
                              "component_activation_verified", "live_hmi_dsi_delivery_verified"):
                    self.assertFalse(report[field])
                self.assertEqual(json.loads((output / "manifest.json").read_text()), report)
                self.assertEqual(source.read_bytes(), raw)
                self.assertEqual(meta.read_bytes(), original_meta)
                with self.assertRaises(FileExistsError):
                    prepare(source, meta, output, source_format)

    def test_invalid_capture_never_creates_output(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, meta, output = root / "source", root / "provenance", root / "prepared"
            source.write_bytes(b"bad")
            meta.write_text(json.dumps(provenance(b"bad")))
            with self.assertRaises(ValueError):
                prepare(source, meta, output, "raw")
            self.assertFalse(output.exists())

    def test_existing_input_aliases_are_never_overwritten(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, meta, alias = root / "source", root / "provenance", root / "alias"
            raw = b"\xff\x80\x03\xfe"
            source.write_bytes(raw)
            meta.write_text(json.dumps(provenance(raw)))
            before_meta = meta.read_bytes()
            alias.symlink_to(source)
            for output in (source, meta, alias, root):
                with self.subTest(output=output), self.assertRaises(FileExistsError):
                    prepare(source, meta, output, "raw")
            self.assertEqual(source.read_bytes(), raw)
            self.assertEqual(meta.read_bytes(), before_meta)


if __name__ == "__main__":
    unittest.main()
