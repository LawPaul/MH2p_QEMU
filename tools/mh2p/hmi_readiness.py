#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify original HMI display initialization, not sustained unit readiness."""
import argparse
from pathlib import Path
import re
import shlex


DISPLAY_SUCCESS = (
    rb"\bDEBUG[ \t]+<Fw\.Display>[ \t]+"
    rb"waitForDSIDisplayManagement: succeeded!")
DISPLAY_TIMEOUT = b"NATIVE_HMI_DISPLAY_WAIT_TIMEOUT"
DISPLAY_LOG_ERROR = b"NATIVE_HMI_DISPLAY_LOG_ERROR"
DISPLAY_FAILURE = (
    rb"\bERROR[ \t]+<Fw\.Display>[ \t]+"
    rb"waitForDSIDisplayManagement: failed!")


def verify_original_hmi_display(connection):
    if re.search(rb"(?m)^[^\r\n]*\b" + DISPLAY_LOG_ERROR + rb"\r*$", connection):
        raise RuntimeError("Original HMI display log capture failed")
    if re.search(rb"(?m)^[^\r\n]*\b" + DISPLAY_TIMEOUT + rb"\r*$", connection):
        raise RuntimeError("Original HMI display-management wait timed out")
    if re.search(rb"(?m)^[^\r\n]*" + DISPLAY_FAILURE + rb"\r*$", connection):
        raise RuntimeError("Original HMI display-management initialization failed")
    if not re.search(rb"(?m)^[^\r\n]*" + DISPLAY_SUCCESS + rb"\r*$", connection):
        raise RuntimeError(
            "Original HMI display-management success log line is missing")


def display_wait_command(log="/tmp/hmi.log", attempts=180, *, slog=False):
    """Poll QNX slog or a file; also capture HMI stdout and verify the result."""
    if isinstance(attempts, bool) or not isinstance(attempts, int) or attempts <= 0:
        raise ValueError("Display wait attempts must be a positive integer")
    if not isinstance(log, str) or not log or "\0" in log:
        raise ValueError("Display log must be a nonempty shell path without NUL")
    if not isinstance(slog, bool):
        raise ValueError("Select QNX slog with a boolean")
    path = shlex.quote(log)
    # Split the success literal so a console echo cannot satisfy the wait.
    pattern = (
        r"(^|[[:space:]])(DEBUG|ERROR)[[:space:]]+<Fw\.Display>[[:space:]]+"
        r"waitForDSIDisplayManagement: (suc''ceeded!|fai''led!)[[:cntrl:]]?$")
    capture = ""
    search = f"grep -Eq '{pattern}' -- {path}"
    emit = ""
    if slog:
        capture = (
            "if ! display_log=$(sloginfo); then "
            "echo NATIVE_HMI_DISPLAY_LOG_ERROR >&2; break; fi; ")
        search = f'printf "%s\\n" "$display_log" | grep -Eq \'{pattern}\''
        emit = 'printf "%s\\n" "$display_log"; '
    return (
        f"attempt=0; while :; do {capture}"
        f"if {search}; then break; fi; "
        f'if [ "$attempt" -ge {attempts} ]; then '
        "echo NATIVE_HMI_DISPLAY_WAIT_TIMEOUT >&2; break; fi; "
        "sleep 1; attempt=$((attempt + 1)); done; "
        f"{emit}cat -- {path}; netstat -an; pidin -p j9; pidin ar; "
        "echo PRIORITY_HMI_OBSERVER_END")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="Captured guest console output")
    args = parser.parse_args()
    try:
        verify_original_hmi_display(args.capture.read_bytes())
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"MH2P display readiness failed: {error}\n")
    print("Original HMI display initialization observed; sustained health unverified")


if __name__ == "__main__":
    main()
