#!/usr/bin/env python3
"""Check tracked files for CRLF line endings, and report what the gate reads.

Task A9 of specs/03-remaining-tasks.md. The determinism gate compares emitter
output byte for byte, so a CRLF introduced by a checkout on Windows would fail it
in a way that reads like a correctness bug rather than like a checkout problem.

This script is the check, not the fix. The fix is the repository's .gitattributes,
which pins every tracked text file to LF in the working tree on every platform.

Run:
    python3 scripts-and-commands/check_line_endings.py

Exits 0 when every tracked file is LF only, 1 otherwise.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def tracked_files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    return [ROOT / name.decode() for name in out.split(b"\0") if name]


def main() -> int:
    offenders = []
    unreadable = []
    checked = 0

    for path in tracked_files():
        try:
            data = path.read_bytes()
        except OSError:
            unreadable.append(path)
            continue
        checked += 1
        if b"\r\n" in data:
            offenders.append((path, data.count(b"\r\n")))

    print(f"tracked files checked: {checked}")

    if unreadable:
        print(f"unreadable (skipped):  {len(unreadable)}")
        for path in unreadable:
            print(f"    {path.relative_to(ROOT)}")

    if offenders:
        print(f"\nCRLF found in {len(offenders)} file(s):")
        for path, count in offenders:
            print(f"    {path.relative_to(ROOT)}   {count} CRLF")
        print(
            "\nThe working tree disagrees with .gitattributes. Re-normalize with:\n"
            "    git add --renormalize .\n"
            "and commit the result before running the determinism gate."
        )
        return 1

    print("no CRLF in any tracked file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
