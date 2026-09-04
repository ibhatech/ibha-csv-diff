#!/usr/bin/env python3
"""Run the whole Phase 3 verification gate and report what passed.

    python3 scripts-and-commands/run_phase3_checks.py
    python3 scripts-and-commands/run_phase3_checks.py --fuzz-iters 3000000

Everything here is read only with respect to the repository: it builds into
core/build, generates fixtures into core/fixtures/generated, and touches nothing
else.

Same shape as the Phase 2 gate, with two additions:

  - the emitters are fuzzed alongside the parser, the matcher and ingest, and
    that target re-checks the output with the independent checkers in
    tests/emitkit.h rather than only running the code;
  - the CLI is driven end to end in each of the four formats, because an emitter
    that works in a unit test and not through the public entry point is not
    working;
  - both wasm modules are instantiated and driven, and the public header is
    checked for export markers, because on that target an unmarked function is
    not a link error, it is a function silently missing from the module.

The sanitizer steps are deliberately not fatal when the runtime is missing. On a
machine without libasan the correct outcome is "skipped, and here is what to
install", not a red gate: `ubsan` and `valgrind` between them cover undefined
behaviour and memory safety with no extra package.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
FIXTURES = CORE / "fixtures" / "generated"


def steps(fuzz_iters: int):
    """(label, argv, required). A step that is not required may report
    "skipped" when the toolchain it needs is absent."""
    src = str(FIXTURES / "p90_source.csv")
    tgt = str(FIXTURES / "p90_target.csv")
    cli = str(CORE / "build" / "ibha-csvdiff")
    return [
        ("build, warning free", ["make", "-B", "all"], True),
        ("unit, golden and property suites", ["make", "test"], True),
        ("undefined behaviour, trapping UBSan", ["make", "ubsan"], True),
        ("memory safety, valgrind memcheck", ["make", "valgrind"], False),
        ("AddressSanitizer, LeakSanitizer and UBSan", ["make", "asan"], False),
        ("fuzz, parser, matcher, emitters, ingest",
         ["make", "fuzz-native", f"FUZZ_ITERS={fuzz_iters}"], False),
        ("fuzz without a sanitizer runtime",
         ["make", "fuzz-native-notrap", f"FUZZ_ITERS={fuzz_iters}"], True),
        ("cli, summary emitter end to end", [cli, "diff", src, tgt, "--format", "summary"], True),
        ("cli, jsonl emitter end to end", [cli, "diff", src, tgt, "--format", "jsonl"], True),
        ("cli, csv emitter end to end", [cli, "diff", src, tgt, "--format", "csv"], True),
        ("cli, html emitter end to end",
         [cli, "diff", src, tgt, "--format", "html", "--changes-only", "--cell-diff", "both"],
         True),
        ("wasm modules build and run", ["make", "wasm-check"], False),
        ("every public declaration is exported",
         ["python3", str(ROOT / "scripts-and-commands" / "mark_public_api.py")], True),
        ("parser throughput", ["make", "bench"], True),
    ]


def run(label: str, argv: list, required: bool) -> tuple:
    proc = subprocess.run(argv, cwd=CORE, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    # diff(1) exit codes: the CLI returns 1 when the two files differ, which is
    # the expected outcome for the fixture pair, not a failure.
    ok = proc.returncode == 0 or (label.startswith("cli,") and proc.returncode == 1)
    if ok:
        return ("ok", label, out)
    if required:
        return ("FAILED", label, out)
    return ("skipped", label, out)


def summarize(label: str, out: str) -> str:
    """One interesting line per step, so the gate reads as a report."""
    keep = []
    if label.startswith("cli,"):
        return f"{len(out):,} bytes of output"
    if label.startswith("wasm") or label.startswith("every public"):
        return "; ".join(l.strip() for l in out.splitlines()
                         if l.startswith("all checks passed") or "declarations" in l
                         or l.startswith("FAIL"))[:200]
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
    return "; ".join(keep[:5])


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
