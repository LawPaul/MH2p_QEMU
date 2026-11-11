#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an isolated, opt-in CarPlay-only Java contact-slot experiment."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from zipfile import ZipFile


CONTROLLER = ("de/audi/app/terminalmode/dsi/carplay/"
              "CarplayDSILifecycleController$TerminalModeDSIKeyEventsController.class")
CONTROLLER_SHA256 = "ab48a4d9271cac254f285d392d3a36be4edb9a19ce5b188a7fab23e556ec619a"
HELPER = "mh2p/lab/CarplayContactSlots.class"
DESCRIPTOR = "([Lde/audi/app/terminalmode/keyevents/TouchEvent;[I)V"
MANAGER = "de/audi/app/terminalmode/smartphone/carplay/CarPlayDSIManager.class"
MANAGER_SHA256 = "34f25cd949cd76d4d9006a29388f8a36c8473f26578c3e0a0934fc5de3eaa0d8"
LISTENER = "de/audi/app/terminalmode/keyevents/TMTouchscreenVBListener.class"
LISTENER_SHA256 = "0c06b67f814a91f6ca1fc10885a6fbc8e16fe6a1205135ec2ec421628b4301b4"
FILTER = "de/audi/app/terminalmode/keyevents/TouchPointFilter.class"
FILTER_SHA256 = "1aa14b8c8197438cf1495255fde86fa12ab2902ac34f531fa7dd51a89ba602b7"
ARCHIVE_SHA256 = "f73ea7da610016418aefdcd4faa51dadae8926c63cef580da798383cb6d0f8eb"


def patch_controller(source):
    return patch_method(
        source, CONTROLLER_SHA256, "updateTouchEvents", DESCRIPTOR, "normalize",
        "(Ljava/lang/Object;[Lde/audi/app/terminalmode/keyevents/TouchEvent;)"
        "[Lde/audi/app/terminalmode/keyevents/TouchEvent;",
        b"\x2a\x2b", b"\x4c\x2b\xbe\x9a\x00\x08\xb1\0\0\0\0")


def patch_manager(source):
    # #72 is the inherited active flag; inactive deactivation retains original behavior.
    return patch_method(
        source, MANAGER_SHA256, "deactivate", "()V", "cancel",
        "(Lde/audi/app/terminalmode/dsi/carplay/CarplayDSILifecycleController;"
        "Lde/audi/app/terminalmode/keyevents/TMVirtualButtonListener;)V",
        b"\x2a\xb4\x00\x48\x99\x00\x14"
        b"\x2a\xb4\x00\x1f\x2a\xb4\x00\x08\xb9\x00\x50\x01\x00", b"\0")


def patch_listener(source):
    # Original #10 is lastTouchEvents, #14 the immutable filter instance.
    return patch_method(
        source, LISTENER_SHA256, "determineTouchEvents",
        "(Lde/audi/app/terminalmode/keyevents/TouchPointFilter$TouchPointFilterResults;)"
        "[Lde/audi/app/terminalmode/keyevents/TouchEvent;",
        "prepareListener", "(Ljava/lang/Object;Ljava/util/Map;"
        "Lde/audi/app/terminalmode/keyevents/TouchPointFilter;)V",
        b"\x2a\x2a\xb4\x00\x0a\x2a\xb4\x00\x0e", b"")


def patch_filter(source):
    # Original #3 is filteredFingers; only a CarPlay-marked filter is reset.
    return patch_method(
        source, FILTER_SHA256, "filter",
        "([Lde/audi/atip/datatypes/TouchPoint;Lde/audi/app/terminalmode/device/TMDevice;)"
        "Lde/audi/app/terminalmode/keyevents/TouchPointFilter$TouchPointFilterResults;",
        "prepareFilter", "(Ljava/lang/Object;Lde/audi/atip/utils/generics/GSet;)V",
        b"\x2a\x2a\xb4\x00\x03", b"")


def patch_method(source, expected_sha256, method, signature, helper_method,
                 helper_signature, before_call, after_call):
    if hashlib.sha256(source).hexdigest() != expected_sha256:
        raise ValueError(f"Expected original P2865 CarPlay class for {method}")
    shift = len(before_call) + 3 + len(after_call)
    if shift % 4:
        raise ValueError("Expected switch-aligned method prefix")
    count = struct.unpack_from(">H", source, 8)[0]
    strings = {}
    at, index = 10, 1
    while index < count:
        tag = source[at]
        at += 1
        if tag == 1:
            length = struct.unpack_from(">H", source, at)[0]
            strings[index] = source[at + 2:at + 2 + length].decode("utf-8")
            at += 2 + length
        elif tag in (3, 4, 9, 10, 11, 12):
            at += 4
        elif tag in (5, 6):
            at += 8
            index += 1
        elif tag in (7, 8):
            at += 2
        else:
            raise ValueError(f"Unexpected original constant pool tag: {tag}")
        index += 1
    pool_end = at
    at += 6
    interfaces = struct.unpack_from(">H", source, at)[0]
    at += 2 + 2 * interfaces
    target = None
    for kind in ("fields", "methods"):
        members = struct.unpack_from(">H", source, at)[0]
        at += 2
        for _ in range(members):
            _, name, descriptor, attributes = struct.unpack_from(">4H", source, at)
            at += 8
            for _ in range(attributes):
                attribute_start = at
                attribute_name, length = struct.unpack_from(">HI", source, at)
                at += 6
                if (kind == "methods" and strings[name] == method and
                        strings[descriptor] == signature and
                        strings[attribute_name] == "Code"):
                    if target is not None:
                        raise ValueError("Duplicate original controller method")
                    target = (attribute_start, at, length)
                at += length
    if target is None:
        raise ValueError("Original CarPlay touch method absent")
    attribute_start, code_start, attribute_length = target
    code = bytearray(source[code_start:code_start + attribute_length])
    length = struct.unpack_from(">I", code, 4)[0]
    original_body = bytes(code[8:8 + length])
    at = 8 + length
    if struct.unpack_from(">H", code, at)[0]:
        raise ValueError("Unexpected controller exception handlers")
    at += 2
    attributes = struct.unpack_from(">H", code, at)[0]
    at += 2
    for _ in range(attributes):
        name, size = struct.unpack_from(">HI", code, at)
        at += 6
        entries = struct.unpack_from(">H", code, at)[0]
        if strings[name] == "LineNumberTable":
            stride = 4
        elif strings[name] == "LocalVariableTable":
            stride = 10
        else:
            raise ValueError(f"Unexpected controller code attribute: {strings[name]}")
        if size != 2 + entries * stride:
            raise ValueError("Invalid original controller debug table")
        for row in range(entries):
            offset = at + 2 + row * stride
            start = struct.unpack_from(">H", code, offset)[0]
            struct.pack_into(">H", code, offset, start + shift)
        at += size
    if at != len(code):
        raise ValueError("Unexpected trailing controller Code bytes")
    def utf8(text):
        raw = text.encode("ascii")
        return b"\x01" + struct.pack(">H", len(raw)) + raw

    constants = (
        utf8("mh2p/lab/CarplayContactSlots") +
        b"\x07" + struct.pack(">H", count) +
        utf8(helper_method) + utf8(helper_signature) +
        b"\x0c" + struct.pack(">HH", count + 2, count + 3) +
        b"\x0a" + struct.pack(">HH", count + 1, count + 4))
    # Aligned prefixes preserve the original tableswitch padding.
    prefix = before_call + b"\xb8" + struct.pack(">H", count + 5) + after_call
    code[8:8 + length] = prefix + original_body
    struct.pack_into(">I", code, 4, length + shift)
    header = bytearray(source[:10])
    struct.pack_into(">H", header, 8, count + 6)
    replacement = source[attribute_start:attribute_start + 2] + struct.pack(">I", len(code)) + code
    return (header + source[10:pool_end] + constants +
            source[pool_end:attribute_start] + replacement +
            source[code_start + attribute_length:])


def validate_archives(original, prepared):
    before = [hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (original, prepared)]
    if before[0] != ARCHIVE_SHA256:
        raise ValueError("Expected original P2865 HMI archive")
    with ZipFile(original) as source, ZipFile(prepared) as target:
        names = target.namelist()
        if len(set(names)) != len(names) or set(names) != set(source.namelist()) | {HELPER}:
            raise ValueError("Prepared Java archive entry set differs")
        for name, patch in ((CONTROLLER, patch_controller), (MANAGER, patch_manager),
                            (LISTENER, patch_listener), (FILTER, patch_filter)):
            if target.read(name) != patch(source.read(name)):
                raise ValueError(f"Prepared class differs from guarded plan: {name}")
        if target.read(HELPER)[:8] != bytes.fromhex("cafebabe00000030"):
            raise ValueError("Prepared helper is not Java 1.4 class format")
        for name in source.namelist():
            if name not in (CONTROLLER, MANAGER, LISTENER, FILTER) and source.read(name) != target.read(name):
                raise ValueError(f"Unrelated prepared Java entry differs: {name}")
    return before


def prepare(archive, output, java, ecj):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    original_sha256 = hashlib.sha256(archive.read_bytes()).hexdigest()
    with ZipFile(archive) as original:
        names = original.namelist()
        if len(set(names)) != len(names) or HELPER in names:
            raise ValueError("Duplicate archive entries or pre-existing slot helper")
        if any(name.upper().startswith("META-INF/") and
               name.upper().endswith((".SF", ".RSA", ".DSA", ".EC"))
               for name in names):
            raise ValueError("Signed archives are not supported by this laboratory tool")
        controller = original.read(CONTROLLER)
        modified = patch_controller(controller)
        manager = patch_manager(original.read(MANAGER))
        listener = patch_listener(original.read(LISTENER))
        filter_class = patch_filter(original.read(FILTER))
        replacements = {
            CONTROLLER: modified, MANAGER: manager, LISTENER: listener, FILTER: filter_class}
        with tempfile.TemporaryDirectory(prefix="mh2p-carplay-java-slots-") as name:
            root = Path(name)
            compiled = subprocess.run(
                [str(java), "-jar", str(ecj), "-source", "1.4", "-target", "1.4",
                 "-proc:none", "-classpath", str(archive.resolve()),
                 "-d", str(root), str(Path(__file__).with_name("CarplayContactSlots.java"))],
                capture_output=True, text=True, timeout=60)
            if compiled.returncode:
                raise RuntimeError(
                    f"Java 1.4 helper compilation failed ({compiled.returncode}):\n"
                    f"{compiled.stdout}{compiled.stderr}")
            helper = (root / HELPER).read_bytes()
            if helper[:8] != bytes.fromhex("cafebabe00000030"):
                raise ValueError("Helper compiler did not produce Java 1.4 class format")
        output.mkdir(parents=True)
        prepared = output / "lsd.jar"
        with ZipFile(prepared, "x") as target:
            target.comment = original.comment
            for entry in original.infolist():
                target.writestr(copy.copy(entry), replacements[entry.filename]
                                if entry.filename in replacements else original.read(entry.filename))
            target.writestr(HELPER, helper)
        with ZipFile(prepared) as target:
            if set(target.namelist()) != set(names) | {HELPER}:
                raise RuntimeError("Prepared Java archive entry set differs")
            for name in names:
                expected = replacements[name] if name in replacements else original.read(name)
                if target.read(name) != expected:
                    raise RuntimeError(f"Prepared Java entry differs: {name}")
            if target.read(HELPER) != helper:
                raise RuntimeError("Prepared Java helper differs")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != original_sha256:
        raise RuntimeError("Original Java archive changed during preparation")
    report = {
        "scope": "opt-in laboratory CarPlay-only stable contact-slot adapter",
        "original_controller_sha256": CONTROLLER_SHA256,
        "modified_controller_sha256": hashlib.sha256(modified).hexdigest(),
        "original_manager_sha256": MANAGER_SHA256,
        "modified_manager_sha256": hashlib.sha256(manager).hexdigest(),
        "manager_deactivation_cancellation_hook": True,
        "manager_deactivation_hook_requires_original_active_flag": True,
        "original_listener_sha256": LISTENER_SHA256,
        "modified_listener_sha256": hashlib.sha256(listener).hexdigest(),
        "listener_reset_only_after_carplay_deactivation": True,
        "original_filter_sha256": FILTER_SHA256,
        "modified_filter_sha256": hashlib.sha256(filter_class).hexdigest(),
        "filter_reset_only_after_carplay_deactivation": True,
        "helper_sha256": hashlib.sha256(helper).hexdigest(),
        "helper_class_major_version": 48,
        "original_archive_sha256": original_sha256,
        "prepared_archive_sha256": hashlib.sha256(prepared.read_bytes()).hexdigest(),
        "original_entries_except_four_guarded_classes_preserved": True,
        "touchscreen_listener_changed": True,
        "original_controller_body_preserved_after_aligned_prefix": True,
        "screen_fingers_configuration_changed": False,
        "installed": False,
        "live_hmi_dsi_delivery_verified": False,
        "iphone_gestures_verified": False,
    }
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--java", type=Path, default=Path("java"))
    parser.add_argument("--ecj", type=Path, required=True,
                        help="Eclipse compiler JAR supporting Java 1.4")
    parser.add_argument("--experimental-carplay-contact-slots", required=True,
                        action="store_true")
    args = parser.parse_args()
    print(json.dumps(prepare(args.archive, args.output, args.java, args.ecj), indent=2))


if __name__ == "__main__":
    main()
