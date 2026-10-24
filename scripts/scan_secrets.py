#!/usr/bin/env python3
"""Fail closed when source files contain likely credentials.

The scanner deliberately reports only a file, line and rule name. Echoing the
candidate would copy a real credential into CI logs and artifact retention.
"""

from __future__ import annotations

import argparse
from collections import Counter
import math
from pathlib import Path
import re
import subprocess
import sys


ALLOW_MARKER = "secret-scan: allow"
MAX_FILE_BYTES = 5 * 1024 * 1024

PROVIDER_PATTERNS = (
    ("private-key", re.compile(r"-----BEGIN (?:(?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY|PGP PRIVATE KEY BLOCK)-----")),  # secret-scan: allow
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("github-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{40,})\b")),
    ("gitlab-token", re.compile(r"\bglpat-[A-Za-z0-9_-]{20,}\b")),
    ("slack-token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("stripe-live-key", re.compile(r"\b(?:sk|rk)_live_[0-9A-Za-z]{16,}\b")),
    ("npm-token", re.compile(r"\bnpm_[A-Za-z0-9]{36}\b")),
    ("digitalocean-token", re.compile(r"\bdop_v1_[A-Fa-f0-9]{64}\b")),
    ("jwt", re.compile(r"\beyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\b")),
)

CREDENTIAL_ASSIGNMENT = re.compile(
    r"(?ix)"
    r"(?:api[_-]?key|secret|token|password|passwd|private[_-]?key|client[_-]?secret|access[_-]?key)"
    r"[A-Za-z0-9_-]*[\"']?\s*(?:=|:)\s*[\"']?"
    r"(?P<value>[^\s\"'`,;#]+)"
)

CREDENTIAL_URL = re.compile(
    r"(?i)\b(?:postgres(?:ql)?|mysql|mariadb|mongodb(?:\+srv)?)://"
    r"[^:/@\s]+:(?P<value>[^@/\s]+)@"
)

PLACEHOLDER_WORDS = ("REPLACE", "CHANGEME", "PLACEHOLDER", "EXAMPLE", "DUMMY")


def entropy(value: str) -> float:
    counts = Counter(value)
    length = len(value)
    return -sum((count / length) * math.log2(count / length) for count in counts.values())


def is_placeholder(value: str) -> bool:
    upper = value.upper()
    if any(word in upper for word in PLACEHOLDER_WORDS):
        return True
    if "${" in value or "$(" in value or value.startswith("$"):
        return True
    if value.startswith("<") and value.endswith(">"):
        return True
    # Repeated-character fixtures are syntactically valid but carry no entropy
    # and cannot be mistaken for generated credentials.
    return len(set(value)) <= 3


def high_entropy_credential(value: str) -> bool:
    value = value.strip().rstrip("\\")
    if len(value) < 32 or is_placeholder(value):
        return False
    return entropy(value) >= 3.4


def repository_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        check=True,
        stdout=subprocess.PIPE,
    )
    return [Path(item.decode("utf-8")) for item in result.stdout.split(b"\0") if item]


def scan_file(path: Path) -> list[tuple[int, str]]:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error
    if len(payload) > MAX_FILE_BYTES:
        raise RuntimeError(f"refusing to skip oversized source file {path}")
    if b"\0" in payload[:8192]:
        return []

    findings: list[tuple[int, str]] = []
    for line_number, line in enumerate(payload.decode("utf-8", errors="replace").splitlines(), 1):
        if ALLOW_MARKER in line:
            continue
        rules: set[str] = set()
        for name, pattern in PROVIDER_PATTERNS:
            if pattern.search(line):
                rules.add(name)
        for match in CREDENTIAL_ASSIGNMENT.finditer(line):
            if high_entropy_credential(match.group("value")):
                rules.add("generic-high-entropy-credential")
        for match in CREDENTIAL_URL.finditer(line):
            value = match.group("value")
            if value != "test-password" and not is_placeholder(value):
                rules.add("credential-in-url")
        findings.extend((line_number, rule) for rule in sorted(rules))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="scan source files for likely secrets without logging values")
    parser.add_argument("--files", nargs="+", type=Path, help="explicit files instead of the Git working tree")
    args = parser.parse_args()

    try:
        files = args.files if args.files is not None else repository_files()
        findings = []
        for path in files:
            if path.is_symlink():
                raise RuntimeError(f"refusing to follow source symlink {path}")
            if not path.is_file():
                raise RuntimeError(f"source path is not a regular file {path}")
            findings.extend((path, line, rule) for line, rule in scan_file(path))
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"secret scan failed closed: {error}", file=sys.stderr)
        return 2

    for path, line, rule in findings:
        print(f"potential secret: {path}:{line} [{rule}] (value redacted)", file=sys.stderr)
    if findings:
        print(f"secret scan rejected {len(findings)} finding(s)", file=sys.stderr)
        return 1
    print(f"secret scan passed ({len(files)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
