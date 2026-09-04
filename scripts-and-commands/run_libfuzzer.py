#!/usr/bin/env python3
"""Seed a corpus and run the four libFuzzer targets, reporting what they did.

    python3 scripts-and-commands/run_libfuzzer.py
    python3 scripts-and-commands/run_libfuzzer.py --seconds 600
    python3 scripts-and-commands/run_libfuzzer.py --target emit --seconds 3600

This is the coverage guided run, which is the strong one.
`core/build/*_native` under `make fuzz-native` runs the same entry points with a
dumb generator and no feedback; it exists so the untrusted input surface is
exercised on a machine with no libFuzzer, not because it is equivalent.

The pass condition is not "the process exited zero". libFuzzer writes a
crash-*, leak-*, timeout-* or oom-* file next to itself when it finds something,
so this checks for those explicitly and fails the run if any appeared, whatever
the exit status said.

The corpus is kept in core/corpus, which is gitignored: coverage feedback grows
it across runs, and throwing it away each time throws away the interesting inputs
along with it.
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
BUILD = CORE / "build"
CORPUS = CORE / "corpus"
FIXTURES = CORE / "fixtures" / "generated"

ARTIFACTS = ("crash-", "leak-", "timeout-", "oom-")

# Seeds per target. The p90 pair is deliberately left out: at 15 MB each it drops
# the execution rate by two orders of magnitude and coverage feedback finds more
# in a hundred thousand small inputs than in a hundred large ones.
SEEDS = {
    "parse": lambda p: p.suffix == ".csv" and not p.name.startswith("p90_"),
    "diff": lambda p: p.name.startswith("tiny_") or p.name == "xss.csv",
    "emit": lambda p: p.name.startswith("tiny_") or p.name in ("xss.csv", "latin1.csv"),
    "ingest": lambda p: p.suffix == ".csv" and not p.name.startswith("p90_"),
}


def seed(target: str) -> pathlib.Path:
    out = CORPUS / target
    out.mkdir(parents=True, exist_ok=True)
    keep = SEEDS[target]
    for src in sorted(FIXTURES.glob("*.csv")):
        if keep(src) and not (out / src.name).exists():
            shutil.copy(src, out / src.name)
    return out


def run_one(target: str, seconds: int, jobs: int) -> tuple:
    binary = BUILD / f"fuzz_{target}"
    if not binary.exists():
        return ("missing", f"{binary.relative_to(ROOT)} is not built. Run: cd core && make fuzz", 0)

    corpus = seed(target)
    before = {p.name for p in CORE.glob("*") if p.name.startswith(ARTIFACTS)}

    proc = subprocess.run(
        [str(binary), f"-max_total_time={seconds}", "-rss_limit_mb=4096",
         "-print_final_stats=1", f"-jobs={jobs}" if jobs > 1 else "-jobs=0", str(corpus)],
        cwd=CORE, capture_output=True, text=True)
    out = proc.stdout + proc.stderr

    after = {p.name for p in CORE.glob("*") if p.name.startswith(ARTIFACTS)}
    found = sorted(after - before)
    if found:
        return ("FOUND", f"{len(found)} artifact(s): {', '.join(found[:4])}", len(list(corpus.iterdir())))

    runs = re.search(r"stat::number_of_executed_units:\s*(\d+)", out)
    rate = re.search(r"stat::average_exec_per_sec:\s*(\d+)", out)
    added = re.search(r"stat::new_units_added:\s*(\d+)", out)
    detail = (f"{int(runs.group(1)):,} runs, {int(rate.group(1)):,}/s, "
              f"{int(added.group(1)):,} new corpus entries" if runs and rate and added
              else out.strip().splitlines()[-1][:120] if out.strip() else "no output")
    return ("ok", detail, len(list(corpus.iterdir())))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=90, help="per target, default 90")
    ap.add_argument("--jobs", type=int, default=1, help="parallel libFuzzer jobs per target")
    ap.add_argument("--target", choices=sorted(SEEDS), help="run only this one")
    args = ap.parse_args()

    targets = [args.target] if args.target else ["parse", "diff", "emit", "ingest"]
    failures = 0

    print(f"{args.seconds}s per target, corpus in {CORPUS.relative_to(ROOT)}\n")
    for target in targets:
        status, detail, size = run_one(target, args.seconds, args.jobs)
        if status == "FOUND":
            failures += 1
        print(f"[{status:>7}] {target:<7} {detail}")
        if status == "ok":
            print(f"           corpus now {size:,} files")

    print()
    if failures:
        print("libFuzzer found something. The artifact files are in core/ and each one is")
        print("an input that reproduces: build/fuzz_<target> core/crash-<hash>")
        return 1
    print("no crash, leak, timeout or oom artifacts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
