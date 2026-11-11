# SPDX-License-Identifier: GPL-2.0-or-later
import sqlite3
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from native_persistence import (
    verify_client, verify_database, verify_service_exit,
    verify_managed_provider, verify_subscription, assess,
    verify_rcc_timeout, verify_qdb_pending, verify_qdb_ready, assess_rcc_timeout,
)


class NativePersistenceTests(unittest.TestCase):
    def database(self, version="DSI-unversioned", kind=1, payload=None, duplicate=False, profile='""'):
        with sqlite3.connect(":memory:") as db:
            db.execute('CREATE TABLE "persistence-partitions"(name,profile,id,version)')
            db.execute('CREATE TABLE "persistence-data"(partition,key,type,value)')
            db.execute('INSERT INTO "persistence-partitions" VALUES(?,?,?,?)',
                       ("mh2p-persistence-development", profile, 1, version))
            row = (1, 1, kind, payload if payload is not None else (424242).to_bytes(8, "big"))
            db.execute('INSERT INTO "persistence-data" VALUES(?,?,?,?)', row)
            if duplicate:
                db.execute('INSERT INTO "persistence-data" VALUES(?,?,?,?)', row)
            db.commit()
            return db.serialize()

    def test_original_integer_status_and_exact_readback(self):
        text = "mh2p# reader; echo NATIVE_PERSISTENCE_READ_RC=$?\r\n424242\r\nNATIVE_PERSISTENCE_READ_RC=0\r\n"
        self.assertEqual(verify_client(text, "READ", 424242)["value"], 424242)
        self.assertEqual(verify_client("NATIVE_PERSISTENCE_WRITE_RC=0\n", "WRITE")["status"], 0)

    def test_echo_missing_failed_duplicate_and_wrong_values_rejected(self):
        for text in ("echo NATIVE_PERSISTENCE_READ_RC=0\n",
                     "424242\nNATIVE_PERSISTENCE_READ_RC=2\n",
                     "424242\nNATIVE_PERSISTENCE_READ_RC=0\nNATIVE_PERSISTENCE_READ_RC=0\n",
                     "123\nNATIVE_PERSISTENCE_READ_RC=0\n",
                     "424242\n424242\nNATIVE_PERSISTENCE_READ_RC=0\n",
                     "USAGE:\n424242\nNATIVE_PERSISTENCE_READ_RC=0\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_client(text, "READ", 424242)

    def test_exact_committed_sqlite_native_encoding(self):
        self.assertEqual(
            verify_database(self.database(), "mh2p-persistence-development", 1, 424242)
            ["stored_bytes_hex"], "0000000000067932")

    def test_service_exit_is_not_liveness_and_requires_actual_status(self):
        self.assertFalse(verify_service_exit("NATIVE_SERVICE_EXIT_RC=0\r\n")["service_liveness_verified"])
        for reply in ("echo NATIVE_SERVICE_EXIT_RC=0\n", "",
                      "NATIVE_SERVICE_EXIT_RC=139\n",
                      "NATIVE_SERVICE_EXIT_RC=0\nNATIVE_SERVICE_EXIT_RC=0\n"):
            with self.subTest(reply=reply), self.assertRaises(ValueError):
                verify_service_exit(reply)

    def test_managed_provider_requires_exact_identity_and_unchanged_pid(self):
        line = " 217103 /eso/bin/apps/persistenceTNG_mibhigh -sm-name persistence_mibhigh -sm-id 104 -sm-time 8790 -sm-restart 0\n"
        self.assertEqual(verify_managed_provider(line, 217103)["pid"], 217103)
        for text in ("echo " + line.strip(), line + line,
                     line.replace("-sm-restart 0", "-sm-restart 1"),
                     line.replace("-sm-id 104", "-sm-id 105"),
                     "pidin -p persistenceTNG_mibhigh\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_managed_provider(text)
        with self.assertRaises(ValueError):
            verify_managed_provider(line, 217104)

    def test_subscription_order_type_values_and_explicit_teardown(self):
        self.assertEqual(verify_subscription("INTEGER\t424242\r\n", [424242])
                         ["integer_notifications"], [424242])
        reply = "INTEGER\t424242\nINTEGER\t424243\nSUBSCRIBER_STOP_RC=143\n"
        self.assertTrue(verify_subscription(reply, [424242, 424243], True)
                        ["explicit_sigterm_teardown_verified"])
        for text in (reply.replace("424243", "424244"),
                     reply.replace("INTEGER\t424243\n", ""),
                     reply.replace("INTEGER", "STRING"),
                     reply.replace("143", "127"),
                     "echo " + reply,
                     reply + "ERROR\t3 PERS_STATUS_TYPE_MISMATCH\n",
                     reply + "123 /eso/bin/dumb_persistence_subscriber -p string\n",
                     reply + "INTEGER\t424243\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_subscription(text, [424242, 424243], True)
        with self.assertRaises(ValueError):
            verify_subscription(reply, [424242, 424243])

    def experiment(self, directory):
        provider = " 217103 /eso/bin/apps/persistenceTNG_mibhigh -sm-name persistence_mibhigh -sm-id 104 -sm-time 8790 -sm-restart 0\n"
        logs = {
            "boot-command.json": json.dumps({"state": "completed", "command": "sync"}),
            "subscription-baseline.log": "NATIVE_PERSISTENCE_WRITE_RC=0\n",
            "write.log": "NATIVE_PERSISTENCE_WRITE_RC=0\n",
            "subscriber-initial.log": "INTEGER\t424242\n",
            "subscriber-change-stop.log": "INTEGER\t424242\nINTEGER\t424243\nSUBSCRIBER_STOP_RC=143\n" + provider,
            "post-write-service.log": provider,
        }
        for boot in range(2):
            logs.update({
                f"boot-{boot}-manager.log": provider,
                f"boot-{boot}-read.log": "424243\nNATIVE_PERSISTENCE_READ_RC=0\n",
                f"boot-{boot}-final-health.log": provider,
                f"boot-{boot}-sync.log": "MMX_PCIE_" + "a" * 32 + ":0\n",
                f"owned-host-{boot}.json": json.dumps({"qemu_pid": 2147483646 - boot}),
            })
        for name, text in logs.items():
            (directory / name).write_text(text)
        (directory / "committed-persistence.sqlite").write_bytes(
            self.database(payload=(424243).to_bytes(8, "big")))

    def test_complete_assessment_and_cli(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.experiment(directory)
            result = assess(directory)
            self.assertTrue(result["same_boot_provider_identity_preserved"])
            self.assertFalse(result["native_rcc_persistence_verified"])
            process = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("native_persistence.py")),
                 "--directory", str(directory)], capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(json.loads(process.stdout)["committed_database"]["value"], 424243)

    def test_assessment_rejects_missing_callbacks_restarts_and_reused_hosts(self):
        for name, text in (
            ("boot-command.json", json.dumps({"state": "failed", "command": "sync"})),
            ("boot-0-sync.log", "echo MMX_PCIE_" + "a" * 32 + ":0\n"),
            ("subscriber-change-stop.log", "SUBSCRIBER_STOP_RC=143\n"),
            ("boot-0-final-health.log", "123 /eso/bin/apps/persistenceTNG_mibhigh -sm-name persistence_mibhigh -sm-id 104 -sm-time 8790 -sm-restart 1\n"),
            ("owned-host-1.json", json.dumps({"qemu_pid": 2147483646})),
            ("owned-host-1.json", json.dumps({"qemu_pid": "2147483645"})),
        ):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                self.experiment(directory)
                (directory / name).write_text(text)
                with self.assertRaises(ValueError):
                    assess(directory)

    def test_cli_failure_is_explicit_and_nonzero(self):
        with tempfile.TemporaryDirectory() as temp:
            process = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("native_persistence.py")),
                 "--directory", temp], capture_output=True, text=True)
            self.assertEqual(process.returncode, 1)
            self.assertFalse(json.loads(process.stderr)["verified"])
            self.assertEqual(process.stdout, "")

    def test_rcc_native_timeout_requires_actual_error_provider_and_rstp_waits(self):
        provider = "598043 /eso/bin/apps/persistenceTNG_mibhigh -sm-name persistence_mibhigh -sm-id 104 -sm-time 142160 -sm-restart 0\n"
        reply = ("RCC_READ_BEGIN\n"
                 "/eso/bin/dumb_persistence_reader: read failed: PERS_STATUS_TIMEOUT [12]\n"
                 "RCC_PERSISTENCE_READ_RC=1\nRCC_READ_END\n" + provider +
                 " 4 438294 rw 0 /dev/rstp/persistence\n"
                 "475156 1 persistencebackend 10r REPLY 438294\n"
                 "475156 11 persistencebackend 10r REPLY 438294\n")
        self.assertFalse(verify_rcc_timeout(reply)["persistence_value_verified"])
        for text in (reply.replace("RCC_PERSISTENCE_READ_RC=1", "RCC_PERSISTENCE_READ_RC=0"),
                     reply.replace("PERS_STATUS_TIMEOUT", "PERS_STATUS_FAILED"),
                     reply.replace("/dev/rstp/persistence", "/dev/null"),
                     reply.replace("REPLY 438294", "REPLY 123"),
                     reply + reply, "echo " + reply):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_rcc_timeout(text)

    def test_pending_qdb_is_not_proven_root_cause(self):
        reply = ("[n]@persis_qdb_BAT.cfg\nStatus::Valid\n"
                 "[n]@persis_qdb_DB.cfg\nStatus::AttachWait\n"
                 "[n]@persis_qdb_IMM.cfg\nStatus::Valid\n"
                 "[n]@persis_qdb_TMP.cfg\nStatus::Valid\n")
        self.assertFalse(verify_qdb_pending(reply)["cause_of_read_timeout_verified"])
        for text in (reply.replace("AttachWait", "Valid"),
                     reply.replace("Status::Valid", "Status::Invalid"),
                     reply + reply, "echo " + reply):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_qdb_pending(text)

    def test_qdb_ready_requires_original_attachment_names_and_every_valid_state(self):
        reply = "".join(f"[n]@{name}\nStatus::Valid\n" for name in
                        ("persisDB", "persisDB_BAT", "persisDB_IMM", "persisDB_TMP"))
        self.assertFalse(verify_qdb_ready(reply)["persistence_transactions_verified"])
        for text in (reply.replace("Status::Valid", "Status::AttachWait", 1),
                     reply + reply, reply.replace("persisDB_BAT", "persis_qdb_BAT.cfg"),
                     reply.replace("[n]@persisDB_TMP\nStatus::Valid\n", ""),
                     "echo " + reply):
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify_qdb_ready(text)

    def test_paired_rcc_assessment_is_blocked_even_with_valid_qdb(self):
        provider = "598043 /eso/bin/apps/persistenceTNG_mibhigh -sm-name persistence_mibhigh -sm-id 104 -sm-time 142160 -sm-restart 0\n"
        health = ("RCC_READ_BEGIN\n"
                  "/eso/bin/dumb_persistence_reader: read failed: PERS_STATUS_TIMEOUT [12]\n"
                  "RCC_PERSISTENCE_READ_RC=1\nRCC_READ_END\n" + provider +
                  " 4 438294 rw 0 /dev/rstp/persistence\n"
                  "475156 1 persistencebackend 10r REPLY 438294\n"
                  "475156 11 persistencebackend 10r REPLY 438294\n")
        qdb = "".join(f"[n]@{name}\nStatus::Valid\n" for name in
                      ("persisDB", "persisDB_BAT", "persisDB_IMM", "persisDB_TMP"))
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            for boot in range(2):
                (directory / f"boot-{boot}-read-health.log").write_text(health)
                (directory / f"boot-{boot}-manager.log").write_text(provider)
                (directory / f"boot-{boot}-rcc-health.log").write_text(qdb)
                (directory / f"owned-host-{boot}.json").write_text(json.dumps(
                    {"mmx_pid": 2147483646, "rcc_pid": 2147483645}))
                for core in range(2):
                    (directory / f"ipu1-core-{core}-fault-boot-{boot}.bin").write_bytes(bytes(20))
            (directory / "mmx-uart-wire.bin").write_bytes(b"native traffic")
            (directory / "rcc-uart-wire.bin").write_bytes(b"")
            result = assess_rcc_timeout(directory)
            self.assertEqual(result["status"], "blocked")
            self.assertFalse(result["native_rcc_persistence_verified"])
            self.assertEqual(result["boots"][0]["qdb"]["aggregate_database"], "Valid")
            with patch("native_persistence.Path.exists", return_value=True):
                with self.assertRaisesRegex(ValueError, "still exists"):
                    assess_rcc_timeout(directory)
            (directory / "rcc-uart-wire.bin").write_bytes(b"reply")
            with self.assertRaisesRegex(ValueError, "no RCC UART1"):
                assess_rcc_timeout(directory)
            (directory / "rcc-uart-wire.bin").write_bytes(b"")
            (directory / "ipu1-core-0-fault-boot-0.bin").write_bytes(b"\x01" + bytes(19))
            with self.assertRaisesRegex(ValueError, "fault-register"):
                assess_rcc_timeout(directory)

    def test_wrong_version_type_encoding_duplicates_and_factory_names_rejected(self):
        for options in ({"version": "wrong"}, {"profile": 7}, {"kind": 2},
                        {"payload": (424242).to_bytes(8, "little")},
                        {"payload": b""}, {"duplicate": True}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                verify_database(self.database(**options), "mh2p-persistence-development", 1, 424242)
        with self.assertRaises(ValueError):
            verify_database(self.database(), "FAZITID", 1, 424242)
        with self.assertRaises(ValueError):
            verify_database(b"not sqlite", "mh2p-persistence-development", 1, 424242)
