#!/usr/bin/env python3
"""Builds the JNI shared library for the host platform.

    python3 scripts-and-commands/build_jni.py [--out DIR] [--cc gcc] [--quiet]

Three things happen here and the middle one is the point.

1. `javac -h` generates the JNI header from `NativeEngine.java`.
2. The C glue **includes that generated header**, so every native signature is
   checked by the C compiler against the Java declaration. A parameter added,
   removed or retyped on either side is a compile error here rather than an
   `UnsatisfiedLinkError` in production, which is the failure mode hand written JNI
   is notorious for.
3. The core sources and the glue are compiled into one shared library.

The core sources are compiled into the library directly rather than linked from
`libibha_csvdiff.a`, because a shared object needs position independent code and
the static library is not built that way. It is the same set of files the Makefile
builds, listed once here and asserted against the Makefile by the phase check.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
JAVA = ROOT / "java"

# The native build's source set, which is CORE_SRC in core/Makefile with the libc
# system layer. Kept in step with the Makefile by check_jni_sources() below.
CORE_SOURCES = [
    "arena.c", "ctx.c", "reader.c", "ingest.c", "hash.c", "parse.c", "schema.c",
    "normalize.c", "match.c", "diff.c", "validate.c", "segment.c", "emit.c", "sys_libc.c",
]

WARN = [
    "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion", "-Wsign-conversion",
    "-Wstrict-prototypes", "-Wmissing-prototypes", "-Wpointer-arith", "-Wwrite-strings",
]

NATIVE_ENGINE = JAVA / "src/main/java/com/ibhatech/csvdiff/jni/NativeEngine.java"
JNI_GLUE = JAVA / "src/main/c/ibha_csvdiff_jni.c"


def platform_tag() -> str:
    system = platform.system().lower()
    if system == "darwin":
        osname = "darwin"
    elif system.startswith("win") or system.startswith("cygwin") or system.startswith("msys"):
        osname = "windows"
    else:
        osname = "linux"

    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}
    return f"{osname}-{arch.get(machine, machine)}"


def library_name() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "libibha_csvdiff_jni.dylib"
    if system.startswith("win") or system.startswith("cygwin") or system.startswith("msys"):
        return "ibha_csvdiff_jni.dll"
    return "libibha_csvdiff_jni.so"


def java_home() -> Path:
    """Where jni.h lives.

    JAVA_HOME first, then the JDK that owns the javac on PATH, because a machine
    with several JDKs installed usually has JAVA_HOME pointing at the one the user
    means and a javac symlink pointing wherever the package manager put it.
    """
    env = os.environ.get("JAVA_HOME")
    if env and (Path(env) / "include" / "jni.h").is_file():
        return Path(env)

    javac = shutil.which("javac")
    if javac:
        home = Path(javac).resolve().parent.parent
        if (home / "include" / "jni.h").is_file():
            return home

    out = subprocess.run(
        ["java", "-XshowSettings:properties", "-version"],
        capture_output=True, text=True, check=False,
    )
    for line in (out.stderr or "").splitlines():
        if "java.home" in line:
            home = Path(line.split("=", 1)[1].strip())
            if (home / "include" / "jni.h").is_file():
                return home

    sys.exit(
        "error: cannot find jni.h. Set JAVA_HOME to a JDK, not a JRE.\n"
        "       A JRE has no include directory and cannot build a JNI binding."
    )


def jni_includes(home: Path) -> list[str]:
    """The platform include directory is named after the OS, not the target."""
    inc = home / "include"
    candidates = [inc / "linux", inc / "darwin", inc / "win32"]
    args = [f"-I{inc}"]
    for c in candidates:
        if c.is_dir():
            args.append(f"-I{c}")
    return args


def check_jni_sources() -> None:
    """The Makefile is the authority on which sources make up the engine."""
    text = (CORE / "Makefile").read_text(encoding="utf-8")
    start = text.index("CORE_SRC :=")
    end = text.index("CORE_OBJ", start)
    declared = {
        part.strip().removeprefix("src/")
        for part in text[start:end].replace("CORE_SRC :=", "").replace("\\", " ").split()
        if part.strip().endswith(".c")
    }
    ours = set(CORE_SOURCES)
    if declared != ours:
        sys.exit(
            "error: build_jni.py and core/Makefile disagree about the engine's sources.\n"
            f"       only in the Makefile: {sorted(declared - ours)}\n"
            f"       only here:            {sorted(ours - declared)}"
        )


def run(cmd: list[str], quiet: bool) -> None:
    if not quiet:
        print("  " + " ".join(cmd))
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(JAVA / "target/native"),
                    help="directory to write <platform>/<library> into")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    check_jni_sources()
    home = java_home()
    tag = platform_tag()
    out_dir = Path(args.out) / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / library_name()

    with tempfile.TemporaryDirectory(prefix="ibha-jni-") as tmp:
        gen = Path(tmp) / "include"
        classes = Path(tmp) / "classes"

        # 1. The generated header, which is what makes the two sides agree.
        run(["javac", "--release", "21", "-h", str(gen), "-d", str(classes),
             str(NATIVE_ENGINE)], args.quiet)

        # 2. and 3. The engine and the glue, one shared library.
        cmd = [args.cc, "-std=c11", *WARN, "-O3", "-fno-strict-aliasing", "-fPIC", "-shared",
               f"-I{CORE / 'include'}", f"-I{gen}", *jni_includes(home)]
        if platform.system().lower() == "darwin":
            cmd += ["-undefined", "dynamic_lookup"]
        cmd += [str(CORE / "src" / name) for name in CORE_SOURCES]
        cmd += [str(JNI_GLUE), "-o", str(out_file)]
        run(cmd, args.quiet)

    size = out_file.stat().st_size
    print(f"  jni library: {out_file} ({size} bytes, {tag})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
