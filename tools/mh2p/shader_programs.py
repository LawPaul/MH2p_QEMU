#!/usr/bin/env python3
"""Recover original GLSL and explicitly verified NV program transcriptions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from compositor_programs import (
    ANIMATION_SHA256, DISPLAYMANAGER_SHA256, animation_programs, compositor_programs,
)
from eal_programs import PROGRAMS as EAL_PROGRAMS, eal_program

sys.path.append(str(Path(__file__).resolve().parents[1] / "mhi2"))
from extract_kanzi_shaders import blend_mode, shader_hash

MAGIC = b"MH2PGLSL"
MAX_BINARY = 4 * 1024 * 1024
MAX_SOURCE = 1024 * 1024
EAL_SHA256 = "b6e8cb109e58fcd09a4b09eff95ab852cc06cfe15f78f0a31e72946950a16ef2"


def elf_sources(data):
    """Read exported source-pointer objects through ELF32 load/section tables."""
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Expected little-endian ELF32")
    if struct.unpack_from("<HH", data, 16) != (3, 40):
        raise ValueError("Expected an ARM shared library")
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phsize, phcount, shsize, shcount = struct.unpack_from("<4H", data, 42)
    if (phsize != 32 or shsize != 40 or not phcount or not shcount or
            phoff + phsize * phcount > len(data) or
            shoff + shsize * shcount > len(data)):
        raise ValueError("Invalid ELF tables")
    segments = [struct.unpack_from("<8I", data, phoff + i * phsize)
                for i in range(phcount)]
    sections = [struct.unpack_from("<10I", data, shoff + i * shsize)
                for i in range(shcount)]

    def address(virtual, size):
        for kind, offset, start, _, length, _, _, _ in segments:
            if kind == 1 and start <= virtual and virtual + size <= start + length:
                begin = offset + virtual - start
                if offset + length > len(data):
                    break
                return begin, offset + length
        raise ValueError("Source pointer is outside file-backed ELF segments")

    result = {}
    for section in sections:
        _, kind, _, _, offset, size, link, _, _, stride = section
        if kind != 11:
            continue
        if stride != 16 or size % stride or link >= len(sections):
            raise ValueError("Invalid ELF symbol table")
        strings = sections[link]
        start, length = strings[4:6]
        if (strings[1] != 3 or offset + size > len(data) or
                start + length > len(data)):
            raise ValueError("ELF symbols exceed library size")
        for at in range(offset, offset + size, stride):
            name, value, extent, info, _, index = struct.unpack_from("<IIIBBH", data, at)
            if name >= length:
                raise ValueError("Invalid ELF symbol name")
            end = data.find(b"\0", start + name, start + length)
            if end < 0:
                raise ValueError("Unterminated ELF symbol name")
            symbol = data[start + name:end]
            if not symbol.startswith(b"KZU_") or not symbol.endswith(b"_SHADER_SOURCE"):
                continue
            if not index or extent != 4 or info & 15 != 1:
                raise ValueError("Invalid exported shader source object")
            pointer, _ = address(value, 4)
            source, limit = address(struct.unpack_from("<I", data, pointer)[0], 1)
            end = data.find(b"\0", source, min(limit, source + MAX_SOURCE + 1))
            if end < 0 or not re.search(rb"\bvoid\s+main\s*\(", data[source:end]):
                raise ValueError("Invalid exported GLSL source")
            result[symbol.decode("ascii")] = data[source:end]
    if not result:
        raise ValueError("No exported shader sources")
    return result


def builtin_programs(data):
    # These associations are specific to the verified P2865 Kanzi library.
    if hashlib.sha256(data).hexdigest() != EAL_SHA256:
        raise ValueError("Unverified EAL library version")
    sources = elf_sources(data)
    associations = {
        "Default": ("DEFAULT", "DEFAULT"),
        "Error": ("ERROR", "ERROR"),
        "SolidColor": ("SOLID_COLOR", "SOLID_COLOR"),
        "OpaqueSolidColor": ("SOLID_COLOR", "SOLID_COLOR"),
        "Textured": ("TEXTURED", "TEXTURED"),
        "DebugLight": ("DEBUG_LIGHT", "DEBUG_LIGHT"),
        "LayerAlpha": ("LAYER", "LAYER"),
        "LayerOpaque": ("LAYER", "LAYER_OPAQUE"),
        "LayerCompositionAlpha": ("LAYER", "LAYER_COMPOSITION"),
        "LayerCompositionOpaque": ("LAYER", "LAYER_COMPOSITION_OPAQUE"),
        "LayerTransition": ("TRANSITION_LAYER", "TRANSITION_LAYER"),
    }
    containers = []
    for match in re.finditer(rb"\.KZB", data):
        at = match.start()
        if at + 60 <= len(data):
            size = struct.unpack_from(">I", data, at + 4)[0]
            if (60 < size <= len(data) - at and
                    data[at + 8:at + 16] == struct.pack(">II", 44, 81)):
                containers.append(data[at:at + size])
    verified = [list(programs(container)) for container in containers]
    if len(verified) != 1:
        raise ValueError("Expected one embedded built-in shader resource")
    for name, binary, pair in verified[0]:
        if name in associations:
            vertex, fragment = associations[name]
            pair = [sources[f"KZU_{vertex}_VERTEX_SHADER_SOURCE"],
                    sources[f"KZU_{fragment}_FRAGMENT_SHADER_SOURCE"]]
            if b"gl_Position" not in pair[0] or b"gl_Position" in pair[1]:
                raise ValueError("Exported shader stage mismatch")
        elif name in EAL_PROGRAMS:
            pair = eal_program(name, binary)
        yield name, binary, pair


def resources(data):
    if len(data) < 60 or data[:4] != b".KZB":
        raise ValueError("Not a KZB resource")
    if struct.unpack_from(">I", data, 4)[0] != len(data):
        raise ValueError("KZB file size mismatch")
    end = 0x34 + struct.unpack_from(">I", data, 0x34)[0]
    if not 60 < end <= len(data):
        raise ValueError("Invalid KZB directory size")
    result = [None]

    def unpack(fmt, at, limit):
        size = struct.calcsize(fmt)
        if at + size > limit:
            raise ValueError("Truncated directory")
        return struct.unpack_from(fmt, data, at), at + size

    def name_end(at, size, limit):
        if not size or at + size > limit:
            raise ValueError("Invalid directory name")
        aligned = (at + size + 3) & ~3
        if aligned > limit or any(data[at + size:aligned]):
            raise ValueError("Invalid directory alignment")
        return aligned

    def directory(at, limit, depth):
        if depth > 32:
            raise ValueError("Directory nesting limit exceeded")
        (count,), at = unpack(">I", at, limit)
        for _ in range(count):
            start = at
            (size, length), at = unpack(">IH", at, limit)
            if not at < start + size <= limit:
                raise ValueError("Invalid child directory size")
            at = name_end(at, length, start + size)
            at = directory(at, start + size, depth + 1)
        (count,), at = unpack(">I", at, limit)
        for _ in range(count):
            (offset, size, kind, flags, length), at = unpack(">IIIIH", at, limit)
            next_at = name_end(at, length, limit)
            if not end <= offset or offset + 4 + size > len(data):
                raise ValueError("Resource extent exceeds file")
            result.append((offset, size, kind, flags, data[at:at + length]))
            at = next_at
        if at != limit:
            raise ValueError("Directory extent mismatch")
        return at

    directory(60, end, 0)
    return result


def programs(data):
    entries = resources(data)
    for offset, size, kind, flags, name in entries[1:]:
        if kind != 1120:
            continue
        if flags:
            raise ValueError("Unsupported shader resource flags")
        payload = data[offset + 4:offset + 4 + size]
        matches = list(re.finditer(b"NV_K1\x00\x01\x00\x00\x00", payload))
        if len(matches) != 1:
            raise ValueError("Expected one combined NV_K1 program")
        start, end = matches[0].span()
        if start < 14 or end + 4 > len(payload):
            raise ValueError("Truncated program descriptor")
        vertex, fragment, count, length = struct.unpack_from("<IIIH",
                                                            payload, start - 14)
        if count != 1 or length != 5:
            raise ValueError("Unsupported program format descriptor")
        binary_size = struct.unpack_from("<I", payload, end)[0]
        binary = payload[end + 4:end + 4 + binary_size]
        if (not 4 <= binary_size <= MAX_BINARY or len(binary) != binary_size
                or binary[:4] != b"\x0a\0\0\0"):
            raise ValueError("Invalid NVIDIA program extent/version")
        sources = []
        for stage, ref in enumerate((vertex, fragment)):
            if not 0 < ref < len(entries):
                raise ValueError("Invalid shader source reference")
            source_offset, source_size, source_kind, source_flags, _ = entries[ref]
            if source_kind != 2010 or source_flags or source_size > MAX_SOURCE:
                raise ValueError("Invalid shader source resource")
            source = data[source_offset + 4:source_offset + 4 + source_size]
            if source and (b"\0" in source or not re.search(rb"\bvoid\s+main\s*\(", source)
                           or (b"gl_Position" in source) != (stage == 0)):
                raise ValueError("Shader source stage mismatch")
            sources.append(source)
        yield name.decode("utf-8"), binary, sources


def cache_record(binary, sources):
    if not all(sources):
        raise ValueError("Original shader source was stripped")
    mode = blend_mode(sources[1])
    sources = [re.sub(rb"^#pragma profilepragma blendoperation[^\n]*\n", b"", source,
                      flags=re.M) for source in sources]
    return (MAGIC + struct.pack("<4I", len(binary), len(sources[0]), len(sources[1]), mode)
            + binary + sources[0] + sources[1])


def extract(root, output, eal=None, displaymanager=None, animation=None):
    output.mkdir(parents=True, exist_ok=False)
    report = {"programs": {}, "stripped": [], "resource_files": 0}
    def add(origin, binary, sources):
        if not all(sources):
            report["stripped"].append(origin)
            return
        record = cache_record(binary, sources)
        key = f"{shader_hash(binary):016x}-{len(binary)}"
        target = output / (key + ".program")
        if target.exists():
            previous = target.read_bytes()
            if (previous[24:24 + len(binary)] != binary or
                    previous[20:24] != record[20:24]):
                raise ValueError(f"Conflicting program identity/blending: {key}")
        else:
            target.write_bytes(record)
            report["programs"][key] = {
                "binary_sha256": hashlib.sha256(binary).hexdigest(),
                "cache_sha256": hashlib.sha256(record).hexdigest(),
                "origins": [],
            }
        report["programs"][key]["origins"].append(origin)

    for path in sorted(root.rglob("*.kzb")):
        report["resource_files"] += 1
        for name, binary, sources in programs(path.read_bytes()):
            add({"resource": str(path.relative_to(root)), "material": name},
                binary, sources)
    if eal:
        for name, binary, sources in builtin_programs(eal.read_bytes()):
            origin = {"library": eal.name, "material": name}
            if name in EAL_PROGRAMS:
                origin["source_kind"] = "NVvp5/NVfp5 instruction transcription"
            add(origin, binary, sources)
        report["builtin_library_sha256"] = EAL_SHA256
    if displaymanager:
        for name, binary, sources in compositor_programs(displaymanager.read_bytes()):
            add({"library": displaymanager.name, "material": name,
                 "source_kind": "NVvp5/NVfp5 instruction transcription"},
                binary, sources)
        report["compositor_sha256"] = DISPLAYMANAGER_SHA256
        report["unsupported_compositor_programs"] = []
    if animation:
        for name, binary, sources in animation_programs(animation.read_bytes()):
            add({"library": animation.name, "material": name,
                 "source_kind": "NVvp5/NVfp5 instruction transcription"},
                binary, sources)
        report["animation_sha256"] = ANIMATION_SHA256
    if not report["resource_files"] or not report["programs"]:
        raise ValueError("No original shader programs recovered")
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--eal", type=Path,
                        help="Original P2865 libeal.so for built-in sources and verified NV transcriptions")
    parser.add_argument("--displaymanager", type=Path,
                        help="Optional original P2865 compositor for verified NV transcriptions")
    parser.add_argument("--animation", type=Path,
                        help="Optional original P2865 libdm_modAnim.so for verified NV transcriptions")
    args = parser.parse_args()
    report = extract(args.resources, args.output, args.eal, args.displaymanager,
                     args.animation)
    print(f"{len(report['programs'])} exact program identities from "
          f"{report['resource_files']} resources; "
          f"{len(report['stripped'])} stripped source pairs")


if __name__ == "__main__":
    main()
