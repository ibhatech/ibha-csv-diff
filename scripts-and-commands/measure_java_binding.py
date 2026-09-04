#!/usr/bin/env python3
"""Measure what the Java binding costs on the p90 pair.

    python3 scripts-and-commands/measure_java_binding.py

Regenerates the cost table in java/README.md rather than having anyone edit it by
hand. The counterpart for the JS binding is measure_binding.mjs, and the two are
deliberately measuring the same things in the same order so the tables can be read
side by side.
"""
import pathlib
import platform
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA = ROOT / "java"
FIXTURES = ROOT / "core" / "fixtures" / "generated"


def platform_tag() -> str:
    system = platform.system().lower()
    osname = "darwin" if system == "darwin" else "windows" if system.startswith("win") else "linux"
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}
    return f"{osname}-{arch.get(machine, machine)}"


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


def main() -> int:
    if not (FIXTURES / "p90_source.csv").is_file():
        print("error: the p90 fixtures are missing. Run python3 core/fixtures/gen_fixtures.py")
        return 2
    if shutil.which("mvn") is None:
        print("error: maven is not installed")
        return 2

    if subprocess.run(["mvn", "-q", "-B", "test-compile"], cwd=JAVA).returncode != 0:
        return 1

    classes = JAVA / "target" / "classes"
    test_classes = JAVA / "target" / "test-classes"
    native = JAVA / "target" / "native"

    argv = ["java", *java_flags(), f"-Dibha.csvdiff.nativeLibPath={native}",
            "-cp", f"{classes}:{test_classes}",
            "com.ibhatech.csvdiff.tools.BenchMain", str(FIXTURES)]

    print(f"# {platform_tag()}, JNI backend, p90 pair (15 MB a side)")
    return subprocess.run(argv, cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
