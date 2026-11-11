#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Smoke-test experimental contact classes on the original guest JVM, not live HMI."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from check_guest_graphics import wait_console
from prepare_carplay_java_slots import CONTROLLER, FILTER, HELPER, LISTENER, MANAGER
from prepare_carplay_lab_image import Qnx6
from prepare_mnand import digest
from probe_carplay_java_slots import (
    EXPECTED_RECONNECT_STABLE, EXPECTED_STABLE, parse_trace, run_java, validate_archives)
from probe_mmx_pcie import BANNER, PREREQUISITES, execute, verify_media
from probe_rcc import QEMUMachine


GUEST_SCENARIOS = {
    "lifecycle": (EXPECTED_STABLE + EXPECTED_RECONNECT_STABLE,
                  b"MH2P_CARPLAY_GUEST_LIFECYCLE_PASS"),
    "filter-restart": ([
        [[373, 269, 1], [639, 269, 1]],
        [[0, 254, 2], [666, 284, 2]],
        [[100, 200, 1]],
        [[100, 200, 2]],
    ], b"MH2P_CARPLAY_GUEST_FILTER_RESTART_PASS"),
    "filter-sticky": ([
        [[373, 269, 1], [639, 269, 1]],
        [[0, 254, 2], [666, 284, 2]],
        [[100, 200, 2]],
        [[373, 269, 2], [100, 200, 2]],
        [[100, 200, 1]],
        [[100, 200, 2]],
    ], b"MH2P_CARPLAY_GUEST_FILTER_STICKY_PASS"),
    "disabled": ([], b"MH2P_CARPLAY_GUEST_TOUCHSCREEN_DISABLED_PASS"),
}


def verify_image_archive(mnand, expected_sha256):
    fs = Qnx6(mnand, 4096 * 512)
    try:
        inode = 1
        for component in ("eso", "hmi", "lsd", "lsd.jar"):
            entries = dict(fs.listdir(inode))
            if component not in entries:
                raise ValueError(f"APP filesystem is missing HMI archive component: {component}")
            inode = entries[component]
        if fs.inode(inode)["mode"] & 0xf000 != 0x8000:
            raise ValueError("APP HMI archive is not a regular file")
        observed = hashlib.sha256(fs.read_file(inode)).hexdigest()
        if observed != expected_sha256:
            raise ValueError("Mounted APP archive does not match the pinned original HMI archive")
        return observed
    finally:
        fs.f.close()


def verify_overlay_readback(response, payload):
    match = re.search(rb"CARPLAY_JAR_BEGIN\r?\n(.*?)\r?\nCARPLAY_JAR_END", response, re.S)
    if match is None or match[1].replace(b"\r\n", b"\n") != payload:
        raise RuntimeError("Guest overlay archive readback differs")


def verify_guest_result(result, scenario="lifecycle"):
    expected, scenario_token = GUEST_SCENARIOS[scenario]
    for token in (b"MH2P_CARPLAY_GUEST_JAVA_CHECKS_PASS", scenario_token):
        if token not in result.splitlines():
            raise RuntimeError(f"Original guest Java check missing: {token.decode()}")
    text = "\n".join(line[len(b"GUEST_"):].decode("ascii")
                     for line in result.splitlines() if line.startswith(b"GUEST_DSI "))
    frames = parse_trace(text)
    if frames != expected:
        raise RuntimeError("Original guest Java DSI frames differ from lifecycle expectations")
    return frames


def probe(args):
    if args.output.exists():
        raise FileExistsError(f"Output already exists: {args.output}")
    verify_media(args.media)
    identities = validate_archives(args.original, args.prepared)
    verify_image_archive(args.mnand, identities[0])
    sources = {"nor": args.nor, "mnand": args.mnand}
    before = {name: digest(path) for name, path in sources.items()}
    args.output.mkdir(parents=True)
    overlay = args.output.resolve() / "guest-contact-checks.jar"
    with tempfile.TemporaryDirectory(prefix="mh2p-guest-contact-checks-") as name:
        root = Path(name)
        run_java(
            [str(args.java), "-jar", str(args.ecj), "-source", "1.4", "-target", "1.4",
             "-proc:none", "-classpath", str(args.prepared.resolve()), "-d", str(root),
             str(Path(__file__).with_name("CarplayGuestChecks.java"))], timeout=60)
        smoke = (root / "CarplayGuestChecks.class").read_bytes()
        if smoke[:8] != bytes.fromhex("cafebabe00000030"):
            raise RuntimeError("Guest smoke harness is not Java 1.4 class format")
        with ZipFile(args.prepared) as prepared, ZipFile(
                overlay, "x", compression=ZIP_DEFLATED) as target:
            for entry in (CONTROLLER, MANAGER, LISTENER, FILTER, HELPER):
                target.writestr(entry, prepared.read(entry))
            for entry in sorted(root.glob("CarplayGuestChecks*.class")):
                target.writestr(entry.name, entry.read_bytes())
    qemu_args = [
        "-M", "mh2p", "-S", "-kernel", str((args.media / "startup.bin").resolve()),
        "-dtb", str((args.media / "board.dtb").resolve()), "-bios", str(args.nor.resolve()),
        "-drive", "if=sd,index=0,format=raw,snapshot=on,file=" +
        str(args.mnand.resolve()).replace(",", ",,"),
        "-display", "none", "-nic", "none", "-icount", "shift=0,align=off,sleep=on",
        "-d", "unimp,guest_errors", "-D", str(args.output.resolve() / "unimplemented.log"),
    ]
    with QEMUMachine(str(args.qemu.resolve()), args=qemu_args) as vm:
        vm.set_console(console_index=1)
        vm.launch()
        vm._qmp.settimeout(10)
        (args.output / "qemu-command.json").write_text(
            json.dumps(list(vm._qemu_full_args), indent=2) + "\n")
        captured = bytearray()
        with (args.output / "guest-console.log").open("wb") as log:
            vm.cmd("cont")
            banner = wait_console(vm.console_socket, captured, log,
                                  re.escape(BANNER), 0, 60)
            wait_console(vm.console_socket, captured, log, rb"mh2p# ", banner.end(), 60)
            for prerequisite in PREREQUISITES[:2]:
                execute(vm.console_socket, captured, log, prerequisite, 120)
            execute(vm.console_socket, captured, log, ": >/tmp/carplay-guest-checks.jar", 30)
            payload = overlay.read_bytes()
            for offset in range(0, len(payload), 128):
                escaped = "".join(f"\\{value:03o}" for value in payload[offset:offset + 128])
                execute(vm.console_socket, captured, log,
                        f"printf '{escaped}' >>/tmp/carplay-guest-checks.jar", 30)
            readback = execute(
                vm.console_socket, captured, log,
                "echo CARPLAY_JAR_BEGIN; cat /tmp/carplay-guest-checks.jar; "
                "echo; echo CARPLAY_JAR_END", 30)
            # Serial line discipline expands LF to CRLF.
            verify_overlay_readback(readback, payload)
            java_command = (
                "LD_LIBRARY_PATH=/ifs/jre/bin:/ifs/jre/lib:/lib:/usr/lib:/mnt/app/eso/lib "
                "/ifs/jre/bin/j9 -verify -Xmx32m "
                "-Xbootclasspath:/tmp/carplay-guest-checks.jar:/mnt/app/eso/hmi/lsd/lsd.jar "
                "CarplayGuestChecks")
            scenario_frames = {}
            for scenario in GUEST_SCENARIOS:
                option = "" if scenario == "lifecycle" else f" --{scenario}"
                result = execute(vm.console_socket, captured, log, java_command + option, 180)
                filename = ("guest-java-result.log" if scenario == "lifecycle"
                            else f"guest-{scenario}-result.log")
                (args.output / filename).write_bytes(result)
                scenario_frames[scenario] = verify_guest_result(result, scenario)
            sequences = {
                "lifecycle": scenario_frames["lifecycle"][:5],
                "cancellation": scenario_frames["lifecycle"][5:],
                "filter-restart": scenario_frames["filter-restart"],
                "filter-sticky": scenario_frames["filter-sticky"],
            }
            for label, sequence in sequences.items():
                (args.output / f"guest-{label}-dsi-frames.json").write_text(
                    json.dumps(sequence, indent=2) + "\n")
    after = {name: digest(path) for name, path in sources.items()}
    if after != before:
        raise RuntimeError("Guest Java smoke probe changed an input image")
    if validate_archives(args.original, args.prepared) != identities:
        raise RuntimeError("Guest Java smoke probe changed an input archive")
    report = {
        "original_archive_sha256": identities[0],
        "prepared_archive_sha256": identities[1],
        "input_image_sha256": before,
        "mounted_original_archive_identity_verified": True,
        "overlay_sha256": hashlib.sha256(payload).hexdigest(),
        "overlay_guest_readback_verified": True,
        "original_qnx_jvm_executed": True,
        "jvm_verification_requested": True,
        "helper_contact_lifecycle_executed_in_guest": True,
        "modified_original_classes_loaded_in_guest": True,
        "original_listener_and_key_controller_constructors_executed": True,
        "original_listener_and_carplay_conversion_executed_in_guest": True,
        "original_public_touchscreen_callback_executed": True,
        "original_touch_point_filter_executed": True,
        "filter_results_supplied": False,
        "device_state_is_explicit_carplay_fixture": True,
        "touchscreen_config_is_explicit_1920x720_fixture": True,
        "captured_guest_dsi_frames": sum(len(frames) for frames in scenario_frames.values()),
        "guest_scenario_frame_counts": {
            scenario: len(frames) for scenario, frames in scenario_frames.items()},
        "guest_cancellation_helper_invoked_directly": False,
        "original_manager_constructor_executed": True,
        "guest_manager_deactivation_executed": True,
        "manager_active_flag_fixture": True,
        "full_manager_activation_executed": False,
        "original_active_deactivation_body_executed": True,
        "original_inactive_deactivation_preserves_contacts": True,
        "original_deactivation_notification_and_active_flag_verified": True,
        "filter_rejection_cleared_after_deactivation_verified": True,
        "filter_rejection_preserved_without_deactivation_verified": True,
        "original_touchscreen_disabled_gate_preserved": True,
        "caller_services_are_strict_diagnostic_proxies": True,
        "dsi_interface_is_capture_proxy": True,
        "temporary_overlay_only": True,
        "source_images_preserved": True,
        "vm_exited": True,
        "live_hmi_dsi_delivery_verified": False,
        "phone_disconnect_reconnect_verified": False,
        "iphone_gestures_verified": False,
    }
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("qemu", "media", "nor", "mnand", "original", "prepared", "ecj", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--java", type=Path, default=Path("java"))
    print(json.dumps(probe(parser.parse_args()), indent=2))


if __name__ == "__main__":
    main()
