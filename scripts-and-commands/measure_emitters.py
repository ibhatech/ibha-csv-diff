#!/usr/bin/env python3
"""Time each emitter over the generated 15 MB pair and report size and rate.

    python3 scripts-and-commands/measure_emitters.py

Reads only from core/fixtures/generated and writes only into a temporary
directory it removes afterwards. The numbers it prints are what core/README.md
quotes, so re-run it rather than editing the table by hand.
"""
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
CLI = CORE / "build" / "ibha-csvdiff"
SRC = CORE / "fixtures" / "generated" / "p90_source.csv"
TGT = CORE / "fixtures" / "generated" / "p90_target.csv"

CASES = [
    ("summary", ["--format", "summary"]),
    ("jsonl", ["--format", "jsonl"]),
    ("jsonl, changes only", ["--format", "jsonl", "--changes-only"]),
    ("csv", ["--format", "csv"]),
    ("html", ["--format", "html"]),
    ("html, changes only, cell diff", ["--format", "html", "--changes-only", "--cell-diff", "both"]),
    ("counts only, no emitter", []),
]

REPEATS = 3


def main() -> int:
    for path in (CLI, SRC, TGT):
        if not path.exists():
            print(f"missing {path}. Run: cd core && make all fixtures")
            return 1

    input_mb = (SRC.stat().st_size + TGT.stat().st_size) / (1024 * 1024)
    print(f"input {input_mb:.2f} MB, best of {REPEATS}\n")
    print(f"{'emitter':<32} {'seconds':>9} {'output':>12} {'MB/s in':>9}")

    with tempfile.TemporaryDirectory() as tmp:
        out_path = pathlib.Path(tmp) / "out"
        for label, args in CASES:
            best = None
            size = 0
            for _ in range(REPEATS):
                with out_path.open("wb") as out:
                    start = time.monotonic()
                    proc = subprocess.run([str(CLI), "diff", str(SRC), str(TGT)] + args,
                                          stdout=out, stderr=subprocess.PIPE)
                    elapsed = time.monotonic() - start
                # diff(1) exit codes: 1 means differences were found.
                if proc.returncode not in (0, 1):
                    print(f"{label:<32} FAILED: {proc.stderr.decode().strip()}")
                    best = None
                    break
                size = out_path.stat().st_size
                if best is None or elapsed < best:
                    best = elapsed
            if best is None:
                continue
            mb = size / (1024 * 1024)
            shown = f"{mb:.1f} MB" if mb >= 1 else f"{size:,} B"
            print(f"{label:<32} {best:9.3f} {shown:>12} {input_mb / best:9.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
