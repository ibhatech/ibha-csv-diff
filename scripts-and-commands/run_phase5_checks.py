#!/usr/bin/env python3
"""Run the whole Phase 5 verification gate and report what passed.

    python3 scripts-and-commands/run_phase5_checks.py
    python3 scripts-and-commands/run_phase5_checks.py --quick   # skip the 15 MB pair
    python3 scripts-and-commands/run_phase5_checks.py --view-only

The Phase 4 gate in full, then everything the view packages add:

  - **the shipped CSS is what the generator produces**, the same argument
    gen_abi.mjs makes about the struct offsets: a bundler imports a file and the
    component calls a function, so both exist, and only one of them is checked by
    anything unless it is checked here;
  - the headless suite, which includes the **parity test**: the HTML emitter's
    saved report and the view's own layout of the same rows, compared row for row
    and cell for cell over the real engine at the p90. Spec 13.3 says the two
    agree by construction because they consume the same cursor, and that is a
    claim about a design rather than about the two decode paths, which are
    separate code in two languages;
  - the React suite, which renders the XSS corpus through the real component and
    runs an independently written safety checker over the markup.

**Build runs before typecheck, which is not the Phase 4 order.** `dist` is
gitignored and the view package's declarations are what the React package
typechecks against, so on a clean checkout typecheck has nothing to read until
the build has produced it. Reversing the two would report a missing module as a
type error.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JS = ROOT / "js"
SCRIPTS = ROOT / "scripts-and-commands"


def js_steps(quick: bool, view_only: bool):
    """(label, argv, cwd, required)."""
    have_pnpm = shutil.which("pnpm") is not None
    node_modules = (JS / "node_modules").is_dir()
    installed = have_pnpm and node_modules

    steps = []
    if not view_only:
        determinism = ["node", str(SCRIPTS / "check_determinism.mjs")]
        if quick:
            determinism.append("--quick")
        steps += [
            ("wasm modules copied into the package",
             ["node", str(JS / "scripts" / "copy-wasm.mjs")], JS, True),
            ("wasm32 struct offsets are current",
             ["node", str(SCRIPTS / "gen_abi.mjs"), "--check"], ROOT, True),
            # Java joins this comparison as a fourth producer when the binding has
            # been built; the check says which it compared.
            ("determinism, native against the wasm builds and java when built",
             determinism, ROOT, True),
            ("binding suite",
             ["node", "--test", "src/*.test.ts"], JS / "packages" / "core", True),
        ]

    steps += [
        ("shipped stylesheets are current",
         ["node", str(SCRIPTS / "gen_stylesheet.mjs"), "--check"], ROOT, True),
        # These two need vitest, and vitest needs an install. They are reported as
        # skipped rather than failed when it is absent, exactly as the typecheck
        # and the build are, because the alternative is a gate that cannot be run
        # on a fresh checkout at all.
        ("view suite, including emitter parity",
         ["pnpm", "test"], JS / "packages" / "view", installed),
        ("react suite, including the XSS corpus",
         ["pnpm", "test"], JS / "packages" / "react", installed),
        ("package build", ["pnpm", "-r", "build"], JS, installed),
        ("typescript typecheck", ["pnpm", "-r", "typecheck"], JS, installed),
    ]
    return steps


def run(label: str, argv: list, cwd: pathlib.Path, required: bool) -> tuple:
    if not required:
        return ("skipped", label, "not installed")
    try:
        proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError as err:
        return ("skipped", label, f"{err}")
    out = proc.stdout + proc.stderr
    if proc.returncode == 0:
        return ("ok", label, out)
    return ("FAILED", label, out)


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
        elif s.startswith("Tests ") or s.startswith("Test Files "):
            keep.append(s)
        elif "stylesheets are current" in s:
            keep.append(s)
        elif "entries" in s and "abi.ts" in s:
            keep.append(s)
    return "; ".join(keep[:6])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true",
                    help="skip the 15 MB pair in the determinism check")
    ap.add_argument("--c-only", action="store_true", help="run only the Phase 3 gate")
    ap.add_argument("--js-only", action="store_true", help="skip the C engine gate")
    ap.add_argument("--view-only", action="store_true",
                    help="skip the engine and the binding, run only Phase 5")
    ap.add_argument("--fuzz-iters", type=int, default=200000)
    args = ap.parse_args()

    failures = 0

    if not args.js_only and not args.view_only:
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

    print("== the binding and the view, Phases 4 and 5 ==", flush=True)
    for label, argv, cwd, required in js_steps(args.quick, args.view_only):
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
