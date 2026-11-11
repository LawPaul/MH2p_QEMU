# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware-independent checks of the opt-in diagnostic class override."""
from io import BytesIO
import struct
import unittest
from zipfile import ZipFile

import hmi_diagnostic_input as diagnostic


def synthetic_gateway(duplicate_target=False):
    constants = {}

    def text(index, value):
        constants[index] = b"\x01" + struct.pack(">H", len(value)) + value

    def reference(index, tag, *values):
        constants[index] = bytes([tag]) + struct.pack(
            ">" + "H" * len(values), *values)

    text(1, diagnostic.GATEWAY)
    reference(2, 7, 1)
    text(3, b"fwHMI")
    text(4, b"L"+diagnostic.FRAMEWORK+b";")
    reference(5, 9, 2, 6)
    reference(6, 12, 3, 4)
    text(7, diagnostic.FRAMEWORK)
    reference(8, 7, 7)
    text(9, b"getMonotonicTime")
    text(10, b"()J")
    reference(11, 12, 9, 10)
    text(12, b"fireGestureEvent")
    text(13, b"(IJIII)V")
    reference(14, 12, 12, 13)
    text(15, b"sendGestureEvent")
    text(16, b"(III)V")
    text(17, b"Code")
    text(18, b"java/lang/Object")
    reference(19, 7, 18)
    text(20, b"unchangedMethod")
    text(21, b"()V")
    for index in range(22, 113):
        text(index, b"unused")
    reference(113, 10, 2, 11)
    reference(114, 10, 8, 14)

    def member(name, descriptor, code, stack, locals_):
        content = (struct.pack(">HHI", stack, locals_, len(code)) +
                   code + bytes(4))
        return (struct.pack(">5HI", 2, name, descriptor, 1, 17, len(content)) +
                content)

    pool = b"".join(constants[i] for i in range(1, 115))
    header = bytes.fromhex("cafebabe00000030") + struct.pack(">H", 115)
    fields = struct.pack(">5H", 1, 2, 3, 4, 0)
    target = member(15, 16, diagnostic.ORIGINAL_CODE, 7, 4)
    body = (struct.pack(">4H", 0x21, 2, 19, 0) + fields +
            struct.pack(">H", 3 if duplicate_target else 2) +
            target + (target if duplicate_target else b"") +
            member(20, 21, b"\xb1", 0, 1) + bytes(2))
    return header + pool + body


class DiagnosticInputTests(unittest.TestCase):
    def test_single_method_change_and_deterministic_single_class_archive(self):
        source = synthetic_gateway()
        result = diagnostic.diagnostic_release_class(source)
        self.assertEqual(source.count(diagnostic.ORIGINAL_CODE), 1)
        self.assertNotIn(diagnostic.ORIGINAL_CODE, result)
        self.assertEqual(result[:8], source[:8])
        self.assertEqual(int.from_bytes(result[8:10], "big"), 118)
        self.assertTrue(result.endswith(source[-29:]))
        archive = diagnostic.diagnostic_release_jar(source)
        self.assertEqual(archive, diagnostic.diagnostic_release_jar(source))
        with ZipFile(BytesIO(archive)) as jar:
            self.assertEqual(jar.namelist(), [diagnostic.CLASS_NAME])
            self.assertEqual(jar.read(diagnostic.CLASS_NAME), result)

    def test_release_only_changes_finger_count_and_preserves_all_arguments(self):
        result = diagnostic.diagnostic_release_class(synthetic_gateway())
        start = result.index(diagnostic.ORIGINAL_CODE[:11])
        code = result[start:start+30]
        for event in (10902, 10903, 10904, 10905, 10906):
            for x, y in ((0, 0), (70, 33), (1919, 719)):
                stack, at, delivered = [], 0, None
                locals_ = ["gateway", event, x, y]
                while at < len(code):
                    op = code[at]
                    if op in (0x2a, 0x1b, 0x1c, 0x1d):
                        slot = {0x2a: 0, 0x1b: 1, 0x1c: 2, 0x1d: 3}[op]
                        stack.append(locals_[slot])
                        at += 1
                    elif op in (3, 4):
                        stack.append(op-3)
                        at += 1
                    elif op == 0x11:
                        stack.append(struct.unpack_from(">h", code, at+1)[0])
                        at += 3
                    elif op == 0xb4:
                        self.assertEqual(stack.pop(), "gateway")
                        stack.append("framework")
                        at += 3
                    elif op == 0xb6:
                        index = struct.unpack_from(">H", code, at+1)[0]
                        if index == 113:
                            self.assertEqual(stack.pop(), "gateway")
                            stack.append(123456789)
                        else:
                            self.assertEqual(index, 117)
                            delivered = stack[:]
                            stack.clear()
                        at += 3
                    elif op == 0xa0:
                        right, left = stack.pop(), stack.pop()
                        jump = struct.unpack_from(">h", code, at+1)[0]
                        at += jump if left != right else 3
                    elif op == 0xa7:
                        at += struct.unpack_from(">h", code, at+1)[0]
                    elif op == 0xb1:
                        self.assertEqual(stack, [])
                        break
                    else:
                        self.fail(f"Unexpected instruction {op:#x}")
                self.assertEqual(delivered, [
                    "framework", event, 123456789, x, y,
                    0 if event == 10903 else 1, 0, 0, 0])

    def test_rejects_unknown_truncated_already_modified_and_changed_classes(self):
        source = synthetic_gateway()
        modified = diagnostic.diagnostic_release_class(source)
        for invalid in (
                b"", source[:9], source[:-1], source + b"extra",
                source[:7]+b"\x31"+source[8:], bytes(65537),
                source.replace(b"getMonotonicTime", b"getDifferentTime"),
                source.replace(b"(IJIII)V", b"(IJIIJ)V"),
                source.replace(b"sendGestureEvent", b"lostGestureEvent"),
                source.replace(diagnostic.ORIGINAL_CODE,
                               diagnostic.ORIGINAL_CODE[:-1]+b"\x00"),
                synthetic_gateway(duplicate_target=True), modified):
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                diagnostic.diagnostic_release_class(invalid)

    def test_every_truncated_prefix_fails_explicitly(self):
        source = synthetic_gateway()
        for length in range(len(source)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                diagnostic.diagnostic_release_class(source[:length])


if __name__ == "__main__":
    unittest.main()
