#!/usr/bin/env python3
"""Check generated docs for em dash / en dash, which project rules forbid.

Usage: python3 scripts-and-commands/check_dashes.py [path ...]
Defaults to scanning specs/ and docs/ under the repo root.
"""
import pathlib
import sys

FORBIDDEN = {"—": "em dash", "–": "en dash"}
ROOT = pathlib.Path(__file__).resolve().parent.parent


def targets(argv):
    if argv:
        return [pathlib.Path(a) for a in argv]
    out = []
    for d in ("specs", "docs"):
        out.extend(sorted((ROOT / d).rglob("*.md")))
    return out


def main():
    hits = 0
    for path in targets(sys.argv[1:]):
        if not path.is_file():
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for ch, name in FORBIDDEN.items():
                if ch in line:
                    hits += 1
                    print(f"{path}:{lineno}: {name}: {line.strip()[:100]}")
    print("clean" if hits == 0 else f"{hits} violation(s)")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
