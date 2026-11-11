# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-free checks that evidence probes cannot overwrite their inputs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class CarPlayProbeInputTests(unittest.TestCase):
    probes = (
        ("probe_carplay_hid.py", ("--library", "--config", "--backend", "--events")),
        ("probe_carplay_coding.py", ("--binary",)),
        ("probe_carplay_country_policy.py", ("--original", "--modified")),
        ("probe_carplay_wireless_policy.py", ("--original", "--modified")),
    )

    def check_aliases(self, alias):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for script, options in self.probes:
                paths = []
                arguments = []
                for index, option in enumerate(options):
                    path = root / f"{script}-{index}"
                    path.write_bytes(b"untouched private input")
                    paths.append(path)
                    arguments += [option, str(path)]
                for index, source in enumerate(paths):
                    with self.subTest(script=script, option=options[index], alias=alias):
                        output = root / "evidence"
                        if alias == "symlink":
                            output.symlink_to(source)
                        elif alias == "hardlink":
                            os.link(source, output)
                        else:
                            output = source
                        reply = subprocess.run(
                            [sys.executable, str(Path(__file__).with_name(script)),
                             *arguments, "--output", str(output)],
                            capture_output=True, text=True)
                        self.assertEqual(reply.returncode, 2, reply.stderr)
                        self.assertIn("must not overwrite", reply.stderr)
                        for path in paths:
                            self.assertEqual(path.read_bytes(), b"untouched private input")
                        if output != source:
                            output.unlink()

    def test_exact_input_paths_rejected(self):
        self.check_aliases("exact")

    def test_symlink_input_aliases_rejected(self):
        self.check_aliases("symlink")

    def test_hardlink_input_aliases_rejected(self):
        self.check_aliases("hardlink")

    def test_indexed_experiment_rejects_output_alias_before_compilation(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "original"
            path.write_bytes(b"untouched private input")
            reply = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("probe_carplay_hid.py")),
                 "--library", str(path), "--config", str(path), "--output", str(path),
                 "--indexed-experiment", "--compiler", "/compiler-must-not-run"],
                capture_output=True, text=True)
            self.assertEqual(reply.returncode, 2, reply.stderr)
            self.assertIn("must not overwrite", reply.stderr)
            self.assertEqual(path.read_bytes(), b"untouched private input")


if __name__ == "__main__":
    unittest.main()
