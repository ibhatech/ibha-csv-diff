#!/usr/bin/env python3
"""Measure what the JSON row source costs, against a document tree of the same array.

    python3 scripts-and-commands/measure_json_rows.py [rows]

The claim under test is the one in JsonRows: a row array of any size costs one row
of live data, where parsing the same document into Map and List objects allocates
millions of them. So this does not measure time alone. It runs each mode at
ascending heap sizes and reports the smallest one that completes, which is the
figure that decides how many concurrent diffs fit on a batch worker.

Writes its generated array under java/target/, which is build output, and reads
nothing outside this repository.
"""
import pathlib
import platform
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA = ROOT / "java"
OUT = JAVA / "target" / "json-bench"

HEAPS = ["16m", "32m", "64m", "128m", "256m", "512m", "1024m", "2048m"]


def java_flags() -> list:
    """--enable-native-access exists from JDK 24 and not before."""
    out = subprocess.run(["java", "-XshowSettings:properties", "-version"],
                         capture_output=True, text=True, check=False)
    for line in (out.stderr or "").splitlines():
        if "java.specification.version" in line:
            try:
                if int(line.split("=", 1)[1].strip()) >= 24:
                    return ["--enable-native-access=ALL-UNNAMED"]
            except ValueError:
                pass
    return []


def generate(rows: int) -> pathlib.Path:
    """An array of objects with the five columns JsonBenchMain declares."""
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"rows_{rows}.json"
    if path.is_file():
        return path
    regions = ("NE", "SE", "NW", "SW", "MW")
    with path.open("w", encoding="utf-8") as f:
        f.write("[")
        for i in range(rows):
            if i:
                f.write(",\n ")
            f.write(
                '{"id":"%d","name":"account %d","region":"%s","premium":"%d.%02d",'
                '"updated":"2026-01-%02dT09:%02d:00"}'
                % (i, i, regions[i % 5], 100 + i % 9000, i % 100, 1 + i % 28, i % 60))
        f.write("]\n")
    return path


def run(mode: str, path: pathlib.Path, heap: str, flags: list) -> tuple:
    """@return (ok, one line of output or the failure)"""
    argv = ["java", f"-Xmx{heap}", *flags,
            f"-Dibha.csvdiff.nativeLibPath={JAVA / 'target' / 'native'}",
            "-cp", f"{JAVA / 'target' / 'classes'}:{JAVA / 'target' / 'test-classes'}",
            "com.ibhatech.csvdiff.JsonBenchMain", mode, str(path)]
    done = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, check=False)
    if done.returncode == 0:
        return True, done.stdout.strip()
    text = (done.stderr or "") + (done.stdout or "")
    reason = "OutOfMemoryError" if "OutOfMemoryError" in text else text.strip().splitlines()[:1]
    return False, reason if isinstance(reason, str) else (reason[0] if reason else "failed")


def smallest_heap(mode: str, path: pathlib.Path, flags: list) -> None:
    for heap in HEAPS:
        ok, message = run(mode, path, heap, flags)
        if ok:
            print(f"  -Xmx{heap:<6} {message}")
            return
        print(f"  -Xmx{heap:<6} {mode}  {message}")
    print(f"  {mode}: did not complete at {HEAPS[-1]}")


def main() -> int:
    rows = int(sys.argv[1]) if len(sys.argv) > 1 else 150_000
    if shutil.which("mvn") is None:
        print("error: maven is not installed")
        return 2
    if subprocess.run(["mvn", "-q", "-B", "test-compile"], cwd=JAVA).returncode != 0:
        return 1

    path = generate(rows)
    size = path.stat().st_size
    flags = java_flags()

    print(f"# {platform.system().lower()}, {rows:,} rows, {size / 1e6:.1f} MB of JSON")
    print("# the smallest heap each one completes at, both sides for stream, one for tree")
    smallest_heap("stream", path, flags)
    smallest_heap("tree", path, flags)
    return 0


if __name__ == "__main__":
    sys.exit(main())
