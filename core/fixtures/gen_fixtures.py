#!/usr/bin/env python3
"""Generate benchmark and golden fixtures for ibha-csvdiff.

Writes into fixtures/generated/, which is gitignored: these files are large and
reproducible, so they are built on demand rather than committed.

    python3 fixtures/gen_fixtures.py            # small + the 15 MB p90 pair
    python3 fixtures/gen_fixtures.py --all      # adds the 50 MB and 150 MB cases

The pair generator applies a known edit script to the source and writes the
script alongside the target as JSON. Phase 2 asserts the diff recovers exactly
that script, which is the property test described in spec section 10.
"""
import argparse
import json
import pathlib
import random

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "generated"

# The four header rows from the spec: KEY markers, REQUIRED markers, declared
# types, then column names.
COLUMNS = [
    ("account_id", "KEY", "REQUIRED", "VARCHAR(20)"),
    ("period", "KEY", "REQUIRED", "CHAR(7)"),
    ("customer_name", "", "REQUIRED", "VARCHAR(60)"),
    ("region", "", "REQUIRED", "VARCHAR(20)"),
    ("premium_amount", "", "REQUIRED", "DECIMAL(12,2)"),
    ("currency", "", "REQUIRED", "CHAR(3)"),
    ("status", "", "", "VARCHAR(12)"),
    ("term_months", "", "", "INTEGER"),
    ("notes", "", "", "VARCHAR(200)"),
    ("is_active", "", "", "BOOLEAN"),
    ("commission_rate", "", "", "DECIMAL(5,3)"),
    ("effective_date", "", "", "DATE"),
]

REGIONS = ["NORTH", "SOUTH", "EAST", "WEST", "CENTRAL"]
STATUSES = ["ACTIVE", "PENDING", "LAPSED", "CANCELLED"]
# Names chosen to exercise the quoting and escaping paths: an embedded comma, an
# embedded double quote, and an embedded newline.
NAMES = [
    "Alice Anderson",
    "Smith, John",
    'O"Brien Holdings',
    "Multi\nLine Corp",
    "Plain Trading Co",
]


# Cell values that must not become live markup when rendered. Kept as data here
# and escaped into valid CSV by the writer.
XSS_ROW = [
    "<script>alert(1)</script>",
    "2026-01",
    '" onload="x',
    "NORTH",
    "1.00",
    "USD",
    "ACTIVE",
    "6",
    "javascript:alert(1)",
    "TRUE",
    "0.010",
    "2026-01-15",
]


def csv_escape(value: str) -> str:
    if any(c in value for c in ',"\n\r'):
        return '"' + value.replace('"', '""') + '"'
    return value


def header_rows() -> list[str]:
    rows = []
    for idx in (1, 2, 3, 0):  # KEY row, REQUIRED row, type row, name row
        cells = [c[idx] for c in COLUMNS]
        rows.append(",".join(csv_escape(x) for x in cells))
    return rows


def make_row(rng: random.Random, i: int) -> list[str]:
    return [
        f"ACC-{i:08d}",
        f"2026-{(i % 12) + 1:02d}",
        NAMES[i % len(NAMES)],
        REGIONS[i % len(REGIONS)],
        f"{rng.randint(10000, 999999) / 100:.2f}",
        "USD",
        STATUSES[i % len(STATUSES)],
        str(rng.choice([6, 12, 24, 36])),
        "routine renewal" if i % 3 else "escalated, see file",
        "TRUE" if i % 2 else "FALSE",
        f"{rng.randint(10, 150) / 1000:.3f}",
        f"2026-{(i % 12) + 1:02d}-15",
    ]


def write_csv(path: pathlib.Path, rows: list[list[str]]) -> int:
    with path.open("w", encoding="utf-8", newline="") as f:
        for line in header_rows():
            f.write(line + "\n")
        for row in rows:
            f.write(",".join(csv_escape(c) for c in row) + "\n")
    return path.stat().st_size


def edit_cell(rng: random.Random, row: list[str], col: int) -> str:
    """A new value for one cell that is guaranteed to differ *in value*.

    "In value" matters: the declared types select the comparators of spec 5.3, so
    rewriting 1.50 as 1.500 is not an edit at all and recording it as one would
    make the property test assert something false.
    """
    before = row[col]
    if col == 4:  # premium_amount, DECIMAL(12,2)
        return f"{float(before) + 1.5:.2f}"
    if col == 10:  # commission_rate, DECIMAL(5,3)
        return f"{min(float(before) + 0.25, 9.999):.3f}"
    if col == 6:  # status, VARCHAR(12)
        return "CANCELLED" if before != "CANCELLED" else "REINSTATED"
    return "amended, reviewed" if before != "amended, reviewed" else "amended twice"


def apply_edits(rng: random.Random, rows: list[list[str]]) -> tuple[list[list[str]], dict]:
    """Applies a known edit script and returns (target rows, script).

    Spec section 10 wants the diff to recover exactly this script, so the edits
    are kept disjoint: no row is modified and then deleted, no added row is then
    removed again, and the moved row is neither modified nor deleted. An
    overlapping script would be a fixture that no correct diff could reproduce.

    Each entry carries the row's key rather than its position, because position
    is exactly what the edits change.
    """
    n = len(rows)
    touch = max(1, n // 100)  # roughly 1% of rows, which is realistic
    n_mod = touch
    n_del = max(1, touch // 4)
    n_add = max(1, touch // 4)

    # The moved row is taken from the middle of the file. At the very front the
    # longest increasing subsequence would have two equally long solutions and
    # "which row moved" would stop being a single well defined answer.
    move_idx = rng.randrange(n // 3, (2 * n) // 3) if n >= 9 else n - 1

    pool = [i for i in range(n) if i != move_idx]
    picks = rng.sample(pool, min(n_mod + n_del, len(pool)))
    mod_idx = sorted(picks[:n_mod])
    del_idx = set(picks[n_mod:])

    script: dict = {"modified": [], "added": [], "deleted": [], "moved": []}

    # Carry the source index alongside each row so the edits stay attributable
    # while positions move underneath them.
    target: list[list] = [[i, list(rows[i])] for i in range(n)]

    for i in mod_idx:
        col = rng.choice([4, 6, 8, 10])  # amount, status, notes, rate
        target[i][1][col] = edit_cell(rng, target[i][1], col)
        script["modified"].append(
            {"key": [rows[i][0], rows[i][1]], "column": COLUMNS[col][0]}
        )

    for i in sorted(del_idx):
        script["deleted"].append({"key": [rows[i][0], rows[i][1]]})
    target = [t for t in target if t[0] not in del_idx]

    # A single row moved to the front, which is the case a naive move detector
    # reports as "every row moved". See spec 6.2.
    pos = next(k for k, t in enumerate(target) if t[0] == move_idx)
    moved = target.pop(pos)
    target.insert(0, moved)
    script["moved"].append({"key": [moved[1][0], moved[1][1]]})

    for k in range(n_add):
        row = make_row(rng, 900000 + k)
        target.insert(rng.randrange(len(target) + 1), [None, row])
        script["added"].append({"key": [row[0], row[1]]})

    return [t[1] for t in target], script


def script_lines(script: dict) -> list[str]:
    """The script as sorted tab separated tuples.

    The C property test reads this rather than the JSON: the shape is fixed, and
    a JSON parser in the test suite would be a second thing to get right for no
    benefit. Both sides sort the same tuples and compare them element by element.
    """
    lines = []
    for e in script["modified"]:
        lines.append("M\t%s\t%s\t%s" % (e["key"][0], e["key"][1], e["column"]))
    for e in script["added"]:
        lines.append("A\t%s\t%s\t" % (e["key"][0], e["key"][1]))
    for e in script["deleted"]:
        lines.append("D\t%s\t%s\t" % (e["key"][0], e["key"][1]))
    for e in script["moved"]:
        lines.append("V\t%s\t%s\t" % (e["key"][0], e["key"][1]))
    return sorted(lines)


def build_pair(label: str, target_mb: int, seed: int = 1234) -> None:
    rng = random.Random(seed)

    # Measure the real average row width from a sample rather than guessing a
    # constant, so the generated file actually lands on the requested size.
    sample = [make_row(random.Random(seed), i) for i in range(1000)]
    avg = sum(len(",".join(csv_escape(c) for c in r)) + 1 for r in sample) / len(sample)
    n_rows = max(4, int((target_mb * 1024 * 1024) / avg))

    rows = [make_row(rng, i) for i in range(n_rows)]
    src = OUT / f"{label}_source.csv"
    tgt = OUT / f"{label}_target.csv"

    src_bytes = write_csv(src, rows)
    target_rows, script = apply_edits(rng, rows)
    tgt_bytes = write_csv(tgt, target_rows)

    (OUT / f"{label}_edits.json").write_text(json.dumps(script, indent=2), encoding="utf-8")
    with (OUT / f"{label}_edits.tsv").open("w", encoding="utf-8", newline="\n") as f:
        for line in script_lines(script):
            f.write(line + "\n")
    print(f"  {label:10s} {n_rows:>9,} rows  source {src_bytes/1048576:6.2f} MB  target {tgt_bytes/1048576:6.2f} MB")


def build_edge_cases() -> None:
    """Small hand shaped files covering the parser quirks from spec 5.1."""
    cases = {
        "empty.csv": "",
        "header_only.csv": "\n".join(header_rows()) + "\n",
        "no_trailing_newline.csv": "\n".join(header_rows()) + "\nACC-1,2026-01,X,NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15",
        "crlf.csv": "\r\n".join(header_rows()) + "\r\nACC-1,2026-01,X,NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15\r\n",
        "mixed_endings.csv": header_rows()[0] + "\r\n" + header_rows()[1] + "\n" + header_rows()[2] + "\r" + header_rows()[3] + "\n",
        "bom.csv": "﻿" + "\n".join(header_rows()) + "\n",
        "multiline_quoted.csv": "\n".join(header_rows()) + "\nACC-1,2026-01,\"Line one\nLine two\",NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15\n",
        "escaped_quotes.csv": "\n".join(header_rows()) + "\nACC-1,2026-01,\"O\"\"Brien, Ltd\",NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15\n",
        "ragged.csv": "\n".join(header_rows()) + "\nACC-1,2026-01,X\nACC-2,2026-01,Y,NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15,EXTRA\n",
        "names_only_header.csv": header_rows()[3] + "\nACC-1,2026-01,X,NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15\n",
        # Spec 13.3: the HTML emitter must escape these. A cell that becomes live
        # markup is stored XSS against the approver. The row is built through
        # csv_escape so the file is valid RFC 4180: the payload has to survive
        # parsing to reach the emitter that must neutralize it, and a file that
        # fails to parse tests nothing.
        "xss.csv": header_rows()[3] + "\n" + ",".join(csv_escape(c) for c in XSS_ROW) + "\n",
        "latin1.csv": None,  # written as bytes below
    }
    for name, body in cases.items():
        if body is None:
            continue
        # Explicit open rather than Path.write_text(newline=...), which needs
        # Python 3.10. These fixtures carry deliberate CR, LF and CRLF endings, so
        # newline="" is not optional: universal newline translation would rewrite
        # exactly the bytes the parser tests are about.
        with (OUT / name).open("w", encoding="utf-8", newline="") as f:
            f.write(body)

    # Not valid UTF-8. The engine is byte oriented and must not reject it.
    (OUT / "latin1.csv").write_bytes(
        header_rows()[3].encode("utf-8") + b"\nACC-1,2026-01,Caf\xe9 Ltd,NORTH,1.00,USD,ACTIVE,6,n,TRUE,0.010,2026-01-15\n"
    )
    print(f"  edge cases {len(cases):>9,} files")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="also build the 50 MB and 150 MB pairs")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    print(f"writing fixtures to {OUT}")

    build_edge_cases()
    build_pair("tiny", 1)
    build_pair("p90", 15)
    if args.all:
        build_pair("large", 50)
        build_pair("max", 150)

    print("done")


if __name__ == "__main__":
    main()
