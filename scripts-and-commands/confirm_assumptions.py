#!/usr/bin/env python3
"""Show what the five flagged assumptions actually do, so they can be confirmed.

Group B of specs/03-remaining-tasks.md. Each of the five is implemented, has never
been confirmed against real data, and fails silently: the diff returns a wrong
answer that looks like a right answer.

This script builds the JNI library if it is missing, then runs
com.ibhatech.csvdiff.AssumptionsMain, which drives the real engine through the real
binding on the smallest input that makes each rule observable and prints what
happened beside what the alternative would have produced.

It decides nothing. Each of the five ends in a question whose answer is a fact
about the data a deployment really holds, and that fact is not in this repository.

Run:
    python3 scripts-and-commands/confirm_assumptions.py            # all five
    python3 scripts-and-commands/confirm_assumptions.py b1 b2      # only some
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA = ROOT / "java"
CLASSES = JAVA / "target" / "classes"
TEST_CLASSES = JAVA / "target" / "test-classes"
NATIVE = JAVA / "target" / "native"

VALID = ("b1", "b2", "b3", "b4", "b5")


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


def build() -> bool:
    """The classes and the native library, only when something is missing."""
    if not any(NATIVE.rglob("*ibha_csvdiff_jni*")):
        print("building the jni library ...")
        done = subprocess.run([sys.executable, str(ROOT / "scripts-and-commands" / "build_jni.py")],
                              cwd=ROOT, check=False)
        if done.returncode != 0:
            print("error: the jni library did not build")
            return False

    if not (TEST_CLASSES / "com" / "ibhatech" / "csvdiff" / "AssumptionsMain.class").exists():
        if shutil.which("mvn") is None:
            print("error: maven is not installed and the classes are not built")
            return False
        print("compiling the java classes ...")
        done = subprocess.run(
            ["mvn", "-q", "-DskipTests", "-DskipNative=true", "test-compile"],
            cwd=JAVA, check=False)
        if done.returncode != 0:
            print("error: the java classes did not compile")
            return False
    return True


def main() -> int:
    want = [a.lower() for a in sys.argv[1:]] or list(VALID)
    for one in want:
        if one not in VALID:
            print(f"error: unknown assumption {one}; expected one of {', '.join(VALID)}")
            return 2

    if not build():
        return 1

    argv = ["java", *java_flags(),
            f"-Dibha.csvdiff.nativeLibPath={NATIVE}",
            "-cp", f"{CLASSES}:{TEST_CLASSES}",
            "com.ibhatech.csvdiff.AssumptionsMain", *want]

    print()
    print("The five flagged assumptions, run against the real engine.")
    print("Each ends in a question. The answers are decisions, and they are Manas's.")
    # Flush, or this header lands after the child's output rather than before it.
    sys.stdout.flush()
    done = subprocess.run(argv, cwd=ROOT, check=False)
    if done.returncode != 0:
        print(f"\nerror: the demonstration exited {done.returncode}")
        return done.returncode

    print()
    print("=" * 78)
    print("Record the answers in specs/03-remaining-tasks.md group B, then do B6:")
    print("move each confirmed assumption into specs/02-solution-proposal.md as a")
    print("decision, so the next handoff does not carry the same five items.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
