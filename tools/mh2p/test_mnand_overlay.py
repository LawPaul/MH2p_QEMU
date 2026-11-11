# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

from mnand_overlay import create, validate


class MnandOverlayTests(unittest.TestCase):
    def test_writes_survive_reopen_without_changing_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            original = bytes(1024 * 1024)
            source.write_bytes(original)
            directory = root / "disk"
            image = create(source, directory)
            subprocess.run(["qemu-io", "-f", "qcow2", "-c",
                            "write -P 0x5a 4096 512", str(image)],
                           capture_output=True, check=True)
            self.assertEqual(validate(source, directory), image)
            # A separate process reopens the disk, as a later boot would.
            subprocess.run(["qemu-io", "-f", "qcow2", "-c",
                            "read -P 0x5a 4096 512", str(image)],
                           capture_output=True, check=True)
            subprocess.run(["qemu-io", "-f", "qcow2", "-c",
                            "read -P 0x00 8192 512", str(image)],
                           capture_output=True, check=True)
            self.assertEqual(source.read_bytes(), original)
            with self.assertRaises(FileExistsError):
                create(source, directory)

    def test_changed_source_metadata_and_symlink_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            source.write_bytes(bytes(4096))
            directory = root / "disk"
            image = create(source, directory)
            source.write_bytes(b"x" * 4096)
            with self.assertRaisesRegex(ValueError, "identity"):
                validate(source, directory)
            source.write_bytes(bytes(4096))
            metadata = directory / "identity.json"
            identity = json.loads(metadata.read_text())
            metadata.write_text("{}")
            with self.assertRaisesRegex(ValueError, "identity"):
                validate(source, directory)
            metadata.write_text(json.dumps(identity))
            relocated = directory / "relocated.qcow2"
            image.rename(relocated)
            image.symlink_to(relocated)
            with self.assertRaisesRegex(ValueError, "regular"):
                validate(source, directory)

    def test_rebased_overlay_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            other = root / "other.bin"
            source.write_bytes(bytes(4096))
            other.write_bytes(bytes(4096))
            directory = root / "disk"
            image = create(source, directory)
            subprocess.run(["qemu-img", "rebase", "-u", "-f", "qcow2",
                            "-F", "raw", "-b", str(other), str(image)],
                           capture_output=True, check=True)
            with self.assertRaisesRegex(ValueError, "backing"):
                validate(source, directory)
