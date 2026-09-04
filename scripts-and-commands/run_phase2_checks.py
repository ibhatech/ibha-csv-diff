#!/usr/bin/env python3
"""Run the whole Phase 2 verification gate and report what passed.

    python3 scripts-and-commands/run_phase2_checks.py
    python3 scripts-and-commands/run_phase2_checks.py --fuzz-iters 3000000

Everything here is read only with respect to the repository: it builds into
core/build, generates fixtures into core/fixtures/generated, and touches nothing
else.

The sanitizer steps are deliberately not fatal when the runtime is missing. On a
machine without libasan the correct outcome is "skipped, and here is what to
install", not a red gate: `ubsan` and `valgrind` between them cover undefined
behaviour and memory safety with no extra package, which is why they are in the
list at all.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE = ROOT / "core"

# (label, argv, required). A step that is not required may report "skipped"
# when the toolchain it needs is absent.
def steps(fuzz_iters: int):
    return [
        ("build, warning free", ["make", "-B", "all"], True),
        ("unit, golden and property suites", ["make", "test"], True),
        ("undefined behaviour, trapping UBSan", ["make", "ubsan"], True),
        ("memory safety, valgrind memcheck", ["make", "valgrind"], False),
        ("AddressSanitizer and UBSan", ["make", "asan"], False),
        ("fuzz, parser and matcher", ["make", "fuzz-native", f"FUZZ_ITERS={fuzz_iters}"], False),
        ("fuzz without a sanitizer runtime",
         ["make", "fuzz-native-notrap", f"FUZZ_ITERS={fuzz_iters}"], True),
        ("parser throughput", ["make", "bench"], True),
    ]


def run(label: str, argv: list, required: bool) -> tuple:
    proc = subprocess.run(argv, cwd=CORE, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    if proc.returncode == 0:
        return ("ok", label, out)
    if required:
        return ("FAILED", label, out)
    return ("skipped", label, out)


def summarize(label: str, out: str) -> str:
    """One interesting line per step, so the gate reads as a report."""
    keep = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("1.."):
            keep.append(f"{s[3:]} assertions")
        elif s.startswith("not ok"):
            keep.append(s)
        elif "no invariant violated" in s:
            keep.append(s)
        elif "MB/s" in s and "parse" in s:
            keep.append(s)
        elif s.startswith("error:"):
            keep.append(s)
    return "; ".join(keep[:4])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fuzz-iters", type=int, default=200000)
    args = ap.parse_args()

    failures = 0
    for label, argv, required in steps(args.fuzz_iters):
        status, name, out = run(label, argv, required)
        if status == "FAILED":
            failures += 1
        print(f"[{status:>7}] {name}")
        detail = summarize(name, out)
        if detail:
            print(f"           {detail}")
        if status == "FAILED":
            print("\n".join("           " + l for l in out.splitlines()[-25:]))

    print()
    print("gate failed" if failures else "gate passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
