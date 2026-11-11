#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run inspected P2865 HID builders; no QNX boot, radio or phone is simulated."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time

from probe_rcc import QEMUMachine, ROOT
from prepare_hmi_development import original_json
from prepare_carplay_country_policy import file_offset
from probe_carplay_country_policy import InlineProgram
from carplay_hid import touch_layout, verify_report


BASE = 0x81000000
STUBS = 0x81200000
DATA = 0x81300000
HEAP = 0x81400000
BACKEND_BASE = 0x83000000
INDEXED_BASE = 0x81500000


def expected_report(contacts, maximum, release, indexed):
    result = bytearray()
    for slot in range(maximum):
        x, y, down = contacts[slot] if slot < len(contacts) else (0, 0, 0)
        if indexed:
            result.append(slot)
        result.extend(struct.pack("<BHH", 0 if release else down, x, y))
    return bytes(result)


def build_indexed_hid(compiler):
    source = Path(__file__).with_name("carplay_indexed_hid.c")
    with tempfile.TemporaryDirectory(prefix="mh2p-indexed-hid-build-") as name:
        output = Path(name) / "indexed.elf"
        command = [
            compiler, "-mcpu=cortex-a9", "-marm", "-mfloat-abi=soft",
            "-mno-unaligned-access", "-ffreestanding", "-fno-builtin",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
            "-fno-stack-protector", "-nostdlib", "-O2",
            "-Wall", "-Wextra", "-Werror",
            f"-Wl,-Ttext={INDEXED_BASE:#x},--entry=indexed_descriptor",
            f"-Wl,--defsym=malloc={STUBS + 0x40:#x}",
            f"-Wl,--defsym=free={STUBS + 0x100:#x}",
            str(source), "-o", str(output),
        ]
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(f"Indexed HID compiler failed ({built.returncode}):\n"
                               f"{built.stdout}{built.stderr}")
        binary = output.read_bytes()
        version = subprocess.run([compiler, "--version"], capture_output=True,
                                 text=True, check=True).stdout.splitlines()[0]
    shoff = struct.unpack_from("<I", binary, 32)[0]
    stride, count = struct.unpack_from("<HH", binary, 46)
    sections = [struct.unpack_from("<10I", binary, shoff + i * stride)
                for i in range(count)]
    image = bytearray(65536)
    end = 0
    for section in sections:
        _, kind, flags, address, offset, length, _, _, _, _ = section
        if not flags & 2 or not length:
            continue
        if (kind != 1 or not INDEXED_BASE <= address < address + length <=
                INDEXED_BASE + len(image)):
            raise ValueError("Unexpected indexed laboratory ELF allocation")
        start = address - INDEXED_BASE
        image[start:start + length] = binary[offset:offset + length]
        end = max(end, start + length)
    table = next(section for section in sections if section[1] == 2)
    strings_section = sections[table[6]]
    strings = binary[strings_section[4]:strings_section[4] + strings_section[5]]
    symbols = {}
    for at in range(table[4], table[4] + table[5], 16):
        name, value, _, _, _, section = struct.unpack_from("<IIIBBH", binary, at)
        text = strings[name:strings.index(0, name)].decode()
        if section == 0 and text:
            raise ValueError(f"Unresolved indexed laboratory import: {text}")
        if text in ("indexed_descriptor", "indexed_report"):
            if not INDEXED_BASE <= value < INDEXED_BASE + end:
                raise ValueError("Indexed laboratory entry outside loaded code")
            symbols[text] = value
    if len(symbols) != 2:
        raise ValueError("Missing indexed laboratory builder entry")
    return bytes(image[:end]), symbols, {
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "elf_sha256": hashlib.sha256(binary).hexdigest(),
        "compiler": version, "compiler_diagnostics": built.stdout + built.stderr,
        "native_arm_wrappers": True,
        "original_builders_called": True, "installed": False,
        "index_is_fixed_slot_not_physical_finger_id": True,
    }


def load_dsi_frames(path):
    frames = json.loads(path.read_text())
    if not isinstance(frames, list) or not 1 <= len(frames) <= 6:
        raise ValueError("Expected one through six captured DSI frames")
    for frame in frames:
        if not isinstance(frame, list) or not 1 <= len(frame) <= 4:
            raise ValueError("Expected one through four contacts per DSI frame")
        for contact in frame:
            if (not isinstance(contact, list) or len(contact) != 3 or
                    any(type(value) is not int for value in contact) or
                    not 0 <= contact[0] <= 65535 or not 0 <= contact[1] <= 65535 or
                    contact[2] not in (1, 2)):
                raise ValueError("Expected DSI contact [uint16 x, uint16 y, eventType 1 or 2]")
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True,
                        help="Private original P2865 libairplay.so (identity checked)")
    parser.add_argument("--config", type=Path, required=True,
                        help="Prepared dio_manager.json whose contact limit to execute")
    parser.add_argument("--backend", type=Path,
                        help="Also execute original dio_manager contact conversion")
    parser.add_argument("--events", type=Path,
                        help="Up to six captured DSI frames of [x,y,eventType] contacts")
    parser.add_argument("--indexed-experiment", action="store_true",
                        help="Execute lab ARM wrappers adding indices; never installs firmware")
    parser.add_argument("--compiler", default="arm-none-eabi-gcc",
                        help="ARM compiler used only for --indexed-experiment")
    parser.add_argument("--output", type=Path, required=True,
                        help="JSON evidence report; parent directory must exist")
    args = parser.parse_args()
    sources = ([args.library, args.config] + ([args.backend] if args.backend else []) +
               ([args.events] if args.events else []))
    if (args.output.resolve() in {path.resolve() for path in sources} or
            args.output.exists() and any(
                path.exists() and args.output.samefile(path) for path in sources)):
        parser.error("Evidence output must not overwrite an input file")
    source = args.library.read_bytes()
    if hashlib.sha256(source).hexdigest() != (
            "e0782b5f060a56a4e66ca88ea2ea24c0af935699534382a78df90b021f55a642"):
        raise ValueError("Original library does not match the inspected P2865 image")
    raw = args.config.read_bytes()
    config = original_json(raw.decode("utf-8"))
    if not isinstance(config, dict) or not isinstance(config.get("screen"), dict):
        raise ValueError("Expected dio_manager screen configuration")
    configured = config["screen"].get("fingers")
    if type(configured) is not int or not 1 <= configured <= 4:
        raise ValueError("Expected integer screen.fingers from 1 through 4")
    config_sha256 = hashlib.sha256(raw).hexdigest()
    captured_frames = load_dsi_frames(args.events) if args.events else []
    events_sha256 = hashlib.sha256(args.events.read_bytes()).hexdigest() if args.events else None
    backend = args.backend.read_bytes() if args.backend else None
    if backend is not None and hashlib.sha256(backend).hexdigest() != (
            "429ef95595bd67d13acf51df63cb6fb3f86e7ed8effa2eeec25e085c6cdadb77"):
        raise ValueError("Unrecognized original dio_manager")
    candidate, entries, experiment = (build_indexed_hid(args.compiler)
                                      if args.indexed_experiment else (None, {}, None))
    descriptor_entry = entries.get("indexed_descriptor", BASE + 0xbc775)
    report_entry = entries.get("indexed_report", BASE + 0xbca05)
    backend_loads = []
    backend_got = backend_logger = backend_cache = None
    primary_mapping = None
    if backend is not None:
        def backend_bytes(address, length):
            at = file_offset(backend, address, length)
            return backend[at:at + length]

        backend_got = (0x7a3fa + struct.unpack("<I", backend_bytes(0x7a7a4, 4))[0]) & 0xffffffff
        backend_logger = (backend_got + struct.unpack(
            "<I", backend_bytes(0x7a7ac, 4))[0]) & 0xffffffff
        conversion = (backend_bytes(0x7a45c, 0x7a588 - 0x7a45c) +
                      b"\x1c\x9b\x18\x47")
        backend_loads = [
            ("backend-conversion", BACKEND_BASE + 0x7a45c, conversion),
            ("backend-literals", BACKEND_BASE + 0x7a7ac, backend_bytes(0x7a7ac, 24)),
        ]
        cache_got = (0x596fc + struct.unpack("<I", backend_bytes(0x59864, 4))[0]) & 0xffffffff
        backend_guard = (cache_got + struct.unpack(
            "<I", backend_bytes(0x59868, 4))[0]) & 0xffffffff
        backend_initialized = (cache_got + struct.unpack(
            "<I", backend_bytes(0x5986c, 4))[0]) & 0xffffffff
        backend_cache = (0x59718 + struct.unpack(
            "<I", backend_bytes(0x59870, 4))[0]) & 0xffffffff
        backend_cache = BACKEND_BASE + backend_cache + 0x55 * 32
        backend_loads += [
            ("backend-config-cache", BACKEND_BASE + 0x596ec,
             backend_bytes(0x596ec, 0x59894 - 0x596ec)),
            ("backend-config-getter", BACKEND_BASE + 0x59c64,
             backend_bytes(0x59c64, 0x59ca4 - 0x59c64)),
            ("backend-integer-extraction", BACKEND_BASE + 0x5fcdc,
             backend_bytes(0x5fcdc, 0x10)),
            ("backend-report-import", BACKEND_BASE + 0x20338,
             struct.pack("<II", 0xe51ff004, report_entry)),
            ("backend-display-feature-selection", BACKEND_BASE + 0x77b22,
             backend_bytes(0x77b22, 0x1c) + struct.pack("<HH", 0x6002, 0x4770)),
            ("backend-display-touchpad-feature", BACKEND_BASE + 0x77c7c,
             backend_bytes(0x77c7c, 6)),
            ("backend-primary-input-mapping", BACKEND_BASE + 0x77990,
             backend_bytes(0x77990, 0x18)),
        ]
        primary_table = 0x7799c + struct.unpack("<i", backend_bytes(0x779a4, 4))[0]
        primary_mapping = list(struct.unpack("<8I", backend_bytes(primary_table, 32)))
        backend_loads.append(("backend-primary-input-table", BACKEND_BASE + primary_table,
                              backend_bytes(primary_table, 32)))
    phoff = struct.unpack_from("<I", source, 28)[0]
    stride, count = struct.unpack_from("<HH", source, 42)
    segments = [
        struct.unpack_from("<8I", source, phoff + i * stride)
        for i in range(count)
    ]
    image = bytearray(max(s[2] + s[5] for s in segments if s[0] == 1))
    for kind, offset, address, _, length, _, _, _ in segments:
        if kind == 1:
            image[address:address + length] = source[offset:offset + length]
    shoff = struct.unpack_from("<I", source, 32)[0]
    stride, count = struct.unpack_from("<HH", source, 46)
    sections = [
        struct.unpack_from("<10I", source, shoff + i * stride)
        for i in range(count)
    ]
    symbols_section = next(s for s in sections if s[1] == 11)
    strings_section = sections[symbols_section[6]]
    strings = source[strings_section[4]:strings_section[4] + strings_section[5]]
    symbols = []
    for offset in range(symbols_section[4],
                        symbols_section[4] + symbols_section[5], 16):
        name, value, _, _, _, section = struct.unpack_from("<IIIBBH", source, offset)
        name = strings[name:strings.index(0, name)].decode()
        symbols.append((name, value, section))

    stubs = bytearray(4096)

    def stub(at, words):
        struct.pack_into(f"<{len(words)}I", stubs, at, *words)
        return STUBS + at

    # Unknown dependencies trap rather than returning a success-shaped value.
    trap = stub(0, [0xeafffffe])
    guard = STUBS + 0x300
    allocator = stub(0x40, [
        0xe59f301c,  # LDR r3, heap-cursor-address
        0xe1a02000,  # MOV r2, size
        0xe5930000,  # LDR r0, [r3]
        0xe2822007,  # ADD r2, r2, #7
        0xe3c22007,  # BIC r2, r2, #7
        0xe0822000,  # ADD r2, r2, r0
        0xe5832000,  # STR r2, [r3]
        0xe12fff1e,  # BX lr
        0xe1a00000,
        STUBS + 0x304,
    ])
    calloc = stub(0x80, [0xe0000091, 0xeaffffed])
    memset = stub(0xc0, [
        0xe1a03000, 0xe3520000, 0x012fff1e,
        0xe4c31001, 0xe2522001, 0x1afffffc, 0xe12fff1e,
    ])
    free = stub(0x100, [0xe12fff1e])  # Fixture bump allocations live until VM teardown.
    struct.pack_into("<II", stubs, 0x300, 0x12345678, HEAP)
    if backend is not None:
        backend_entry = stub(0x600, [
            0xe58de070, 0xe51ff004, BACKEND_BASE + 0x7a45d,
        ])
    provided = {
        "calloc": calloc,
        "malloc": allocator,
        "_Znaj": allocator,
        "memset": memset,
        "free": free,
        "__stack_chk_guard": guard,
    }
    unresolved = {}
    for sec in sections:
        if sec[1] != 9:
            continue
        for offset in range(sec[4], sec[4] + sec[5], 8):
            address, info = struct.unpack_from("<II", source, offset)
            kind, symbol_index = info & 255, info >> 8
            if kind == 23:
                value = BASE + struct.unpack_from("<I", image, address)[0]
            elif kind in (2, 21, 22):
                name, value, defined = symbols[symbol_index]
                if defined:
                    value += BASE
                else:
                    value = provided.get(name, trap)
                    if name not in provided:
                        unresolved[name] = hex(address)
                if kind == 2:
                    value += struct.unpack_from("<I", image, address)[0]
            else:
                raise ValueError(f"Unsupported original ELF relocation {kind}")
            struct.pack_into("<I", image, address, value)
    category = next(value for name, value, _ in symbols
                    if name == "gLogCategory_HIDUtilityEso")
    # No logger is linked: disable fixture diagnostics, not functional checks.
    struct.pack_into("<I", image, category, 0x7fff)

    program = InlineProgram()
    program.load(0, 0x80020000)
    program.words.append(0xee0c0f10)
    # MMU-off memory is Device memory on ARMv7, not the OS's normal RAM.
    for instruction, value in ((0xee020f10, 0x82000000),
                               (0xee020f50, 0), (0xee030f10, 3)):
        program.load(0, value)
        program.words.append(instruction)
    program.words += [0xee110f10, 0xe3c00002, 0xe3800501,
                      0xe3800001, 0xee010f10, 0xf57ff06f]
    program.load(13, 0x81f00000)
    if backend is not None:
        program.write(BACKEND_BASE + backend_logger, STUBS + 0x500)
        program.write(STUBS + 0x540, 7)
        program.write(BACKEND_BASE + backend_guard, guard)
        program.write(BACKEND_BASE + backend_initialized, STUBS + 0x550)
        program.write(STUBS + 0x550, 1)
        program.write(backend_cache, 1)
        program.write(backend_cache + 0x10, 3)
    descriptors = []
    reports = []
    display_cases = []
    primary_cases = []
    if backend is not None:
        for mask in range(16):
            at = DATA + 0x9000 + mask * 4
            program.load(0, at)
            for register, bit in ((11, 0), (7, 2), (6, 1), (4, 3)):
                program.load(register, (mask >> bit) & 1)
            program.load(12, BACKEND_BASE + 0x77b23)
            program.words.append(0xe12fff3c)
            display_cases.append({"native_input_mask": mask, "at": at})
        for index, value in enumerate((*range(10), 0xffffffff)):
            at = DATA + 0x9100 + index * 4
            program.load(0, value)
            program.load(12, BACKEND_BASE + 0x77991)
            program.words.append(0xe12fff3c)
            program.load(1, at)
            program.words.append(0xe5810000)
            primary_cases.append({"native_primary_input": value, "at": at})
    contacts = [(100, 200, 1), (700, 500, 1)]
    for index, maximum in enumerate(sorted({1, configured, 4})):
        at = DATA + index * 0x100
        program.write(at + 0x10, 1920 | (720 << 16))
        program.write(at + 0x14, maximum)
        program.load(0, at)
        program.load(1, at + 4)
        program.load(2, at + 0x10)
        program.load(3, descriptor_entry)
        program.words.append(0xe12fff33)
        program.load(1, at + 8)
        program.words.append(0xe5810000)
        descriptors.append({"maximum": maximum, "at": at})
    report_inputs = [
        (1, contacts, False, "original-single-contact"),
        (configured, contacts, False, "two-contacts"),
        (4, contacts, False, "inactive-tail"),
        (configured, contacts, True, "force-release"),
        (configured, contacts[:1], False, "one-contact"),
        (configured, [], False, "empty"),
        (configured, [(100, 200, 0), (710, 510, 1)], False, "first-lift-second-moves"),
        (configured, [(100, 200, 0), (710, 510, 0)], False, "both-lifted"),
        (configured, [(110, 210, 1), (710, 510, 1)], False, "first-slot-reused"),
        (4, [(10, 20, 1), (30, 40, 1), (50, 60, 1), (70, 80, 1)],
         False, "four-contacts"),
        (configured, [(0, 0, 1), (65535, 65535, 1)], False, "uint16-boundaries"),
        (configured, [(1920, 720, 1), (1, 1, 0)], False, "display-boundaries"),
        (configured, [(100, 200, 1), (710, 510, 0)], False, "second-lift-first-held"),
    ]
    if captured_frames:
        for index, frame in enumerate(captured_frames):
            report_inputs.append((
                configured, [(x, y, int(kind != 2)) for x, y, kind in frame],
                False, f"captured-java-dsi-{index}"))
    for index, (maximum, events, release, name) in enumerate(report_inputs):
        at = DATA + 0x1000 + index * 0x100
        for contact_index, (x, y, down) in enumerate(events):
            target = at + 0x20 + contact_index * 6
            if backend is not None:
                vector = DATA + 0x4000 + index * 0x200
                event = vector + 0x40 + contact_index * 12
                program.write(vector + contact_index * 12 + 4, event)
                program.write(event, x)
                program.write(event + 4, y)
                program.write(event + 8, 1 if down else 2)
                continue
            # STRH via ARM register offsets avoids unaligned word writes.
            program.load(0, target)
            program.load(1, x)
            program.words.append(0xe1c010b0)
            program.load(1, y)
            program.words.append(0xe1c010b2)
            program.load(1, down)
            program.words.append(0xe5c01004)
        if backend is not None and events:
            program.write(0x81f0001c, 1)  # Original absolute-coordinate branch.
            program.write(0x81f00020, vector + len(events) * 12)
            program.write(0x81f00038, BACKEND_BASE + backend_got)
            program.write(0x81f00064, at + 0x20)
            program.write(0x81f00068, at + 0x20 + len(events) * 6)
            program.load(7, vector)
            program.load(8, at + 0x20)
            program.load(10, DATA + 0xc000)
            program.write(backend_cache + 0x18, maximum)
            program.load(12, backend_entry)
            program.words.append(0xe12fff3c)
            program.load(1, at + 0x14)
            program.words.append(0xe5810000)
            program.load(1, 0x81f00050)
            program.words.append(0xe8910005)
            program.load(1, at + 0xc)
            program.words.append(0xe8810005)
        program.write(0x81f00000, maximum)
        program.write(0x81f00004, int(release))
        program.load(0, at)
        program.load(1, at + 4)
        program.load(2, at + 0x20)
        program.load(3, len(events))
        program.load(12, report_entry)
        program.words.append(0xe12fff3c)
        program.load(1, at + 8)
        program.words.append(0xe5810000)
        reports.append({"maximum": maximum, "event_count": len(events),
                        "active_count": sum(down for _, _, down in events),
                        "force_release": release, "at": at,
                        "case": name, "input_contacts": events})
    rejected = []
    if candidate is not None:
        for index, (builder, maximum, null_argument) in enumerate((
                ("indexed_descriptor", 0, False),
                ("indexed_descriptor", 5, False),
                ("indexed_descriptor", 2, True),
                ("indexed_report", 0, False),
                ("indexed_report", 5, False),
                ("indexed_report", 2, True))):
            at = DATA + 0x8000 + index * 0x100
            program.write(at, 0xdeadbeef)
            program.write(at + 4, 0xdeadbeef)
            program.write(at + 0x10, 1920 | (720 << 16))
            program.write(at + 0x14, maximum)
            program.write(0x81f00000, maximum)
            program.write(0x81f00004, 0)
            program.load(0, at)
            program.load(1, at + 4)
            program.load(2, 0 if null_argument else at + 0x10)
            program.load(3, 1)
            program.load(12, entries[builder])
            program.words.append(0xe12fff3c)
            program.load(1, at + 8)
            program.words.append(0xe5810000)
            rejected.append({"builder": builder, "maximum": maximum,
                             "null_input": null_argument, "at": at})
    program.write(DATA + 0x3000, 0x5a17c0de)

    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mh2p-original-hid-") as tmp:
        root = Path(tmp)
        (root / "kernel").write_bytes(program.image()[0x100:])
        (root / "library").write_bytes(image)
        (root / "abi").write_bytes(stubs)
        (root / "vectors").write_bytes(struct.pack("<8I", *([0xeafffffe] * 8)))
        page_table = bytearray(16384)
        for section in range(0x800, 0x900):
            struct.pack_into("<I", page_table, section * 4,
                             (section << 20) | 0x11c0e)
        (root / "mmu").write_bytes(page_table)
        qemu_args = [
            "-M", "mh2p", "-m", "256M", "-kernel", str(root / "kernel"),
            "-S", "-display", "none", "-serial", "null", "-nic", "none",
            "-device", f"loader,file={root / 'library'},addr={BASE:#x},force-raw=on",
            "-device", f"loader,file={root / 'abi'},addr={STUBS:#x},force-raw=on",
            "-device", f"loader,file={root / 'vectors'},addr=0x80020000,force-raw=on",
            "-device", f"loader,file={root / 'mmu'},addr=0x82000000,force-raw=on",
        ]
        for name, address, data in backend_loads:
            path = root / name
            path.write_bytes(data)
            qemu_args += ["-device", f"loader,file={path},addr={address:#x},force-raw=on"]
        if candidate is not None:
            (root / "indexed").write_bytes(candidate)
            qemu_args += ["-device",
                          f"loader,file={root / 'indexed'},addr={INDEXED_BASE:#x},force-raw=on"]
        with QEMUMachine(str(ROOT / "build/qemu-system-arm"), args=qemu_args) as vm:
            vm.launch()
            vm._qmp.settimeout(5)

            def word(address):
                reply = vm.cmd("human-monitor-command",
                               command_line=f"xp /1wx {address:#x}")
                return int(reply.split(":")[1].strip(), 16)

            def memory(address, length):
                path = root / "readback"
                reply = vm.cmd("human-monitor-command",
                               command_line=f"pmemsave {address:#x} {length:#x} "
                               f"{json.dumps(str(path))}")
                if reply:
                    raise RuntimeError(f"Original HID readback failed: {reply}")
                value = path.read_bytes()
                path.unlink()
                return value

            vm.cmd("cont")
            deadline = time.monotonic() + 5
            while word(DATA + 0x3000) != 0x5a17c0de:
                if time.monotonic() >= deadline:
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    raise TimeoutError(f"Original HID did not return: {registers}")
                time.sleep(0.01)
            vm.cmd("stop")
            for case in display_cases:
                mask = case["native_input_mask"]
                expected = ((2 if mask & 1 else 0) | (4 if mask & 2 else 0) |
                            (8 if mask & 4 else 0) | (16 if mask & 8 else 0))
                observed = word(case.pop("at"))
                if observed != expected:
                    raise AssertionError(f"Original display feature mismatch: {case}")
                case["display_features"] = observed
            for case in primary_cases:
                value = case["native_primary_input"]
                expected = primary_mapping[value - 1] if 1 <= value <= 8 else 0
                observed = word(case.pop("at"))
                if observed != expected:
                    raise AssertionError(f"Original primary-input mapping mismatch: {case}")
                case["primaryInputDevice"] = observed
            for case in descriptors:
                at = case.pop("at")
                pointer, length, status = struct.unpack("<III", memory(at, 12))
                if status or length != 7 + (69 if candidate is not None else 55) * case["maximum"]:
                    raise AssertionError(f"Descriptor mismatch: {case}, {length}, {status}")
                descriptor = memory(pointer, length)
                case["bytes"] = length
                case["descriptor_hex"] = descriptor.hex()
                fields = touch_layout(descriptor)
                case["report_bytes_from_descriptor"] = sum(field.size for field in fields) // 8
                if descriptor.count(bytes.fromhex("0922a102")) != case["maximum"]:
                    raise AssertionError("Original descriptor contact collection count mismatch")
            for case in reports:
                at = case.pop("at")
                if backend is not None:
                    events = case["input_contacts"]
                    converted = memory(at + 0x20, len(events) * 6) if events else b""
                    expected_events = b"".join(struct.pack("<HHBB", x, y, down, 0)
                                               for x, y, down in events)
                    if converted != expected_events:
                        raise AssertionError(f"Original backend conversion mismatch: {case}")
                    case["native_backend_contacts_hex"] = converted.hex()
                    case["native_backend_conversion_executed"] = bool(events)
                    if events:
                        pointer, length, status = struct.unpack(
                            "<III", memory(at + 0xc, 12))
                        dispatched = memory(pointer, length)
                        expected = expected_report(events, case["maximum"], False,
                                                   candidate is not None)
                        if status or dispatched != expected:
                            raise AssertionError(
                                f"Original backend config/report dispatch mismatch: {case}")
                        case["native_backend_dispatch_report_hex"] = dispatched.hex()
                        case["native_backend_config_report_dispatch_executed"] = True
                pointer, length, status = struct.unpack("<III", memory(at, 12))
                report = memory(pointer, length)
                expected = expected_report(case["input_contacts"], case["maximum"],
                                           case["force_release"], candidate is not None)
                if status or report != expected:
                    raise AssertionError(f"Original report mismatch: {case}, "
                                         f"{report.hex()} != {expected.hex()}, status {status}")
                case["report_hex"] = report.hex()
                case["bytes"] = length
                descriptor = next(bytes.fromhex(item["descriptor_hex"])
                                  for item in descriptors if item["maximum"] == case["maximum"])
                case["decoded_contacts"] = verify_report(
                    descriptor, report, case["input_contacts"], case["maximum"],
                    case["force_release"], candidate is not None)
            for case in rejected:
                at = case.pop("at")
                pointer, length, status = struct.unpack("<IIi", memory(at, 12))
                if (pointer, length, status) != (0xdeadbeef, 0xdeadbeef, -50):
                    raise AssertionError(f"Indexed invalid input not rejected: {case}")
                case["status"] = status
                case["output_unchanged"] = True
    result = {
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "config_sha256": config_sha256,
        "configured_contact_limit": configured,
        "indexed_experiment": experiment,
        "allocator_fixture": "bump allocation; free retains storage until VM teardown",
        "captured_dsi_frames_sha256": events_sha256,
        "captured_dsi_frame_count": len(captured_frames),
        "live_java_dsi_transport_executed": False,
        "backend_sha256": hashlib.sha256(backend).hexdigest() if backend is not None else None,
        "native_backend_conversion_executed": backend is not None,
        "backend_fixture": ({
            "original_range": "0x7a45c..0x7a554",
            "preallocated_contact_vector": True,
            "absolute_coordinates": True,
            "return_endpoint": "0x7a588, before session send",
            "additional_dispatch_original_range": "0x7a554..0x7a588",
            "original_config_cache_getter_executed": True,
            "config_cache_initialized_fixture": True,
            "original_library_import_linked": candidate is None,
            "indexed_wrapper_import_linked": candidate is not None,
            "dispatch_return_endpoint": "0x7a588, before session send",
            "dispatch_skipped_for_empty_cases": True,
            "backend_dispatch_force_release_argument": False,
            "full_dsi_dispatch_executed": False,
        } if backend is not None else None),
        "scope": ("laboratory ARM indexed wrappers calling original builders"
                  if candidate is not None else "original descriptor/report instructions") +
                 " with explicit allocator/memset ABI fixtures",
        "unknown_dependencies_trap": True,
        "qnx_boot": False,
        "iphone_session": False,
        "elapsed_host_seconds": time.monotonic() - started,
        "descriptors": descriptors,
        "reports": reports,
        "rejected_indexed_inputs": rejected,
        "display_advertisement": ({
            "scope": "original feature-selection and primary-enum instructions only",
            "native_input_flags_are_fixtures": True,
            "live_info_response_verified": False,
            "original_feature_range": "0x77b22..0x77b3e plus 0x77c7c..0x77c82",
            "feature_capture_endpoint": "0x77b3e before CF dictionary insertion",
            "original_primary_mapping_range": "0x77990..0x779a8",
            "features": display_cases, "primary_inputs": primary_cases,
        } if backend is not None else None),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
