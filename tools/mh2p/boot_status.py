#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report paired boot progress without opening a guest console or claiming readiness."""
import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import time


def save_command(directory, record):
    path = Path(directory) / "boot-command.json"
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(record, indent=2) + "\n")
    temporary.replace(path)


@contextmanager
def record_command(directory, command, timeout):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Command timeout must be positive")
    record = {"command": command, "timeout_seconds": timeout,
              "started_unix": time.time(), "state": "running"}
    save_command(directory, record)
    started = time.monotonic()
    try:
        yield
    except BaseException as exc:
        record.update(state="failed", error_type=type(exc).__name__, error=str(exc))
        raise
    else:
        record["state"] = "completed"
    finally:
        record.update(ended_unix=time.time(), elapsed_seconds=time.monotonic() - started)
        save_command(directory, record)


def assess(directory):
    directory = Path(directory)
    if not directory.is_dir():
        raise ValueError(f"Boot directory missing: {directory}")
    evidence = {}
    for name in ("boot-command.json", "live-session.json"):
        path = directory / name
        if path.exists():
            value = json.loads(path.read_text())
            if not isinstance(value, dict):
                raise ValueError(f"Expected an object in {path}")
            evidence[name] = value
    if not evidence:
        raise ValueError("No boot-command or live-session evidence; legacy runs need their owner log")
    command = evidence.get("boot-command.json")
    live = evidence.get("live-session.json")
    if command and command.get("state") not in ("running", "completed", "failed"):
        raise ValueError("Invalid boot command state")
    if command and (not isinstance(command.get("command"), str) or
                    not isinstance(command.get("timeout_seconds"), (int, float)) or
                    not math.isfinite(command["timeout_seconds"]) or
                    command["timeout_seconds"] <= 0):
        raise ValueError("Invalid boot command identity or timeout")
    if live and live.get("state") not in ("control-ready", "control-closed", "failed"):
        raise ValueError("Invalid live session state")
    if (command and command["state"] == "failed") or (live and live["state"] == "failed"):
        state = "failed"
    elif live:
        state = live["state"]
    else:
        state = "starting"
    return {
        "state": state,
        "control_available_recorded": state == "control-ready",
        "full_hmi_readiness_verified": False,
        "liveness_verified": False,
        "scope": "Recorded boot progress only; no process, service or application liveness assertion",
        "command": command,
        "session": live,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = assess(args.directory)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Boot status failed: {exc}\n")
    print(json.dumps(report, indent=2))
    if report["state"] == "failed":
        parser.exit(1)


if __name__ == "__main__":
    main()
