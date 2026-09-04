#!/usr/bin/env python3
"""Run the javadoc build and group its warnings, so they can be worked through.

Phase J1 of docs/PUBLISHING-PLAN.md. The javadoc jar is gated with doclint=all and
failOnWarnings=true, because the javadoc is this artifact's reference documentation
and Maven Central rejects a javadoc jar that failed to build.

Two things this exists to make visible:

  1. javadoc caps reported warnings at 100 by default, so "100 warnings" is a
     ceiling and not a count, and a hard error can be pushed off the end of the
     output entirely. The pom passes -Xmaxwarns 10000 to lift it. This script
     reports the real total.
  2. The warnings arrive interleaved and unsorted. Grouped by file and by kind they
     are a work list.

Run:
    python3 scripts-and-commands/javadoc_warnings.py
    python3 scripts-and-commands/javadoc_warnings.py --file DiffOptions.java
"""

from __future__ import annotations

import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA = ROOT / "java"

WARN = re.compile(r"^\[(?:ERROR|WARNING)\]\s+(?P<path>\S+\.java):(?P<line>\d+):"
                  r"\s+warning:\s+(?P<what>.*)$")
HARD = re.compile(r"^\[(?:ERROR|WARNING)\]\s+(?P<path>\S+\.java):(?P<line>\d+):"
                  r"\s+error:\s+(?P<what>.*)$")


def main() -> int:
    only = None
    if "--file" in sys.argv:
        only = sys.argv[sys.argv.index("--file") + 1]

    done = subprocess.run(
        ["mvn", "-DskipTests", "-DskipNative=true", "javadoc:jar"],
        cwd=JAVA, capture_output=True, text=True, check=False)
    out = (done.stdout or "") + (done.stderr or "")

    warns, errors = [], []
    for line in out.splitlines():
        m = HARD.match(line)
        if m:
            errors.append((pathlib.Path(m.group("path")).name, int(m.group("line")),
                           m.group("what")))
            continue
        m = WARN.match(line)
        if m:
            warns.append((pathlib.Path(m.group("path")).name, int(m.group("line")),
                          m.group("what")))

    if errors:
        print(f"HARD ERRORS ({len(errors)}), which are what actually fail the build:")
        for f, ln, what in errors:
            print(f"    {f}:{ln}  {what}")
        print()

    if only:
        rows = sorted((w for w in warns if w[0] == only), key=lambda w: w[1])
        print(f"{only}: {len(rows)} warnings")
        for f, ln, what in rows:
            print(f"    {ln:>5}  {what}")
    else:
        by_file = collections.Counter(w[0] for w in warns)
        by_kind = collections.Counter(re.sub(r"for \S+$", "for <x>", w[2]) for w in warns)
        print(f"total warnings: {len(warns)}")
        print("\nby file:")
        for f, n in by_file.most_common():
            print(f"    {n:>5}  {f}")
        print("\nby kind:")
        for k, n in by_kind.most_common():
            print(f"    {n:>5}  {k}")

    if done.returncode == 0 and not warns and not errors:
        print("\njavadoc builds clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
