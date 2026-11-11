# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free package/install/uninstall tests using the actual Korn shell scripts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

import prepare_carplay_modkit as modkit


class CarplayModkitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mh2p-modkit-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.original = self.root / "original.jar"
        self.prepared = self.root / "prepared.jar"
        for path in (self.original, self.prepared):
            with ZipFile(path, "w") as archive:
                for name in modkit.OVERLAY_ENTRIES:
                    archive.writestr(name, b"synthetic:" + name.encode())
                archive.writestr("unrelated", b"not packaged")
        self.launcher = self.root / "lsd.sh"
        self.launcher.write_text(
            'VMOPTIONS="$VMOPTIONS -DIS_PRODUCTION_MODE=true"\n'
            + modkit.ANCHOR + "\n")
        self.launcher.chmod(0o755)
        self.native = self.root / "dio_manager"
        self.native.write_text(
            '#!/bin/sh\nprintf "CONFIG=%s\\nKEEP=%s\\n" '
            '"$IPL_CONFIG_DIR_DIO_MANAGER" "$KEEP"\nprintf "<%s>\\n" "$@"\n')
        self.native.chmod(0o755)
        self.config = self.root / "dio_manager.json"
        self.config.write_text('{"screen":{"fingers":1,"width":1920},"keep":true}\n')
        for name, path in (("LAUNCHER_SHA256", self.launcher),
                           ("NATIVE_SHA256", self.native), ("CONFIG_SHA256", self.config)):
            guard = patch.object(modkit, name, hashlib.sha256(path.read_bytes()).hexdigest())
            guard.start()
            self.addCleanup(guard.stop)
            guard = patch.object(modkit, "build_checksum",
                                 return_value=b"#!/bin/sh\nexec /usr/bin/cksum\n")
            guard.start()
            self.addCleanup(guard.stop)
        guard = patch.object(modkit, "validate_archives", return_value=["source", "target"])
        guard.start()
        self.addCleanup(guard.stop)
        self.output = self.root / "package"
        self.ksh = os.environ.get("MH2P_TEST_KSH") or shutil.which("ksh")
        if not self.ksh:
            self.fail("Korn shell missing: set MH2P_TEST_KSH or install ksh")

    def build(self):
        return modkit.prepare(self.original, self.prepared, self.launcher,
                              self.native, self.config, self.output)

    def install_fixture(self):
        self.build()
        self.update = self.output / "Mods" / modkit.MOD / "Update"
        self.app = self.root / "app"
        self.state = self.app / "eso/hmi/lsd/.mh2p-carplay-multitouch"
        self.live_launcher = self.app / "eso/hmi/lsd/lsd.sh"
        self.live_native = self.app / "eso/bin/apps/dio_manager"
        self.live_launcher.parent.mkdir(parents=True)
        self.live_native.parent.mkdir(parents=True)
        shutil.copy2(self.launcher, self.live_launcher)
        shutil.copy2(self.original, self.live_launcher.with_name("lsd.jar"))
        shutil.copy2(self.native, self.live_native)
        for name in ("common.sh", "dio_manager.sh", "lsd.sh"):
            path = self.update / name
            path.write_bytes(path.read_bytes().replace(b"/mnt/app", str(self.app).encode()))
        identity = self.update / "identity.sh"
        source = identity.read_text()
        for key, name in (("LAUNCHER_PATCHED_CKSUM", "lsd.sh"),
                          ("WRAPPER_CKSUM", "dio_manager.sh")):
            source = "\n".join(
                f"{key}='{modkit.checksum((self.update / name).read_bytes())}'"
                if line.startswith(key + "=") else line for line in source.splitlines()) + "\n"
        identity.write_text(source)
        (self.update / "identity.cksum").write_text(modkit.checksum(identity.read_bytes()) + "\n")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "mount").write_text('#!/bin/sh\nexit "${MOUNT_STATUS:-0}"\n')
        (self.bin / "mount").chmod(0o755)
        self.env = dict(
            os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
            MOD_PATH=str(self.update), MODKIT_VERSION="2", RELEASE_VERSION=modkit.RELEASE,
            REGION="ER", OEM="PO", TYPE="G35", RELEASE_TYPE="P", SOFTWARE_VERSION="2865")
        (self.update.parent / "enable-experimental.txt").touch()

    def run_script(self, name, success=True, **env):
        result = subprocess.run([self.ksh, str(self.update / name)], env=dict(self.env, **env),
                                text=True, capture_output=True, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("ERROR:", result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ERROR:", result.stdout + result.stderr)
        return result

    def test_package_shape_determinism_and_minimal_payload(self):
        before = {path: path.read_bytes() for path in (
            self.original, self.prepared, self.launcher, self.native, self.config)}
        report = self.build()
        repeated = self.root / "repeat"
        modkit.prepare(self.original, self.prepared, self.launcher,
                       self.native, self.config, repeated)
        bundle = f"{modkit.MOD}-{modkit.RELEASE}.zip"
        self.assertEqual((self.output / bundle).read_bytes(), (repeated / bundle).read_bytes())
        update = self.output / "Mods" / modkit.MOD / "Update"
        with ZipFile(update / "contacts.jar") as archive:
            self.assertEqual(archive.namelist(), list(modkit.OVERLAY_ENTRIES))
            self.assertNotIn("unrelated", archive.namelist())
        with ZipFile(self.output / bundle) as archive:
            self.assertTrue(all(name.startswith(f"Mods/{modkit.MOD}/") for name in archive.namelist()))
            self.assertFalse(any(name.endswith("/dio_manager") for name in archive.namelist()))
        config = json.loads((update / "dio_manager.json").read_text())
        self.assertEqual(config, {"screen": {"fingers": 2, "width": 1920}, "keep": True})
        self.assertNotIn("terminalmode.dio", (update / "lsd.sh").read_text())
        self.assertIn("-DIS_PRODUCTION_MODE=true", (update / "lsd.sh").read_text())
        self.assertTrue(report["experimental"])
        self.assertFalse(report["iphone_multitouch_verified"])
        for path, raw in before.items():
            self.assertEqual(path.read_bytes(), raw)

    def test_unknown_sources_and_existing_output_rejected_before_creation(self):
        for path in (self.launcher, self.native, self.config):
            original = path.read_bytes()
            path.write_bytes(original + b"unexpected")
            with self.assertRaises(ValueError):
                self.build()
            self.assertFalse(self.output.exists())
            path.write_bytes(original)
        self.output.mkdir()
        with self.assertRaises(FileExistsError):
            self.build()

    def test_install_repeat_uninstall_reinstall_and_native_arguments(self):
        self.install_fixture()
        self.run_script("install.sh")
        self.run_script("install.sh")
        result = subprocess.run(
            ["sh", str(self.live_native), "-sm-name", "dio_manager", "a b", ""],
            env=dict(self.env, KEEP="unchanged", IPL_CONFIG_DIR_DIO_MANAGER="/original"),
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout,
                         f"CONFIG={self.state}\nKEEP=unchanged\n"
                         "<-sm-name>\n<dio_manager>\n<a b>\n<>\n")
        self.run_script("uninstall.sh")
        self.assertEqual(self.live_native.read_bytes(), self.native.read_bytes())
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())
        self.run_script("uninstall.sh")
        self.run_script("install.sh")
        self.run_script("uninstall.sh")

    def test_staging_and_private_payload_symlinks_refused_before_copy(self):
        self.install_fixture()
        unrelated = self.root / "unrelated"
        unrelated.write_bytes(b"must remain unchanged")
        staged = self.live_native.with_name("dio_manager.mh2p-multitouch.tmp")
        staged.symlink_to(unrelated)
        self.run_script("install.sh", success=False)
        self.assertEqual(unrelated.read_bytes(), b"must remain unchanged")
        self.assertEqual(self.live_native.read_bytes(), self.native.read_bytes())
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())
        staged.unlink()
        self.run_script("install.sh")
        payload = self.state / "contacts.jar"
        payload.unlink()
        payload.symlink_to(unrelated)
        self.run_script("install.sh", success=False)
        self.assertEqual(unrelated.read_bytes(), b"must remain unchanged")
        self.assertEqual(self.live_native.read_bytes(), self.native.read_bytes())
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())

    def test_missing_or_symlink_checksum_helper_fails_before_live_changes(self):
        self.install_fixture()
        helper = self.update / "cksum"
        helper.unlink()
        self.run_script("install.sh", success=False)
        helper.symlink_to("/usr/bin/cksum")
        self.run_script("install.sh", success=False)
        self.assertFalse(self.state.exists())
        self.assertEqual(self.live_native.read_bytes(), self.native.read_bytes())
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())

    def test_firmware_acknowledgement_mount_and_payload_fail_closed(self):
        self.install_fixture()
        for env in ({"SOFTWARE_VERSION": "2870"}, {"MODKIT_VERSION": "1"},
                    {"RELEASE_VERSION": "MH2p_US_PO416_P2865"}, {"MOUNT_STATUS": "1"}):
            self.run_script("install.sh", success=False, **env)
            self.assertFalse(self.state.exists())
        acknowledgement = self.update.parent / "enable-experimental.txt"
        acknowledgement.unlink()
        self.run_script("install.sh", success=False)
        acknowledgement.touch()
        (self.update / "contacts.jar").write_bytes(b"corrupt")
        self.run_script("install.sh", success=False)
        self.assertFalse(self.state.exists())
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())

    def test_conflicting_live_files_backups_and_symlinks_are_not_overwritten(self):
        self.install_fixture()
        self.run_script("install.sh")
        original = self.live_launcher.read_bytes()
        self.live_launcher.write_bytes(b"other mod")
        for name in ("install.sh", "uninstall.sh"):
            self.run_script(name, success=False)
            self.assertEqual(self.live_launcher.read_bytes(), b"other mod")
        self.live_launcher.write_bytes(original)
        backup = self.state / "original/dio_manager"
        raw = backup.read_bytes()
        backup.write_bytes(b"corrupt")
        self.run_script("uninstall.sh", success=False)
        backup.write_bytes(raw)
        saved = self.root / "saved-launcher"
        self.live_launcher.rename(saved)
        self.live_launcher.symlink_to(saved)
        self.run_script("install.sh", success=False)
        self.run_script("uninstall.sh", success=False)
        self.assertEqual(saved.read_bytes(), original)

    def test_partial_install_uninstalls_and_failed_copy_rolls_back(self):
        self.install_fixture()
        self.run_script("install.sh")
        shutil.copy2(self.launcher, self.live_launcher)
        self.run_script("uninstall.sh")
        (self.bin / "cp").write_text(
            '#!/bin/sh\nfor arg do\n'
            'case "$arg" in */Update/lsd.sh) exit 7;; esac\ndone\nexec /bin/cp "$@"\n')
        (self.bin / "cp").chmod(0o755)
        self.run_script("install.sh", success=False)
        self.assertEqual(self.live_launcher.read_bytes(), self.launcher.read_bytes())
        self.assertEqual(self.live_native.read_bytes(), self.native.read_bytes())
        (self.bin / "cp").unlink()
        self.run_script("install.sh")
        self.run_script("uninstall.sh")

    def test_uninstall_absent_and_missing_native_runtime_explicit(self):
        self.install_fixture()
        self.run_script("uninstall.sh")
        self.run_script("install.sh")
        (self.state / "dio_manager.json").unlink()
        result = subprocess.run(["sh", str(self.live_native)], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR:", result.stderr)
        self.run_script("uninstall.sh")


if __name__ == "__main__":
    unittest.main()
