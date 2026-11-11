# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for the opt-in Java contact-slot archive patch."""
import hashlib
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

import prepare_carplay_java_slots as slots


def utf8(text):
    encoded = text.encode("ascii")
    return b"\x01" + struct.pack(">H", len(encoded)) + encoded


def fixture(method="updateTouchEvents", descriptor=slots.DESCRIPTOR):
    pool = b"".join(utf8(text) for text in (
        method, descriptor, "Code", "LineNumberTable",
        "LocalVariableTable"))
    body = b"\x00\xaa" + b"\x00" * 18 + b"\xb1"
    line = struct.pack(">HIHHH", 4, 6, 1, 1, 42)
    local = struct.pack(">HI6H", 5, 12, 1, 1, 10, 1, 2, 1)
    code = (struct.pack(">HHI", 2, 3, len(body)) + body +
            struct.pack(">HH", 0, 2) + line + local)
    prefix = (bytes.fromhex("cafebabe00000030") + struct.pack(">H", 6) + pool +
              struct.pack(">6H", 0, 0, 0, 0, 0, 1) +
              struct.pack(">4H", 0, 1, 2, 1))
    return prefix + struct.pack(">HI", 3, len(code)) + code + b"\0\0", body


class JavaSlotPreparationTests(unittest.TestCase):
    def setUp(self):
        self.source, self.body = fixture()
        self.manager, _ = fixture("deactivate", "()V")
        self.listener, _ = fixture(
            "determineTouchEvents",
            "(Lde/audi/app/terminalmode/keyevents/TouchPointFilter$TouchPointFilterResults;)"
            "[Lde/audi/app/terminalmode/keyevents/TouchEvent;")
        listener_guard = patch.object(slots, "LISTENER_SHA256",
                                      hashlib.sha256(self.listener).hexdigest())
        listener_guard.start()
        self.addCleanup(listener_guard.stop)
        self.filter, _ = fixture(
            "filter",
            "([Lde/audi/atip/datatypes/TouchPoint;Lde/audi/app/terminalmode/device/TMDevice;)"
            "Lde/audi/app/terminalmode/keyevents/TouchPointFilter$TouchPointFilterResults;")
        filter_guard = patch.object(slots, "FILTER_SHA256",
                                    hashlib.sha256(self.filter).hexdigest())
        filter_guard.start()
        self.addCleanup(filter_guard.stop)
        manager_guard = patch.object(slots, "MANAGER_SHA256",
                                      hashlib.sha256(self.manager).hexdigest())
        manager_guard.start()
        self.addCleanup(manager_guard.stop)
        self.guard = patch.object(slots, "CONTROLLER_SHA256",
                                  hashlib.sha256(self.source).hexdigest())
        self.guard.start()
        self.addCleanup(self.guard.stop)

    def test_exact_aligned_prefix_and_debug_offsets(self):
        result = slots.patch_controller(self.source)
        prefix = b"\x2a\x2b\xb8\x00\x0b\x4c\x2b\xbe\x9a\x00\x08\xb1\0\0\0\0"
        self.assertIn(prefix + self.body, result)
        self.assertEqual(len(prefix) % 4, 0)
        self.assertEqual(struct.unpack_from(">H", result, 8)[0], 12)
        self.assertIn(struct.pack(">HIHHH", 4, 6, 1, 17, 42), result)
        self.assertIn(struct.pack(">HI6H", 5, 12, 1, 17, 10, 1, 2, 1), result)
        self.assertTrue(result.endswith(b"\0\0"))

    def test_manager_and_listener_prefixes_preserve_alignment_and_bodies(self):
        manager = slots.patch_manager(self.manager)
        listener = slots.patch_listener(self.listener)
        filter_class = slots.patch_filter(self.filter)
        self.assertIn(
            b"\x2a\xb4\x00\x48\x99\x00\x14"
            b"\x2a\xb4\x00\x1f\x2a\xb4\x00\x08\xb9\x00\x50\x01\x00"
            b"\xb8\x00\x0b\0" + self.body, manager)
        self.assertIn(
            b"\x2a\x2a\xb4\x00\x0a\x2a\xb4\x00\x0e\xb8\x00\x0b" + self.body, listener)
        self.assertIn(b"\x2a\x2a\xb4\x00\x03\xb8\x00\x0b" + self.body, filter_class)
        self.assertIn(struct.pack(">HIHHH", 4, 6, 1, 25, 42), manager)
        self.assertIn(struct.pack(">HIHHH", 4, 6, 1, 13, 42), listener)
        self.assertIn(struct.pack(">HIHHH", 4, 6, 1, 9, 42), filter_class)

    def test_unknown_controller_rejected(self):
        with self.assertRaisesRegex(ValueError, "original P2865"):
            slots.patch_controller(self.source + b"changed")

    def test_shared_archive_validator_checks_every_unchanged_entry(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            original = root / "source.jar"
            self.write_archive(original)
            with patch.object(slots.subprocess, "run", side_effect=self.compiler):
                slots.prepare(original, root / "prepared", "java", "ecj")
            prepared = root / "prepared/lsd.jar"
            expected = hashlib.sha256(original.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "original P2865 HMI"):
                slots.validate_archives(original, prepared)
            with patch.object(slots, "ARCHIVE_SHA256", expected):
                self.assertEqual(slots.validate_archives(original, prepared)[0], expected)
                for changed_name, changed_bytes, message in (
                        ("unrelated/data.bin", b"changed", "Unrelated prepared"),
                        (slots.MANAGER, b"changed", "Prepared class differs"),
                        (slots.HELPER, bytes.fromhex("cafebabe00000034"), "not Java 1.4")):
                    with self.subTest(entry=changed_name):
                        changed = root / "changed.jar"
                        with ZipFile(prepared) as source, ZipFile(changed, "w") as target:
                            for entry in source.infolist():
                                target.writestr(entry, changed_bytes if entry.filename ==
                                                changed_name else source.read(entry.filename))
                        with self.assertRaisesRegex(ValueError, message):
                            slots.validate_archives(original, changed)

    def write_archive(self, path, extra=None):
        with ZipFile(path, "w") as archive:
            archive.comment = b"preserve archive metadata"
            archive.writestr(slots.CONTROLLER, self.source)
            archive.writestr(slots.MANAGER, self.manager)
            archive.writestr(slots.LISTENER, self.listener)
            archive.writestr(slots.FILTER, self.filter)
            info = ZipInfo("unrelated/data.bin", (2005, 6, 7, 8, 9, 10))
            info.compress_type = ZIP_DEFLATED
            info.comment = b"entry metadata"
            archive.writestr(info, b"unchanged content")
            if extra:
                archive.writestr(extra, b"unsupported")

    @staticmethod
    def compiler(command, **kwargs):
        destination = Path(command[-2]) / slots.HELPER
        destination.parent.mkdir(parents=True)
        destination.write_bytes(bytes.fromhex("cafebabe00000030") + b"test fixture")
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    def test_only_four_guarded_classes_and_new_helper_changed(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            archive = root / "original.jar"
            self.write_archive(archive)
            before = archive.read_bytes()
            with patch.object(slots.subprocess, "run", side_effect=self.compiler):
                result = slots.prepare(archive, root / "prepared", "java", "ecj")
            self.assertEqual(archive.read_bytes(), before)
            self.assertEqual(result["helper_class_major_version"], 48)
            self.assertTrue(result["manager_deactivation_hook_requires_original_active_flag"])
            self.assertTrue(result["filter_reset_only_after_carplay_deactivation"])
            with ZipFile(archive) as source, ZipFile(root / "prepared/lsd.jar") as target:
                self.assertEqual(source.comment, target.comment)
                self.assertEqual(target.read(slots.CONTROLLER),
                                 slots.patch_controller(self.source))
                self.assertEqual(target.read(slots.MANAGER), slots.patch_manager(self.manager))
                self.assertEqual(target.read(slots.LISTENER), slots.patch_listener(self.listener))
                self.assertEqual(target.read(slots.FILTER), slots.patch_filter(self.filter))
                self.assertEqual(source.read("unrelated/data.bin"),
                                 target.read("unrelated/data.bin"))
                for field in ("date_time", "compress_type", "comment", "external_attr"):
                    self.assertEqual(getattr(source.getinfo("unrelated/data.bin"), field),
                                     getattr(target.getinfo("unrelated/data.bin"), field))

    def test_reject_signed_preexisting_helper_and_duplicate_entries(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for index, extra in enumerate(("META-INF/X.RSA", slots.HELPER,
                                           slots.CONTROLLER)):
                archive = root / f"source-{index}.jar"
                with self.assertWarns(UserWarning) if extra == slots.CONTROLLER else (
                        self.subTest(extra=extra)):
                    self.write_archive(archive, extra)
                output = root / f"output-{index}"
                with self.assertRaises(ValueError):
                    slots.prepare(archive, output, "java", "ecj")
                self.assertFalse(output.exists())

    def test_compile_failure_has_diagnostics_and_no_output(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            archive = root / "source.jar"
            self.write_archive(archive)
            with patch.object(slots.subprocess, "run", return_value=SimpleNamespace(
                    returncode=1, stdout="", stderr="missing compiler classes")):
                with self.assertRaisesRegex(RuntimeError, "missing compiler classes"):
                    slots.prepare(archive, root / "output", "java", "ecj")
            self.assertFalse((root / "output").exists())

    def test_java_eight_helper_rejected_before_output_created(self):
        def modern_compiler(command, **kwargs):
            result = self.compiler(command, **kwargs)
            (Path(command[-2]) / slots.HELPER).write_bytes(
                bytes.fromhex("cafebabe00000034"))
            return result

        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            archive = root / "source.jar"
            self.write_archive(archive)
            with patch.object(slots.subprocess, "run", side_effect=modern_compiler):
                with self.assertRaisesRegex(ValueError, "Java 1.4 class format"):
                    slots.prepare(archive, root / "output", "java", "ecj")
            self.assertFalse((root / "output").exists())

    def test_existing_output_rejected_before_inputs_opened(self):
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(FileExistsError):
                slots.prepare(Path("missing.jar"), Path(name), "java", "ecj")


if __name__ == "__main__":
    unittest.main()
