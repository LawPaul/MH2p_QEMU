#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serialized, bounded checks against one already booted paired HMI."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import time
import uuid


class LiveSession:
    def __init__(self, directory, identity_files, idle_seconds=300, max_seconds=1800):
        if not 1 <= idle_seconds <= max_seconds <= 86400:
            raise ValueError("Require 1 <= idle timeout <= maximum lifetime <= 86400")
        self.directory = Path(directory)
        self.identity_files = identity_files
        self.idle_seconds = idle_seconds
        self.max_seconds = max_seconds
        self.stop_requested = False

    def __enter__(self):
        self.started = self.last_activity = time.monotonic()
        self.record = {
            "boot_id": uuid.uuid4().hex, "pid": os.getpid(),
            "started_unix": time.time(), "state": "control-ready",
            "idle_seconds": self.idle_seconds, "max_seconds": self.max_seconds,
            "identity_sha256": {},
            "full_hmi_readiness_verified": False,
        }
        for path in self.identity_files:
            path = Path(path)
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            self.record["identity_sha256"][str(path)] = digest.hexdigest()
        self.save()
        return self

    def save(self):
        path = self.directory / "live-session.json"
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.record, indent=2) + "\n")
        temporary.replace(path)

    def activity(self):
        self.last_activity = time.monotonic()

    def should_stop(self):
        now = time.monotonic()
        reason = ("requested" if self.stop_requested else
                  "maximum-lifetime" if now - self.started >= self.max_seconds else
                  "idle-timeout" if now - self.last_activity >= self.idle_seconds else None)
        if reason:
            self.record["stop_reason"] = reason
        return reason is not None

    def __exit__(self, exc_type, exc, traceback):
        self.record.update(
            state="failed" if exc_type else "control-closed",
            ended_unix=time.time())
        if exc_type:
            self.record["error"] = str(exc)
        self.save()


def publish_control_request(directory, request):
    temporary = directory / f".control-request-{uuid.uuid4().hex}.tmp"
    try:
        with temporary.open("x") as stream:
            json.dump(request, stream)
        os.link(temporary, directory / "control-request.json")
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def control_transaction(directory, timeout):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Control timeout must be positive")
    deadline = time.monotonic() + timeout
    with (directory / "control-client.lock").open("a") as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Timed out waiting for live control owner")
                time.sleep(0.1)
        try:
            yield deadline
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def request_control(directory, action, command="", timeout=120):
    """Keep ambiguous transactions blocked, even after a client timeout."""
    directory = Path(directory)
    with control_transaction(directory, timeout) as deadline:
        inflight = directory / "control-inflight.json"
        if inflight.exists():
            raise RuntimeError(
                f"Unresolved control transaction: {inflight}; inspect its response "
                "before issuing another request")
        request = {"id": uuid.uuid4().hex, "action": action, "command": command}
        evidence = directory / "control-checks"
        evidence.mkdir(exist_ok=True)
        record = {"request": request, "started_unix": time.time()}
        session_path = directory / "live-session.json"
        if session_path.exists():
            record["session"] = json.loads(session_path.read_text())
            if record["session"]["state"] != "control-ready":
                raise RuntimeError("Live session control is closed; do not reuse it")
        record_path = evidence / f"{request['id']}.json"
        with inflight.open("x") as stream:
            json.dump(request, stream)
        try:
            try:
                publish_control_request(directory, request)
            except BaseException:
                inflight.unlink()
                raise
            while time.monotonic() < deadline:
                response_path = directory / "control-response.json"
                if response_path.exists():
                    try:
                        response = json.loads(response_path.read_text())
                    except json.JSONDecodeError:
                        time.sleep(0.1)
                        continue
                    if response.get("id") == request["id"]:
                        record["response"] = response
                        if ("result" in response) == ("error" in response):
                            raise RuntimeError("Malformed matching control response")
                        inflight.unlink()
                        if "error" in response:
                            raise RuntimeError(response["error"])
                        return response["result"]
                time.sleep(0.1)
            raise TimeoutError(
                f"No response to {request['id']}; unresolved transaction retained")
        except BaseException as exc:
            record["error"] = str(exc)
            raise
        finally:
            record["finished_unix"] = time.time()
            record_path.write_text(json.dumps(record, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-directory", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    actions = parser.add_subparsers(dest="action", required=True)
    query = actions.add_parser("diagnosis")
    query.add_argument("command")
    actions.add_parser("shutdown")
    actions.add_parser("scanout")
    actions.add_parser("rcc-health")
    actions.add_parser("health")
    actions.add_parser("startup")
    actions.add_parser("persistence-health")
    actions.add_parser("status")
    args = parser.parse_args()
    try:
        if args.action == "status":
            result = json.loads(
                (args.control_directory / "live-session.json").read_text())
        else:
            result = request_control(
                args.control_directory, args.action,
                getattr(args, "command", ""), args.timeout)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f"Live HMI control failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
