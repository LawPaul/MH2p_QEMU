# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import check_publication as publication


class PublicationTests(unittest.TestCase):
    def test_findings_report_only_locations_and_rules(self):
        secret = b"ghp_" + b"A" * 36
        data = b"first line\n" + secret + b"\n"
        self.assertEqual(publication.findings("README.md", data),
                         [(2, "github-token")])

    def test_personal_paths_are_detected_in_scoped_text(self):
        for path in (b"/" + b"home/example/private",
                     b"/" + b"Users/example/private",
                     b"C:" + b"\\Users\\example\\private"):
            with self.subTest(path=path):
                self.assertEqual(publication.findings(
                    "tools/mh2p/example.py", path, check_personal_paths=True),
                    [(1, "workstation-path")])

    def test_generic_paths_and_attribution_are_not_personal_leaks(self):
        self.assertEqual(publication.findings(
            "README.md", b"/path/to/media\nfixture@example.invalid\n"
            b"https://github.com/owner/project\n", check_personal_paths=True), [])

    def test_payloads_and_binary_files_are_rejected_in_tools(self):
        self.assertIn((1, "head-unit-payload-candidate"),
                      publication.findings("tools/mh2p/startup.bin", b"payload"))
        self.assertIn((1, "binary-in-head-unit-tools"),
                      publication.findings("tools/mhi2/innocent.txt", b"ELF\0"))
        self.assertEqual(publication.findings("pc-bios/firmware.bin", b"\0"), [])

    def test_public_qemu_test_keys_are_not_global_exemptions(self):
        key = b"-----BEGIN " + b"PRIVATE KEY-----"
        self.assertEqual(publication.findings("tests/keys/id_rsa", key), [])
        self.assertEqual(publication.findings("private.txt", key),
                         [(1, "private-key")])
        token = b"github_pat_" + b"a" * 36
        self.assertEqual(publication.findings("tests/keys/id_rsa", token),
                         [(1, "github-token")])

    def test_private_configuration_names_are_rejected(self):
        self.assertEqual(publication.findings("tools/mhi2/.env", b""),
                         [(1, "private-config-file")])

    def test_real_git_inventory_includes_untracked_but_not_ignored_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("private-output/\n")
            (root / "README.md").write_text("source\n")
            subprocess.run(["git", "-C", directory, "add", "."], check=True)
            subprocess.run([
                "git", "-C", directory, "-c", "user.name=Test",
                "-c", "user.email=fixture@example.invalid", "commit",
                "-qm", "test fixture"], check=True)
            (root / "tools/mh2p").mkdir(parents=True)
            (root / "tools/mh2p/disk.raw").write_bytes(b"private disk")
            (root / "notes.txt").write_bytes(
                b"/" + b"home/example/private\n")
            (root / "private-output").mkdir()
            (root / "private-output/dump.raw").write_bytes(b"private")
            (root / "external").symlink_to(root.parent)
            count, problems = publication.check(root)
            self.assertEqual(count, 4)
            self.assertIn(("notes.txt", 1, "workstation-path"), problems)
            self.assertIn(("tools/mh2p/disk.raw", 1,
                           "head-unit-payload-candidate"), problems)
            self.assertIn(("external", 1, "external-symlink"), problems)
            self.assertFalse(any("private-output" in name
                                 for name, _, _ in problems))

    def test_cli_fails_closed_without_printing_secret_content(self):
        with patch.object(publication, "check", side_effect=OSError(
                "private detail must not be printed")), \
                patch("sys.argv", ["check_publication.py"]), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(publication.main(), 2)
        self.assertNotIn("private detail", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
