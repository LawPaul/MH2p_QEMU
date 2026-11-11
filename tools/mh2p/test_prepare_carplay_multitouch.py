# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from prepare_carplay_multitouch import (
    GUEST_CONFIG_DIRECTORY, carplay_launcher, multitouch_config, prepare,
)


class CarPlayMultitouchTests(unittest.TestCase):
    def setUp(self):
        self.config = {
            "screen": {"fingers": 1, "maxFPS": 60},
            "test": {"hmi": {"enable": False, "relativeCoordinates": False}},
            "coding": {"carplay": False},
            "unrelated": ["keep", 4],
        }
        self.source = "## Original production comment\n" + json.dumps(self.config)

    def test_only_contact_limit_changes(self):
        for fingers in (2, 3, 4):
            with self.subTest(fingers=fingers):
                result, old = multitouch_config(self.source, fingers)
                expected = copy.deepcopy(self.config)
                expected["screen"]["fingers"] = fingers
                self.assertEqual(result, expected)
                self.assertEqual(old, 1)

    def test_rejects_invalid_requested_limits(self):
        for fingers in (True, False, 1, 0, -1, 5, 2.0, "2", None):
            with self.subTest(fingers=fingers):
                with self.assertRaisesRegex(ValueError, "contact limit"):
                    multitouch_config(self.source, fingers)

    def test_rejects_invalid_original_limits_and_shapes(self):
        for screen in (None, [], {}, {"fingers": True}, {"fingers": 0},
                       {"fingers": 5}, {"fingers": "1"}, {"fingers": 1.0}):
            with self.subTest(screen=screen):
                with self.assertRaises(ValueError):
                    multitouch_config(json.dumps({"screen": screen}))
        with self.assertRaises(ValueError):
            multitouch_config("[]")
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            multitouch_config('{"screen":{"fingers":1,"fingers":2}}')

    def test_isolated_output_identity_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "original.json", root / "mod"
            source.write_text(self.source)
            metadata = prepare(source, output)
            modified = (output / "dio_manager.json").read_bytes()
            self.assertEqual(source.read_text(), self.source)
            self.assertEqual(metadata["source_sha256"],
                             hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(metadata["output_sha256"],
                             hashlib.sha256(modified).hexdigest())
            self.assertEqual(json.loads(modified)["screen"]["fingers"], 2)
            self.assertFalse(metadata["installed"])
            self.assertFalse(metadata["iphone_session_verified"])
            self.assertEqual(json.loads((output / "manifest.json").read_text()),
                             metadata)
            with self.assertRaises(FileExistsError):
                prepare(source, output, 3)
            self.assertEqual((output / "dio_manager.json").read_bytes(), modified)

    def test_invalid_input_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "original.json", root / "mod"
            source.write_text("{}")
            with self.assertRaises(ValueError):
                prepare(source, output)
            self.assertFalse(output.exists())

    def test_cli_reports_errors_and_returns_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "original.json", root / "mod"
            source.write_text("{}")
            reply = subprocess.run(
                [sys.executable, str(Path(__file__).with_name(
                    "prepare_carplay_multitouch.py")), "--config", str(source),
                 "--output", str(output)], capture_output=True, text=True)
            self.assertNotEqual(reply.returncode, 0)
            self.assertIn("CarPlay multi-touch preparation failed", reply.stderr)
            self.assertFalse(output.exists())

    def test_launcher_changes_only_original_backend_config_directory(self):
        original = {
            "children": {
                "carplay": {
                    "exec": "dio_manager",
                    "envs": ["LD_LIBRARY_PATH=/eso/lib",
                             "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production"],
                    "wireless": {"enable5GHzCountryAdaption": True},
                },
                "gal": {"envs": ["UNCHANGED=yes"]},
            },
            "settings": {"forceConnection": -1},
        }
        expected = copy.deepcopy(original)
        expected["children"]["carplay"]["envs"][1] = (
            f"IPL_CONFIG_DIR_DIO_MANAGER={GUEST_CONFIG_DIRECTORY}")
        self.assertEqual(carplay_launcher(json.dumps(original)), expected)

    def test_rejects_missing_or_ambiguous_launcher(self):
        for config in ({}, {"children": []}, {"children": {"carplay": {}}},
                       {"children": {"carplay": {"exec": "dio_manager", "envs": []}}},
                       {"children": {"carplay": {"exec": "dio_manager", "envs": [
                           "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production",
                           "IPL_CONFIG_DIR_DIO_MANAGER=/tmp"]}}}):
            with self.subTest(config=config):
                with self.assertRaises(ValueError):
                    carplay_launcher(json.dumps(config))

    def test_prepares_and_hashes_optional_launcher(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, smartphone, output = root / "dio", root / "si", root / "mod"
            source.write_text(self.source)
            smartphone.write_text(json.dumps({"children": {"carplay": {
                "exec": "dio_manager",
                "envs": ["IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production"]}}}))
            original = smartphone.read_bytes()
            metadata = prepare(source, output, smartphone_source=smartphone)
            self.assertEqual(smartphone.read_bytes(), original)
            self.assertEqual(metadata["guest_config_directory"], GUEST_CONFIG_DIRECTORY)
            self.assertEqual(metadata["smartphone_launcher"]["output_sha256"],
                             hashlib.sha256(
                                 (output / "smartphone_integrator.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
