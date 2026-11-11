import struct
import tempfile
from hashlib import sha256
from pathlib import Path
import unittest
from unittest.mock import patch

from shader_programs import (
    builtin_programs, cache_record, elf_sources, extract, programs, resources,
)
from compositor_programs import (
    ANIMATION_PROGRAMS, ANIMATION_SHA256, PROGRAMS as COMPOSITOR_PROGRAMS,
    animation_programs, compositor_programs,
)
from eal_programs import PROGRAMS as EAL_PROGRAMS, eal_program


VERTEX = b"void main(){gl_Position=vec4(0.0);}"
FRAGMENT = b"void main(){gl_FragColor=vec4(1.0);}"
BINARY = b"\x0a\0\0\0test-program"


def fixture():
    data = bytearray(8192)
    data[:4] = b".KZB"
    struct.pack_into(">I", data, 4, len(data))
    node = bytearray(struct.pack(">IH", 0, 8) + b"Material" + bytes(2))
    node += struct.pack(">II", 0, 4)
    descriptor = (struct.pack("<IIIH", 2, 3, 1, 5) +
                  b"NV_K1\x00\x01\0\0\0" + struct.pack("<I", len(BINARY)) + BINARY)
    for offset, kind, flags, name, payload in (
        (0x400, 10, 16, b"Other", b"data"),
        (0x900, 2010, 0, b"Vertex", VERTEX),
        (0x500, 2010, 0, b"Fragment", FRAGMENT),
        (0xc00, 1120, 0, b"Program", descriptor),
    ):
        node += struct.pack(">IIIIH", offset, len(payload), kind, flags, len(name))
        node += name
        node += bytes(-len(node) % 4)
        data[offset + 4:offset + 4 + len(payload)] = payload
    struct.pack_into(">I", node, 0, len(node))
    directory = struct.pack(">I", 1) + node + struct.pack(">I", 0)
    data[60:60 + len(directory)] = directory
    struct.pack_into(">I", data, 0x34, len(directory) + 8)
    return data


def elf_fixture():
    data = bytearray(2048)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", data, 16, 3, 40)
    struct.pack_into("<II", data, 28, 52, 128)
    struct.pack_into("<4H", data, 42, 32, 1, 40, 3)
    struct.pack_into("<8I", data, 52, 1, 0, 0x10000, 0, len(data), len(data), 4, 4)
    name = b"\0KZU_DEFAULT_VERTEX_SHADER_SOURCE\0"
    struct.pack_into("<10I", data, 168, 0, 3, 0, 0, 400, len(name), 0, 0, 1, 0)
    struct.pack_into("<10I", data, 208, 0, 11, 0, 0, 500, 32, 1, 0, 4, 16)
    data[400:400 + len(name)] = name
    struct.pack_into("<IIIBBH", data, 516, 1, 0x10000 + 600, 4, 0x11, 0, 1)
    struct.pack_into("<I", data, 600, 0x10000 + 700)
    data[700:700 + len(VERTEX)] = VERTEX
    return data


class ProgramTests(unittest.TestCase):
    def test_unverified_eal_transcription_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unverified EAL material"):
            eal_program("unknown", BINARY)
        for name in EAL_PROGRAMS:
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "binary identity mismatch"):
                    eal_program(name, BINARY)

    def test_transcriptions_preserve_application_blending(self):
        for name, (_, vertex, fragment) in EAL_PROGRAMS.items():
            with self.subTest(name=name):
                record = cache_record(BINARY, [vertex, fragment])
                self.assertEqual(struct.unpack_from("<I", record, 20)[0], 0)

    def test_eal_exact_identity_returns_sources(self):
        for name, (_, vertex, fragment) in EAL_PROGRAMS.items():
            with self.subTest(name=name), patch.dict(
                    EAL_PROGRAMS, {name: (sha256(BINARY).hexdigest(), vertex, fragment)}):
                self.assertEqual(eal_program(name, BINARY), [vertex, fragment])
                with self.assertRaisesRegex(ValueError, "binary identity mismatch"):
                    eal_program(name, BINARY + b"\0")

    def test_builtin_transcription_provenance(self):
        builtins = [
            ("eal_font", BINARY + b"font", list(EAL_PROGRAMS["eal_font"][1:])),
            ("Default", BINARY + b"default", [VERTEX, FRAGMENT]),
            ("FontSystemBitmap", BINARY + b"stripped", [b"", b""]),
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "resources"
            root.mkdir()
            (root / "one.kzb").write_bytes(fixture())
            eal = Path(directory) / "libeal.so"
            eal.write_bytes(b"fixture")
            with patch("shader_programs.builtin_programs", return_value=builtins):
                report = extract(root, Path(directory) / "cache", eal=eal)
            self.assertEqual(len(report["programs"]), 3)
            origins = {origin["material"]: origin
                       for item in report["programs"].values()
                       for origin in item["origins"]}
            self.assertEqual(origins["eal_font"]["source_kind"],
                             "NVvp5/NVfp5 instruction transcription")
            self.assertNotIn("source_kind", origins["Default"])
            self.assertEqual(report["stripped"],
                             [{"library": "libeal.so", "material": "FontSystemBitmap"}])

    def test_unverified_compositor_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unverified"):
            list(compositor_programs(b"unknown executable"))

    def test_animation_binary_identity_and_blending(self):
        with self.assertRaisesRegex(ValueError, "Unverified"):
            list(animation_programs(BINARY))
        name, _, _, _, vertex, fragment = ANIMATION_PROGRAMS[0]
        entry = (name, 0, len(BINARY), sha256(BINARY).hexdigest(), vertex, fragment)
        with patch("compositor_programs.ANIMATION_PROGRAMS", (entry,)), patch(
                "compositor_programs.ANIMATION_SHA256", sha256(BINARY).hexdigest()):
            self.assertEqual(list(animation_programs(BINARY)),
                             [(name, BINARY, [vertex, fragment])])
            self.assertEqual(struct.unpack_from(
                "<I", cache_record(BINARY, [vertex, fragment]), 20)[0], 0)
        damaged = BINARY[:-1] + b"!"
        with patch("compositor_programs.ANIMATION_PROGRAMS", (entry,)), patch(
                "compositor_programs.ANIMATION_SHA256", sha256(damaged).hexdigest()):
            with self.assertRaisesRegex(ValueError, "binary identity mismatch"):
                list(animation_programs(damaged))

    def test_animation_manifest(self):
        builtins = [(name, BINARY + name.encode(), [vertex, fragment])
                    for name, _, _, _, vertex, fragment in ANIMATION_PROGRAMS]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "resources"
            root.mkdir()
            (root / "one.kzb").write_bytes(fixture())
            library = Path(directory) / "libdm_modAnim.so"
            library.write_bytes(b"fixture")
            with patch("shader_programs.animation_programs", return_value=builtins):
                report = extract(root, Path(directory) / "cache", animation=library)
            self.assertEqual(len(report["programs"]), 2)
            self.assertEqual(report["animation_sha256"], ANIMATION_SHA256)
            origins = [origin for item in report["programs"].values()
                       for origin in item["origins"] if "library" in origin]
            self.assertEqual(origins, [{
                "library": library.name, "material": builtins[0][0],
                "source_kind": "NVvp5/NVfp5 instruction transcription",
            }])

    def test_compositor_binary_identities_and_blending(self):
        for name, _, _, _, vertex, fragment in COMPOSITOR_PROGRAMS:
            with self.subTest(name=name):
                entry = (name, 0, len(BINARY), sha256(BINARY).hexdigest(),
                         vertex, fragment)
                with patch("compositor_programs.PROGRAMS", (entry,)), patch(
                        "compositor_programs.DISPLAYMANAGER_SHA256",
                        sha256(BINARY).hexdigest()):
                    self.assertEqual(list(compositor_programs(BINARY)),
                                     [(name, BINARY, [vertex, fragment])])
                    record = cache_record(BINARY, [vertex, fragment])
                    self.assertEqual(struct.unpack_from("<I", record, 20)[0], 0)
                damaged = BINARY[:-1] + b"!"
                with patch("compositor_programs.PROGRAMS", (entry,)), patch(
                        "compositor_programs.DISPLAYMANAGER_SHA256",
                        sha256(damaged).hexdigest()):
                    with self.assertRaisesRegex(ValueError, "binary identity mismatch"):
                        list(compositor_programs(damaged))

    def test_complete_compositor_manifest(self):
        builtins = [(name, BINARY + name.encode(), [vertex, fragment])
                    for name, _, _, _, vertex, fragment in COMPOSITOR_PROGRAMS]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "resources"
            root.mkdir()
            (root / "one.kzb").write_bytes(fixture())
            executable = Path(directory) / "displaymanager"
            executable.write_bytes(b"fixture")
            with patch("shader_programs.compositor_programs", return_value=builtins):
                report = extract(root, Path(directory) / "cache",
                                 displaymanager=executable)
            self.assertEqual(len(report["programs"]), 5)
            self.assertEqual(report["unsupported_compositor_programs"], [])
            self.assertEqual(sum(origin.get("source_kind") ==
                                 "NVvp5/NVfp5 instruction transcription"
                                 for item in report["programs"].values()
                                 for origin in item["origins"]), 4)

    def test_exported_source_pointer(self):
        self.assertEqual(elf_sources(elf_fixture()),
                         {"KZU_DEFAULT_VERTEX_SHADER_SOURCE": VERTEX})

    def test_invalid_elf_pointer_and_tables(self):
        for offset, value in ((600, 0xffffffff), (28, 0xfffffffe), (516, 0xffffffff)):
            data = elf_fixture()
            struct.pack_into("<I", data, offset, value)
            with self.assertRaises(ValueError):
                elf_sources(data)
        with self.assertRaises(ValueError):
            elf_sources(b"not an ELF")

    def test_unverified_builtin_library_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unverified"):
            list(builtin_programs(elf_fixture()))

    def test_exact_reference_order(self):
        data = fixture()
        self.assertEqual(len(resources(data)), 5)
        name, binary, sources = list(programs(data))[0]
        self.assertEqual((name, binary, sources), ("Program", BINARY, [VERTEX, FRAGMENT]))
        record = cache_record(binary, sources)
        self.assertEqual(record[:8], b"MH2PGLSL")
        self.assertEqual(record[24:], BINARY + VERTEX + FRAGMENT)

    def test_swapped_and_invalid_references(self):
        for first, second in ((3, 2), (0, 3), (99, 3), (1, 3)):
            data = fixture()
            struct.pack_into("<II", data, 0xc04, first, second)
            with self.assertRaises(ValueError):
                list(programs(data))

    def test_corrupt_extents(self):
        for offset, value in ((4, 8193), (0x34, 0xffffffff), (64, 0xffffffff)):
            data = fixture()
            struct.pack_into(">I", data, offset, value)
            with self.assertRaises(ValueError):
                list(programs(data))
        with self.assertRaises(ValueError):
            list(programs(fixture()[:120]))

    def test_invalid_binary_and_format(self):
        for offset, value in ((0xc04 + 8, 2), (0xc04 + 24, 0xffffffff),
                              (0xc04 + 28, 9)):
            data = fixture()
            struct.pack_into("<I", data, offset, value)
            with self.assertRaises(ValueError):
                list(programs(data))

    def test_stripped_source_is_not_replaced(self):
        with self.assertRaisesRegex(ValueError, "stripped"):
            cache_record(BINARY, [b"", FRAGMENT])

    def test_original_blend_metadata(self):
        fragment = (b"#pragma profilepragma blendoperation(gl_FragColor, "
                    b"GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)\n" + FRAGMENT)
        record = cache_record(BINARY, [VERTEX, fragment])
        self.assertEqual(struct.unpack_from("<I", record, 20)[0], 1)
        self.assertEqual(record[24:], BINARY + VERTEX + FRAGMENT)

    def test_persistent_exact_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "input"
            root.mkdir()
            (root / "one.kzb").write_bytes(fixture())
            (root / "two.kzb").write_bytes(fixture())
            out = Path(directory) / "cache"
            report = extract(root, out)
            self.assertEqual(report["resource_files"], 2)
            self.assertEqual(len(report["programs"]), 1)
            entry = next(iter(report["programs"].values()))
            self.assertEqual(len(entry["origins"]), 2)
            self.assertTrue((out / "manifest.json").exists())
            self.assertEqual(next(out.glob("*.program")).read_bytes()[24:],
                             BINARY + VERTEX + FRAGMENT)
            with self.assertRaises(FileExistsError):
                extract(root, out)


if __name__ == "__main__":
    unittest.main()
