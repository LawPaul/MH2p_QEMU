# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from probe_trusted_os import (
    CONTEXT_ADDRESS, LOAD_ADDRESS, QNX_CONTEXT_ADDRESS, QNX_SHIM_ADDRESS,
    SECURE_SIZE, boot_context, bootstrap, qnx_bootstrap, qnx_boot_tags,
    run as run_probe, trusted_payload, trusted_driver_command, verify_trusted_driver,
)
from probe_rcc import QEMUMachine, ROOT
from test_rcc_ipu import ArmProgram


def container(payload):
    header = bytearray(0x200)
    header[:7] = b"NVTOSP\0"
    text = str(len(payload)).encode()
    header[7:7 + len(text)] = text
    struct.pack_into("<I", header, 28, len(payload))
    return header + payload + bytes(0x100)


class TrustedOSContainerTests(unittest.TestCase):
    def test_exact_payload_preserved(self):
        payload = struct.pack("<I", 0xeafffffe) * 4
        self.assertEqual(trusted_payload(container(payload)), payload)

    def test_invalid_headers_and_lengths_rejected(self):
        valid = container(bytes(16))
        bad_magic = bytearray(valid)
        bad_magic[0] = 0
        bad_decimal = bytearray(valid)
        bad_decimal[7] = ord("x")
        bad_size = bytearray(valid)
        struct.pack_into("<I", bad_size, 28, 12)
        for data in (b"", valid[:-1], valid + b"\0", bad_magic,
                     bad_decimal, bad_size, container(b""), container(b"abc")):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                trusted_payload(data)

    def test_bootstrap_literals_match_instruction_addresses(self):
        words = struct.unpack("<17I", bootstrap(CONTEXT_ADDRESS))
        self.assertEqual(words[12:15], (SECURE_SIZE, 0x80008028, LOAD_ADDRESS))
        self.assertEqual(words[15:], (CONTEXT_ADDRESS, 0x60006328))
        for instruction_index, register, literal_index in (
                (0, 3, 16), (4, 0, 12), (5, 1, 15), (7, 14, 13),
                (8, 3, 14)):
            instruction = words[instruction_index]
            self.assertEqual(instruction >> 12 & 15, register)
            self.assertEqual(
                instruction_index * 4 + 8 + (instruction & 0xfff),
                literal_index * 4)

    def test_context_sections_preserve_bytes_and_align_warmboot(self):
        for config in (b"", b"a", b"ab", b"abc", b"abcd"):
            with self.subTest(config=config):
                warmboot = b"\x01\x02\x03\x04"
                data = boot_context(config, warmboot)
                self.assertEqual(struct.unpack_from("<I", data)[0], len(config) + 1)
                self.assertEqual(data[4:5 + len(config)], config + b"\0")
                offset = 4 + ((len(config) + 4) & ~3)
                self.assertEqual(data[5 + len(config):offset],
                                 bytes(offset - 5 - len(config)))
                self.assertEqual(struct.unpack_from("<I", data, offset)[0], 4)
                self.assertEqual(data[offset + 4:], warmboot)

    def test_invalid_context_is_rejected(self):
        for config, warmboot in ((b"x\0y", b""), (b"a" * 65536, b""),
                                 (b"", bytes(65537)), (b"\xff", b"")):
            with self.subTest(length=len(config)), self.assertRaises(ValueError):
                boot_context(config, warmboot)

    def test_context_limit_includes_terminating_nul(self):
        data = boot_context(b"a" * 65535, bytes(65536))
        self.assertEqual(len(data), 2 * 65536 + 8)

    def test_qnx_bootstrap_literals_and_irq_loop(self):
        data = qnx_bootstrap(QNX_CONTEXT_ADDRESS, 0x82000000)
        words = struct.unpack(f"<{len(data) // 4}I", data)
        for index, register, literal, value in (
                (0, 3, 29, 0x60006328), (4, 0, 30, SECURE_SIZE),
                (5, 1, 31, QNX_CONTEXT_ADDRESS),
                (7, 14, 32, QNX_SHIM_ADDRESS + 40), (8, 3, 33, 0x8f000000),
                (11, 0, 34, 0x60004024), (16, 0, 35, 0x80000100),
                (17, 1, 36, QNX_SHIM_ADDRESS + 160),
                (18, 2, 37, len(qnx_boot_tags(256)) // 4),
                (25, 2, 38, 0x82000000), (26, 3, 39, 0x80008000)):
            self.assertEqual(words[index] >> 12 & 15, register)
            self.assertEqual(index * 4 + 8 + (words[index] & 0xfff), literal * 4)
            self.assertEqual(words[literal], value)
        self.assertEqual(words[12:16], (
            0xe3a02005, 0xe4801100, 0xe2522001, 0x1afffffc))
        self.assertEqual(words[19:23], (
            0xe4913004, 0xe4803004, 0xe2522001, 0x1afffffb))
        self.assertEqual(data[160:], qnx_boot_tags(256))
        self.assertGreaterEqual(QNX_CONTEXT_ADDRESS,
                                QNX_SHIM_ADDRESS + len(qnx_bootstrap(0, 0)))

    def test_qnx_memory_tags_exclude_secure_carveout(self):
        for memory in (256, 512, 1024, 2048):
            tags = qnx_boot_tags(memory)
            words = struct.unpack_from("<11I", tags)
            self.assertEqual(words[:5], (5, 0x54410001, 0, 0, 0))
            self.assertEqual(words[5:9], (
                4, 0x54410002, (memory - 16) * 1024 * 1024, 0x80000000))
            self.assertEqual(words[9] * 4 + 36 + 8, len(tags))
            self.assertIn(f"mem={memory - 16}M@2048M".encode(), tags[44:])
            self.assertEqual(tags[-8:], bytes(8))

    def test_invalid_normal_os_options_rejected_before_launch(self):
        for options in (
                {"normal_os_dtb": Path("dtb")},
                {"normal_os_command": "true"},
                {"normal_os_mnand": Path("mnand")},
                {"normal_os_nor": Path("nor")},
                {"normal_os_driver_probe": True},
                {"normal_os_driver_probe": True,
                 "normal_os_startup": Path("startup"),
                 "normal_os_mnand": Path("mnand"), "memory_mib": 256},
                {"memory_mib": 255}, {"memory_mib": 2049},
                {"seconds": 0}, {"seconds": 61}, {"seconds": float("nan")}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                run_probe(Path("qemu"), Path("tos"), Path("evidence"),
                          **({"seconds": 0.02} | options))
        with tempfile.TemporaryDirectory() as directory:
            startup = Path(directory) / "startup"
            for size in (0, QNX_SHIM_ADDRESS - 0x80008000 + 1):
                with startup.open("wb") as stream:
                    stream.truncate(size)
                with self.subTest(size=size), self.assertRaises(ValueError):
                    run_probe(Path("qemu"), Path("tos"), Path("evidence"), 0.02,
                              normal_os_startup=startup)

    def test_driver_probe_command_keeps_lines_bounded_and_failures_visible(self):
        setup = trusted_driver_command()
        self.assertIn("mount -tqnx6 -oro", setup)
        self.assertIn("waitfor /dev/tlkdr 5 &&\n", setup)
        self.assertIn("mount -t qnx4 /dev/diagss0t77 /mnt/ss &&\n", setup)
        self.assertIn("pidin syspage=asinfo &&\n", setup)
        self.assertLess(max(map(len, setup.splitlines())), 256)
        appended = trusted_driver_command("echo explicit-client-command")
        self.assertIn("&&\n(\necho explicit-client-command\n)", appended)
        for command in (setup, appended):
            completed = subprocess.run(
                ["sh", "-n"], input=command, text=True, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
        nor = trusted_driver_command("echo explicit-client-command",
                                     nor_storage=True)
        self.assertIn("/sbin/devf-generic-tegra2 ", nor)
        self.assertIn("waitfor /dev/fs0p3 5 &&\n", nor)
        self.assertIn("mount -o noatime,nosuid,noexec /dev/fs0p3 /mnt/ss &&\n",
                      nor)
        self.assertNotIn("devb-ram", nor)
        self.assertNotIn("diagss", nor)
        self.assertLess(max(map(len, nor.splitlines())), 256)
        completed = subprocess.run(
            ["sh", "-n"], input=nor, text=True, capture_output=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_driver_nor_rejects_truncated_capture_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nor = root / "short-nor"
            nor.write_bytes(b"short")
            with self.assertRaisesRegex(ValueError, "complete 64 MiB"):
                run_probe(Path("qemu"), Path("tos"), root / "output", 0.02,
                          normal_os_startup=root / "startup",
                          normal_os_driver_probe=True, normal_os_nor=nor)
            self.assertFalse((root / "output").exists())

    def test_driver_probe_requires_actual_node_process_and_allocator_boundary(self):
        prefix = "ifs image checksum is valid\n" * 6
        evidence = (
            "MH2P_TLK_DRIVER_EVIDENCE_BEGIN\n"
            "  122890 /sbin/devg-nvtlkdr /mnt/ss\n"
            "  0040) 0000000080000000-00000000beffffff o:0020 a:0017 "
            "p:100 n:/memory/below4G/ram\n"
            "  0280) 000000009d400000-00000000beffffff o:0040 a:0007 "
            "p:100 n:/memory/below4G/ram/sysram\n"
            "crw-rw----   1 root root 0, 1 Jan 01 00:00 /dev/tlkdr\n"
            "MH2P_TLK_DRIVER_EVIDENCE_END\n")
        valid = prefix + evidence
        result = verify_trusted_driver(valid.replace("\n", "\r\n").encode(),
                                       0xbf000000)
        self.assertTrue(result["original_driver_running"])
        self.assertTrue(result["normal_ram_exclusion_verified"])
        self.assertTrue(result["storage_is_empty_ephemeral_fixture"])
        self.assertFalse(result["functional_trusted_client_verified"])
        self.assertEqual(result["normal_ram_ranges_checked"], 2)
        with self.assertRaisesRegex(ValueError, "NOR secure-storage mount"):
            verify_trusted_driver(valid.encode(), 0xbf000000, nor_storage=True)
        with self.assertRaisesRegex(ValueError, "NOR secure-storage mount"):
            verify_trusted_driver(
                ("echo MH2P_TLK_NOR_STORAGE_MOUNTED\n" + valid).encode(),
                0xbf000000, nor_storage=True)
        nor_result = verify_trusted_driver(
            ("MH2P_TLK_NOR_STORAGE_MOUNTED\n" + valid).encode(),
            0xbf000000, nor_storage=True)
        self.assertFalse(nor_result["storage_is_empty_ephemeral_fixture"])
        self.assertTrue(nor_result["original_nor_storage_mount_observed"])
        self.assertFalse(nor_result["functional_trusted_client_verified"])
        for data in (
                evidence, valid + "ifs image checksum is valid\n",
                valid.replace("MH2P_TLK_DRIVER_EVIDENCE_END", ""),
                valid.replace("/sbin/devg-nvtlkdr /mnt/ss", "pidin ar"),
                valid.replace("crw-rw----", "-rw-rw----"),
                valid.replace("beffffff", "bfffffff"),
                valid.replace("/ram/sysram", "/ram/reserved"),
                valid.replace("beffffff", "bdffffff"),
                valid.replace("0080000000-", "00c0000000-"),
                valid.replace("MH2P_TLK_DRIVER_EVIDENCE_BEGIN\n",
                              "echo MH2P_TLK_DRIVER_EVIDENCE_BEGIN\n")):
            with self.subTest(data=data), self.assertRaises(ValueError):
                verify_trusted_driver(data.encode(), 0xbf000000)


class SecureCPUExecutionTests(unittest.TestCase):
    def test_qnx_handoff_abi_irqs_and_bidirectional_console(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        tos = ArmProgram()
        tos.words += [0xe28f0f7e, 0xee0c0f30]  # Relocatable MVBAR.
        for bank in range(5):
            tos.write(0x60004028 + bank * 0x100, 0xffffffff)
        tos.words += [0xe1600070, 0xe12fff1e]
        payload = tos.image()[0x100:]
        payload += bytes(0x208 - len(payload))
        payload += struct.pack("<4I", 0xee110f11, 0xe3800001,
                               0xee010f11, 0xe1b0f00e)
        startup = ArmProgram()
        startup.words += [0xe1a04000, 0xe1a05001, 0xe1a06002]
        for bank in range(5):
            startup.load(0, 0x60004020 + bank * 0x100)
            startup.words.append(0xe5900000 | ((7 + bank) << 12))
        startup.load(0, 0x8000011c)
        startup.words.append(0xe590c000)
        for char in b"mh2p# ":
            startup.write(0x70006040, char)
        receive = len(startup.words)
        startup.load(0, 0x70006054)
        startup.words += [0xe5901000, 0xe3110001]
        index = len(startup.words)
        startup.words.append(0x0a000000 | ((receive - index - 2) & 0xffffff))
        startup.load(0, 0x70006040)
        startup.words += [0xe5901000, 0xe351000a]
        index = len(startup.words)
        startup.words.append(0x1a000000 | ((receive - index - 2) & 0xffffff))
        for char in b"\nMH2P_TLK_COMMAND_RC:7\r\n":
            startup.write(0x70006040, char)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, kernel, mnand = root / "tos", root / "startup", root / "mnand,image"
            image.write_bytes(container(payload))
            kernel.write_bytes(startup.image()[0x100:])
            mnand.write_bytes(bytes(512))
            result = run_probe(
                qemu, image, root / "evidence", 0.5,
                normal_os_startup=kernel, normal_os_command="false",
                normal_os_mnand=mnand, memory_mib=512)
            self.assertTrue(result["trusted_entry_return_observed"])
            self.assertTrue(result["normal_os_shell_observed"])
            self.assertTrue(result["normal_os_command_completed"])
            self.assertEqual(result["normal_os_command_exit_status"], 7)
            self.assertFalse(result["trusted_services_ready"])
            self.assertTrue(result["normal_os_mnand_snapshot"])
            self.assertEqual(result["memory_mib"], 512)
            self.assertEqual(result["load_address"], "0x9f000000")
            self.assertEqual(mnand.read_bytes(), bytes(512))
            for register, value in ((4, 0), (5, 0), (6, 0x80000100),
                                    *((i, 0xffffffff) for i in range(7, 12)),
                                    (12, 496 * 1024 * 1024)):
                self.assertIn(f"R{register:02d}={value:08x}", result["registers"])
            self.assertEqual(json.loads(
                (root / "evidence/result.json").read_text()), result)
            silent_shell = ArmProgram()
            for char in b"mh2p# ":
                silent_shell.write(0x70006040, char)
            successful_command = kernel.read_bytes()
            status_literal = struct.pack("<I", ord("7"))
            self.assertEqual(successful_command.count(status_literal), 1)
            successful_command = successful_command.replace(
                status_literal, struct.pack("<I", ord("0")))
            nor = root / "synthetic-nor"
            with nor.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            nor_hash = hashlib.sha256(nor.read_bytes()).hexdigest()
            inspection = run_probe(
                qemu, image, root / "nor-inspection", 0.5,
                normal_os_startup=kernel, normal_os_command="false",
                normal_os_nor=nor, memory_mib=512)
            self.assertEqual(inspection["normal_os_command_exit_status"], 7)
            self.assertTrue(inspection["normal_os_nor_supplied"])
            self.assertFalse(inspection["normal_os_driver_probe_requested"])
            self.assertNotIn("normal_os_driver_probe_error", inspection)
            self.assertTrue(inspection["sensitive_cpu_snapshots_omitted"])
            self.assertEqual(inspection["normal_os_nor_sha256"], nor_hash)
            self.assertEqual((root / "nor-inspection").stat().st_mode & 0o077, 0)
            for field in ("registers", "stack_words", "pc_bytes"):
                self.assertNotIn(field, inspection)
            for name, startup_data, duration, expected_error in (
                    ("failed-command", kernel.read_bytes(), "0.5", None),
                    ("missing-driver-evidence", successful_command, "0.5", None),
                    ("missing-nor-mount", successful_command, "0.5", None),
                    ("no-shell", struct.pack("<I", 0xeafffffe), "0.02",
                     "QNX shell was not reached"),
                    ("command-timeout", silent_shell.image()[0x100:], "0.1",
                     "Command timed out")):
                kernel.write_bytes(startup_data)
                completed = subprocess.run(
                    [sys.executable, str(ROOT / "tools/mh2p/probe_trusted_os.py"),
                     "--qemu", str(qemu), "--image", str(image),
                     "--output", str(root / name), "--normal-os-startup", str(kernel),
                     "--normal-os-command", "false", "--seconds", duration,
                     *(["--normal-os-driver-probe", "--normal-os-mnand", str(mnand),
                        "--memory-mib", "512"]
                       if name in ("missing-driver-evidence",
                                   "missing-nor-mount") else []),
                     *(["--normal-os-nor", str(nor)]
                       if name == "missing-nor-mount" else [])],
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(completed.returncode, 1, completed.stderr)
                report = json.loads(completed.stdout)
                if expected_error is None:
                    self.assertEqual(
                        report["normal_os_command_exit_status"],
                        0 if name in ("missing-driver-evidence",
                                      "missing-nor-mount") else 7)
                    if name in ("missing-driver-evidence", "missing-nor-mount"):
                        self.assertIn("normal_os_driver_probe_error", report)
                        self.assertNotIn("normal_os_driver_probe", report)
                    if name == "missing-nor-mount":
                        self.assertIn("NOR secure-storage mount",
                                      report["normal_os_driver_probe_error"])
                        self.assertTrue(report["normal_os_nor_supplied"])
                        self.assertTrue(report["sensitive_cpu_snapshots_omitted"])
                        self.assertEqual(report["normal_os_nor_sha256"], nor_hash)
                        self.assertEqual(
                            hashlib.sha256(nor.read_bytes()).hexdigest(),
                            nor_hash)
                        self.assertEqual((root / name).stat().st_mode & 0o077, 0)
                        for field in ("registers", "stack_words", "pc_bytes"):
                            self.assertNotIn(field, report)
                else:
                    self.assertFalse(report["normal_os_command_completed"])
                    self.assertEqual(report["normal_os_command_error"], expected_error)
                self.assertEqual(json.loads(
                    (root / name / "result.json").read_text()), report)

    def test_secure_dram_carveout_cpu_and_dma_view(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        base, limit, result_base = 0x88000000, 0x88100000, 0x80020000
        host = ArmProgram()
        host.load(0, 0x80010000)
        host.words.append(0xee0c0f30)  # MVBAR.
        host.write(base, 0x11223344)
        host.write(limit - 4, 0x55667788)
        host.write(base - 4, 0xaabbccdd)
        host.write(limit, 0x12345678)
        host.write(base + 0x100, 0xe12fff1e)  # bx lr, executable secure RAM.
        host.write(0x70019070, base)
        host.write(0x70019074, 1)
        host.copy(0x70019070, result_base + 4)
        host.copy(0x70019074, result_base + 8)
        host.copy(base, result_base + 12)
        host.copy(limit - 4, result_base + 16)
        host.load(3, base + 0x100)
        host.words.append(0xe12fff33)  # blx r3.
        host.write(result_base + 20, 0x600d)
        host.load(10, 0)
        host.words.append(0xe1600070)  # Enter nonsecure SVC.
        host.load(0, 0x80011000)
        host.words.append(0xee0c0f10)  # Nonsecure VBAR.
        host.copy(base - 4, result_base + 24)
        host.copy(limit, result_base + 28)
        host.load(1, 0xcafe)
        host.copy(base, result_base + 32)
        host.write(base, 0xdeadbeef)
        host.copy(limit - 4, result_base + 36)
        host.write(limit - 4, 0xdeadbeef)
        host.write(0x70019070, 0)
        host.write(0x70019074, 0)
        host.copy(0x70019070, result_base + 40)
        host.copy(0x70019074, result_base + 44)
        host.load(0, result_base + 48)
        host.words.append(0xe580a000)  # Save data-abort count.
        host.load(3, base + 0x100)
        host.words.append(0xe12fff33)  # Protected nonsecure instruction fetch.
        resume_pc = 0x80008000 + len(host.words) * 4
        host.load(0, result_base + 52)
        host.words.append(0xe580a000)
        host.words.append(0xe1600070)  # Return to secure SVC.
        host.copy(base, result_base + 56)
        host.copy(limit - 4, result_base + 60)
        host.load(0, 0x80011000)
        host.words.append(0xee0c0f10)  # Secure VBAR for rejected MMIO writes.
        for i in range(4):
            host.write(0x70012320 + i * 4, 0x01020304 + i)
        host.write(0x7001231c, 0x200)
        host.write(0x70012014, 0x1000)
        host.write(0x70012304, 0x100)
        host.write(0x70012018, 0x80022000)
        host.write(0x70012024, 0x80022020)
        host.write(0x80022000, 0)
        host.write(0x80022004, base)
        host.write(0x80022008, 16)
        host.write(0x80022020, 0)
        host.write(0x80022024, 0x80022200)
        host.write(0x80022028, 16)
        host.write(0x80022200, 0x1234abcd)
        host.write(0x70012008, 1)
        host.copy(0x70012804, result_base + 64)
        host.copy(0x80022200, result_base + 68)
        host.write(0x80022004, 0x80022100)
        host.write(0x80022024, base)
        host.write(0x70012008, 1)
        host.copy(base, result_base + 72)
        host.write(0x70019070, base + 1)
        host.write(0x70019074, 0xffffffff)
        host.write(0x700199bc, 1)
        host.copy(0x70019070, result_base + 76)
        host.copy(0x70019074, result_base + 80)
        host.copy(0x700199bc, result_base + 84)
        host.load(0, result_base + 88)
        host.words.append(0xe580a000)
        host.write(0x70019070, limit)
        host.copy(0x70019070, result_base + 92)
        host.write(0x70019074, 0)
        host.copy(0x70019074, result_base + 96)
        host.write(0x70019070, base)
        host.write(0x70019074, 1)
        host.write(result_base, 0x600d600d)

        monitor = ArmProgram()
        monitor.words = [0xeafffffe, 0xeafffffe,
                         0xee110f11, 0xe2200001, 0xee010f11, 0xe1b0f00e]
        vectors = ArmProgram()
        vectors.words = [0xeafffffe] * 8
        vectors.words[3] = 0xea000002  # Prefetch abort handler at +0x1c.
        vectors.words[4] = 0xea000004  # Data abort handler at +0x28.
        vectors.words[7] = 0xe28aa001
        vectors.load(14, resume_pc)
        vectors.words.append(0xe1b0f00e)  # movs pc, lr.
        vectors.words += [0xe28aa001, 0xe25ef004]  # Skip faulting data access.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, handler, aborts = root / "startup", root / "monitor", root / "vectors"
            kernel.write_bytes(host.image()[0x100:])
            handler.write_bytes(monitor.image()[0x100:])
            aborts.write_bytes(vectors.image()[0x100:])
            args = [
                "-M", "mh2p,experimental-el3=on", "-m", "256M",
                "-kernel", str(kernel), "-S", "-display", "none", "-nic", "none",
                "-serial", "null", "-serial", "null",
                "-device", f"loader,file={handler},addr=0x80010000,force-raw=on",
                "-device", f"loader,file={aborts},addr=0x80011000,force-raw=on",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    reply = vm.cmd("human-monitor-command",
                                   command_line=f"xp /25wx {result_base:#x}")
                    values = [int(word, 16) for line in reply.splitlines()
                              for word in line.split(":")[1].split()]
                    if values[0] == 0x600d600d:
                        break
                    time.sleep(0.01)
                else:
                    registers = vm.cmd("human-monitor-command",
                                       command_line="info registers")
                    self.fail(f"Carveout guest did not finish: {reply}\n{registers}")
                vm.cmd("stop")
                self.assertEqual(values[1:8], [
                    base, 1, 0x11223344, 0x55667788, 0x600d,
                    0xaabbccdd, 0x12345678])
                self.assertEqual(values[8:14], [0xcafe, 0xdeadbeef, base, 1, 4, 5])
                self.assertEqual(values[14:16], [0x11223344, 0x55667788])
                self.assertEqual(values[16:], [
                    2, 0x1234abcd, 0x11223344, base, 1, 0, 10, limit, 0])
                denied = vm.cmd("human-monitor-command",
                                command_line=f"xp /1wx {base:#x}")
                self.assertIn("Cannot access memory", denied)
                vm.cmd("system_reset")
                reset = vm.cmd("human-monitor-command",
                               command_line="xp /2wx 0x70019070")
                self.assertEqual(
                    [int(word, 16) for line in reset.splitlines()
                     for word in line.split(":")[1].split()], [0, 0])
                restored = vm.cmd("human-monitor-command",
                                  command_line=f"xp /1wx {base:#x}")
                self.assertIn("0x11223344", restored)

    def test_tzram_data_cli_and_legacy_alias(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, config, data = root / "tos", root / "config", root / "data"
            image.write_bytes(container(struct.pack("<I", 0xeafffffe) * 4))
            config.write_bytes(b"[Global]\n")
            data.write_bytes(b"public boot data")
            for option in ("--tzram-data", "--warmboot"):
                with self.subTest(option=option):
                    output = root / option.removeprefix("--")
                    result = subprocess.run(
                        [sys.executable, str(ROOT / "tools/mh2p/probe_trusted_os.py"),
                         "--qemu", str(qemu), "--image", str(image),
                         "--output", str(output), "--boot-config", str(config),
                         option, str(data), "--seconds", "0.02"],
                        capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    report = json.loads(result.stdout)
                    self.assertEqual(report["tzram_data_bytes"], 16)
                    self.assertEqual(report["warmboot_bytes"], 16)
                    self.assertFalse(report["trusted_services_ready"])

    def test_explicit_boot_keys_reject_invalid_secrets(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel = root / "startup"
            kernel.write_bytes(struct.pack("<I", 0xeafffffe))
            for slot in (14, 15):
                for length in (0, 15, 17, 32, 63, 65):
                    with self.subTest(slot=slot, length=length):
                        key = root / "public-invalid-test-key"
                        key.write_bytes(b"x" * length)
                        result = subprocess.run(
                            [str(qemu), "-M", "mh2p", "-m", "256M",
                             "-kernel", str(kernel), "-display", "none",
                             "-object", f"secret,id=testkey,file={key}",
                             "-global",
                             f"tegra124-se.boot-slot{slot}-secret=testkey"],
                            capture_output=True, text=True, timeout=5)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(
                            f"boot-slot{slot}-secret requires 16 or 64 bytes",
                            result.stderr)
                result = subprocess.run(
                    [str(qemu), "-M", "mh2p", "-m", "256M",
                     "-kernel", str(kernel), "-display", "none",
                     "-global", f"tegra124-se.boot-slot{slot}-secret=missing"],
                    capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("missing", result.stderr)

    def test_probe_records_unverified_boot_keys_without_cpu_dumps(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "synthetic-tos"
            payload = struct.pack("<I", 0xeafffffe) * 4
            image.write_bytes(container(payload))
            key = root / "public,nist-key"
            key.write_bytes(bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c"))
            result = run_probe(
                qemu, image, root / "evidence", 0.02,
                boot_slot14_file=key, boot_slot15_file=key)
            self.assertEqual(result["boot_key_slots_supplied"], [14, 15])
            self.assertFalse(result["boot_key_material_hardware_verified"])
            self.assertFalse(result["trusted_services_ready"])
            self.assertFalse(result["signature_authenticated"])
            self.assertTrue(result["sensitive_cpu_snapshots_omitted"])
            self.assertNotIn("registers", result)
            self.assertNotIn("stack_words", result)
            self.assertNotIn("pc_bytes", result)
            self.assertEqual(trusted_payload(image.read_bytes()), payload)
            tzram_result = run_probe(
                qemu, image, root / "tzram-evidence", 0.02,
                config=b"[Global]\n", warmboot=b"public-tzram-fixture")
            self.assertEqual(tzram_result["boot_key_slots_supplied"], [])
            self.assertEqual(tzram_result["tzram_data_bytes"], 20)
            self.assertTrue(tzram_result["sensitive_cpu_snapshots_omitted"])
            for field in ("registers", "stack_words", "pc_bytes"):
                self.assertNotIn(field, tzram_result)
            self.assertFalse(tzram_result["trusted_services_ready"])

    def test_el3_rejects_psci_and_multiple_cpus(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        with tempfile.TemporaryDirectory() as directory:
            kernel = Path(directory) / "startup"
            kernel.write_bytes(struct.pack("<I", 0xeafffffe))
            for extra in (["-smp", "2"],
                          ["-M", "experimental-psci=on"]):
                with self.subTest(extra=extra):
                    result = subprocess.run(
                        [str(qemu), "-M", "mh2p,experimental-el3=on",
                         "-m", "256M", "-kernel", str(kernel),
                         "-display", "none", "-nic", "none", *extra],
                        capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        "experimental-el3 requires one CPU and "
                        "experimental-psci=off", result.stderr)

    def test_invalid_fuse_input_is_rejected(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        result = subprocess.run(
            [str(qemu), "-M", "mh2p,experimental-fuse-security-mode=invalid",
             "-display", "none"], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "experimental-fuse-security-mode requires unknown, "
            "development or production", result.stderr)

    def test_guest_monitor_round_trip_and_nonsecure_reset_vector_lock(self):
        qemu = Path(os.environ.get(
            "QTEST_QEMU_BINARY", ROOT / "build/qemu-system-arm")).resolve()
        if not qemu.is_file():
            self.skipTest("Build qemu-system-arm first")
        host = ArmProgram()
        host.load(0, 0x80010000)
        host.words.append(0xee0c0f30)  # MVBAR
        host.write(0x6000f100, 0x80030000)
        host.write(0x6000c200, 2)
        host.write(0x70012004, 0)
        host.write(0x7c010000, 0x11223344)
        host.words.append(0xe1600070)  # SMC #0
        host.write(0x6000f100, 0x80040000)
        host.write(0x6000c200, 0)
        host.write(0x70012004, 1)
        host.copy(0x70012004, 0x80020014)
        host.copy(0x6000f100, 0x80020008)
        host.copy(0x6000c200, 0x8002000c)
        host.words.append(0xe1600070)  # Return to secure SVC.
        host.write(0x6000f100, 0x80050000)
        host.copy(0x6000f100, 0x80020010)
        host.copy(0x7c010000, 0x80020018)
        host.write(0x80020000, 0x600d600d)
        monitor = ArmProgram()
        monitor.words += [0xeafffffe, 0xeafffffe]
        monitor.load(0, 0x80020004)
        monitor.words += [
            0xe10f1000, 0xe5801000,
            0xee110f11, 0xe2200001, 0xee010f11,  # Toggle SCR.NS.
            0xe1b0f00e,
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, handler = root / "startup", root / "monitor"
            kernel.write_bytes(host.image()[0x100:])
            handler.write_bytes(monitor.image()[0x100:])
            args = [
                "-M", "mh2p,experimental-el3=on", "-m", "256M",
                "-kernel", str(kernel), "-S", "-display", "none",
                "-nic", "none", "-serial", "null", "-serial", "null",
                "-device",
                f"loader,file={handler},addr=0x80010000,force-raw=on",
            ]
            with QEMUMachine(str(qemu), args=args) as vm:
                vm.launch()
                vm.cmd("cont")
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    reply = vm.cmd(
                        "human-monitor-command",
                        command_line="xp /7wx 0x80020000")
                    values = [word for line in reply.splitlines()
                              for word in line.split(":")[1].split()]
                    if int(values[0], 16) == 0x600d600d:
                        self.assertEqual(int(values[1], 16) & 31, 0x16)
                        self.assertEqual(
                            [int(word, 16) for word in values[2:]],
                            [0x80030000, 2, 0x80050000, 0, 0x11223344])
                        break
                    time.sleep(0.01)
                else:
                    self.fail(f"Secure monitor did not return: {reply}")


if __name__ == "__main__":
    unittest.main()
