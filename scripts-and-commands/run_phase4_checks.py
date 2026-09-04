#!/usr/bin/env python3
"""Run the whole Phase 4 verification gate and report what passed.

    python3 scripts-and-commands/run_phase4_checks.py
    python3 scripts-and-commands/run_phase4_checks.py --quick   # skip the 15 MB pair
    python3 scripts-and-commands/run_phase4_checks.py --c-only

The Phase 3 gate in full, then everything the JS binding adds:

  - **the determinism check of spec 3.2**, which is the one that gates the rest:
    the native build, the scalar wasm build and the SIMD wasm build must produce
    byte identical emitter output for the same inputs. wasm_smoke.mjs proves the
    engine runs on the target and compares nothing;
  - the generated wasm32 struct offsets are regenerated and diffed, because a
    struct that moves without src/abi.ts moving with it is not a compile error,
    it is a binding that reads n_columns out of the middle of a pointer;
  - the binding's own suite, which drives both wasm builds, the worker and the
    emitters, and which checks the HTML safety invariant with a checker written
    independently of the emitter.

**The JS steps need no npm install.** They run the TypeScript sources directly
under node, which is why every relative import in this package names the .ts file
it resolves to. That keeps the JS half of the gate as self contained as the C
half; `pnpm typecheck` and `pnpm build` are the two things that do need the
toolchain, and they are reported as skipped rather than failed when it is absent.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
JS = ROOT / "js"
SCRIPTS = ROOT / "scripts-and-commands"


def js_steps(quick: bool):
    """(label, argv, cwd, required)."""
    determinism = ["node", str(SCRIPTS / "check_determinism.mjs")]
    if quick:
        determinism.append("--quick")

    have_pnpm = shutil.which("pnpm") is not None
    node_modules = (JS / "node_modules").is_dir()

    steps = [
        ("wasm modules copied into the package",
         ["node", str(JS / "scripts" / "copy-wasm.mjs")], JS, True),
        ("wasm32 struct offsets are current",
         ["node", str(SCRIPTS / "gen_abi.mjs"), "--check"], ROOT, True),
        # Java joins this comparison as a fourth producer when the binding has
            # been built; the check says which it compared.
            ("determinism, native against the wasm builds and java when built",
             determinism, ROOT, True),
        ("binding suite", ["node", "--test", "src/*.test.ts"], JS / "packages" / "core", True),
    ]
    # The published artifact is JavaScript with declaration files, and only tsc
    # produces those. Running from source proves the code works; it does not
    # prove the package builds, so this stays in the gate and reports honestly
    # when the toolchain is not installed.
    steps.append(
        ("typescript typecheck", ["pnpm", "-r", "typecheck"], JS, have_pnpm and node_modules)
    )
    steps.append(("package build", ["pnpm", "-r", "build"], JS, have_pnpm and node_modules))
    return steps


def run(label: str, argv: list, cwd: pathlib.Path, required: bool) -> tuple:
    try:
        proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError as err:
        return ("skipped", label, f"{err}")
    out = proc.stdout + proc.stderr
    if proc.returncode == 0:
        return ("ok", label, out)
    return ("FAILED" if required else "skipped", label, out)


def summarize(label: str, out: str) -> str:
    keep = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("all ") and "identical" in s:
            keep.append(s)
        elif s.startswith("FAILED") or s.startswith("FAIL ") or s.startswith("error:"):
            keep.append(s)
        elif s.startswith("ℹ pass") or s.startswith("ℹ fail") or s.startswith("ℹ tests"):
            keep.append(s.replace("ℹ ", ""))
        elif "entries" in s and "abi.ts" in s:
            keep.append(s)
        elif s.endswith("KB") and label.startswith("wasm modules"):
            keep.append(s)
    return "; ".join(keep[:6])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true",
                    help="skip the 15 MB pair in the determinism check")
    ap.add_argument("--c-only", action="store_true", help="run only the Phase 3 gate")
    ap.add_argument("--js-only", action="store_true", help="skip the Phase 3 gate")
    ap.add_argument("--fuzz-iters", type=int, default=200000)
    args = ap.parse_args()

    failures = 0

    if not args.js_only:
        # Flushed, because the subprocess writes straight to the same terminal
        # and an unflushed header lands after the output it introduces.
        print("== the engine, Phase 3 gate ==", flush=True)
        rc = subprocess.run(
            ["python3", str(SCRIPTS / "run_phase3_checks.py"), "--fuzz-iters", str(args.fuzz_iters)],
            cwd=ROOT,
        ).returncode
        if rc != 0:
            failures += 1
        print()

    if args.c_only:
        print("gate failed" if failures else "gate passed")
        return 1 if failures else 0

    print("== the binding, Phase 4 ==", flush=True)
    for label, argv, cwd, required in js_steps(args.quick):
        status, name, out = run(label, argv, cwd, required)
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
