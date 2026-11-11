# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a single-class P2865 diagnostic override; never edit the firmware JAR."""
from io import BytesIO
import struct
from zipfile import ZIP_STORED, ZipFile, ZipInfo

CLASS_NAME = "de/esolutions/hmi/service/swdiagnosis/HMIDiagGateway.class"
GATEWAY = b"de/esolutions/hmi/service/swdiagnosis/HMIDiagGateway"
FRAMEWORK = b"de/esolutions/hmi/service/FwHMIAdi"
ORIGINAL_CODE = bytes.fromhex("2ab400051b2ab600711c1d03b60072b1")


def diagnostic_release_class(data):
    """Use the full original gesture API to report zero fingers on release."""
    def number(at, size):
        if at < 0 or at + size > len(data):
            raise ValueError("Truncated diagnostic class")
        return int.from_bytes(data[at:at+size], "big")

    def u2(at):
        return number(at, 2)

    def u4(at):
        return number(at, 4)

    if len(data) > 65536 or data[:8] != bytes.fromhex("cafebabe00000030"):
        raise ValueError("Expected original P2865 Java 1.4 diagnostic class")
    count = u2(8)
    if not 115 <= count <= 65532:
        raise ValueError("Unexpected diagnostic constant pool size")
    constants = {}
    at, index = 10, 1
    while index < count:
        tag = number(at, 1)
        at += 1
        if tag == 1:
            length = u2(at)
            number(at+2, length)
            value = data[at+2:at+2+length]
            at += 2 + length
        elif tag in (3, 4, 5, 6):
            size = 8 if tag in (5, 6) else 4
            value = number(at, size)
            at += size
        elif tag in (7, 8):
            value = u2(at)
            at += 2
        elif tag in (9, 10, 11, 12):
            value = (u2(at), u2(at+2))
            at += 4
        else:
            raise ValueError(f"Unexpected diagnostic constant tag {tag}")
        constants[index] = (tag, value)
        index += 2 if tag in (5, 6) else 1
    pool_end = at

    def constant(index, tag):
        entry = constants.get(index)
        if entry is None or entry[0] != tag:
            raise ValueError("Unexpected diagnostic constant reference")
        return entry[1]

    def reference(index, tag):
        owner, member = constant(index, tag)
        name, descriptor = constant(member, 12)
        return (constant(constant(owner, 7), 1),
                constant(name, 1), constant(descriptor, 1))

    if (reference(5, 9) != (GATEWAY, b"fwHMI", b"L"+FRAMEWORK+b";") or
            reference(113, 10) != (GATEWAY, b"getMonotonicTime", b"()J") or
            reference(114, 10) != (FRAMEWORK, b"fireGestureEvent", b"(IJIII)V") or
            constant(constant(u2(at+2), 7), 1) != GATEWAY):
        raise ValueError("Original diagnostic gesture API changed")
    owner, member = constant(114, 10)
    method_name, _ = constant(member, 12)
    descriptor = b"(IJIIIIII)V"
    extra = (b"\x01" + struct.pack(">H", len(descriptor)) + descriptor +
             b"\x0c" + struct.pack(">HH", method_name, count) +
             b"\x0a" + struct.pack(">HH", owner, count+1))
    # The simple original overload hardcodes one finger for every event.
    # The full overload preserves the coordinates and permits a final release.
    code = (ORIGINAL_CODE[:11] +
            bytes.fromhex("1b112a97a0000703a7000404030303b6") +
            struct.pack(">H", count+2) + b"\xb1")

    def skip_attributes(pos):
        size = u2(pos)
        pos += 2
        for _ in range(size):
            end = pos + 6 + u4(pos+2)
            if end > len(data):
                raise ValueError("Truncated diagnostic attribute")
            pos = end
        return pos

    at += 6
    at += 2 + 2*u2(at)
    fields = u2(at)
    at += 2
    for _ in range(fields):
        at = skip_attributes(at+6)
    methods = u2(at)
    at += 2
    replacement = None
    for _ in range(methods):
        name, signature, attributes = u2(at+2), u2(at+4), u2(at+6)
        at += 8
        selected = (constant(name, 1) == b"sendGestureEvent" and
                    constant(signature, 1) == b"(III)V")
        for _ in range(attributes):
            attr, size = u2(at), u4(at+2)
            end = at + 6 + size
            if end > len(data):
                raise ValueError("Truncated diagnostic method attribute")
            if selected and constant(attr, 1) == b"Code":
                if replacement:
                    raise ValueError("Duplicate diagnostic gesture method")
                length = u4(at+10)
                if (size < 12 + length or
                        data[at+14:at+14+length] != ORIGINAL_CODE or
                        u2(at+6) != 7 or u2(at+8) != 4 or
                        u2(at+14+length)):
                    raise ValueError("Original diagnostic instructions changed")
                content = (struct.pack(">HHI", 10, 4, len(code)) +
                           code + struct.pack(">HH", 0, 0))
                replacement = (at+2, end, struct.pack(">I", len(content))+content)
            at = end
    if not replacement:
        raise ValueError("Original diagnostic gesture method was not found")
    if skip_attributes(at) != len(data):
        raise ValueError("Unexpected trailing diagnostic class data")
    begin, end, content = replacement
    result = bytearray(data[:begin] + content + data[end:])
    result[8:10] = struct.pack(">H", count+3)
    result[pool_end:pool_end] = extra
    return bytes(result)


def diagnostic_release_jar(class_data):
    output = BytesIO()
    with ZipFile(output, "w", compression=ZIP_STORED) as archive:
        archive.writestr(ZipInfo(CLASS_NAME), diagnostic_release_class(class_data))
    return output.getvalue()
