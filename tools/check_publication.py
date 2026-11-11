#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check current source for accidental private data and head-unit payloads.

This is a heuristic source-tree check, not a credential audit, license review
or Git-history scrub. Findings report locations and rules, never matched values.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FORK_DIRECTORIES = ("tools/mh2p/", "tools/mhi2/")
PAYLOAD_SUFFIXES = {
    ".7z", ".bin", ".class", ".dmp", ".elf", ".exe", ".gz", ".ifs",
    ".img", ".jar", ".jxe", ".pcap", ".pcapng", ".pem", ".pfx", ".ppm",
    ".qcow2", ".rar", ".raw", ".so", ".tar", ".vmdk", ".xz", ".zip", ".zst",
}
RULES = {
    "private-key": re.compile(
        rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"),
    "github-token": re.compile(
        rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b"),
    "aws-access-key": re.compile(rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "session-path": re.compile(
        rb"\.copilot/" rb"session-state|\.claude/" rb"projects/"),
}
PERSONAL_PATH = re.compile(
    rb"/(?:home|Users)/[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+\\")
PRIVATE_NAMES = {".env", ".netrc", ".npmrc", ".pypirc", "credentials"}

# Deliberately public upstream QEMU test fixtures, not head-unit credentials.
TEST_KEYS = {
    "tests/keys/id_rsa",
    "tests/keys/vagrant",
    "tests/qemu-iotests/common.tls",
    "tests/unit/crypto-tls-x509-helpers.c",
}


def git_names(root, *arguments):
    output = subprocess.check_output(
        ["git", "-C", str(root), *arguments], stderr=subprocess.PIPE)
    return {name.decode("utf-8") for name in output.split(b"\0") if name}


def findings(name, data, *, check_personal_paths=False):
    result = []
    path = Path(name)
    if path.name in PRIVATE_NAMES:
        result.append((1, "private-config-file"))
    if name.startswith(FORK_DIRECTORIES):
        if path.suffix.lower() in PAYLOAD_SUFFIXES or ".so." in path.name:
            result.append((1, "head-unit-payload-candidate"))
        if b"\0" in data:
            result.append((1, "binary-in-head-unit-tools"))
    if b"\0" in data:
        return result
    rules = dict(RULES)
    if check_personal_paths:
        rules["workstation-path"] = PERSONAL_PATH
    for rule, pattern in rules.items():
        if rule == "private-key" and name in TEST_KEYS:
            continue
        for match in pattern.finditer(data):
            result.append((data.count(b"\n", 0, match.start()) + 1, rule))
    return sorted(set(result))


def check(root):
    untracked = git_names(root, "ls-files", "--others",
                          "--exclude-standard", "-z")
    names = git_names(root, "ls-files", "--cached", "-z") | untracked
    changed = git_names(root, "diff", "--name-only", "-z", "HEAD") | untracked
    problems = []
    checked = 0
    for name in sorted(names):
        path = root / name
        if path.is_symlink():
            if not path.resolve().is_relative_to(root):
                problems.append((name, 1, "external-symlink"))
            continue
        if not path.is_file():
            continue
        checked += 1
        for line, rule in findings(
                name, path.read_bytes(),
                check_personal_paths=(
                    name.startswith(FORK_DIRECTORIES) or name in changed)):
            problems.append((name, line, rule))
    return checked, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        checked, problems = check(root)
    except (OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"Publication check could not complete: {type(error).__name__}",
              file=sys.stderr)
        return 2
    for name, line, rule in problems:
        print(f"{name}:{line}: {rule}")
    print(f"Checked {checked} current source files; {len(problems)} findings.")
    print("Git history, ignored files, firmware provenance and release assets "
          "require separate review.")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
