#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify original Java contact lifecycle and prepared CarPlay-only slot retention."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from prepare_carplay_java_slots import validate_archives


EXPECTED_ORIGINAL = [
    [[373, 269, 1], [639, 269, 1]],
    [[346, 254, 1], [666, 284, 1]],
    [[346, 254, 2], [666, 284, 1]],
    [[682, 299, 1]],
    [[682, 299, 2]],
]
EXPECTED_STABLE = EXPECTED_ORIGINAL[:3] + [
    [[346, 254, 2], [682, 299, 1]],
    [[346, 254, 2], [682, 299, 2]],
]
EXPECTED_CANCEL_ORIGINAL = EXPECTED_ORIGINAL[:2] + [
    [[346, 254, 2], [666, 284, 2]],
    [[100, 200, 1]],
    [[100, 200, 2]],
]
EXPECTED_CANCEL_STABLE = EXPECTED_CANCEL_ORIGINAL
EXPECTED_RECONNECT_ORIGINAL = EXPECTED_ORIGINAL[:2] + [
    [[346, 254, 1], [666, 284, 1], [100, 200, 1]],
    [[346, 254, 1], [666, 284, 1], [100, 200, 2]],
]
EXPECTED_RECONNECT_STABLE = EXPECTED_ORIGINAL[:2] + [
    [[346, 254, 2], [666, 284, 2]],
    [[100, 200, 1]],
    [[100, 200, 2]],
]


def parse_trace(text):
    frames = []
    for line in text.splitlines():
        if line.startswith("DSI "):
            frames.append([[int(value) for value in contact.split(",")]
                           for contact in line.split()[1:]])
        elif not line.startswith("HMI "):
            raise ValueError(f"Unexpected Java trace line: {line!r}")
    return frames


def run_java(command, timeout):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Java command failed ({result.returncode}): {command!r}\n"
                           f"{result.stdout}{result.stderr}")
    return result


def probe(original, prepared, output, java, javac):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    before = validate_archives(original, prepared)
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="mh2p-carplay-java-trace-") as name:
        root = Path(name)
        compiled = run_java(
            [str(javac), "-classpath", str(prepared.resolve()), "-d", str(root),
             str(Path(__file__).with_name("CarplayJavaTrace.java")),
             str(Path(__file__).with_name("CarplayContactSlotsChecks.java")),
             str(Path(__file__).with_name("CarplayGuestChecks.java"))],
            timeout=60)
        (output / "compiler.log").write_text(compiled.stderr)
        for archive, label, expected in (
                (original, "original", EXPECTED_ORIGINAL),
                (prepared, "prepared", EXPECTED_STABLE)):
            result = run_java(
                [str(java), "-Xverify:all", "-classpath",
                 f"{root}:{archive.resolve()}", "CarplayJavaTrace"],
                timeout=30)
            (output / f"{label}-trace.log").write_text(result.stdout)
            (output / f"{label}-stderr.log").write_text(result.stderr)
            frames = parse_trace(result.stdout)
            if frames != expected:
                raise RuntimeError(f"Unexpected original Java lifecycle output: {label}")
            (output / f"{label}-dsi-frames.json").write_text(
                json.dumps(frames, indent=2) + "\n")
            cancelled = run_java(
                [str(java), "-Xverify:all", "-classpath",
                 f"{root}:{archive.resolve()}", "CarplayJavaTrace", "--cancellation"],
                timeout=30)
            (output / f"{label}-cancellation-trace.log").write_text(cancelled.stdout)
            expected_cancel = (EXPECTED_CANCEL_ORIGINAL if label == "original"
                               else EXPECTED_CANCEL_STABLE)
            cancel_frames = parse_trace(cancelled.stdout)
            if cancel_frames != expected_cancel:
                raise RuntimeError(f"Unexpected Java cancellation output: {label}")
            (output / f"{label}-cancellation-dsi-frames.json").write_text(
                json.dumps(cancel_frames, indent=2) + "\n")
            reconnected = run_java(
                [str(java), "-Xverify:all", "-classpath",
                 f"{root}:{archive.resolve()}", "CarplayJavaTrace", "--reconnect"],
                timeout=30)
            (output / f"{label}-reconnect-trace.log").write_text(reconnected.stdout)
            reconnect_frames = parse_trace(reconnected.stdout)
            expected_reconnect = EXPECTED_RECONNECT_ORIGINAL
            if reconnect_frames != expected_reconnect:
                raise RuntimeError(f"Unexpected Java reconnect output: {label}")
            (output / f"{label}-reconnect-dsi-frames.json").write_text(
                json.dumps(reconnect_frames, indent=2) + "\n")
        empty_result = run_java(
            [str(java), "-Xverify:all", "-classpath",
             f"{root}:{prepared.resolve()}", "CarplayJavaTrace", "--empty"],
            timeout=30)
        if parse_trace(empty_result.stdout) != EXPECTED_STABLE:
            raise RuntimeError("Empty idle frames changed the prepared Java gesture")
        (output / "prepared-empty-trace.log").write_text(empty_result.stdout)
        checked = run_java(
            [str(java), "-Xverify:all", "-classpath",
             f"{root}:{prepared.resolve()}", "CarplayContactSlotsChecks"],
            timeout=30)
        helper_checks = json.loads(checked.stdout)
        if helper_checks != {"helper_checks_passed": 20}:
            raise RuntimeError(f"Unexpected helper assertion result: {helper_checks}")
        (output / "helper-checks.json").write_text(checked.stdout)
        from probe_carplay_guest_java import GUEST_SCENARIOS, verify_guest_result

        for scenario in GUEST_SCENARIOS:
            option = [] if scenario == "lifecycle" else [f"--{scenario}"]
            checked_public = run_java(
                [str(java), "-Xverify:all", "-classpath",
                 f"{root}:{prepared.resolve()}", "CarplayGuestChecks", *option],
                timeout=30)
            verify_guest_result(checked_public.stdout.encode(), scenario)
            (output / f"prepared-public-{scenario}-trace.log").write_text(checked_public.stdout)
    after = [hashlib.sha256(path.read_bytes()).hexdigest()
             for path in (original, prepared)]
    if before != after:
        raise RuntimeError("Java lifecycle probe changed an input archive")
    report = {
        "original_archive_sha256": before[0], "prepared_archive_sha256": before[1],
        "original_hmi_lifecycle_methods_executed": True,
        "original_carplay_conversion_body_executed": True,
        "original_slot_compaction_reproduced": True,
        "prepared_survivor_slot_preserved": True,
        "frames_per_variant": 5,
        "helper_checks_passed": helper_checks["helper_checks_passed"],
        "original_manager_deactivate_invoked": True,
        "inactive_manager_fixture_preservation_tested": True,
        "public_callback_manager_active_flag_fixture": True,
        "full_manager_activation_executed": False,
        "inactive_manager_deactivation_preserves_contacts": True,
        "active_manager_fixture_deactivation_emits_release": True,
        "repeated_deactivation_does_not_emit_duplicate_release": True,
        "new_gesture_after_late_physical_releases_verified": True,
        "new_gesture_without_late_physical_releases_verified": True,
        "empty_idle_frames_before_and_after_gesture_verified": True,
        "original_public_touchscreen_filter_executed": True,
        "filter_rejection_cleared_only_after_active_deactivation": True,
        "original_touchscreen_disabled_gate_preserved": True,
        "actual_phone_disconnect_reconnect_verified": False,
        "java_class_verification_enabled": True,
        "caller_objects_allocated_without_startup": True,
        "filter_result_fixture_supplied": True,
        "dsi_interface_is_capture_proxy": True,
        "live_hmi_dsi_delivery_verified": False,
        "native_backend_execution_in_this_probe": False,
        "iphone_gestures_verified": False,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--prepared", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--java", type=Path, default=Path("java"))
    parser.add_argument("--javac", type=Path, default=Path("javac"))
    args = parser.parse_args()
    print(json.dumps(probe(args.original, args.prepared, args.output,
                           args.java, args.javac), indent=2))


if __name__ == "__main__":
    main()
