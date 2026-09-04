# Handoff: Phase 2 complete

Date: 2026-08-12
Next phase: Phase 3, the emitters and the output layer

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier in that document where they conflict. 13.3 is Phase 3's specification |
| `specs/02-solution-proposal.md` sections 7, 8 | Cell level intra text diffing and the view component, which are what Phase 3 and 4 build |
| `core/include/ibha_csvdiff.h` | The ABI. The diff, the cursor and the report row are now concrete public types |
| `docs/HANDOFF-phase1.md` sections 4 and 7 | The invariants and the two flagged assumptions, both still open |
| `core/README.md` | Build instructions, the engine in one paragraph, and the measured cost of normalization |

## 2. The headline result

**The diff recovers the generator's edit script exactly.** Spec section 10 asked
for this and it is now assertion 320: the fixture generator applies 2,198 known
edits to a 14.95 MB, 146,580 row file, and the engine reproduces that script with
nothing extra and nothing missing, including reporting exactly **one** moved row
rather than the 146,000 a naive move detector would report.

```
unchanged  144749      parse   0.113 s
modified     1465      match   0.014 s
added         366      cursor  0.002 s
deleted       366
moved           1      232 MB/s end to end
```

Matching 146,580 rows against 146,580 rows takes **14 ms** and draining the whole
report through the cursor takes **1.6 ms**. The diff is not the expensive part of
a diff; the parse is, and Phase 7 should aim there.

## 3. What is done and verified

Run on x86-64 Linux, GCC 11.5. Every step is in
`scripts-and-commands/run_phase2_checks.py`, and every step passes.

- **321 assertions pass**, up from 200. `make test`
- **Clean under AddressSanitizer, LeakSanitizer and UBSan.** `make asan`
- **Clean under trapping UndefinedBehaviorSanitizer.** `make ubsan`
- **Clean under valgrind memcheck.** `make valgrind`
- **3 million fuzz inputs each on the parser, the matcher and the ingest path**,
  no invariant violated. `make fuzz-native FUZZ_ITERS=3000000`
- **Compiles warning free** under the same strict flag set, tests included
- **CLI diffs the 15 MB pair end to end** and returns diff(1) exit codes

**LeakSanitizer found one leak the moment it could run, and it is worth knowing
why it had been missed.** `test_index_memory` in `tests/test_property.c` never
freed its fixture buffer, a Phase 1 defect in the harness rather than the engine.
It went unnoticed because **LeakSanitizer is off by default on macOS**, where
Phase 1's `make asan` ran, and on by default on Linux. Fixed. The lesson for CI
is that "ASan is clean" means different things on the two platforms, so the
Linux run is the one to gate on.

### New in `core/`

| File | Contents |
|---|---|
| `src/normalize.c` | Declared type parsing, the canonical forms, the comparators of spec 5.3 |
| `src/match.c` | Bucket map, keyed matching, duplicate keys, the all-keys path, move detection |
| `src/diff.c` | Diff driver, anchored report ordering, the pull cursor |
| `tests/diffkit.h` | Scaffolding: parse a pair from string literals and render the report as letters |
| `tests/test_normalize.c` | Each comparator, and that the comparator and the digest never disagree |
| `tests/test_match.c` | Matching, duplicate keys, move detection, the all-keys path |
| `tests/test_diff.c` | The cursor contract, cell flags, suppression counting, bounded memory |
| `tests/fuzz_diff.c` | Fuzzes the matcher, asserting the report is a permutation of the inputs |
| `scripts-and-commands/run_phase2_checks.py` | The whole verification gate in one command |

`cli/main.c` gains `diff`. `fixtures/gen_fixtures.py` now applies a *disjoint*
edit script and writes it as a sorted TSV beside the JSON.

## 4. The one coupling everything rests on

Handoff 1 section 5 point 1 said this was the most important thing to get right,
so it is stated here as the invariant it became.

**The row digests are folded from the same normalized bytes the comparators
walk.** `ibha_normalize` produces one byte sequence per cell, and exactly two
primitives consume it: `ibha_norm_cmp` and `ibha_norm_hash`, which are the Phase 1
logical comparator and the Phase 1 logical hash unchanged. There is no second
implementation of equality to keep in step.

That is what makes the fast path of spec 6.1 step 3 sound. A matched row whose
full digests agree is called unchanged and its cells are never read. Had the
digest stayed byte exact while the comparators normalized, every row whose only
edit was `1.50` becoming `1.5` would be reported modified by the fast path and
then found identical by the cell walk behind it: self contradictory output.

It is asserted directly. `tests/test_normalize.c` compares each pair *and* their
row digests in the same assertion, and a comparator that is right while the
digest disagrees is a failure rather than a pass.

Two consequences the next phase inherits:

- **`ibha_csvd_table` carries a `compare_id`**, a digest of the comparison
  settings plus the schema facts the comparators depend on. `ibha_csvd_diff_run`
  refuses a pair whose stamps disagree. Comparing digests computed under
  different normalization would not fail, it would produce a confidently wrong
  report, so it is checked rather than trusted.
- **`row_raw_hash` is a third digest**, the same fold over un-normalized values.
  Equal full hashes with unequal raw hashes means "these rows are equal only
  because normalization suppressed something", which spec 5.3 requires be
  counted. Without it, finding suppressed cells would mean walking every
  unchanged row.

## 5. Decisions made in this phase, and why

Each of these is a place where the spec left room and the choice is worth
knowing before changing it.

1. **Numeric comparison is canonical decimal text, not a scaled `int64`.**
   Spec 5.3 suggested a fixed point integer at the declared scale with a
   normalized string fallback beyond `int64` range. This does the fallback's job
   for every value: one code path, no range limit, and `1.555` stays distinct
   from `1.554`, which truncating to `DECIMAL(12,2)` would have merged. The
   declared scale takes no part in equality; it is what a later phase's precision
   violation is measured against. Exponent notation is handled, so Excel's
   `1.23457E+14` equals `123457000000000`. A value whose canonical form exceeds
   48 bytes falls back to trimmed byte comparison.

2. **`moved` is a flag, not a row kind.** A row can move and be modified in one
   edit; a single enum would lose one of the two facts. `ibha_csvd_row` carries
   `kind` in {UNCHANGED, MODIFIED, ADDED, DELETED} plus `moved` and
   `move_distance`.

3. **`moveDistance` is the shift in rank among matched rows**, not the difference
   of raw row numbers, so rows either side of a deletion are not all reported as
   having drifted by one.

4. **Duplicate keys are an error only when a key is declared.** With no KEY
   column every column is the key, so identical rows are legitimate and their
   multiplicity is exactly what spec 6.4 stage 1 has to preserve. Firing spec
   13.9 there would reject valid files.

5. **The similarity threshold is an integer percentage.** No floating point
   anywhere in the engine, because spec 3.2 requires the wasm32 and native builds
   to produce byte identical output and a fraction comparison is the obvious
   place for them to stop doing so.

6. **Row counts are final when `diff_run` returns; cell counts are not.** The
   matcher answers added, deleted, modified, unchanged and moved from the digests
   without reading a cell. Which cells changed is the work the cursor exists to
   defer, so `cells_changed` and `cells_suppressed` accumulate as the cursor
   advances and are final once it is drained. This is documented on the struct.

7. **Hash collisions.** Key hash matches are verified against the key bytes, so a
   collision costs a comparison and never a wrong pairing, per spec 6.1. Full row
   hash equality is *not* verified, because verifying it is precisely the cell
   walk the fast path exists to avoid. At 64 bits over 300,000 rows the collision
   probability is about 2.4e-9. This is the one place the engine trusts a digest.

## 6. What Phase 3 needs to know

1. **The cursor is the only output primitive and it is finished.**
   `ibha_csvd_cursor_open`, `_next`, `_row`, `_reset`. Several cursors may be
   open on one diff, each with its own position and row buffer, so a JSONL writer
   and a view can consume the same diff without disturbing each other. Draining a
   cursor allocates nothing, which is asserted.

2. **`ibha_csvd_row.cell_flags` points into the cursor's own buffer** and is
   valid until the next `_next` on that cursor. `IBHA_CSVD_CELL_CHANGED` and
   `IBHA_CSVD_CELL_SUPPRESSED` can both be set on the same row, on different
   columns.

3. **Nothing materializes a string yet.** `ibha_csvd_field_copy` remains the
   single sanctioned boundary and the emitters are its first real consumer. The
   HTML emitter's escaping is a security requirement, not a formatting one: spec
   13.3 is explicit, and `fixtures/generated/xss.csv` already carries the payload
   through the parser intact so it reaches the emitter that must neutralize it.

4. **`deletedRowPlacement: 'sourceOrder'` is not implemented.** `anchored` and
   `end` are. The third option of spec 6.5 is the mirror walk, source order with
   additions anchored, and it wants the same bucket machinery pointed the other
   way. `ibha_csvd_deleted_placement` has only the two values, so adding it is
   additive rather than a change.

5. **`IBHA_CSVD_DATE_VALUE` is refused, not faked.** Spec 5.3 makes date value
   comparison opt in *with an explicit input format list*, so that nothing
   guesses between `1/5/2024` and `5/1/2024`. The format list does not exist, so
   asking for the option returns `IBHA_CSVD_ERR_UNIMPLEMENTED` rather than
   quietly comparing exactly and looking like it worked.

6. **Validation findings do not exist yet.** Spec 13.5 distinguishes errors,
   which abort, from validation findings, which are output: a `REQUIRED` column
   with an empty cell, a value longer than its `VARCHAR(n)`, a value that does
   not parse as its `DECIMAL(p,s)`. The schema now carries `col_type`, `col_size`
   and `col_scale`, which is everything those checks need, and the cell flag byte
   has room. They belong with the emitters, because a finding that nothing can
   emit is invisible.

7. **The row feed front end of spec 13.1 is still unbuilt.** The matcher only
   ever sees `ibha_csvd_table`, so a JDBC `ResultSet` front end that fills one
   needs no change here.

## 7. Assumptions still flagged, and one new question

The two from Phase 0 are unchanged and neither was re-decided:

1. **The asymmetric ragged row rule.** A row with *extra* fields that are all
   empty is normalized and counted in `stats.ragged_normalized`; a row with
   *missing* fields is `IBHA_CSVD_ERR_RAGGED_ROW`. Excel emits trailing empty
   columns, which is the case spec 13.5 names; a short row means data is missing
   and padding it would hide the corruption a diff exists to surface. Three line
   change in `row_end` in `src/parse.c` if a short row should be padded instead.

2. **Added and removed columns are errors**, surfacing as
   `IBHA_CSVD_ERR_COLUMN_ORDER` naming both column counts. Spec 13.10 asks
   whether a salesman appending a column should be a warning. **This is now
   cheaper to change than it was:** a finding needs somewhere to go, and Phase 3
   builds that.

One new question, from building the matcher:

3. **Duplicate key row numbers are index based, not line based.** The message
   says "at rows 5 and 7" counting parsed records from 1, which differs from the
   physical line number when the file has blank lines or multiline quoted fields.
   Making it the true line number costs 4 bytes per row for a `row_line` array
   that exists only for error messages. Worth it or not is a judgement about how
   often the two diverge in your files.

## 8. Blockers needing Manas's action

**Resolved during this phase:** the AddressSanitizer runtime was missing, which
is a separate package on RHEL. `sudo dnf install libasan libubsan` fixed it,
`make asan` and `make fuzz-native` both run and are clean, and installing it
immediately paid for itself by surfacing the harness leak described in section 3.
`make ubsan`, `make valgrind` and `make fuzz-native-notrap` stay in the Makefile
as the fallbacks for a machine that lacks the runtime; they are not weaker at
finding undefined behaviour, only at finding heap overruns.

1. **No WebAssembly toolchain.** Unchanged. Blocks the cross-target determinism
   check of spec 3.2 and Phase 4. Nothing added this phase uses floating point or
   anything target dependent, deliberately, but it is still unproven.

2. **No libFuzzer runtime.** Unchanged. `make fuzz-native` is the fallback and a
   real libFuzzer run on CI is still worth having, now with three targets.

3. **JDK 21 is not installed; JDK 17 is.** Blocks Phase 6 only.

4. **Repos are not git initialized as submodules.**

Two smaller things fixed in passing, both environment portability rather than
design: `fixtures/gen_fixtures.py` used `Path.write_text(newline=...)`, which
needs Python 3.10 and this machine has 3.9; and `cli/main.c` and
`bench/bench_parse.c` called `clock_gettime` under `-std=c11`, which hides POSIX
behind glibc's feature test macros. Both now say what they need explicitly.

## 9. Working notes

- `cd core && make test` runs 321 assertions and generates the fixtures if they
  are missing. `make asan`, `make ubsan`, `make valgrind`, `make bench`,
  `make fuzz-native`.
- `python3 scripts-and-commands/run_phase2_checks.py` runs the whole gate. Every
  step passes on this machine; a step whose toolchain is absent reports
  "skipped" with the install line rather than failing the gate.
- The core and the tests compile warning free under
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings`.
  Keep it that way.
- `fixtures/generated/p90_edits.tsv` is the edit script the property test asserts
  against; `p90_edits.json` is the same script in a form a human can read.
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved
  under `scripts-and-commands/`, and a handoff file at the end of each phase.

---

## 10. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project. Phases 0, 1 and 2 are complete
and Phase 3 is next.

Start by reading, in this order:

1. `docs/HANDOFF-phase2.md` - full state and the invariants to preserve.
   Section 4 is the one coupling the engine rests on and section 6 is what
   Phase 3 specifically has to know
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. 13.3 is Phase 3's specification. It overrides earlier sections
   where they conflict, so do not re-derive decisions from sections 1 to 12
3. `specs/02-solution-proposal.md` sections 7 and 13.5 - cell level intra text
   diffing, and the distinction between errors, which abort, and validation
   findings, which are output
4. `core/README.md` and `core/include/ibha_csvdiff.h` - the ABI you will build
   on. The diff, the cursor and the report row are concrete public types

Then implement **Phase 3: the emitters and the validation findings**. Scope: the
`jsonl`, `csv`, `html` and `summary` emitters of spec 13.3, each a thin loop over
`ibha_csvd_cursor_next` writing into a caller supplied sink; the versioned
`schemaVersion` row contract those emitters and their consumers agree on; the
validation findings of spec 13.5 flowing through the cursor as cell flags and
counted in the summary; and the cell level segment API of spec 7 if it fits,
lazily and memoized, or deferred with a note if it does not.

Constraints:

- **The HTML emitter's escaping is a security requirement, not a formatting
  one.** Cell content is untrusted and the output is injected through
  `dangerouslySetInnerHTML` or opened in a browser. Escape `&`, `<`, `>`, `"` and
  `'` in every value, emit only class names from a fixed compiled-in set, never
  interpolate caller data into an attribute name or a URL.
  `fixtures/generated/xss.csv` already carries the payload through the parser
  intact so it reaches the emitter, and it needs explicit assertions.
- Emitters must not require the whole diff in memory. Each is a loop over the
  cursor into a sink; nothing accumulates.
- Keep the invariants: no global state, arena only, first-error-wins, streaming
  only, no string materialization outside `ibha_csvd_field_copy`, deterministic
  output, no floating point.
- Do not start the view or the bindings. Those are Phases 4 and later.

Working notes:

- `cd core && make test` runs 321 assertions; `make asan` runs them under
  AddressSanitizer, LeakSanitizer and UBSan; `make ubsan` and `make valgrind` are
  the fallbacks for a machine with no sanitizer runtime; `make fuzz-native` runs
  the three fuzz targets; `make bench` is the parser benchmark. All currently
  pass and must keep passing.
  `python3 scripts-and-commands/run_phase2_checks.py` runs the whole gate.
- Note that **LeakSanitizer is on by default on Linux and off on macOS**, so
  "ASan is clean" means less on a Mac. Gate on the Linux run.
- The core and the tests compile warning free under a strict flag set. Keep it
  that way.
- Neither a wasm toolchain nor a libFuzzer runtime is available. Ask before
  installing anything.
- Per CLAUDE.md: write python scripts rather than bash with quoted variables,
  save them under `scripts-and-commands/`, and write a handoff file at the end of
  the phase.

Three assumptions are implemented but unconfirmed and are described in handoff
section 7: the asymmetric ragged row rule, added or removed columns being errors
rather than warnings, and duplicate key row numbers being record based rather
than line based. The second is the one Phase 3 makes cheap to change, because a
warning needs a finding and Phase 3 builds findings. Flag them rather than
re-deciding them.
