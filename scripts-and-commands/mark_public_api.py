#!/usr/bin/env python3
"""Put IBHA_CSVD_API on every function declaration in the public header.

    python3 scripts-and-commands/mark_public_api.py          # check only
    python3 scripts-and-commands/mark_public_api.py --write   # apply

Why this is a script rather than a one off edit: the wasm build compiles with
-fvisibility=hidden and links with --gc-sections, so a public function that is
not marked is silently removed from the module. The failure mode is not a link
error, it is a module that loads and exports nothing, which is exactly how the
292 byte module that prompted this got built. So the marking has to be complete,
and completeness is something to check on every run rather than to remember.

Run with no arguments in CI: it exits non zero when a declaration in the header
is missing the marker.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "core" / "include" / "ibha_csvdiff.h"

# A declaration starts at the beginning of a line, is not a comment or a
# preprocessor line, and names an ibha_csvd_ function before its parameter list.
DECL = re.compile(r"^(?!\s|#|/|\*)(?P<decl>[A-Za-z_][^;{]*\bibha_csvd_[a-z_0-9]+\s*\()")


def scan(text: str):
    """Yields (line_number, line) for every public declaration and whether it is
    already marked."""
    for i, line in enumerate(text.splitlines()):
        if not DECL.match(line):
            continue
        # The definition of the macro itself, and anything already marked.
        if "IBHA_CSVD_API" in line:
            yield i, line, True
        else:
            yield i, line, False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="apply the marker")
    args = ap.parse_args()

    text = HEADER.read_text()
    lines = text.splitlines(keepends=True)
    unmarked = [(i, line) for i, line, marked in scan(text) if not marked]
    total = sum(1 for _ in scan(text))

    if not unmarked:
        print(f"{HEADER.relative_to(ROOT)}: {total} declarations, all marked")
        return 0

    if not args.write:
        print(f"{HEADER.relative_to(ROOT)}: {len(unmarked)} of {total} declarations unmarked")
        for i, line in unmarked[:20]:
            print(f"  {i + 1}: {line.strip()[:76]}")
        print("\nre-run with --write to fix")
        return 1

    for i, _ in unmarked:
        lines[i] = "IBHA_CSVD_API " + lines[i]
    HEADER.write_text("".join(lines))
    print(f"{HEADER.relative_to(ROOT)}: marked {len(unmarked)} declarations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
