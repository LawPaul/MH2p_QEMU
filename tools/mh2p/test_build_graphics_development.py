# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import build_graphics_development as graphics
from prepare_graphics_smoke import add_resources, file_entries
from test_prepare import fixture


def library(payload=b""):
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", data, 16, 3, 40)
    struct.pack_into("<I", data, 36, 0x05000000)
    struct.pack_into("<H", data, 42, 32)
    return bytes(data) + payload


def startup():
    image = bytearray(fixture())
    total = sum(word[0] for word in struct.iter_unpack("<I", image[8:260]))
    struct.pack_into("<I", image, 260, -total & 0xffffffff)
    return add_resources(bytes(image), [
        (name, library(name.encode())) for name in graphics.MODULES
    ])[0]


class GraphicsDevelopmentTests(unittest.TestCase):
    def test_replaces_only_adapters_and_retains_source_and_checksums(self):
        source = startup()
        before = hashlib.sha256(source).digest()
        replacements = {name: library(b"new " + name.encode())
                        for name in graphics.MODULES}
        result = graphics.replace_resources(source, replacements)
        _, old_offset = add_resources(source, [])
        _, offset = add_resources(result, [])
        original, updated = source[old_offset:], result[offset:]
        old_entries, entries = file_entries(original), file_entries(updated)
        self.assertEqual(entries.keys(), old_entries.keys())
        for name, (at, size) in entries.items():
            old_at, old_size = old_entries[name]
            self.assertEqual(updated[at:at + size], replacements.get(
                name, original[old_at:old_at + old_size]))
        self.assertEqual(hashlib.sha256(source).digest(), before)

    def test_rejects_missing_resources_wrong_abi_and_native_replacements(self):
        source = startup()
        for replacements in (
                {}, {"usr/sbin/main_stage1.sh": library()},
                {"lib/libmh2p-screen.so": b"not an ELF"},
                {"lib/libmh2p-screen.so": library()[:36] +
                 struct.pack("<I", 0x05000400) + library()[40:]}):
            with self.subTest(resources=list(replacements)), self.assertRaises(ValueError):
                graphics.replace_resources(source, replacements)

    def fake_compile(self, command, **kwargs):
        target = Path(command[command.index("-o") + 1])
        depfile = Path(command[command.index("-MF") + 1])
        if "-MM" not in command:
            target.write_bytes(library(target.name.encode()))
        source = command[-2] if command[-1] == "-MM" else command[-1]
        depfile.write_text(
            f"{target}: {source} \\\n {graphics.ROOT / 'common.h'}\n")
        return subprocess.CompletedProcess(command, 0)

    def make_sources(self, root):
        for name in graphics.MODULES.values():
            source = root / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("test-only source\n")
        (root / "common.h").write_text("test-only shared header\n")

    def test_build_verifies_dependencies_and_rejects_stale_or_altered_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_sources(root)
            source = root / "source.bin"
            original = startup()
            source.write_bytes(original)
            output = root / "build"
            with patch.object(graphics, "ROOT", root), \
                    patch.object(graphics.subprocess, "run",
                                 side_effect=self.fake_compile) as compiler:
                manifest = graphics.build(source, output, "/compiler")
                self.assertEqual(compiler.call_count, 4)
                self.assertEqual(graphics.verify(output), output / "startup.bin")
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual(len(manifest["source_sha256"]), 3)
                self.assertTrue(manifest["all_other_resources_exact"])
                with self.assertRaises(FileExistsError):
                    graphics.build(source, output, "/compiler")
                header = root / "common.h"
                old = header.read_bytes()
                header.write_text("changed source\n")
                with self.assertRaisesRegex(ValueError, "sources changed"):
                    graphics.verify(output)
                header.write_bytes(old)
                startup_file = output / "startup.bin"
                old_startup = startup_file.read_bytes()
                startup_file.write_bytes(old_startup + b"changed")
                with self.assertRaisesRegex(ValueError, "startup identity"):
                    graphics.verify(output)
                startup_file.write_bytes(old_startup)
                module = output / "libmh2p-screen.so"
                module.write_bytes(module.read_bytes() + b"changed")
                with self.assertRaisesRegex(ValueError, "adapter identity"):
                    graphics.verify(output)

    def test_failed_compiler_does_not_publish_a_success_manifest_or_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            original = startup()
            source.write_bytes(original)
            output = root / "build"
            with patch.object(graphics, "ROOT", root), \
                    patch.object(graphics.subprocess, "run",
                                 side_effect=subprocess.CalledProcessError(1, ["compiler"])):
                with self.assertRaises(subprocess.CalledProcessError):
                    graphics.build(source, output, "/compiler")
            self.assertFalse((output / "manifest.json").exists())
            self.assertFalse((output / "startup.bin").exists())
            self.assertEqual(source.read_bytes(), original)

    def test_editing_a_dependency_during_compilation_rejects_the_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_sources(root)
            source = root / "source.bin"
            source.write_bytes(startup())
            output = root / "build"

            def compiling(command, **kwargs):
                result = self.fake_compile(command, **kwargs)
                if "-MM" not in command:
                    (root / "common.h").write_text("edited during compilation\n")
                return result

            with patch.object(graphics, "ROOT", root), \
                    patch.object(graphics.subprocess, "run", side_effect=compiling):
                with self.assertRaisesRegex(ValueError, "changed during build"):
                    graphics.build(source, output, "/compiler")
            self.assertFalse((output / "manifest.json").exists())
            self.assertFalse((output / "startup.bin").exists())

    def test_rejects_path_escape_in_recorded_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "manifest.json").write_text(json.dumps({
                "format": 1, "local_development_only": True,
                "source_sha256": {"../outside": "untrusted"},
                "resource_sha256": dict.fromkeys(graphics.MODULES, "untrusted"),
            }))
            with patch.object(graphics, "ROOT", root), \
                    self.assertRaisesRegex(ValueError, "sources changed"):
                graphics.verify(root)


if __name__ == "__main__":
    unittest.main()
