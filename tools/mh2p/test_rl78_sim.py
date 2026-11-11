# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic CPU tests for the opt-in, locally built GNU RL78 diagnostic."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


SIMULATOR = os.environ.get("MH2P_RL78_SIM")
FLAGS = (
    "MH2P_DIAGNOSTIC_ERASED_DATA_FLASH",
    "MH2P_DIAGNOSTIC_PFDL_API",
    "MH2P_DIAGNOSTIC_F13_PIOR",
    "MH2P_DIAGNOSTIC_F13_CLOCK",
    "MH2P_DIAGNOSTIC_F13_TAU",
    "MH2P_DIAGNOSTIC_F13_IRQ",
    "MH2P_DIAGNOSTIC_F13_PORTS",
    "MH2P_DIAGNOSTIC_F13_TRD",
)
PIOR_MASKS = {
    0xf0016: 0xff,
    0xf0017: 0xff,
    0xf001a: 0x7f,
    0xf001b: 0x0d,
    0xf001d: 0x0b,
}
PORT_CONFIG_MASKS = {
    0xf0023: 0x01, 0xf0024: 0x08, 0xf0025: 0x1d,
    0xf0026: 0x0f, 0xf0027: 0xeb,
    0xf0033: 0x07, 0xf0034: 0xff, 0xf0035: 0xff,
    0xf0036: 0xff, 0xf0037: 0xff,
    0xf0043: 0x01, 0xf0045: 0x10, 0xf0046: 0x0c, 0xf0047: 0x0b,
    0xf0056: 0x0f, 0xf0057: 0x07,
}


def write_byte(address, value):
    return b"\xcf" + struct.pack("<HB", address & 0xffff, value)


def expect_byte(address, value):
    # On mismatch, the original CPU executes the deliberately unsupported write.
    return (b"\x8f" + struct.pack("<H", address & 0xffff)
            + b"\x4c" + bytes([value]) + b"\xdd\x04"
            + write_byte(0xf0001, 0))


def write_word(address, value):
    return b"\x30" + struct.pack("<H", value) + b"\xbf" + struct.pack(
        "<H", address & 0xffff)


def expect_word(address, value):
    fail = write_byte(0xf0001, 0)
    return (b"\xaf" + struct.pack("<H", address & 0xffff)
            + b"\x4c" + bytes([value >> 8]) + b"\xdd\x04" + fail
            + b"\x60\x4c" + bytes([value & 0xff]) + b"\xdd\x04" + fail)


def synthetic_elf(code):
    entry = 0xd8
    offset = 0x1000
    payload = bytes(entry) + b"\x61\xcf" + code
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    elf = bytearray(offset)
    struct.pack_into("<16sHHIIIIIHHHHHH", elf, 0, ident, 2, 197, 1,
                     entry, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", elf, 52, 1, offset, 0, 0,
                     len(payload), len(payload), 5, 0x1000)
    return elf + payload


def api_program(frequency=8, voltage=0):
    code = (b"\xcb\xf8\x20\xfe"
            + write_byte(0xf0090, 1)
            + write_byte(0xffe80, frequency)
            + write_byte(0xffe81, voltage)
            + write_byte(0xf0804, 0x80)
            + write_byte(0xf0805, 0xfe)
            + write_byte(0xf0803, 1)
            + b"\xf2")
    code += bytes(0x11d2 - 0xda - len(code))
    code += b"\xfc\xf8\xff\x0e"
    code += expect_byte(0xffff8, 0x20) + expect_byte(0xffff9, 0xfe)
    code += expect_byte(0xffe1c, 0xd6)
    code += bytes(0x124a - 0xda - len(code))
    return (code + b"\xfc\x08\xf0\x0e"
            + expect_byte(0xffff8, 0x20) + expect_byte(0xffff9, 0xfe)
            + write_byte(0xf0002, 0))


@unittest.skipUnless(SIMULATOR, "Set MH2P_RL78_SIM to the patched GNU runner")
class Rl78SimTests(unittest.TestCase):
    def run_program(self, code, flags=()):
        env = dict(os.environ)
        for flag in FLAGS:
            env.pop(flag, None)
        for flag in flags:
            env[flag] = "1"
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "synthetic.elf"
            image.write_bytes(synthetic_elf(code))
            result = subprocess.run(
                [SIMULATOR, str(image)], env=env, capture_output=True,
                text=True, timeout=5)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result.stderr

    def test_timer_rd_stopped_start_control_reset_and_readback(self):
        flags = ("MH2P_DIAGNOSTIC_F13_CLOCK", "MH2P_DIAGNOSTIC_F13_TRD")
        code = write_byte(0xf02c0, 0x10) + expect_byte(0xf0263, 0x0c)
        for value in (0x0c, 0x08, 0x04, 0):
            code += write_byte(0xf0263, value) + expect_byte(0xf0263, value)
        log = self.run_program(code + write_byte(0xf0002, 0), flags)
        self.assertIn("0xf0002", log)

    def test_timer_rd_pwm_delay_reset_and_word_retention(self):
        flags = ("MH2P_DIAGNOSTIC_F13_CLOCK", "MH2P_DIAGNOSTIC_F13_TRD")
        code = write_byte(0xf02c0, 0x10) + expect_word(0xf0228, 0)
        for value in (0x0001, 0x0080, 0x0100, 0xaaaa, 0x5555, 0xffff, 0):
            code += write_word(0xf0228, value) + expect_word(0xf0228, value)
        log = self.run_program(code + write_byte(0xf0002, 0), flags)
        self.assertIn("0xf0002", log)
        self.assertIn("PWMDLY0 retention enabled", log)

    def test_timer_rd_pwm_delay_rejects_byte_bit_and_overlapping_words(self):
        flags = ("MH2P_DIAGNOSTIC_F13_CLOCK", "MH2P_DIAGNOSTIC_F13_TRD")
        for address in (0xf0228, 0xf0229):
            for code in (write_byte(address, 0), expect_byte(address, 0),
                         b"\x71\x00" + struct.pack("<H", address & 0xffff)):
                with self.subTest(address=address, code=code):
                    log = self.run_program(write_byte(0xf02c0, 0x10) + code, flags)
                    self.assertIn("word-only F13 PWMDLY0", log)
        for address in (0xf0227, 0xf0229):
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=address, code=code):
                    log = self.run_program(write_byte(0xf02c0, 0x10) + code, flags)
                    self.assertIn("overlapping word access to F13 PWMDLY0", log)

    def test_timer_rd_pwm_delay_requires_clock_and_opt_in(self):
        flags = ("MH2P_DIAGNOSTIC_F13_CLOCK", "MH2P_DIAGNOSTIC_F13_TRD")
        for code in (write_word(0xf0228, 0), expect_word(0xf0228, 0)):
            with self.subTest(code=code):
                log = self.run_program(code, flags)
                self.assertIn("Timer RD clock disabled", log)
                log = self.run_program(write_byte(0xf02c0, 0x10) + code,
                                       ("MH2P_DIAGNOSTIC_F13_CLOCK",))
                self.assertIn("Unmodeled MH2P SubCPU peripheral", log)

    def test_timer_rd_rejects_start_reserved_bits_and_word_accesses(self):
        flags = ("MH2P_DIAGNOSTIC_F13_CLOCK", "MH2P_DIAGNOSTIC_F13_TRD")
        for value in (1, 2, 3, 0x0d, 0x10, 0x80, 0xff):
            with self.subTest(value=value):
                log = self.run_program(write_byte(0xf02c0, 0x10) +
                                       write_byte(0xf0263, value), flags)
                self.assertIn("Timer RD start or reserved bits", log)
        for address in (0xf0262, 0xf0263):
            for opcode in (write_word(address, 0),
                           b"\xaf" + struct.pack("<H", address & 0xffff)):
                with self.subTest(address=address, opcode=opcode):
                    log = self.run_program(write_byte(0xf02c0, 0x10) + opcode, flags)
                    self.assertIn("byte-only F13 Timer RD", log)

    def test_timer_rd_requires_opt_in_clock_and_rejects_neighbors(self):
        flag = "MH2P_DIAGNOSTIC_F13_TRD"
        log = self.run_program(expect_byte(0xf0263, 0x0c), (flag,))
        self.assertIn("requires explicit F13 clock", log)
        log = self.run_program(expect_byte(0xf0263, 0x0c),
                               (flag, "MH2P_DIAGNOSTIC_F13_CLOCK"))
        self.assertIn("Timer RD clock disabled", log)
        for address in (0xf0262, 0xf0263, 0xf0264):
            flags = ("MH2P_DIAGNOSTIC_F13_CLOCK",) if address == 0xf0263 else (
                flag, "MH2P_DIAGNOSTIC_F13_CLOCK")
            with self.subTest(address=address):
                log = self.run_program(write_byte(0xf02c0, 0x10) +
                                       expect_byte(address, 0), flags)
                self.assertIn(f"peripheral read at {address:#x}", log)

    def test_pior_reset_values(self):
        code = b"".join(expect_byte(address, 0) for address in PIOR_MASKS)
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[2],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_pior_writable_masks_and_readback(self):
        code = b"".join(
            write_byte(address, 0xff) + expect_byte(address, mask)
            for address, mask in PIOR_MASKS.items())
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[2],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)
        for address in (0xf001a, 0xf001b, 0xf001d):
            self.assertIn(f"Ignoring read-only PIOR bits at 0x{address:05x}",
                          log)

    def test_pior_retains_each_written_value(self):
        code = b"".join(
            write_byte(address, value) + expect_byte(address, value)
            for address, value in (
                (0xf0016, 0x40), (0xf0017, 0x10), (0xf001a, 1),
                (0xf001b, 0x0c), (0xf001d, 1)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[2],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_pior_is_not_enabled_by_other_fixtures(self):
        for flags in ((), (FLAGS[0],), FLAGS[:2]):
            with self.subTest(flags=flags):
                log = self.run_program(write_byte(0xf0016, 0x40), flags)
                self.assertIn("peripheral write at 0xf0016", log)

    def test_pior_does_not_enable_adjacent_registers(self):
        for address in (0xf0018, 0xf0019, 0xf001c, 0xf001e, 0xf00f3):
            with self.subTest(address=hex(address)):
                log = self.run_program(write_byte(address, 0), (FLAGS[2],))
                self.assertIn(f"peripheral write at 0x{address:05x}", log)

    def test_erased_flash_read_requires_enable(self):
        log = self.run_program(expect_byte(0xf1000, 0xff), (FLAGS[0],))
        self.assertIn("Data-flash read while DFLCTL disabled", log)

    def test_erased_flash_reads_only_after_enable(self):
        code = (write_byte(0xf0090, 1) + expect_byte(0xf1000, 0xff)
                + write_byte(0xf0002, 0))
        log = self.run_program(code, (FLAGS[0],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_erased_flash_never_accepts_writes(self):
        for flags in ((FLAGS[0],), FLAGS):
            with self.subTest(flags=flags):
                log = self.run_program(
                    write_byte(0xf0090, 1) + write_byte(0xf1000, 0x5a),
                    flags)
                self.assertIn("peripheral write at 0xf1000", log)

    def test_strict_flash_read_rejected_before_rom_mirroring(self):
        log = self.run_program(
            write_byte(0xf0090, 1) + expect_byte(0xf1000, 0xff))
        self.assertIn("peripheral read at 0xf1000", log)

    def test_api_requires_explicit_erased_flash(self):
        log = self.run_program(write_byte(0xf0002, 0), (FLAGS[1],))
        self.assertIn("requires explicit erased read-only data-flash fixture",
                      log)

    def test_api_rejects_unknown_call_site(self):
        # This call has no original PFDL wrapper or descriptor.
        log = self.run_program(b"\xfc\xf8\xff\x0e", FLAGS[:2])
        self.assertIn("Unsupported diagnostic PFDL API call 0xefff8", log)

    def test_api_parameter_read_must_be_initialized(self):
        log = self.run_program(expect_byte(0xf0804, 0), FLAGS[:2])
        self.assertIn("Uninitialized diagnostic PFDL parameter read", log)

    def test_api_open_close_preserves_call_return_stack(self):
        log = self.run_program(api_program(), FLAGS[:2])
        self.assertIn("Diagnostic PFDL API open:", log)
        self.assertIn("Diagnostic PFDL API close at", log)
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_api_rejects_unmodeled_descriptors(self):
        for frequency, voltage in ((0, 0), (9, 0), (8, 1)):
            with self.subTest(frequency=frequency, voltage=voltage):
                log = self.run_program(
                    api_program(frequency, voltage), FLAGS[:2])
                self.assertIn("Unsupported diagnostic PFDL descriptor", log)
                self.assertNotIn("Diagnostic PFDL API open:", log)

    def test_clock_resets(self):
        code = b"".join(
            expect_byte(address, reset) for address, reset in (
                (0xf00f3, 0), (0xf02c4, 0), (0xf02c5, 0),
                (0xfffa0, 0), (0xfffa1, 0xc0), (0xfffa4, 0),
                (0xf02c2, 0), (0xf02c3, 0), (0xf02c8, 0),
                (0xf00f0, 0), (0xf02c0, 0), (0xf02c1, 0)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[3],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_clock_configuration_retention(self):
        code = b"".join(
            write_byte(address, value) + expect_byte(address, value)
            for address, value in (
                (0xf00f3, 0x80), (0xf02c4, 0), (0xfffa0, 0x30),
                (0xfffa1, 0x80), (0xfffa4, 0), (0xf02c5, 0),
                (0xf02c2, 0), (0xf02c3, 0x10), (0xf02c8, 0),
                (0xf00a0, 0x15), (0xf00a8, 0), (0xf02c7, 0),
                (0xf00f0, 0xa5), (0xf02c0, 0x11), (0xf02c1, 4)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[3],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_clock_status_write_does_not_create_readiness(self):
        code = (write_byte(0xfffa4, 0xa0) + expect_byte(0xfffa4, 0)
                + write_byte(0xf0002, 0))
        log = self.run_program(code, (FLAGS[3],))
        self.assertIn("Ignoring read-only CKC status write", log)
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_clock_reserved_bits_rejected(self):
        for address, value in (
                (0xf00f3, 1), (0xf02c4, 2), (0xf02c5, 8),
                (0xfffa0, 8), (0xfffa1, 2), (0xfffa4, 1),
                (0xf02c2, 2), (0xf02c3, 4), (0xf02c8, 4),
                (0xf00a0, 0x40), (0xf00a8, 8), (0xf02c7, 8),
                (0xf00f0, 0x40), (0xf02c0, 2), (0xf02c1, 2)):
            with self.subTest(address=hex(address)):
                log = self.run_program(write_byte(address, value), (FLAGS[3],))
                self.assertIn("Reserved F13 clock bits", log)

    def test_cmc_is_write_once(self):
        log = self.run_program(
            write_byte(0xfffa0, 0) + write_byte(0xfffa0, 0),
            (FLAGS[3],))
        self.assertIn("Unsupported CMC rewrite or prohibited gain", log)

    def test_cmc_prohibited_gain_rejected(self):
        log = self.run_program(write_byte(0xfffa0, 6), (FLAGS[3],))
        self.assertIn("Unsupported CMC rewrite or prohibited gain", log)

    def test_csc_requires_cmc_and_cannot_stop_cpu_clock(self):
        for code in (write_byte(0xfffa1, 0x80),
                     write_byte(0xfffa0, 0) + write_byte(0xfffa1, 0xc1)):
            with self.subTest(code=code.hex()):
                log = self.run_program(code, (FLAGS[3],))
                self.assertIn("Unsupported CSC request", log)

    def test_cpu_clock_switch_is_not_invented(self):
        for value in (0x10, 0x40, 0x50):
            with self.subTest(value=value):
                log = self.run_program(write_byte(0xfffa4, value), (FLAGS[3],))
                self.assertIn("Unmodeled F13 CPU clock switch", log)

    def test_pll_start_and_switch_are_not_invented(self):
        for value in (1, 4, 5, 0xc0):
            with self.subTest(value=value):
                log = self.run_program(write_byte(0xf02c5, value), (FLAGS[3],))
                self.assertIn("Unsupported PLL start/switch", log)

    def test_clock_fixture_never_enables_status_registers(self):
        for address in (0xfffa2, 0xf02c6):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), (FLAGS[3],))
                self.assertIn(f"peripheral read at 0x{address:05x}", log)

    def test_clock_fixture_disabled_by_default(self):
        log = self.run_program(write_byte(0xf00f3, 0x80), FLAGS[:3])
        self.assertIn("peripheral write at 0xf00f3", log)

    def test_unknown_clock_reset_values_are_never_invented(self):
        for address in (0xf00a0, 0xf00a8, 0xf02c7):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), (FLAGS[3],))
                self.assertIn(
                    "Unknown chip/option-dependent F13 clock reset read", log)
                self.assertIn(f"0x{address:05x}", log)

    def test_unsupported_clock_divisors_rejected(self):
        for address, value in ((0xf00a8, 4), (0xf00a8, 5), (0xf02c7, 7)):
            with self.subTest(address=hex(address), value=value):
                log = self.run_program(write_byte(address, value), (FLAGS[3],))
                self.assertIn("Unsupported F13 clock divider", log)

    def test_clock_word_access_rejected(self):
        for opcode, operation in ((b"\xbf", "write"), (b"\xaf", "read")):
            with self.subTest(operation=operation):
                log = self.run_program(
                    opcode + b"\xa0\xff", (FLAGS[3],))
                self.assertIn(f"Unsupported word {operation}", log)

    def test_clock_enable_does_not_enable_unmodeled_peripherals(self):
        code = write_byte(0xf00f0, 0xa5) + write_byte(0xf01b6, 0)
        log = self.run_program(code, (FLAGS[3],))
        self.assertIn("peripheral write at 0xf01b6", log)

    def test_tau_requires_clock_diagnostic(self):
        log = self.run_program(write_byte(0xf0002, 0), (FLAGS[4],))
        self.assertIn("TAU0 diagnostic requires explicit", log)

    def test_tau_requires_peripheral_clock(self):
        for code in (write_word(0xf01b6, 0x83), expect_word(0xf01b6, 0)):
            with self.subTest(code=code.hex()):
                log = self.run_program(code, FLAGS[3:])
                self.assertIn("TAU0 configuration while PER0.TAU0EN", log)

    def test_tau_reset_and_prescaler_retention(self):
        code = (write_byte(0xf00f0, 1)
                + expect_word(0xf01b6, 0) + expect_word(0xf022a, 0)
                + write_word(0xf01b6, 0x83) + expect_word(0xf01b6, 0x83)
                + write_word(0xf01b6, 0xabcd) + expect_word(0xf01b6, 0xabcd)
                + write_byte(0xf0002, 0))
        log = self.run_program(code, FLAGS[3:])
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_tau_stop_trigger_self_clears(self):
        code = (write_byte(0xf00f0, 1) + write_word(0xf01b4, 0xafff)
                + expect_word(0xf01b4, 0) + write_byte(0xf0002, 0))
        log = self.run_program(code, FLAGS[3:])
        self.assertIn("Ignoring reserved TAU0 write bits at 0xf01b4", log)
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_tau_pwm_delay_reserved_mask(self):
        code = (write_byte(0xf00f0, 1) + write_word(0xf022a, 0xffff)
                + expect_word(0xf022a, 0xfffc) + write_byte(0xf0002, 0))
        log = self.run_program(code, FLAGS[3:])
        self.assertIn("Ignoring reserved TAU0 write bits at 0xf022a", log)
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_tau_disable_after_configuration_is_not_guessed(self):
        code = (write_byte(0xf00f0, 1) + write_word(0xf01b6, 0x83)
                + write_word(0xf022a, 0x1234) + write_byte(0xf00f0, 0))
        log = self.run_program(code, FLAGS[3:])
        self.assertIn("Unsupported TAU0 clock disable after configuration", log)

    def test_tau_diagnostic_only_implements_word_access(self):
        for address in (0xf01b4, 0xf01b5, 0xf01b6, 0xf01b7,
                        0xf022a, 0xf022b):
            with self.subTest(address=hex(address)):
                code = write_byte(0xf00f0, 1) + write_byte(address, 0)
                log = self.run_program(code, FLAGS[3:])
                self.assertIn("Unsupported byte write", log)

    def test_tau_never_invents_timer_start_or_counter(self):
        for code, fault in (
                (write_word(0xf01b2, 1), "write at 0xf01b2"),
                (expect_word(0xf0180, 0), "read at 0xf0180")):
            with self.subTest(fault=fault):
                log = self.run_program(write_byte(0xf00f0, 1) + code,
                                       FLAGS[3:])
                self.assertIn("peripheral " + fault, log)

    def test_tau_input_selector_reset_and_common_sources(self):
        code = expect_byte(0xf0074, 0)
        code += b"".join(
            write_byte(0xf0074, value) + expect_byte(0xf0074, value)
            for value in (2, 3, 4, 5, 0))
        log = self.run_program(code + write_byte(0xf0002, 0), FLAGS[3:5])
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_tau_noise_filter_reset_and_retention_without_clock(self):
        code = expect_byte(0xf0071, 0)
        code += b"".join(
            write_byte(0xf0071, value) + expect_byte(0xf0071, value)
            for value in (1, 0x55, 0xaa, 0xff, 0))
        code += expect_byte(0xfffe1, 0) + expect_byte(0xfffe2, 0)
        log = self.run_program(code + write_byte(0xf0002, 0),
                               (FLAGS[3], FLAGS[4], FLAGS[5]))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_tau_noise_filter_rejects_words_neighbors_and_default_access(self):
        for address in (0xf0070, 0xf0071):
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=hex(address), code=code):
                    log = self.run_program(code, FLAGS[3:5])
                    self.assertIn("Unsupported word access to F13 TAU0", log)
        for address in (0xf0070, 0xf0072):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), FLAGS[3:5])
                self.assertIn(f"peripheral read at 0x{address:05x}", log)
        for code, operation in ((write_byte(0xf0071, 0), "write"),
                                (expect_byte(0xf0071, 0), "read")):
            with self.subTest(operation=operation):
                log = self.run_program(code)
                self.assertIn(f"peripheral {operation} at 0xf0071", log)

    def test_tau_input_selector_rejects_unknown_sources_and_word_access(self):
        for value in (1, 6, 7, 8, 0x10, 0x20, 0x40, 0x80, 0xff):
            with self.subTest(value=value):
                log = self.run_program(write_byte(0xf0074, value),
                                       FLAGS[3:5])
                self.assertIn("Unsupported F13 TIS0 selection", log)
        for address in (0xf0073, 0xf0074):
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=hex(address), code=code):
                    log = self.run_program(code, FLAGS[3:5])
                    self.assertIn("Unsupported word access to F13 TAU0", log)

    def test_tau_channel0_stopped_configuration_and_output_latch(self):
        code = write_byte(0xf00f0, 1)
        code += b"".join(expect_word(address, 0) for address in (
            0xf0190, 0xf01b8, 0xf01ba))
        code += b"".join(
            write_word(0xf0190, value) + expect_word(0xf0190, value)
            for value in (0x0104, 0x020c, 0x1006, 0xc000, 0))
        code += write_word(0xf0190, 0x0904) + expect_word(0xf0190, 0x0104)
        code += write_word(0xf01b8, 0xa5) + expect_word(0xf01b8, 0xa5)
        code += write_word(0xf01ba, 0) + expect_word(0xf01ba, 0)
        log = self.run_program(code + write_byte(0xf0002, 0), FLAGS[3:5])
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)
        self.assertIn("Ignoring fixed F13 TMR00 bit 11", log)

    def test_tau_channel0_rejects_prohibited_config_and_waveforms(self):
        for value in (0x2000, 0x10, 0x20, 0x0304, 0x0404,
                      2, 3, 7, 10, 11, 13, 14, 15):
            with self.subTest(value=value):
                log = self.run_program(write_byte(0xf00f0, 1)
                                       + write_word(0xf0190, value),
                                       FLAGS[3:5])
                self.assertIn("Prohibited F13 TMR00 configuration", log)
        for address, value, error in (
                (0xf01b8, 0x100, "Reserved F13 TAU0 output bits"),
                (0xf01ba, 0x100, "Reserved F13 TAU0 output bits"),
                (0xf01ba, 1, "waveform output enable"),
                (0xf01ba, 0xff, "waveform output enable")):
            with self.subTest(address=hex(address), value=value):
                log = self.run_program(write_byte(0xf00f0, 1)
                                       + write_word(address, value),
                                       FLAGS[3:5])
                self.assertIn(error, log)

    def test_tau_new_registers_require_clock_and_reject_partial_words(self):
        for address in (0xf0190, 0xf01b8, 0xf01ba):
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=hex(address), code=code):
                    log = self.run_program(code, FLAGS[3:5])
                    self.assertIn("PER0.TAU0EN is disabled", log)
            for byte_address in (address, address + 1):
                for code, operation in (
                        (write_byte(byte_address, 0), "write"),
                        (expect_byte(byte_address, 0), "read")):
                    with self.subTest(address=hex(byte_address)):
                        log = self.run_program(code, FLAGS[3:5])
                        self.assertIn("Unsupported byte " + operation, log)
            for code in (write_word(address + 1, 0),
                         expect_word(address + 1, 0)):
                with self.subTest(address=hex(address + 1), code=code):
                    log = self.run_program(code, FLAGS[3:5])
                    self.assertIn("Unsupported word access to F13 TAU0", log)

    def test_tau_new_registers_disabled_by_default_and_neighbors_rejected(self):
        for address in (0xf0074, 0xf0190, 0xf01b8, 0xf01ba):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0))
                self.assertIn(f"peripheral read at 0x{address:05x}", log)
        for address in (0xf0073, 0xf0075, 0xf018f, 0xf0192, 0xf01bc):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), FLAGS[3:5])
                self.assertIn(f"peripheral read at 0x{address:05x}", log)

    def test_tau_channel0_configuration_blocks_unmodeled_clock_removal(self):
        for address, value in ((0xf0190, 0x0104), (0xf01b8, 1)):
            with self.subTest(address=hex(address)):
                log = self.run_program(
                    write_byte(0xf00f0, 1) + write_word(address, value)
                    + write_byte(0xf00f0, 0), FLAGS[3:5])
                self.assertIn("clock disable after configuration", log)

    def test_irq_reset_flags_masks_and_priorities(self):
        code = b"".join(
            expect_byte(address, value)
            for address, value in (
                (0xfffd0, 0), (0xfffd1, 0), (0xfffd2, 0),
                (0xfffe0, 0), (0xfffe1, 0), (0xfffe2, 0),
                (0xfffe3, 0), (0xfffd4, 0xff), (0xfffd5, 0xff),
                (0xfffd6, 0xff), (0xfffd8, 0xff), (0xfffd9, 0xff),
                (0xfffda, 0xff), (0xfffdc, 0xff), (0xfffdd, 0xff),
                (0xfffde, 0xff), (0xfffe4, 0xff), (0xfffef, 0xff)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[5],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_irq_mask_and_priority_retention(self):
        code = b"".join(
            write_byte(address, 0x5a) + expect_byte(address, 0x5a)
            for address in (
                0xfffd4, 0xfffd5, 0xfffd6, 0xfffd8, 0xfffd9,
                0xfffda, 0xfffdc, 0xfffdd, 0xfffde,
                *range(0xfffe4, 0xffff0)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[5],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_irq_external_edges_reset_and_retention(self):
        code = b"".join(
            expect_byte(address, 0)
            + write_byte(address, mask) + expect_byte(address, mask)
            + write_byte(address, 0x15) + expect_byte(address, 0x15)
            + write_byte(address, 0) + expect_byte(address, 0)
            for address, mask in (
                (0xfff38, 0xff), (0xfff39, 0xff),
                (0xfff3a, 0x3f), (0xfff3b, 0x3f)))
        code += expect_byte(0xfffe0, 0) + expect_byte(0xfffe1, 0)
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[5],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_irq_external_edges_reject_reserved_bits(self):
        for address in (0xfff3a, 0xfff3b):
            for value in (0x40, 0x80, 0xff):
                with self.subTest(address=hex(address), value=value):
                    log = self.run_program(write_byte(address, value),
                                           (FLAGS[5],))
                    self.assertIn("Reserved F13 external interrupt edge bits",
                                  log)

    def test_irq_external_edges_reject_all_word_accesses(self):
        for address in range(0xfff37, 0xfff3c):
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=hex(address), code=code):
                    log = self.run_program(code, (FLAGS[5],))
                    self.assertIn("Unsupported word access to F13 interrupt",
                                  log)

    def test_irq_external_edges_do_not_enable_neighboring_registers(self):
        for address in (0xfff37, 0xfff3c):
            for code, operation in (
                    (write_byte(address, 0), "write"),
                    (expect_byte(address, 0), "read")):
                with self.subTest(address=hex(address), operation=operation):
                    log = self.run_program(code, (FLAGS[5],))
                    self.assertIn(
                        f"peripheral {operation} at 0x{address:05x}", log)

    def test_irq_external_edges_disabled_by_default(self):
        for address in range(0xfff38, 0xfff3c):
            for code, operation in (
                    (write_byte(address, 0), "write"),
                    (expect_byte(address, 0), "read")):
                with self.subTest(address=hex(address), operation=operation):
                    log = self.run_program(code)
                    self.assertIn(
                        f"peripheral {operation} at 0x{address:05x}", log)

    def test_irq_word_masks_and_priorities(self):
        code = b"".join(
            write_word(address, 0xa55a) + expect_word(address, 0xa55a)
            for address in (0xfffd4, 0xfffd8, 0xfffdc,
                            *range(0xfffe4, 0xffff0, 2)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[5],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_irq_software_requests_are_not_silently_accepted(self):
        for address in (0xfffd0, 0xfffd1, 0xfffd2,
                        0xfffe0, 0xfffe1, 0xfffe2, 0xfffe3):
            with self.subTest(address=hex(address)):
                log = self.run_program(write_byte(address, 1), (FLAGS[5],))
                self.assertIn("Unmodeled F13 software interrupt request", log)

    def test_irq_flags_can_be_cleared(self):
        code = b"".join(
            write_byte(address, 0) + expect_byte(address, 0)
            for address in (0xfffd0, 0xfffd1, 0xfffd2,
                            0xfffe0, 0xfffe1, 0xfffe2, 0xfffe3))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[5],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_irq_register_holes_remain_unsupported(self):
        for address in (0xfffd3, 0xfffd7, 0xfffdb, 0xfffdf):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), (FLAGS[5],))
                self.assertIn(f"peripheral read at 0x{address:05x}", log)

    def test_irq_single_byte_registers_reject_word_access(self):
        for address in (0xfffd2, 0xfffd6, 0xfffda, 0xfffde):
            with self.subTest(address=hex(address)):
                log = self.run_program(write_word(address, 0), (FLAGS[5],))
                self.assertIn("Unsupported word access to F13 interrupt", log)

    def test_irq_fixture_disabled_by_default(self):
        log = self.run_program(expect_byte(0xfffe6, 0xff))
        self.assertIn("peripheral read at 0xfffe6", log)

    def test_uninitialized_ram_does_not_select_a_board_configuration(self):
        log = self.run_program(expect_byte(0xfe8cd, 0x33))
        self.assertIn("Uninitialized MH2P SubCPU RAM read at 0xfe8cd", log)

    def test_original_code_can_write_and_read_ram(self):
        code = (write_byte(0xfe8cd, 5) + expect_byte(0xfe8cd, 5)
                + write_byte(0xf0002, 0))
        log = self.run_program(code)
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_port_configuration_resets_and_masks(self):
        code = b"".join(
            expect_byte(address, reset)
            + write_byte(address, value) + expect_byte(address, expected)
            for address, reset, value, expected in (
                (0xf0077, 0, 0xff, 1), (0xf0076, 0, 0x0b, 0x0b),
                (0xf0030, 0, 0xff, 0x0f), (0xf0051, 0, 2, 2),
                (0xf0031, 0, 0xff, 0xff), (0xf0041, 0, 0xff, 0xdb),
                (0xf0021, 0, 0xff, 0xdb), (0xfff20, 0xff, 0, 0xf0),
                (0xfff21, 0xff, 0x90, 0x90)))
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[6],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_port_input_levels_are_not_invented(self):
        for address in (0xfff00, 0xfff01, *range(0xfff03, 0xfff08)):
            with self.subTest(address=hex(address)):
                log = self.run_program(expect_byte(address, 0), (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_latch_reads_require_output_mode(self):
        code = (write_byte(0xfff20, 0xf0) + expect_byte(0xfff00, 0)
                + write_byte(0xfff00, 0x0a) + expect_byte(0xfff00, 0x0a)
                + write_byte(0xfff21, 0) + expect_byte(0xfff01, 0)
                + write_byte(0xfff01, 0xa5) + expect_byte(0xfff01, 0xa5)
                + write_byte(0xf0002, 0))
        log = self.run_program(code, (FLAGS[6],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_port_physical_output_read_is_not_latch_read(self):
        for address in (0xfff00, 0xfff01, *range(0xfff03, 0xfff08)):
            with self.subTest(address=hex(address)):
                fixed = 0xf0 if address == 0xfff00 else (
                    0xe0 if address == 0xfff03 else 0)
                code = (write_byte(address + 0x20, fixed)
                        + write_byte(0xf0077, 1)
                        + expect_byte(address, 0))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_extended_configuration_masks_and_pu4_reset(self):
        code = b"".join(
            expect_byte(address, int(address == 0xf0034))
            + write_byte(address, 0xff) + expect_byte(address, mask)
            + write_byte(address, 0) + expect_byte(address, 0)
            for address, mask in PORT_CONFIG_MASKS.items())
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[6],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)
        for address, mask in PORT_CONFIG_MASKS.items():
            if mask != 0xff:
                self.assertIn(f"Ignoring fixed F13 port bits at "
                              f"0x{address:05x}", log)

    def test_port_extended_latch_and_mode_masks(self):
        code = b""
        for port in range(3, 8):
            address = 0xfff00 + port
            mask = 0x1f if port == 3 else 0xff
            fixed = ~mask & 0xff
            code += expect_byte(address + 0x20, 0xff)
            code += write_byte(address + 0x20, 0)
            code += expect_byte(address + 0x20, fixed)
            code += expect_byte(address, 0)
            code += write_byte(address, 0xff) + expect_byte(address, mask)
            code += write_byte(address, 0) + expect_byte(address, 0)
        log = self.run_program(code + write_byte(0xf0002, 0), (FLAGS[6],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)

    def test_port_mixed_input_mode_never_returns_filled_latch(self):
        for port in range(3, 8):
            with self.subTest(port=port):
                address = 0xfff00 + port
                code = (write_byte(address, 0xff)
                        + write_byte(address + 0x20, 1)
                        + expect_byte(address, 0xff))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_extended_pullups_are_not_external_levels(self):
        for port in range(3, 8):
            with self.subTest(port=port):
                code = (write_byte(0xf0030 + port, 0xff)
                        + expect_byte(0xfff00 + port, 0xff))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_masked_logic_does_not_require_discarded_input_bits(self):
        for opcode, operand, expected in (
                (0x5a, 0x20, 0x20), (0x6a, 0x03, 0xa7),
                (0x5a, 0, 0), (0x6a, 0xff, 0xff)):
            with self.subTest(opcode=opcode, operand=operand):
                code = (write_byte(0xfff07, 0xa5)
                        + write_byte(0xfff27, 3)
                        + bytes([opcode, 7, operand])
                        + (b"\xdd\x04" if expected == 0 else b"\xdf\x04")
                        + write_byte(0xf0001, 0)
                        + write_byte(0xfff27, 0)
                        + expect_byte(0xfff07, expected)
                        + write_byte(0xf0002, 0))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("peripheral write at 0xf0002", log)
                self.assertNotIn("peripheral write at 0xf0001", log)
                self.assertNotIn("external pin read", log)

    def test_port_masked_logic_rejects_required_unknown_bits(self):
        for opcode, operand in ((0x5a, 1), (0x6a, 0xfe)):
            with self.subTest(opcode=opcode):
                code = (write_byte(0xfff07, 0xff)
                        + write_byte(0xfff27, 1)
                        + bytes([opcode, 7, operand]))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)
                self.assertIn("required mask=0x01", log)

    def test_port_bit_writes_preserve_other_latches_without_reading_pins(self):
        for pms in (0, 1):
            with self.subTest(pms=pms):
                code = (write_byte(0xfff07, 0xa5)
                        + write_byte(0xf0077, pms)
                        + b"\x71\x62\x07\x71\x23\x07"
                        + write_byte(0xf0077, 0)
                        + write_byte(0xfff27, 0)
                        + expect_byte(0xfff07, 0xe1)
                        + write_byte(0xf0002, 0))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("peripheral write at 0xf0002", log)
                self.assertNotIn("peripheral write at 0xf0001", log)
                self.assertNotIn("external pin read", log)

    def test_port_bit_read_requires_only_selected_output_bit(self):
        code = (write_byte(0xfff07, 0x20) + write_byte(0xfff27, 3)
                + b"\xf1\x71\x54\x07\x71\x89\x4c\x01\xdd\x04"
                + write_byte(0xf0001, 0) + write_byte(0xf0002, 0))
        log = self.run_program(code, (FLAGS[6],))
        self.assertIn("peripheral write at 0xf0002", log)
        self.assertNotIn("peripheral write at 0xf0001", log)
        self.assertNotIn("external pin read", log)
        for pms, mode, bit in ((0, 3, 0), (1, 0, 5)):
            with self.subTest(pms=pms, mode=mode, bit=bit):
                code = (write_byte(0xfff27, mode)
                        + write_byte(0xf0077, pms)
                        + bytes([0x71, (bit << 4) | 4, 7]))
                log = self.run_program(code, (FLAGS[6],))
                self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_masks_do_not_enable_unmodeled_or_default_ports(self):
        for address, flags in ((7, ()), (8, (FLAGS[6],))):
            for opcode in (0x5a, 0x6a):
                with self.subTest(address=address, opcode=opcode):
                    log = self.run_program(bytes([opcode, address, 0]), flags)
                    self.assertIn(
                        f"peripheral read at 0xfff{address:02x}", log)

    def test_port_extended_config_rejects_words_and_disabled_defaults(self):
        for address in PORT_CONFIG_MASKS:
            for code in (write_word(address, 0), expect_word(address, 0)):
                with self.subTest(address=hex(address), code=code):
                    log = self.run_program(code, (FLAGS[6],))
                    self.assertIn("Unsupported word access to F13 port", log)
            with self.subTest(address=hex(address), enabled=False):
                log = self.run_program(expect_byte(address, 0))
                self.assertIn(f"peripheral read at 0x{address:05x}", log)

    def test_port_pullups_do_not_supply_board_inputs(self):
        code = write_byte(0xf0031, 0xff) + expect_byte(0xfff01, 0xff)
        log = self.run_program(code, (FLAGS[6],))
        self.assertIn("Unmodeled F13 external pin read", log)

    def test_port_word_access_is_not_supported(self):
        log = self.run_program(write_word(0xfff20, 0), (FLAGS[6],))
        self.assertIn("Unsupported word access to F13 port", log)

    def test_port_prohibited_analog_selection_rejected(self):
        log = self.run_program(write_byte(0xf0076, 0x19), (FLAGS[6],))
        self.assertIn("Prohibited F13 ADPC value", log)

    def test_port_fixture_does_not_enable_other_ports(self):
        for address in (0xfff02, 0xfff08, 0xf0044, 0xf0053, 0xf0055):
            with self.subTest(address=hex(address)):
                log = self.run_program(write_byte(address, 0), (FLAGS[6],))
                self.assertIn(f"peripheral write at 0x{address:05x}", log)


if __name__ == "__main__":
    unittest.main()
