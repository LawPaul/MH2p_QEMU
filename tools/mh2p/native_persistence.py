# SPDX-License-Identifier: GPL-2.0-or-later
"""Strict evidence checks for original MMX SQLite development-namespace experiments."""
import argparse
import json
from pathlib import Path
import re
import sqlite3
import struct
import sys


def verify_client(reply, action, value=None):
    if action not in ("WRITE", "READ"):
        raise ValueError("Expected native persistence WRITE or READ action")
    text = reply.replace("\r", "")
    marker = f"NATIVE_PERSISTENCE_{action}_RC"
    matches = re.findall(rf"(?m)^{marker}=(\d+)$", text)
    if matches != ["0"]:
        raise ValueError(f"Original persistence {action.lower()} did not return exactly one zero status")
    if re.search(r"(?im)^(?:PARSE ERROR|USAGE:|Abort|.*: (?:open|read|write|subscribe) failed:)", text):
        raise ValueError("Original persistence client reported an error")
    if action == "READ":
        if type(value) is not int:
            raise ValueError("Expected an integer readback value")
        if re.findall(r"(?m)^(-?\d+)$", text) != [str(value)]:
            raise ValueError("Original persistence integer readback differs or is ambiguous")
    return {"action": action.lower(), "status": 0, "value": value if action == "READ" else None}


def verify_service_exit(reply):
    matches = re.findall(r"(?m)^NATIVE_SERVICE_EXIT_RC=(\d+)$", reply.replace("\r", ""))
    if matches != ["0"]:
        raise ValueError("Original persistence provider did not record exactly one orderly zero exit")
    return {"service_stopped": True, "exit_status": 0, "service_liveness_verified": False}


def verify_managed_provider(reply, expected_pid=None):
    matches = re.findall(
        r"(?m)^[ \t]*(\d+)[ \t]+/eso/bin/apps/persistenceTNG_mibhigh "
        r"-sm-name persistence_mibhigh -sm-id 104 -sm-time \d+ -sm-restart 0[ \t]*$",
        reply.replace("\r", ""))
    if len(matches) != 1:
        raise ValueError("Expected exactly one original managed persistence process without restart")
    pid = int(matches[0])
    if pid <= 0 or (expected_pid is not None and pid != expected_pid):
        raise ValueError("Original managed persistence provider identity changed")
    return {"pid": pid, "managed_identity": "persistence_mibhigh",
            "managed_id": 104, "restart_count": 0}


def verify_subscription(reply, values, stopped=False):
    if not values or any(type(value) is not int for value in values):
        raise ValueError("Expected explicit integer subscription values")
    text = reply.replace("\r", "")
    records = re.findall(r"(?m)^(INTEGER|STRING|BLOB|ERROR)\t(.*)$", text)
    if records != [("INTEGER", str(value)) for value in values]:
        raise ValueError("Original subscription notifications differ, are missing, or include errors")
    if re.search(r"(?im)^USAGE:|^PARSE ERROR", text):
        raise ValueError("Original subscriber rejected its arguments")
    statuses = re.findall(r"(?m)^SUBSCRIBER_STOP_RC=(\d+)$", text)
    if stopped:
        if statuses != ["143"]:
            raise ValueError("Original subscriber was not explicitly terminated and reaped with SIGTERM status")
        if re.search(r"(?m)^[ \t]*\d+[ \t]+/eso/bin/dumb_persistence_subscriber(?: |$)", text):
            raise ValueError("Original subscriber is still in the final process list")
    elif statuses:
        raise ValueError("Original subscriber stopped before lifecycle verification")
    return {"integer_notifications": values, "explicit_sigterm_teardown_verified": stopped}


def verify_database(raw, namespace, key, value):
    if not raw.startswith(b"SQLite format 3\0"):
        raise ValueError("Committed persistence file is not SQLite")
    if not isinstance(namespace, str) or not namespace.startswith("mh2p-persistence-development"):
        raise ValueError("Only explicit development namespaces are supported")
    if type(key) is not int or type(value) is not int or not -(1 << 63) <= value < (1 << 63):
        raise ValueError("Expected an integer key and signed 64-bit value")
    with sqlite3.connect(":memory:") as database:
        database.deserialize(raw)
        database.execute("PRAGMA query_only=ON")
        if database.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
            raise ValueError("Committed persistence SQLite integrity failed")
        rows = database.execute(
            'SELECT p.profile,p.version,d.type,d.value FROM "persistence-partitions" p '
            'JOIN "persistence-data" d ON p.id=d.partition '
            'WHERE p.name=? AND d.key=?', (namespace, key)).fetchall()
    expected = [('""', "DSI-unversioned", 1, value.to_bytes(8, "big", signed=True))]
    if rows != expected:
        raise ValueError("Committed native partition/version/type/value differs or is ambiguous")
    return {"sqlite_integrity": "ok", "namespace": namespace, "profile": '""', "key": key,
            "version": "DSI-unversioned", "type": "integer",
            "value": value, "stored_bytes_hex": expected[0][3].hex()}


def assess(directory):
    directory = Path(directory)

    def read(name):
        return (directory / name).read_text(encoding="latin1")

    command = json.loads(read("boot-command.json"))
    if not isinstance(command, dict) or command.get("state") != "completed" or command.get("command") != "sync":
        raise ValueError("Managed experiment did not finish its final sync command")
    verify_client(read("subscription-baseline.log"), "WRITE")
    initial = verify_subscription(read("subscriber-initial.log"), [424242])
    verify_client(read("write.log"), "WRITE")
    changes = verify_subscription(read("subscriber-change-stop.log"), [424242, 424243], True)
    boots = []
    for boot in range(2):
        provider = verify_managed_provider(read(f"boot-{boot}-manager.log"))
        if boot == 0:
            verify_managed_provider(read("post-write-service.log"), provider["pid"])
            verify_managed_provider(read("subscriber-change-stop.log"), provider["pid"])
        verify_client(read(f"boot-{boot}-read.log"), "READ", 424243)
        verify_managed_provider(read(f"boot-{boot}-final-health.log"), provider["pid"])
        if len(re.findall(r"(?m)^MMX_PCIE_[0-9a-f]{32}:0$",
                          read(f"boot-{boot}-sync.log").replace("\r", ""))) != 1:
            raise ValueError("Missing unique completed guest sync before host shutdown")
        owner = json.loads(read(f"owned-host-{boot}.json"))
        pid = owner.get("qemu_pid") if isinstance(owner, dict) else None
        if type(pid) is not int or pid <= 0:
            raise ValueError("Invalid owned QEMU PID record")
        if Path(f"/proc/{pid}").exists():
            raise ValueError("Owned QEMU PID still exists; cold-boot cleanup is not verified")
        boots.append({"boot": boot, "qemu_pid": pid, "provider": provider})
    if boots[0]["qemu_pid"] == boots[1]["qemu_pid"]:
        raise ValueError("Expected two distinct cold-boot QEMU processes")
    database = verify_database((directory / "committed-persistence.sqlite").read_bytes(),
                               "mh2p-persistence-development", 1, 424243)
    return {"scope": "Original managed MMX default SQLite development namespace",
            "boots": boots, "initial_subscription": initial, "subscription_changes": changes,
            "committed_database": database, "native_default_sqlite_cold_survival_verified": True,
            "same_boot_provider_identity_preserved": True, "both_owned_hosts_absent": True,
            "native_rcc_persistence_verified": False, "power_loss_recovery_verified": False,
            "full_system_startup_verified": False}


def verify_rcc_timeout(reply):
    text = reply.replace("\r", "")
    records = re.findall(r"(?ms)^RCC_READ_BEGIN\n(.*?)^RCC_READ_END$", text)
    expected = ("/eso/bin/dumb_persistence_reader: read failed: PERS_STATUS_TIMEOUT [12]\n"
                "RCC_PERSISTENCE_READ_RC=1\n")
    if records != [expected]:
        raise ValueError("Expected exactly one original RCC read timeout and client exit 1")
    provider = verify_managed_provider(text)
    descriptors = re.findall(r"(?m)^[ \t]*4[ \t]+(\d+)[ \t]+rw[ \t]+0 "
                             r"/dev/rstp/persistence[ \t]*$", text)
    if len(descriptors) != 1:
        raise ValueError("Missing unique original RSTP persistence descriptor")
    rstp_pid = int(descriptors[0])
    if len(re.findall(rf"(?m)^[ \t]*\d+[ \t]+\d+[ \t]+persistencebackend[ \t]+"
                      rf"\S+[ \t]+REPLY[ \t]+{rstp_pid}[ \t]*$", text)) < 2:
        raise ValueError("Missing original backend threads waiting for the RSTP server")
    return {"client_status": 1, "native_error": "PERS_STATUS_TIMEOUT", "native_error_code": 12,
            "provider": provider, "rstp_server_pid": rstp_pid,
            "persistence_value_verified": False}


def verify_qdb_pending(reply):
    text = reply.replace("\r", "")
    records = re.findall(r"(?m)^\[n\]@persis_qdb_(BAT|DB|IMM|TMP)\.cfg\nStatus::(\w+)$", text)
    if records != [("BAT", "Valid"), ("DB", "AttachWait"), ("IMM", "Valid"), ("TMP", "Valid")]:
        raise ValueError("Expected original valid child QDBs and pending aggregate attachment")
    return {"child_databases": "Valid", "aggregate_database": "AttachWait",
            "cause_of_read_timeout_verified": False}


def verify_qdb_ready(reply):
    text = reply.replace("\r", "")
    records = re.findall(r"(?m)^\[n\]@(persisDB(?:_BAT|_IMM|_TMP)?)\nStatus::(\w+)$", text)
    if records != [("persisDB", "Valid"), ("persisDB_BAT", "Valid"),
                   ("persisDB_IMM", "Valid"), ("persisDB_TMP", "Valid")]:
        raise ValueError("Original RCC QDB aggregate and all children are not valid")
    return {"aggregate_database": "Valid", "child_databases": "Valid",
            "persistence_transactions_verified": False}


def assess_rcc_timeout(directory):
    directory = Path(directory)
    boots = []
    owned = set()
    for boot in range(2):
        health = (directory / f"boot-{boot}-read-health.log").read_text(encoding="latin1")
        result = verify_rcc_timeout(health)
        startup = verify_managed_provider(
            (directory / f"boot-{boot}-manager.log").read_text(encoding="latin1"))
        if startup["pid"] != result["provider"]["pid"]:
            raise ValueError("Managed MMX provider changed during the RCC read")
        qdb_log = (directory / f"boot-{boot}-rcc-health.log").read_text(encoding="latin1")
        result["qdb"] = (verify_qdb_ready(qdb_log) if
                         re.search(r"(?m)^\[n\]@persisDB\r?$", qdb_log)
                         else verify_qdb_pending(qdb_log))
        result["m4_faults"] = []
        for core in range(2):
            raw = (directory / f"ipu1-core-{core}-fault-boot-{boot}.bin").read_bytes()
            if len(raw) != 20 or struct.unpack("<5I", raw) != (0, 0, 0, 0, 0):
                raise ValueError("Missing or nonzero original M4 fault-register evidence")
            result["m4_faults"].append({"core": core, "cfsr": 0, "hfsr": 0})
        owner = json.loads((directory / f"owned-host-{boot}.json").read_text())
        if not isinstance(owner, dict):
            raise ValueError("Invalid paired host ownership record")
        pids = [owner.get("mmx_pid"), owner.get("rcc_pid")]
        if any(type(pid) is not int or pid <= 0 for pid in pids) or len(set(pids)) != 2:
            raise ValueError("Invalid or duplicate paired QEMU PIDs")
        for pid in pids:
            if Path(f"/proc/{pid}").exists():
                raise ValueError("Owned paired QEMU PID still exists")
            owned.add(pid)
        boots.append(result)
    if len(owned) != 2:
        raise ValueError("Expected the same paired QEMU hosts across system reset")
    counts = {name: (directory / f"{name}-uart-wire.bin").stat().st_size
              for name in ("mmx", "rcc")}
    if counts["mmx"] <= 0 or counts["rcc"] != 0:
        raise ValueError("Expected observed MMX traffic and no RCC UART1 transmit bytes")
    return {"status": "blocked", "scope": "Original RCC-backed read diagnostic",
            "namespace": 678364556, "key": 21, "boots": boots, "uart_tx_bytes": counts,
            "both_owned_hosts_absent": True, "persistence_value_verified": False,
            "native_rcc_persistence_verified": False,
            "note": "Native read times out with a live managed provider and RSTP backend. "
                    "QDB status is recorded separately and RCC UART1 has no transmit bytes; "
                    "these observations do not prove which dependency causes the timeout."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True,
                        help="Closed managed subscription experiment with committed-persistence.sqlite")
    parser.add_argument("--rcc-read-timeout", action="store_true",
                        help="Assess a closed paired original RCC read diagnostic; blocked exits 2")
    args = parser.parse_args()
    try:
        result = assess_rcc_timeout(args.directory) if args.rcc_read_timeout else assess(args.directory)
    except (OSError, ValueError, sqlite3.Error) as error:
        print(json.dumps({"verified": False, "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 2 if args.rcc_read_timeout else 0


if __name__ == "__main__":
    sys.exit(main())
