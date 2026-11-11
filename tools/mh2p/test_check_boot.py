import unittest

from check_boot import (
    CLOCK_INIT_REGISTERS, GPU_EARLY_INIT_REGISTERS, GPU_RING_INIT_REGISTERS,
    SMMU_INIT_REGISTERS, check_one, verify_clock_init, verify_gpu_early_init,
    verify_gpu_ring_init, verify_hmi_startup, verify_normal_mnand,
    verify_runtime_services, verify_smmu_init,
)


class NormalMnandTests(unittest.TestCase):
    def log(self, count=6, ready=2840, blocks=49, size=35):
        return (
            b"ifs image checksum is valid\n" * count
            + f"time {ready} ms, [[3] wait for MNAND done]\n".encode()
            + f"mnandCounter = {blocks}, readsize: {size} MiB\n".encode()
        )

    def test_complete_normal_startup(self):
        self.assertEqual(verify_normal_mnand(self.log()), 2840)

    def test_exact_volume_count(self):
        for count in (0, 5, 7):
            with self.subTest(count=count), self.assertRaises(RuntimeError):
                verify_normal_mnand(self.log(count=count))

    def test_deadline_boundary(self):
        self.assertEqual(verify_normal_mnand(self.log(ready=4999)), 4999)
        for ready in (5000, 5001):
            with self.subTest(ready=ready), self.assertRaises(RuntimeError):
                verify_normal_mnand(self.log(ready=ready))

    def test_requires_actual_reads(self):
        for blocks, size in ((0, 35), (49, 0)):
            with self.subTest(blocks=blocks, size=size), self.assertRaises(RuntimeError):
                verify_normal_mnand(self.log(blocks=blocks, size=size))

    def test_missing_evidence(self):
        for line in (b"time ", b"mnandCounter"):
            log = b"\n".join(part for part in self.log().splitlines()
                             if not part.startswith(line))
            with self.subTest(line=line), self.assertRaises(RuntimeError):
                verify_normal_mnand(log)


class HmiStartupTests(unittest.TestCase):
    def lines(self):
        return [
            b"Starting 'LSD.main' thread with prio: 7\r\n",
            b"# HMI         : HMI_PAG_MIB2P_H_ER_G35_C28_212400\r\n",
            b"Jan 01 00:00:09 5 11474 0 MMX BENCH_Startup: StartupActivator done\r\n",
            b"Jan 01 00:00:09 5 11474 0 #FWP: "
            b"de.esolutions.fw.dsi.AdapterActivator started\r\n",
            b"  208912 /ifs/jre/bin/j9 -Xbootclasspath:/ifs/lsd.jxe de.dreisoft.lsd.LSD\r\n",
        ]

    def test_actual_framework_startup(self):
        result = verify_hmi_startup(b"".join(self.lines()))
        self.assertEqual(result["version"], "HMI_PAG_MIB2P_H_ER_G35_C28_212400")
        self.assertTrue(result["live_hmi_process_verified"])
        self.assertIn("not verified", result["note"])

    def test_each_required_record_is_unique(self):
        lines = self.lines()
        for i in range(len(lines)):
            for log in (b"".join(lines[:i] + lines[i + 1:]), b"".join(lines + [lines[i]])):
                with self.subTest(i=i, log=log), self.assertRaises(RuntimeError):
                    verify_hmi_startup(log)

    def test_echoes_wrong_client_and_wrong_firmware(self):
        good = b"".join(self.lines())
        for log in (
            good.replace(b"HMI_PAG_", b"HMI_AUDI_"),
            good.replace(b"/ifs/jre/bin/j9", b"/bin/ksh /ifs/jre/bin/j9"),
            good.replace(b"de.dreisoft.lsd.LSD\r\n", b"de.dreisoft.lsd.LSD-test\r\n"),
            b"mh2p# echo " + good.replace(b"\r\n", b" "),
            b"Starting runHMI.sh\r\n",
        ):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_hmi_startup(log)

    def test_echoed_individual_records_rejected(self):
        lines = self.lines()
        for i in range(len(lines)):
            modified = lines[:i] + [b"mh2p# echo " + lines[i]] + lines[i + 1:]
            with self.subTest(i=i), self.assertRaises(RuntimeError):
                verify_hmi_startup(b"".join(modified))

    def test_incompatible_probe_mode_rejected_before_launch(self):
        for shell, mnand, shift in ((False, "disk", 0), (True, None, 0),
                                    (True, "disk", None), (True, "disk", 1)):
            with self.subTest(shell=shell, mnand=mnand, shift=shift):
                with self.assertRaisesRegex(ValueError, "HMI startup requires"):
                    check_one(None, None, None, shell, mnand=mnand,
                              icount_shift=shift, hmi_startup=True)


class RuntimeServicesTests(unittest.TestCase):
    def test_live_services(self):
        verify_runtime_services(
            b"  pid Arguments\r\n  101 nvmemctrl\r\n"
            b"  102 /sbin/devg-nvclock\r\n  103 devg-nvpowergate -v\r\n"
            b"  104 /sbin/devg-nvsys\r\n"
            b"  105 devg-nvrm\r\n"
        )

    def test_requires_each_service(self):
        services = [b"nvmemctrl", b"devg-nvclock", b"devg-nvpowergate",
                    b"devg-nvsys", b"devg-nvrm"]
        for missing in services:
            log = b"\n".join(b" 100 " + name for name in services if name != missing)
            with self.subTest(missing=missing), self.assertRaises(RuntimeError):
                verify_runtime_services(log)

    def test_rejects_echoes_and_partial_names(self):
        for log in (
            b"mh2p# nvmemctrl && devg-nvclock && devg-nvpowergate && devg-nvsys\n",
            b"100 nvmemctrl-old\n101 devg-nvclock-old\n102 devg-nvpowergate-old\n"
            b"103 devg-nvsys-old\n",
            b"100 /bin/ksh nvmemctrl devg-nvclock devg-nvpowergate devg-nvsys\n",
        ):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_runtime_services(log)


    def test_resource_manager_requires_live_exact_name(self):
        other_services = (
            b"100 nvmemctrl\n101 devg-nvclock\n102 devg-nvpowergate\n"
            b"103 devg-nvsys\n"
        )
        for row in (b"104 devg-nvrm-old\n", b"104 /bin/ksh devg-nvrm\n",
                    b"mh2p# devg-nvrm\n"):
            with self.subTest(row=row), self.assertRaises(RuntimeError):
                verify_runtime_services(other_services + row)


class GpuEarlyInitTests(unittest.TestCase):
    def lines(self):
        return [f"{address:08x}: 0x{value:08x}\r\n"
                for address, value in GPU_EARLY_INIT_REGISTERS.items()]

    def test_native_configuration(self):
        result = verify_gpu_early_init("".join(self.lines()))
        self.assertEqual(result["0x57000000"], "0x0ea000a1")
        self.assertEqual(result["0x57001140"], "0x0000000e")

    def test_missing_duplicate_extra_or_echo(self):
        lines = self.lines()
        for log in ("", "".join(lines[:-1]), "".join(lines + lines[:1]),
                    "".join(lines) + "57001100: 0x00000000\n",
                    "(qemu) " + " ".join(line.strip() for line in lines)):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_gpu_early_init(log)

    def test_wrong_identity_reset_or_incomplete_setup(self):
        good = "".join(self.lines())
        for before, after in (
            ("0x0ea000a1", "0x12b000a1"),
            ("0x0000000e", "0x00000000"),
            ("0x00000020", "0x00000000"),
            ("7000e6d4: 0x00000000", "7000e6d4: 0x00000001"),
        ):
            with self.subTest(before=before), self.assertRaises(RuntimeError):
                verify_gpu_early_init(good.replace(before, after))


class GpuRingInitTests(unittest.TestCase):
    def lines(self):
        return [f"{address:08x}: 0x{value:08x}\r\n"
                for address, value in GPU_RING_INIT_REGISTERS.items()]

    def test_native_configuration(self):
        result = verify_gpu_ring_init("".join(self.lines()))
        self.assertEqual(len(result), 7)
        self.assertEqual(result["0x57120050"], "0x00000001")
        self.assertEqual(result["0x57009080"], "0x800186a0")

    def test_missing_duplicate_extra_or_echo(self):
        lines = self.lines()
        for log in ("", "".join(lines[:-1]), "".join(lines + lines[:1]),
                    "".join(lines) + "57120058: 0x00000000\n",
                    "(qemu) " + " ".join(line.strip() for line in lines)):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_gpu_ring_init(log)

    def test_busy_reset_or_incomplete_configuration(self):
        good = "".join(self.lines())
        for address, value in GPU_RING_INIT_REGISTERS.items():
            wrong = 4 if value == 0 else 0
            bad = good.replace(f"{address:08x}: 0x{value:08x}",
                               f"{address:08x}: 0x{wrong:08x}")
            with self.subTest(address=address), self.assertRaises(RuntimeError):
                verify_gpu_ring_init(bad)


class ClockInitTests(unittest.TestCase):
    def lines(self):
        return [
            f"0x{address:08x} : 0x{value:08x}\r\n".encode()
            for address, value in CLOCK_INIT_REGISTERS.items()
        ]

    def test_expected_registers(self):
        result = verify_clock_init(b"".join(self.lines()))
        self.assertEqual(result["0x600060c0"], "0x4803c00c")

    def test_missing_or_duplicate_register(self):
        lines = self.lines()
        for log in (b"", b"".join(lines[:-1]), b"".join(lines + lines[:1])):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_clock_init(log)

    def test_bad_state_and_echo(self):
        good = b"".join(self.lines())
        for log in (good.replace(b"0x00080000", b"0x00000000"),
                    good.replace(b"0x4803c00c", b"0x4003c00c"),
                    b"mh2p# echo " + good.replace(b"\r\n", b" ")):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_clock_init(log)


class SmmuInitTests(unittest.TestCase):
    def lines(self):
        return [f"{address:08x}: 0x{value:08x}\r\n"
                for address, value in SMMU_INIT_REGISTERS.items()]

    def test_native_configuration(self):
        result = verify_smmu_init("".join(self.lines()))
        self.assertEqual(result["0x70019010"], "0x00000001")

    def test_missing_duplicate_or_reset_state(self):
        lines = self.lines()
        for log in ("", "".join(lines[:-1]), "".join(lines + lines[:1]),
                    "".join(lines).replace("0x00000001", "0x00000000"),
                    "".join(lines).replace("0x30000020", "0x20000000"),
                    "echo " + "".join(lines).replace("\r\n", " ")):
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                verify_smmu_init(log)


if __name__ == "__main__":
    unittest.main()
