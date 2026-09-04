#!/usr/bin/env python3
"""Catch the workflow YAML mistakes that only GitHub would otherwise report.

Written after shipping `.github/workflows/ci.yml` with this line:

    - name: The packed manifests must not carry workspace: specifiers

which is invalid YAML, because a plain unquoted scalar cannot contain ": ". The
file was structurally checked before committing, but with no YAML parser on the
machine that check only looked at tabs and line endings, so the error survived
until GitHub refused the file.

This does not parse YAML. It checks the specific mistakes that are easy to make in
a workflow and invisible without a parser:

  1. an unquoted scalar containing ": ", which is the one that actually happened
  2. tabs, which YAML forbids for indentation
  3. CRLF line endings
  4. a `uses:` with no version pin

For a real parse, install a parser outside this repository and use it, for example
`npm install yaml` in a scratch directory. Nothing here depends on one, because a
lint dependency in a library's manifest is a cost paid by every consumer.

Run:
    python3 scripts-and-commands/check_workflows.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"

# key: value, where value is not quoted and not a block scalar
PLAIN = re.compile(r"^(?P<indent>\s*)(?:-\s+)?(?P<key>[A-Za-z_][\w-]*):\s+(?P<val>[^'\"|>\s].*)$")


def main() -> int:
    if not WORKFLOWS.is_dir():
        print(f"no workflows at {WORKFLOWS}")
        return 0

    problems = 0
    files = sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml"))
    if not files:
        print("no workflow files")
        return 0

    for path in files:
        text = path.read_text()
        found = []

        for n, line in enumerate(text.split("\n"), 1):
            if "\t" in line:
                found.append((n, "TAB, which YAML forbids for indentation", line))
            if line.endswith("\r"):
                found.append((n, "CRLF line ending", line))

            m = PLAIN.match(line)
            if m:
                val = m.group("val")
                # A trailing comment is not part of the scalar.
                val = re.split(r"\s+#", val)[0].rstrip()
                if ": " in val:
                    found.append((
                        n,
                        f'unquoted scalar for "{m.group("key")}" contains ": ", '
                        "which ends the scalar and starts a nested mapping. Quote it",
                        line,
                    ))
                # A local reusable workflow is referenced by path and correctly
                # carries no @version: it is this same commit by definition.
                if (m.group("key") == "uses"
                        and "@" not in val
                        and not val.startswith("./")):
                    found.append((n, "uses: with no version pin", line))

        if found:
            problems += len(found)
            print(f"\n{path.name}")
            for n, why, line in found:
                print(f"  line {n}: {why}")
                print(f"    {line.strip()}")
        else:
            print(f"ok  {path.name}")

    if problems:
        print(f"\n{problems} problem(s). GitHub would reject or misread these.")
        return 1
    print(f"\n{len(files)} workflow file(s), no problems of the kinds checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
