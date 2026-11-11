# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks for opt-in HMI boot-classpath integration."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

import prepare_carplay_java_overlay as overlay


class CarplayOverlayTests(unittest.TestCase):
    def test_only_boot_classpath_changes(self):
        source = ("VMOPTIONS=\"$VMOPTIONS -DIS_PRODUCTION_MODE=true -noverify\"\n"
                  + overlay.CLASSPATH_ANCHOR + "\n"
                  '$JAVA $VMOPTIONS -Xbootclasspath:$BOOTCLASSPATH de.dreisoft.lsd.LSD &\n')
        result = overlay.overlay_launcher(source)
        self.assertIn("-DIS_PRODUCTION_MODE=true -noverify", result)
        self.assertIn(f'BOOTCLASSPATH="{overlay.GUEST_OVERLAY}:$BOOTCLASSPATH:$LSD_JXE"', result)
        self.assertIn('exit 1;', result)
        self.assertTrue(result.endswith(
            '$JAVA $VMOPTIONS -Xbootclasspath:$BOOTCLASSPATH de.dreisoft.lsd.LSD &\n'))
        self.assertNotIn("terminalmode.dio", result)

    def test_java_activation_requires_explicit_laboratory_opt_in(self):
        source = 'VMOPTIONS="$VMOPTIONS -DIS_PRODUCTION_MODE=true"\n' + overlay.CLASSPATH_ANCHOR
        result = overlay.overlay_launcher(source, laboratory_carplay=True)
        self.assertEqual(result.count(overlay.LAB_CARPLAY_PROPERTY), 1)
        self.assertIn('VMOPTIONS="$VMOPTIONS -Dterminalmode.dio=true"', result)
        self.assertIn("-DIS_PRODUCTION_MODE=true", result)
        for value in ("true", "false"):
            with self.assertRaisesRegex(ValueError, "activation property"):
                overlay.overlay_launcher(
                    f'VMOPTIONS="$VMOPTIONS -Dterminalmode.dio={value}"\n' + source,
                    laboratory_carplay=True)

    def test_unknown_duplicate_and_existing_overlays_rejected(self):
        for source in ("", overlay.CLASSPATH_ANCHOR * 2,
                       overlay.GUEST_OVERLAY + "\n" + overlay.CLASSPATH_ANCHOR):
            with self.subTest(source=source), self.assertRaises(ValueError):
                overlay.overlay_launcher(source)

    def test_composes_with_diagnostic_release_overlay(self):
        source = ('BOOTCLASSPATH="/lib/mh2p-diagnostic-release.jar:'
                  '$BOOTCLASSPATH:$LSD_JXE"')
        result = overlay.overlay_launcher(source)
        self.assertIn(f'BOOTCLASSPATH="{overlay.GUEST_OVERLAY}:'
                      '/lib/mh2p-diagnostic-release.jar:$BOOTCLASSPATH:$LSD_JXE"', result)

    def test_existing_output_rejected_before_sources(self):
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(FileExistsError):
                overlay.prepare(Path("missing"), Path("missing"), Path("missing"), Path(name))

    def test_overlay_contains_only_guarded_classes_and_preserves_sources(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = root / "original.jar"
            prepared = root / "prepared.jar"
            launcher = root / "lsd.sh"
            launcher.write_text(overlay.CLASSPATH_ANCHOR)
            for path in (source, prepared):
                with ZipFile(path, "w") as archive:
                    for entry in overlay.OVERLAY_ENTRIES:
                        archive.writestr(entry, b"private test fixture:" + entry.encode())
                    archive.writestr("unrelated/resource", b"do not copy")
            before = {path: path.read_bytes() for path in (source, prepared, launcher)}
            with patch.object(overlay, "validate_archives", return_value=["original", "prepared"]):
                report = overlay.prepare(source, prepared, launcher, root / "output")
                overlay.prepare(source, prepared, launcher, root / "repeat")
                lab_report = overlay.prepare(
                    source, prepared, launcher, root / "laboratory", laboratory_carplay=True)
            self.assertFalse(report["installed"])
            self.assertFalse(report["laboratory_java_carplay_activation"])
            self.assertFalse(report["native_coding_or_fec_supplied"])
            self.assertTrue(lab_report["laboratory_java_carplay_activation"])
            self.assertFalse(lab_report["native_coding_or_fec_supplied"])
            self.assertIn(overlay.LAB_CARPLAY_PROPERTY,
                          (root / "laboratory/lsd-carplay-slots.sh").read_text())
            self.assertTrue(report["source_archives_and_launcher_preserved"])
            with ZipFile(root / "output/mh2p-carplay-contacts.jar") as archive:
                self.assertEqual(archive.namelist(), list(overlay.OVERLAY_ENTRIES))
                for entry in overlay.OVERLAY_ENTRIES:
                    self.assertEqual(archive.read(entry), b"private test fixture:" + entry.encode())
            self.assertEqual(
                (root / "output/mh2p-carplay-contacts.jar").read_bytes(),
                (root / "repeat/mh2p-carplay-contacts.jar").read_bytes())
            for path, data in before.items():
                self.assertEqual(path.read_bytes(), data)

    def test_bad_launcher_rejected_without_output(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            launcher = root / "lsd.sh"
            launcher.write_text("unknown launcher")
            with patch.object(overlay, "validate_archives", return_value=["a", "b"]):
                with self.assertRaisesRegex(ValueError, "boot classpath"):
                    overlay.prepare(Path("source"), Path("target"), launcher, root / "output")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
