# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess

import prepare_carplay_lab_image as lab
import prepare_carplay_country_policy as country
import prepare_carplay_wireless_policy as wireless
from qnx6_image import build
from test_prepare_carplay_country_policy import fixture as country_fixture
from test_prepare_carplay_wireless_policy import fixture as wireless_fixture


class LabImageTests(unittest.TestCase):
    def test_isolated_copy_changes_only_guarded_data_bytes(self):
        for use_wireless, shared_ap in ((True, False), (False, True), (True, True)):
            with self.subTest(wireless=use_wireless, shared_ap=shared_ap), \
                    tempfile.TemporaryDirectory() as name:
                root = Path(name)
                apps = root / "files/eso/bin/apps"
                apps.mkdir(parents=True)
                originals = {"smartphone_integrator": wireless_fixture(),
                             "connectionmanager": country_fixture()}
                for filename, raw in originals.items():
                    (apps / filename).write_bytes(raw)
                source, output = root / "source.img", root / "lab"
                build(str(root / "files"), str(source))
                before = source.read_bytes()
                with patch.object(wireless, "ORIGINAL_SHA256", hashlib.sha256(
                        originals["smartphone_integrator"]).hexdigest()), \
                        patch.object(country, "ORIGINAL_SHA256", hashlib.sha256(
                            originals["connectionmanager"]).hexdigest()):
                    manifest = lab.prepare(source, output, partition_offset=0,
                                           wireless=use_wireless, shared_ap=shared_ap)
                self.assertEqual(source.read_bytes(), before)
                expected = bytearray(before)
                for app in manifest["applications"]:
                    for write in app["writes"]:
                        self.assertEqual(expected[write["image_offset"]], write["before"])
                        expected[write["image_offset"]] = write["after"]
                self.assertEqual((output / "mnand.bin").read_bytes(), expected)
                self.assertTrue(manifest["output_executables_verified"])
                self.assertFalse(manifest["filesystem_metadata_changed"])
                self.assertFalse(manifest["guest_startup_verified"])
                with self.assertRaises(FileExistsError):
                    lab.prepare(source, output, partition_offset=0)

    def test_wrong_identity_leaves_no_copy(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            apps = root / "files/eso/bin/apps"
            apps.mkdir(parents=True)
            (apps / "smartphone_integrator").write_bytes(b"unknown executable")
            source, output = root / "source.img", root / "lab"
            build(str(root / "files"), str(source))
            before = source.read_bytes()
            with self.assertRaisesRegex(ValueError, "identity"):
                lab.prepare(source, output, partition_offset=0)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), before)

    def test_explicit_selection_and_offset_required(self):
        with self.assertRaisesRegex(ValueError, "Select"):
            lab.prepare(Path("unused"), Path("unused"), wireless=False, shared_ap=False)
        for offset in (-1, True, 1.5):
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "offset"):
                lab.prepare(Path("unused"), Path("unused"), partition_offset=offset)

    def test_missing_application_leaves_no_copy(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / "files").mkdir()
            source, output = root / "source.img", root / "lab"
            build(str(root / "files"), str(source))
            with self.assertRaisesRegex(ValueError, "missing"):
                lab.prepare(source, output, partition_offset=0)
            self.assertFalse(output.exists())

    def test_nonzero_partition_offset_and_failed_copy(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            apps = root / "files/eso/bin/apps"
            apps.mkdir(parents=True)
            raw = wireless_fixture()
            (apps / "smartphone_integrator").write_bytes(raw)
            partition, source = root / "partition.img", root / "disk.img"
            build(str(root / "files"), str(partition))
            before = b"\x5a" * 0x200000 + partition.read_bytes()
            source.write_bytes(before)
            with patch.object(wireless, "ORIGINAL_SHA256", hashlib.sha256(raw).hexdigest()):
                failed = root / "failed"
                with patch.object(lab.subprocess, "run",
                                  side_effect=subprocess.CalledProcessError(1, ["cp"])):
                    with self.assertRaises(subprocess.CalledProcessError):
                        lab.prepare(source, failed)
                self.assertFalse((failed / "manifest.json").exists())
                output = root / "lab"
                result = lab.prepare(source, output)
            self.assertEqual(source.read_bytes(), before)
            self.assertTrue(all(w["image_offset"] >= 0x200000
                                for app in result["applications"] for w in app["writes"]))
            self.assertEqual((output / "mnand.bin").read_bytes()[:0x200000], before[:0x200000])


if __name__ == "__main__":
    unittest.main()
