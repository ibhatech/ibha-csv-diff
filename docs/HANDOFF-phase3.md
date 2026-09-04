# Handoff: Phase 3 complete, and both toolchain blockers cleared

Dates: 2026-08-12 and 2026-08-13
Next phase: Phase 4, the wasm binding and `@ibhatech/csvdiff-core`

Everything described here is committed and the tree is clean. Five commits:

```
cf5ee51 make fuzz works: libFuzzer came with the clang installed for wasm
ad06e3b Build the wasm module, and fix the three things that stopped it being real
85f7e7c Rewrite the Phase 4 prompt, and record two things it has to deal with
37ce52d Added and removed columns are a config setting, per kind, at the call site
8e9c48d Phase 3: the emitters, the validation findings and the intra cell diff
```

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier where they conflict. 13.3 was Phase 3's specification; 13.7 and 13.11 are Phase 4's |
| `specs/02-solution-proposal.md` sections 2.5, 4.2, 4.3 | The latency budget, the random access `DiffHandle`, and worker execution: what Phase 4 builds |
| `core/include/ibha_csvdiff.h` | The ABI. The cursor, the emitters, the sink and the cell segments are all public |
| `core/README.md` | Build, the output layer in one paragraph, the row contract, the column policy, and what each emitter costs |
| This file, sections 4 and 6 | The contracts Phase 4 has to hold to, and the eleven things it specifically needs to know |

## 2. The headline results

**The output layer is a loop over the cursor, and it shows in the numbers.** A
pass/fail summary of the 15 MB pair costs 3 ms more than the bare diff, and the
emitter's own memory is one 8 KB buffer and one report row whatever the size of
the report.

```
                                seconds   output   MB/s in
counts only, no emitter           0.135     593 B      221
summary                           0.138     480 B      217
jsonl, changes only               0.139    1.3 MB      216
html, changes only, cell diff     0.139    1.5 MB      215
csv                               0.197   21.3 MB      152
html                              0.250   79.5 MB      120
jsonl                             0.268   86.2 MB      112
```

Read the bottom three rows as the concrete form of spec 13.3's rule that the HTML
emitter is for bounded output. A full JSONL report of this pair is **86 MB from
30 MB of input**, and half the wall time is producing those bytes rather than
diffing. Changes-only is 1.3 MB and costs nothing measurable.

**The engine now runs in WebAssembly.** Both modules build, instantiate, and
execute a parse, a diff and a cursor drain, and report the same four rows the
native build reports for the same inputs.

```
build/ibha_csvdiff.wasm        120 KB, 44 exports, 0 imports
build/ibha_csvdiff.simd.wasm   138 KB, 44 exports, 0 imports
```

## 3. What is done and verified

Native on x86-64 Oracle Linux 9, GCC 11.5; the wasm and libFuzzer builds on clang
21.1.8. Every step below is in `scripts-and-commands/run_phase3_checks.py` and
every step passes.

- **465 assertions pass**, up from 321. `make test`
- **Clean under AddressSanitizer, LeakSanitizer and UBSan**, now with
  `detect_stack_use_after_return=1`. `make asan`
- **Clean under trapping UndefinedBehaviorSanitizer** and **valgrind memcheck**
- **1 million inputs each on four targets** under the self contained driver, and
  **a coverage guided libFuzzer run on all four**, both clean.
  `make fuzz-native FUZZ_ITERS=1000000`,
  `python3 scripts-and-commands/run_libfuzzer.py`
- **Compiles warning free** under the strict flag set, tests included, on both
  targets
- **The CLI drives all four emitters end to end** over the 15 MB pair
- **Both wasm modules build, instantiate and run**, exporting exactly the public
  ABI and importing nothing. `make wasm-check`

### New in `core/`

| File | Contents |
|---|---|
| `src/emit.c` | The buffered writer, the escapers, and the four emitters |
| `src/validate.c` | The validation findings of spec 13.5 |
| `src/segment.c` | The intra cell diff of spec 7: word, character and word-then-character |
| `tests/emitkit.h` | **Independent** checkers: a strict JSON parser and the HTML safety invariant |
| `tests/test_emit.c` | The emitters, the row contract, the XSS corpus, the overlong UTF-8 case |
| `tests/test_validate.c` | Each finding, and that a finding is never an error |
| `tests/test_columns.c` | The column policy of spec 6.6: what it allows and what it never relaxes |
| `tests/test_segment.c` | The rebuild law, the caps, and the UTF-8 boundaries |
| `tests/fuzz_emit.c` | Fuzzes the emitters, re-checking their output with `emitkit.h` |

### New in `scripts-and-commands/`

| File | Contents |
|---|---|
| `run_phase3_checks.py` | The whole gate, including the CLI in each format and the wasm modules |
| `measure_emitters.py` | The table in section 2, so it is re-run rather than edited |
| `run_libfuzzer.py` | Seeds a corpus, runs the four libFuzzer targets, fails on any artifact |
| `wasm_smoke.mjs` | Drives the engine inside a wasm instance, deliberately free of struct layouts |
| `mark_public_api.py` | Checks every public declaration is marked exported, which the wasm build needs to be true |

`src/reader.c` gains the push sinks, `cli/main.c` gains `--format`,
`--changes-only`, `--cell-diff`, `--max-rows`, `--no-validate`,
`--allow-added-columns` and `--allow-removed-columns`.

## 4. The contracts Phase 4 inherits

### 4.1 The row shape is versioned, and one rule in it is easy to get wrong

`IBHA_CSVD_SCHEMA_VERSION` is 1 and rides on every JSONL row, every CSV row and
the HTML container. The emitter and the consumer are separate components, so
changing a shape is a breaking change and bumps it.

The rule a consumer must implement correctly: **a matched row's cell carries
`source` exactly when that cell differs in bytes from the target.** Its absence
means the two sides are byte identical. This is what keeps an unchanged 90,000 row
report from being written out twice, and a consumer that treats a missing `source`
as an empty string will render every unchanged cell as a deletion.

### 4.2 The HTML escaping is a security requirement, and it is arranged to be correct by construction

Not by remembering to escape. Four structural properties, all asserted:

1. Every value goes through one function, `wr_value`, which escapes for the format
   in hand. Everything else written is a compiled in literal or an integer.
2. Class names come from a fixed compiled in set. The one caller supplied string
   that reaches the markup, the class prefix, is **validated** against
   `[A-Za-z][A-Za-z0-9_-]{0,31}` and refused with `INVALID_ARG` otherwise, not
   escaped. A prefix is an identifier; one that is not is a mistake worth failing.
3. No caller data reaches an attribute name or a URL, and no URL is emitted.
4. **Ill formed UTF-8 is replaced with U+FFFD in the JSON and HTML paths.** This
   is a second XSS defence rather than tidiness: the overlong encoding of `<` is
   `C0 BC`, it is not the byte `0x3C`, so a byte oriented escaper passes it
   through untouched and a decoder that accepts overlongs then sees a tag. There
   is an explicit assertion for exactly that payload. CSV is byte transparent
   because it has no encoding contract and carries no markup.

`tests/emitkit.h` states the invariant positively and independently: **every `<`
in the output opens one of a fixed list of tags this emitter may write, and every
`&` opens one of five entities.** It shares no code with the emitter, and the fuzz
target re-checks it on every input. A test that merely grepped for `<script>`
would pass on `<img src=x onerror=alert(1)>`; this cannot.

### 4.3 Findings are output, errors abort

Four cell flags, on top of the two comparison flags:
`IBHA_CSVD_CELL_REQUIRED_EMPTY`, `_TOO_LONG`, `_NOT_NUMERIC`, `_PRECISION`, with
`IBHA_CSVD_CELL_FINDING` as the mask. They are evaluated against the **source**
file's schema, and on the values the report row actually carries: the target row
where there is one, the source row for a deleted row.

`changes_only` never drops a row whose only news is a finding. A finding on an
otherwise unchanged row is the point of running the comparison.

### 4.4 A report row's columns are not always the source file's columns

By default they are, and `stats.n_columns_compared` equals the source's column
count. When the caller allows an added or removed column, a report row carries the
columns the two files have in common, in the source's order, and the added ones
never appear in it. A consumer that assumes `row->n_columns` equals the source
header's width is wrong in that case: use `row->n_columns`, and take the column
names from the report itself rather than from the file.

### 4.5 The cursor now reads cells it used to skip, and that is the one behaviour change

Validation is on by default and is the only thing that makes the cursor look at an
unchanged row's cells: a digest settles whether a row changed and cannot settle
whether it satisfies the schema. It costs 13 ms on 1.76 million cells, taking the
drain from 1.7 ms to 14.9 ms. A per column pre-filter makes it free when the
schema declares nothing to check, and `diff_opts.validate = 0` turns it off.

## 5. Decisions made in this phase, and why

1. **The segment API is not memoized, and that is deliberate.** Spec 7 says
   results are memoized in the arena. A memo table keyed by cell is state that
   grows with the number of cells looked at, which is exactly what spec 13.3
   forbids the streaming path from carrying, and in a streaming emitter each cell
   is computed once and used once so it would buy nothing. The natural owner is
   the random access consumer of spec 4.2, which already retains an index and
   knows which cells its viewport keeps asking about. **Phase 4 should add it
   there**, keyed by report position, not here.

2. **The segment cap is on the edit distance, not only on the length.** A pair
   longer than `max_bytes` (4096) reports as wholly replaced without running
   Myers, and a pair needing more than 64 edits does the same. The trace is
   `65 x 131` int32, so 64 costs 34 KB of scratch and 128 would cost 133 KB, and
   a cell needing sixty-four edits produces a highlight nobody can read. The
   common prefix and suffix are stripped first, so a 300 byte cell with one word
   changed is a Myers over a handful of elements.

3. **Character mode is code point granularity, not grapheme cluster.** Spec 7 asks
   for grapheme cluster awareness at the boundaries. Doing that properly needs
   Unicode tables the engine deliberately does not carry, so a UTF-8 sequence is
   never split but a combining mark is its own element. If a real file makes that
   visible, the tables are the cost of fixing it.

4. **The CSV emitter guards against formula injection by default.** A value
   opening with `=`, `+`, `@`, a tab or a CR is a formula to Excel, and spec 13.3
   names "loading the report back into a spreadsheet" as this emitter's whole
   purpose, so a report of untrusted data is a script delivery mechanism unless
   something intervenes. Such a value is prefixed with `'`, Excel's own text
   marker. **A leading `-` is guarded only when what follows is not a plain
   number**, because prefixing every negative amount in a financial report would
   be worse than the risk. `csv_formula_guard = 0` turns it off.

5. **The summary emitter recounts from zero.** Cell level counters accumulate as a
   cursor advances, so a caller who has already written JSONL would otherwise see
   the summary double count. The summary is defined as the numbers of exactly one
   pass, so it zeroes them and drains its own cursor. Emitting it twice produces
   identical bytes, which is asserted.

6. **A modified row is two CSV lines, `side=source` then `side=target`.** The
   alternative was an `old -> new` syntax inside a cell, which nothing can parse
   back into a spreadsheet. Deleted rows are one line and added rows are one line.

7. **The schema findings of spec 13.8 are computed in the summary emitter, not at
   parse time.** The parse path never compares the target's metadata rows, so
   putting the check there would cost every caller. The summary has both tables
   and both schemas in hand, and it is the only consumer, so it does the
   comparison itself and reports `metadataDisagreement` entries. The comparison
   only runs when the two files have the same header layout; a names-only upload
   has no metadata to disagree with.

8. **The column policy of spec 6.6 is a projection, not a column map.** When the
   two files do not carry the same columns and the caller allowed that, each side
   gets a table holding exactly the compared columns in the source's order, and
   its digests are refolded over them. The alternative was threading a per side
   column map through the engine, and column *c* means the same thing on both
   sides in a dozen places, several of which compare two rows of the *same* side.
   A projection makes all of them correct by construction instead of by review.
   It costs one rebuilt index per side, about 12 bytes per compared cell, paid
   only when the sets actually differ.

   Three things the flags never relax, each asserted: reordering stays a hard
   error, a missing KEY column is always an error, and a file with no column name
   row cannot use the flags at all because there is nothing to tell an added
   column from a shifted one. A column swapped for a differently named one in the
   same position needs *both* flags, because by names alone it is
   indistinguishable from a rename and it is reported as what the file actually
   shows: one removal and one addition.

9. **The wasm build's three defects were all silent, and the fixes are all
   structural.** The build had never compiled once, so nothing in it had ever been
   exercised. Section 6 points 9 to 11 state the properties that now hold; what is
   worth carrying forward is the shape of all three failures. None was a link
   error or a crash. The module linked cleanly and exported nothing, linked
   cleanly and imported a function no host would supply, and would have corrupted
   any host that allocated memory. A target nobody builds for accumulates that
   kind of defect, which is the argument for `make wasm-check` being in the gate
   rather than a thing someone runs occasionally.

## 6. What Phase 4 needs to know

1. **The sink is the mirror of the reader.** `ibha_csvd_write_fn` returns 0 or a
   negative to abort, which surfaces as `IBHA_CSVD_ERR_IO`. A JS binding wires it
   to a `WritableStream` or an accumulating `Uint8Array`; the buffer sink counts
   past the end rather than truncating, so a caller can size a buffer from a first
   pass.

2. **`ibha_csvd_cell_segments` is what the view calls per visible cell.** It
   returns `[op, start, len]` triples as three `uint32_t`, no padding, so a
   binding can view an array of them as a plain `Uint32Array`. Offsets are **byte
   offsets into the logical value**; `EQUAL` and `DELETE` index the source,
   `INSERT` indexes the target. A helper converting to UTF-16 code unit offsets
   for DOM ranges belongs with the view, and getting that boundary wrong is the
   classic source of mangled non ASCII text.

3. **The scratch is per diff and reused, so two segment calls on one diff must not
   run concurrently.** The engine is thread agnostic per spec 2.6.3: parallelism
   is across diffs.

4. **The HTML emitter writes a cell's newline through unchanged.** It does not
   invent `<br>`. The styling contract of spec 8.3 should set `white-space:
   pre-wrap` on the cell class, which is the right place for that decision.

5. **The classes the view has to style** are the fixed set, all carrying the
   prefix: `report`, `table`, `th`, `num`, `row`, `cell`, `unchanged`, `modified`,
   `added`, `deleted`, `moved`, `changed`, `suppressed`, `finding`, `old`, `new`,
   `del`, `ins`. Plus `data-schema-version` on the container and `data-finding` on
   a cell carrying one, whose value is from a compiled in set.

6. **`deletedRowPlacement: 'sourceOrder'` is still not implemented.** Unchanged
   from Phase 2: `anchored` and `end` are, the third is the mirror walk, and
   adding it is additive.

7. **`IBHA_CSVD_DATE_VALUE` is still refused rather than faked**, for the same
   reason: spec 5.3 makes it opt in with an explicit input format list, and the
   format list does not exist.

8. **The TypeScript row types in `js/packages/core/src/index.ts` disagree with the
   contract they claim to implement.** They were written in Phase 0, before the C
   side existed, and they export `DIFF_ROW_SCHEMA_VERSION = 1` while describing a
   different shape from `IBHA_CSVD_SCHEMA_VERSION` 1: `moved` is a kind rather
   than a flag, cells carry `oldValue` and `newValue` rather than `source` and
   `target`, `violations` predates the typed findings, and the summary has no
   findings or column counts. Reconciling them is the first thing Phase 4 should
   do, before writing anything against them. The C side is the one that is
   implemented, tested and fuzzed, so it is the one that wins.

9. **The wasm build works, and the determinism check of spec 3.2 is the obvious
   next thing.** `make wasm-check` instantiates both modules and runs a parse, a
   diff and a cursor drain inside them, and `scripts-and-commands/wasm_smoke.mjs`
   is deliberately layout free so that nothing in it has to be revised when the
   Phase 4 binding works out the wasm32 struct offsets. What it does not do is
   compare *emitter output* between the two targets byte for byte, which is what
   spec 3.2 actually asks for and what Phase 4 should build first.

10. **The host memory contract is "the engine owns exactly the pages it grew
    itself".** Not everything above `__heap_base`. A binding stages its bytes with
    its own `memory.grow` and the two can never overlap. Phase 4 must not
    reintroduce a shared break pointer, and if it wants engine lifetime staging it
    should get it from the arena rather than from a second allocator.

11. **The module exports exactly the declarations carrying `IBHA_CSVD_API`.**
    `scripts-and-commands/mark_public_api.py` checks that every declaration in the
    public header carries it, and it is in the gate: a public function that is not
    marked is not a link error, it is a function missing from the module.

## 7. Assumptions still flagged

Three stand and are unconfirmed. The fourth was resolved this phase.

1. **The asymmetric ragged row rule.** A row with *extra* fields that are all
   empty is normalized and counted in `stats.ragged_normalized`; a row with
   *missing* fields is `IBHA_CSVD_ERR_RAGGED_ROW`. Three line change in `row_end`
   in `src/parse.c` if a short row should be padded instead.

2. **Duplicate key row numbers are record based, not line based.** The message
   says "at rows 5 and 7" counting parsed records from 1, which differs from the
   physical line number when the file has blank lines or multiline quoted fields.
   Phase 3 made this convention wider rather than narrower: every row number in
   every emitter is the same 1 based record number. Changing it now means changing
   the emitters too, and costs 4 bytes per row for a `row_line` array.

3. **`VARCHAR(n)` is counted in characters, not bytes.** A UTF-8 lead byte counts
   as one character, so `café` is four characters in five bytes and does not
   violate `VARCHAR(5)`. That matches every database that would be receiving these
   files. If the destination column is declared in bytes, the check is one line in
   `char_count` in `src/validate.c`.

4. ~~Added and removed columns are errors.~~ **Resolved on 2026-08-12: it is a
   config setting, per kind, at the call site.** `compare.allow_added_columns` and
   `compare.allow_removed_columns`, both defaulting to 0, which is spec 13.10
   unchanged. Section 5 point 8 has what they do and what they deliberately do not
   relax.

## 8. Blockers needing Manas's action

1. ~~No WebAssembly toolchain.~~ **Resolved on 2026-08-12**: `dnf install clang
   lld` on Oracle Linux 9, clang 21.1.8 with the wasm32 backend and `wasm-ld`.
   `make wasm` and `make wasm-check` both work.

2. ~~No libFuzzer runtime.~~ **Resolved on 2026-08-12**, by the same install.
   `make fuzz` builds all four targets with `FUZZ_CC`, which defaults to clang and
   leaves `CC` alone for everything else, and
   `scripts-and-commands/run_libfuzzer.py` seeds a corpus, runs them, and fails on
   any artifact rather than trusting the exit status. First real run: 90 seconds
   each, no crash, leak, timeout or oom, and the corpus grew to about 2,100 inputs
   kept in `core/corpus` between runs. `make fuzz-native` stays as the fallback for
   a machine without clang; it is not equivalent, and the rate difference is why:
   on the emitters libFuzzer does about 4,500 executions a second with feedback and
   builds on what it finds.

3. **JDK 21 is not installed; JDK 17 is.** Blocks Phase 6 only.

4. **Repos are not git initialized as submodules.** `core/`, `js/` and `java/` are
   ordinary directories in one repository. Nothing so far needs them split, and
   splitting them later costs a history rewrite, so it is worth deciding before
   the JS package is published.

## 9. Working notes

- `cd core && make test` runs 465 assertions. `make asan`, `make ubsan`,
  `make valgrind`, `make bench`, `make fuzz-native`, `make wasm`, `make wasm-check`.
- `python3 scripts-and-commands/run_phase3_checks.py` runs the whole gate:
  the suites under three sanitizers, the fuzz targets, the CLI in each of the four
  formats, both wasm modules, and the export marker check.
- `python3 scripts-and-commands/run_libfuzzer.py --seconds 90` is the coverage
  guided fuzz run. It is not in the gate because it is time boxed rather than
  pass/fail; it belongs on a schedule in CI.
- `python3 scripts-and-commands/measure_emitters.py` regenerates the cost table in
  `core/README.md`. Re-run it rather than editing the table.
- **`make asan` sets `detect_stack_use_after_return=1`.** It is off by default and
  it earned its place: a test helper handed `ibha_csvd_parse_borrow` a stack
  buffer, so the index pointed into a dead frame. That read correctly at `-O3` and
  wrongly under a sanitizer, which is the worst way to find out. The parser borrows
  rather than copies, so any fixture must outlive the context.
- The core and the tests compile warning free under
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings`, on
  both GCC and the clang wasm build.

---

## 10. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project. Phases 0 through 3 are complete,
committed, and the tree is clean. Phase 4 is next.

**Both toolchain blockers are cleared.** clang 21.1.8 with the wasm32 backend and
`wasm-ld` is installed, so `make wasm` produces both modules, `make wasm-check`
instantiates them and runs a parse, a diff and a cursor drain inside them, and
`make fuzz` builds the four libFuzzer targets. Ask before installing anything
further.

Start by reading, in this order:

1. `docs/HANDOFF-phase3.md` - full state. **Section 4 is the contracts Phase 4 has
   to hold to**, section 5 point 1 is a piece of work deliberately left for this
   phase, and section 6 is the eleven things Phase 4 specifically needs to know
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. 13.7 is the browser fetch contract, 13.3 is the output contract and
   13.11 makes desktop browsers the only browser target. It overrides earlier
   sections where they conflict, so do not re-derive decisions from sections 1
   to 12
3. `specs/02-solution-proposal.md` sections 2.5, 4.2 and 4.3 - the latency budget,
   the random access `DiffHandle`, and worker execution
4. `core/README.md` and `core/include/ibha_csvdiff.h` - the ABI you are binding.
   The cursor, the sink, the emitters and the cell segments are all public
5. `js/packages/core/src/index.ts` and `js/packages/core/src/source.ts` - what
   already exists on the JS side, and see the warning about it below

Then implement **Phase 4: `@ibhatech/csvdiff-core`, the JS binding over the wasm
module**. Scope, in this order:

- **The determinism check of spec 3.2 first**, because it is cheap now and it
  gates everything after it: the same fixture pair through the native build and
  the wasm build must produce **byte identical emitter output**.
  `scripts-and-commands/wasm_smoke.mjs` proves the engine runs on the target but
  compares nothing, and comparing is the point
- **The binding**: module load, linear memory, the wasm32 struct offsets, the pull
  reader fed from the byte sources that already exist in `source.ts`, the diff, and
  a lazy cursor
- **The random access consumer of spec 4.2**, which retains an index so the view
  can seek, and which is where the cell segment memoization belongs (handoff
  section 5 point 1)
- **The worker wrapper of spec 4.3**, so a 150 MB diff does not block the main
  thread

**Reconcile the TypeScript row types before writing code against them.** The types
in `js/packages/core/src/index.ts` were written in Phase 0 and predate the C
contract, and they export `DIFF_ROW_SCHEMA_VERSION = 1` while disagreeing with
what `IBHA_CSVD_SCHEMA_VERSION` 1 actually is. Four differences, all of which the C
side decided deliberately and which the TS side should follow:

- `ChangeKind` has `'moved'` as a kind. In the ABI `moved` is a *flag* on the row,
  because a row can move and be modified in one edit and a single enum loses one
  of the two facts
- `DiffCell` has `oldValue` and `newValue`. The contract is `source` and `target`,
  and **`source` is present only when the cell differs in bytes** from the target.
  Its absence means they are identical, which is what keeps an unchanged 90,000
  row report from being written out twice
- `violations?: string[]` predates the findings. There are four typed cell flags
  and a `findings` array carrying `kind`, and `limit` or `precision` and `scale`
- `DiffSummary` has no findings counts, no column counts and no `identical` flag

Constraints:

- **Bytes in, never strings, and never a materialized diff.** Both rules are
  already stated at the top of `index.ts` and both are why the engine exists. A
  90,000 row diff as plain JS objects is a million allocations
- **The engine owns exactly the pages it grew itself**, per handoff section 6
  point 10. Stage bytes in pages the binding grows, never above `__heap_base`, and
  do not add a second allocator
- **The parser borrows rather than copies.** Anything handed to
  `ibha_csvd_parse_borrow` must outlive the context, which in JS means keeping the
  staging region alive, not just its contents
- **`row.n_columns` is not always the source file's column count.** With the
  column policy of spec 6.6 a report row carries the columns the two files share.
  Read the width from the row, not from the header
- Keep the invariants: no global state, arena only, first-error-wins, streaming
  only, deterministic output, no floating point in anything that decides an
  outcome
- **Anything the JS side renders itself**, rather than taking from the HTML
  emitter, needs the same escaping guarantee proved the same way: an independent
  checker, not a review. `core/tests/emitkit.h` is the model
- Do not start `@ibhatech/csvdiff-view` or `-react`; those are Phase 5. The Java
  binding is Phase 6 and SIMD is Phase 7

Working notes:

- `cd core && make test` runs 465 assertions; `make asan` runs them under
  AddressSanitizer, LeakSanitizer and UBSan with
  `detect_stack_use_after_return=1`; `make wasm-check` builds and drives both wasm
  modules; `make fuzz-native` runs four fuzz targets; `make bench` is the parser
  benchmark. `python3 scripts-and-commands/run_phase3_checks.py` runs the whole
  gate, and `python3 scripts-and-commands/run_libfuzzer.py --seconds 90` is the
  coverage guided fuzz run, which the gate does not include because it is time
  boxed rather than pass/fail. All currently pass and must keep passing.
- Gate on the Linux sanitizer run: LeakSanitizer is off by default on macOS.
- The core and the tests compile warning free under a strict flag set, on GCC and
  on the clang wasm build. Keep it that way.
- The JS monorepo is pnpm with three packages already scaffolded, and
  `js/scripts/copy-wasm.mjs` and `pnpm wasm` already exist. Node 20 or later; this
  machine has 24.
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved
  under `scripts-and-commands/`, and a handoff file at the end of the phase.

Three assumptions are implemented but unconfirmed and are described in handoff
section 7: the asymmetric ragged row rule, duplicate key row numbers being record
based rather than line based, and `VARCHAR(n)` being counted in characters rather
than bytes. Flag them rather than re-deciding them.
