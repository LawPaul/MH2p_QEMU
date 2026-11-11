# SPDX-License-Identifier: GPL-2.0-or-later
"""Strict decoder for the original and laboratory CarPlay touchscreen layouts."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Field:
    slot: int
    usage: str
    bit: int
    size: int
    minimum: int | None
    maximum: int | None


USAGES = {(13, 0x33): "touch", (13, 0x38): "index",
          (1, 0x30): "x", (1, 0x31): "y"}


def touch_layout(descriptor):
    state = {}
    usages = []
    collections = []
    fields = []
    at = bit = 0
    slot = -1
    while at < len(descriptor):
        prefix = descriptor[at]
        at += 1
        if prefix == 0xfe:
            raise ValueError("Long HID items are outside the inspected layouts")
        length = (0, 1, 2, 4)[prefix & 3]
        if at + length > len(descriptor):
            raise ValueError("Truncated HID item")
        data = descriptor[at:at + length]
        at += length
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        value = int.from_bytes(data, "little")
        if kind == 1:
            if tag not in (0, 1, 2, 7, 9):
                raise ValueError(f"Unsupported HID global item {tag}")
            state[tag] = int.from_bytes(data, "little", signed=True) if tag == 1 else value
        elif kind == 2:
            if tag != 0 or 0 not in state:
                raise ValueError("Unsupported HID local item or missing usage page")
            usages.append((state[0], value))
        elif kind == 0:
            if tag == 10:
                usage = usages[-1] if usages else None
                expected = (13, 4) if not collections else (13, 0x22)
                if (len(collections) > 1 or usage != expected or
                        value != (1 if not collections else 2)):
                    raise ValueError("Expected touchscreen application with finger collections")
                if collections:
                    slot += 1
                collections.append(usage)
            elif tag == 12:
                if not collections:
                    raise ValueError("Unbalanced HID collection")
                collections.pop()
            elif tag == 8:
                size, count = state.get(7, 0), state.get(9, 0)
                if len(collections) != 2 or not 1 <= size <= 16 or not 1 <= count <= 8:
                    raise ValueError("Invalid touchscreen HID input shape")
                constant = bool(value & 1)
                if not constant and (value != 2 or len(usages) != count):
                    raise ValueError("Expected absolute variable HID input")
                for index in range(count):
                    usage = "padding" if constant else USAGES.get(usages[index])
                    if usage is None:
                        raise ValueError("Unsupported touchscreen HID usage")
                    fields.append(Field(slot, usage, bit, size, state.get(1), state.get(2)))
                    bit += size
            else:
                raise ValueError(f"Unsupported HID main item {tag}")
            usages.clear()
        else:
            raise ValueError("Reserved HID item")
    if collections or bit % 8 or not 1 <= slot + 1 <= 4:
        raise ValueError("Incomplete touchscreen HID layout")
    previous_names = None
    for index in range(slot + 1):
        contact = [field for field in fields if field.slot == index]
        names = [field.usage for field in contact if field.usage != "padding"]
        if names not in (["touch", "x", "y"], ["index", "touch", "x", "y"]):
            raise ValueError("Unexpected touchscreen contact field order")
        if previous_names is not None and names != previous_names:
            raise ValueError("Mixed touchscreen contact layouts")
        previous_names = names
        sizes = {"index": 8, "touch": 1, "x": 16, "y": 16}
        if any(field.size != sizes[field.usage] for field in contact
               if field.usage != "padding"):
            raise ValueError("Unexpected touchscreen contact field width")
        if sum(field.size for field in contact if field.usage == "padding") != 7:
            raise ValueError("Expected seven padding bits per touchscreen contact")
    return fields


def decode_report(descriptor, report):
    fields = touch_layout(descriptor)
    if len(report) * 8 != sum(field.size for field in fields):
        raise ValueError("HID report length does not match descriptor")
    contacts = [{} for _ in range(fields[-1].slot + 1)]
    bits = int.from_bytes(report, "little")
    for field in fields:
        value = (bits >> field.bit) & ((1 << field.size) - 1)
        if field.usage == "padding":
            if value:
                raise ValueError("Nonzero touchscreen report padding")
        else:
            contacts[field.slot][field.usage] = value
    return contacts


def verify_report(descriptor, report, contacts, maximum, release=False, indexed=False):
    decoded = decode_report(descriptor, report)
    expected = []
    for slot in range(maximum):
        x, y, down = contacts[slot] if slot < len(contacts) else (0, 0, 0)
        contact = {"touch": 0 if release else down, "x": x, "y": y}
        if indexed:
            contact["index"] = slot
        expected.append(contact)
    if decoded != expected:
        raise AssertionError(f"Decoded HID contacts differ: {decoded!r} != {expected!r}")
    return decoded
