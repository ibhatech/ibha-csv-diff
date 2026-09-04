# Handoff: Phase 4 complete, the engine runs in JavaScript and the builds agree

Date: 2026-08-13
Next phase: Phase 5, `@ibhatech/csvdiff-view` and `@ibhatech/csvdiff-react`

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier where they conflict. 13.3 is the output contract, 13.11 makes desktop browsers the only browser target |
| `specs/02-solution-proposal.md` section 8 | The view component, which is all of Phase 5: package structure, web component against React, the styling contract, row presentation modes, table mechanics |
| `js/packages/core/README.md` | The binding's API, what it costs, and the row contract restated for a consumer |
| `core/include/ibha_csvdiff.h` | The ABI, now with three functions that exist for hosts rather than for C callers |
| This file, sections 4 and 6 | The contracts Phase 5 inherits, and the eleven things it needs to know |

## 2. The headline results

**The native build and both wasm builds produce byte identical output.** That is
spec 3.2, and it was the first thing built because everything after it depends on
it being true. 160 comparisons over eight fixture pairs and ten emitter
configurations, including an 86 MB JSONL report, identical across three builds.

```
all 160 comparisons byte identical across native, wasm and wasm+simd
```

Phase 3 left `wasm_smoke.mjs`, which proved the engine ran on the target and
compared nothing. The distinction matters more than it sounds: a browser preview
and a server side batch report of the same pair have to be the same report, or the
preview is not a preview.

**The binding is a lazy cursor, and the numbers say so.** The p90 pair, 15 MB a
side, on Node 24. Regenerate with `node scripts-and-commands/measure_binding.mjs`.

```
what                          seconds   note
compare, parse and match        0.160   82 MB reserved
summary()                       0.035   one drain, constant memory
rows(), values off              0.057   146,946 rows
rows(), values decoded          0.296   1,763,352 cells
index({changesOnly})            0.053   55 KB retained, 2,198 rows
index(), whole report           0.093   3.6 MB retained
100 pages of 50 rows            0.019   decoded from the index
emit jsonl, changes only        0.044   1367 KB, two passes
emit jsonl, whole report        0.473   86 MB, two passes
compareInWorker, end to end     0.221   includes the chunk pump
```

The two rows Phase 5 should read carefully are the `index` ones. A view that seeks
pays 3.6 MB to index the whole report and 55 KB to index the changes, and it
retains no cell values at all: those stay in engine memory and are decoded for the
fifty rows about to be painted. That is the difference between 3.6 MB and the
million object heap the engine exists to prevent.

## 3. What is done and verified

Everything below is in `python3 scripts-and-commands/run_phase4_checks.py`, which
runs the Phase 3 gate in full and then the Phase 4 additions. All of it passes.

- **The Phase 3 gate is unchanged and still green**: 465 C assertions, clean under
  ASan/LSan/UBSan, trapping UBSan and valgrind, four fuzz targets, the CLI in each
  of the four formats, both wasm modules instantiated and driven
- **160 determinism comparisons**, native against scalar wasm against SIMD wasm
- **62 binding assertions**, `node --test "src/*.test.ts"`, which need **nothing
  installed**
- **The wasm32 struct offsets regenerate to the file on disk**, checked in the gate
- **`pnpm -r typecheck` and `pnpm -r build` pass**, all three packages, and the
  built `dist/` runs including the built worker
- **51 public declarations, all marked**; the modules export 48 things and import
  nothing

### New in `core/`

| File | Contents |
|---|---|
| `tools/abi_offsets.c` | Reports the wasm32 size, alignment and field offset of every public struct, as its own tiny module |
| `src/reader.c` | Gains `ibha_csvd_buffer_sink_bind` |
| `src/diff.c` | Gains `ibha_csvd_diff_table`, `ibha_csvd_diff_schema`, `ibha_csvd_diff_columns` |

### New in `js/packages/core/src/`

| File | Contents |
|---|---|
| `abi.ts` | **Generated.** 185 offsets and constants. Do not edit; run `gen_abi.mjs` |
| `memory.ts` | Linear memory: the staging allocator, and the view invalidation rule |
| `module.ts` | Compile, SIMD feature detection, the export and version guards |
| `engine.ts` | Option structs, the streaming parse, the diff, the lazy cursor, the emitters |
| `handle.ts` | The retained index of spec 4.2, and the segment memo |
| `types.ts` | The row contract, reconciled against the C side |
| `push.ts` | A `ByteSource` fed by pushes, with the backpressure the worker needs |
| `protocol.ts`, `worker.ts`, `workerClient.ts` | The worker of spec 4.3 |
| `testkit.ts` | `node:test` glue, fixtures, and an **independent** HTML safety checker |
| `*.test.ts` | 62 assertions across five suites |

### New in `scripts-and-commands/`

| File | Contents |
|---|---|
| `check_determinism.mjs` | Spec 3.2. Native against both wasm builds, over a fixture and emitter matrix |
| `gen_abi.mjs` | Generates `abi.ts`; `--check` fails when it is stale |
| `measure_binding.mjs` | The table in section 2, so it is re-run rather than edited |
| `run_phase4_checks.py` | The whole gate, C and JS |

## 4. The contracts Phase 5 inherits

### 4.1 The row contract is now stated on both sides, and they are checked against each other

`js/packages/core/src/types.ts` mirrors the JSONL emitter field for field, and
`engine.test.ts` asserts that on the p90 pair: every kind, row number, moved flag,
move distance and cell of every changes-only row, against the emitter's own
output. That test is the load bearing one in the suite. The binding decodes cells
itself, straight out of the columnar arrays rather than through an accessor call
each, because the header says an accessor per cell would cost more than the diff.
That buys the speed in section 2 and costs a second implementation of the
contract, and a second implementation nothing compares against the first is just a
place for them to drift.

**The rule a consumer must implement correctly, restated because it has not
changed and is still the easy one to get wrong:** a matched row's cell carries
`source` exactly when that cell differs in bytes from the target. Its absence
means the two sides are byte identical. A view that renders a missing `source` as
an empty string shows every unchanged cell as a deletion.

### 4.2 The Phase 0 TypeScript types are gone, and what replaced each one

They were written before the C side existed and exported
`DIFF_ROW_SCHEMA_VERSION = 1` while describing a different shape. Four changes,
each a decision rather than a rename:

- `ChangeKind` no longer has `'moved'`. It is a flag on the row, because a row can
  move and be modified in one edit and a single enum loses one of the two facts
- `DiffCell.oldValue` and `newValue` became `source` and `target`, with the
  presence rule above
- `violations?: string[]` became a typed `findings` array carrying the declared
  `limit`, or `precision` and `scale`, that the value failed against
- `DiffSummary` gained findings counts, column counts, `identical`, the matching
  facts, the target header detection result and the schema findings

`DIFF_ROW_SCHEMA_VERSION` is now taken from the engine's own
`IBHA_CSVD_SCHEMA_VERSION` rather than restated, so the two cannot disagree again.

### 4.3 The streaming path and the seeking path are separate, and that separation is the API

`handle.rows()` retains nothing: the engine's row buffer is reused and each
decoded row is garbage as soon as the consumer is done with it. `handle.index()`
drains the cursor once and keeps 14 bytes plus one byte per compared column per
report row, and nothing else. Spec 13.3 is explicit that the report index is not
built at all unless a consumer asks for random access, so a summary, a pass/fail
check and an export never build one.

`handle.index({ changesOnly })` filters at build time, so index positions are
positions in the filtered report. That is what a view scrolling a changes-only
report needs, and it means the index and the HTML emitter under the same filter
agree row for row. `handle.test.ts` asserts that seeking and streaming produce
identical rows, which is the property that stops a view and an export of the same
diff disagreeing.

### 4.4 Segment offsets are byte offsets, and the conversion has one home

`ibha_csvd_cell_segments` returns byte offsets into the logical value. A JS string
is UTF-16. Slicing a decoded string at a byte offset is the classic way to mangle
non ASCII text, so `sliceByBytes(value, start, len)` in `handle.ts` is the one
place that conversion happens and it is tested against a surrogate pair. Phase 5
should use it rather than writing a second one.

### 4.5 A caller's bad argument must not poison the diff

The engine holds one error and the first one wins, so an engine level refusal of a
mistyped HTML class prefix aborts a comparison that was otherwise complete, and
every later call on that handle. The binding therefore validates its own arguments
before they reach the context: the class prefix against the engine's own pattern,
the emit format, the cell diff mode, and the single byte options. Phase 5 should
keep that line: data errors belong to the engine, caller mistakes belong to the
binding.

## 5. Decisions made in this phase, and why

1. **The determinism check drives the real binding rather than a private driver.**
   It could have been a self contained script. Making it go through
   `Engine.load`, the option structs, the staging path and the chunked feed means
   every one of the 160 comparisons also exercises those, and a binding bug shows
   up as a byte difference against the native build rather than as a passing test
   over a code path nothing else uses.

2. **The scalar wasm build is in the comparison, not just the SIMD one.** There is
   nothing to catch today: both are `-msimd128` over the same C. Phase 7 replaces
   the parser's inner loop with intrinsics, and this is the harness that will
   catch it when the two disagree on a quoted field straddling a 16 byte lane.
   Wiring it up now is the point, because a check has to already be passing for
   its first failure to mean something.

3. **The struct offsets are generated, by the compiler that builds the module.**
   `tools/abi_offsets.c` compiles to its own wasm32 module and `gen_abi.mjs` reads
   it. The alternative, a hand written table, is not a compile error when a field
   moves; it is a binding that reads `n_columns` out of the middle of a pointer and
   reports a plausible wrong number. The wasm target has already produced that
   class of failure twice in this project, which is why `--check` is in the gate.

4. **Three functions were added to the C ABI, and all three closed a hole rather
   than adding a feature.** `ibha_csvd_buffer_sink_bind`, because a function
   pointer on wasm32 is an index into an unexported table and JavaScript cannot
   portably put a host function into one, so the entire emitter layer was
   unreachable from a binding even though every symbol it needs was exported.
   `ibha_csvd_diff_table` and `ibha_csvd_diff_schema`, because under the column
   policy of spec 6.6 the diff compares *projected* tables, and a consumer
   decoding cells out of the parsed ones reads the wrong column, only when the
   caller allowed a column difference, which makes it a bug that passes every test
   written against the defaults.

5. **The binding's tests run under `node --test`, and imports name `.ts` files.**
   Node 22 and later execute TypeScript by stripping types but resolve specifiers
   exactly as written, so `./source.js` fails where `./source.ts` runs; TypeScript
   5.7's `rewriteRelativeImportExtensions` rewrites them back to `.js` on emit, so
   the published package is unaffected. What this buys is that the determinism
   check and the whole binding suite run on a checkout with nothing installed,
   which is the property the C gate already had. `@ibhatech/csvdiff-view` and
   `-react` keep vitest, because a component test does need a DOM.

6. **`summary()` runs the summary emitter and parses its JSON rather than reading
   the stats struct.** Cell level counters accumulate as a cursor advances, so
   reading the struct gives a number that depends on how many times the caller
   happened to have drained the diff. The summary emitter zeroes them and drains
   its own cursor, so its numbers are always those of exactly one pass, and going
   through it means the summary the binding reports and the summary written to a
   file cannot drift. It costs one extra drain, about 35 ms on the p90 pair.

7. **The segment memo lives in the index, keyed by report position.** This is the
   piece of work Phase 3 deliberately left. In a streaming emitter each cell is
   computed once and used once, so a memo would buy nothing and would be state
   growing with the number of cells looked at, which the streaming path is
   forbidden to carry. In a viewport the same cells are asked for on every
   repaint. The index is the natural owner, and it is an LRU of 4,096 entries
   cleared when the mode changes.

8. **One wasm instance and one worker per comparison.** Linear memory only grows
   and the engine's allocator frees nothing individually, because a context
   releases everything at once. A shared instance would carry the high water mark
   of every comparison it had run. A pleasant consequence: a handle dropped
   without `dispose` is collected rather than leaked, so there is no
   `FinalizationRegistry` here despite spec 4.2 suggesting one.

9. **The worker's chunk pump has backpressure, and it is not optional.** The
   engine parses the source side to completion before pulling a byte of the
   target, so a pump that sent both sides as fast as the messages would go would
   buffer the entire target in the worker before the target parse began. At the
   p90 that is 15 MB held for nothing; at the 150 MB ceiling it is fatal. So
   `PushSource.write` does not resolve until the buffered amount falls below a 4 MB
   watermark, and the client awaits each chunk's acknowledgement.

10. **The worker matches replies to requests by sequence number, not by order.**
    The worker starts each request as it arrives rather than finishing one before
    reading the next, which it has to: the comparison cannot finish until the pump
    has delivered its bytes. So replies come back interleaved, and a queue would
    hand the pump the comparison's answer.

## 6. What Phase 5 needs to know

1. **The HTML emitter and the virtualized view consume the same cursor**, so they
   agree by construction, and that is the reason to keep using the emitter for
   bounded output rather than rendering everything in JS. The rule from spec 13.3:
   HTML emitter for reports, changes-only and a page at a time; the virtualized
   component for interactive browsing of a full diff.

2. **Anything the view renders itself needs the escaping guarantee proved the same
   way.** `testkit.ts` has `htmlSafetyViolation`, which states the invariant
   positively: every `<` in the output opens one of a fixed list of tags and every
   `&` opens one of five entities. It is `core/tests/emitkit.h` restated
   independently rather than translated, and it runs over the XSS corpus in
   `engine.test.ts`. A test that greps for `<script>` passes on
   `<img src=x onerror=alert(1)>`; that one cannot.

3. **The classes to style** are the fixed set, all carrying the prefix: `report`,
   `table`, `th`, `num`, `row`, `cell`, `unchanged`, `modified`, `added`,
   `deleted`, `moved`, `changed`, `suppressed`, `finding`, `old`, `new`, `del`,
   `ins`. Plus `data-schema-version` on the container and `data-finding` on a cell
   carrying one. The prefix is validated against `[A-Za-z][A-Za-z0-9_-]{0,31}` and
   refused otherwise, by the binding now as well as by the engine.

4. **The HTML emitter writes a cell's newline through unchanged.** It does not
   invent `<br>`. The styling contract of spec 8.3 should set
   `white-space: pre-wrap` on the cell class, which is the right place for that
   decision.

5. **`getRowsCompact` is the form a virtualized table should bind to.** Parallel
   typed arrays, no row object per visible row, copied rather than viewed so a
   later page cannot change one the view is still holding, and transferred rather
   than cloned across the worker boundary. `getRows` is the convenient form and
   allocates a row object and a cell object per cell.

6. **`row.cells.length` is not always the source file's column count.** Under the
   column policy a report row carries the columns the two files share. Read the
   width from the row and the names from `handle.columns`.

7. **The summary carries what a header bar needs**, including `identical`, the
   findings counts with an `enabled` flag that distinguishes "nothing found" from
   "not looked at", `columns.added` and `removed`, and `targetHeader.namesOnly` for
   telling a user their upload kept only the column names.

8. **`emit` is two passes over the diff**, a sizing pass with no buffer and then a
   filling pass. `ibha_csvd_emit` drains the whole diff in one call, so there is no
   way for a host to pull a bounded piece of it, and the buffer sink counting past
   the end is what makes the first pass a measurement. It is honest but it is not
   free on a large report: 0.47 s for the 86 MB JSONL. A resumable emitter is the
   fix and it belongs in the engine, not in the binding. See section 7.

9. **`compareInWorker` is the default for anything interactive.** A 400 ms diff on
   the main thread is a 400 ms frozen UI, and at the ceiling it is seconds. The
   fetch stays on the main thread, per spec 13.7, because that is where the
   application's credentials are. `SharedArrayBuffer` remains the opt in fast path
   and is not implemented: it needs COOP and COEP, which an npm library cannot
   require of its consumers.

10. **Every view into linear memory dies when memory grows**, and the engine grows
    on any call that allocates. `WasmHeap` and `CsvTable` re-derive theirs on an
    identity compare against `memory.buffer`. Nothing in Phase 5 should hold a
    typed array over engine memory across a call into it; use the copies the
    binding hands out.

11. **The staging path splits a chunk larger than 1 MB rather than staging it
    whole**, which is free because the parser resumes across any boundary. There is
    a test that feeds a multiline quoted file one byte at a time.

## 7. What is deliberately not done

1. **A resumable emitter.** `ibha_csvd_emit` is all or nothing, so streaming a
   report out of the engine in bounded memory is not possible and `emitStream`
   produces the whole thing first and then hands it out in pieces. The fix is an
   engine change, either an offset on `ibha_csvd_emit_opts` or an emit-one-row
   entry point, and an offset alone would be O(n^2) under `changes_only`.

2. **`SharedArrayBuffer` page access.** Feature detected and unused. Message
   passing costs about 0.1 ms for a 50 row page, which is not what will make a view
   feel slow.

3. **`deletedRowPlacement: 'sourceOrder'`.** Unchanged since Phase 2: `anchored`
   and `end` are implemented, the third is the mirror walk, and adding it is
   additive.

4. **`IBHA_CSVD_DATE_VALUE`.** Still refused rather than faked, for the same
   reason: spec 5.3 makes it opt in with an explicit input format list, and the
   format list does not exist.

5. **`options.signal` in the worker path** aborts the pump and rejects the caller,
   but does not interrupt a comparison already inside the engine. The engine has no
   cancellation point; adding one means a callback in the parse loop.

## 8. Assumptions still flagged

The same three as Phase 3. All are implemented and none is confirmed. **Flag them,
do not re-decide them.**

1. **The asymmetric ragged row rule.** A row with *extra* fields that are all empty
   is normalized and counted in `stats.ragged_normalized`; a row with *missing*
   fields is `IBHA_CSVD_ERR_RAGGED_ROW`. Three line change in `row_end` in
   `src/parse.c` if a short row should be padded instead.

2. **Duplicate key row numbers are record based, not line based.** Every row number
   in every emitter and in every binding row is the same 1 based record number,
   which differs from the physical line when the file has blank lines or multiline
   quoted fields. Phase 4 made this convention wider again rather than narrower.

3. **`VARCHAR(n)` is counted in characters, not bytes.** `café` is four characters
   in five bytes and does not violate `VARCHAR(5)`. One line in `char_count` in
   `src/validate.c` if the destination column is declared in bytes.

## 9. Blockers needing Manas's action

1. ~~No WebAssembly toolchain.~~ Resolved 2026-08-12.
2. ~~No libFuzzer runtime.~~ Resolved 2026-08-12.
3. ~~No pnpm and no installed JS dependencies.~~ **Resolved 2026-08-13** with your
   approval: `corepack enable pnpm`, then `pnpm install` in `js/`, which brought
   TypeScript 5.9.3, vitest 2.1.9 and `@types/node`. Only `pnpm -r typecheck` and
   `pnpm -r build` need it.

4. **JDK 21 is not installed; JDK 17 is.** Blocks Phase 6 only.

5. **Repos are not git initialized as submodules.** `core/`, `js/` and `java/` are
   ordinary directories in one repository. This now costs more to change than it
   did: the JS package is about to be depended on by two more packages, and
   splitting later means a history rewrite. **Worth deciding before Phase 5 ships
   anything**, since `@ibhatech/csvdiff-view` will import `@ibhatech/csvdiff-core`
   across whatever boundary you choose.

## 10. Working notes

- `python3 scripts-and-commands/run_phase4_checks.py` runs everything. `--quick`
  skips the 15 MB pair in the determinism check, `--c-only` and `--js-only` split
  the two halves.
- `cd js/packages/core && node --test "src/*.test.ts"` is the binding suite and
  needs nothing installed. `pnpm -r typecheck`, `pnpm -r build` and `pnpm -r test`
  need `pnpm install` in `js/` once.
- `pnpm wasm` from `js/` rebuilds the modules and copies them into the package.
  **Run `node scripts-and-commands/gen_abi.mjs` after any change to a public
  struct**, or the gate will tell you to.
- `node scripts-and-commands/check_determinism.mjs` writes its derived fixtures
  into `core/build/determinism/`, which is gitignored.
- The C side is unchanged in its verification: 465 assertions, three sanitizers,
  valgrind, four fuzz targets, warning free under the strict flag set on GCC and
  on the clang wasm build. Keep it that way.

---

## 11. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project. Phases 0 through 4 are complete,
committed, and the tree is clean. Phase 5 is next.

**All toolchain blockers for this phase are cleared.** clang 21.1.8 with the wasm32
backend and `wasm-ld`, libFuzzer, Node 24, and pnpm 9.12 with the JS dependencies
installed. Ask before installing anything further.

Start by reading, in this order:

1. `docs/HANDOFF-phase4.md` - full state. **Section 4 is the contracts Phase 5 has
   to hold to**, section 6 is the eleven things it specifically needs to know, and
   section 7 is what was deliberately left undone and why
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. 13.3 is the output contract, 13.0 fixes the CSS class prefix, and
   13.11 makes desktop browsers the only browser target. It overrides earlier
   sections where they conflict, so do not re-derive decisions from sections 1
   to 12
3. `specs/02-solution-proposal.md` **section 8** - the view component in full:
   package structure, web component against React, the styling contract, row
   presentation modes, and how the table mechanics phase in
4. `js/packages/core/README.md` and `js/packages/core/src/index.ts` - the API you
   are building on, and `handle.ts` for the paging and segment interfaces the view
   will actually call
5. `js/packages/view/src/index.ts` and `js/packages/react/src/index.ts` - what
   exists, which is Phase 0 scaffolding written before the engine did

Then implement **Phase 5: `@ibhatech/csvdiff-view` and `@ibhatech/csvdiff-react`**.
Scope, in this order:

- **The styling contract of spec 8.3 first**, because both packages depend on it
  and it is cheap: the fixed class set the HTML emitter already writes, the
  `data-finding` attribute, `white-space: pre-wrap` on the cell class, and a
  stylesheet that works with the emitter's output untouched
- **The virtualized table**, which is the real work: bind to `getRowsCompact` and
  decode only the visible page, with the row presentation modes of spec 8.4 and
  the table mechanics of 8.5
- **The React wrapper**, thin over the above per spec 8.2

Constraints:

- **Never materialize the diff.** The view asks for the fifty rows it is about to
  paint and lets them be collected. `handle.index()` retains 14 bytes plus one byte
  per column per row and no cell values; keep it that way
- **Anything the view renders itself needs the escaping guarantee proved the same
  way**: an independent checker, not a review.
  `js/packages/core/src/testkit.ts` has `htmlSafetyViolation`, which is
  `core/tests/emitkit.h` restated independently, and it is the model
- **`row.cells.length` is not the source file's column count** under the column
  policy of spec 6.6. Read the width from the row and the names from
  `handle.columns`
- **Segment offsets are byte offsets, not UTF-16 code unit offsets.** Use
  `sliceByBytes` from `@ibhatech/csvdiff-core`; do not write a second conversion
- **The HTML emitter and the view consume the same cursor and must agree.** Use the
  emitter for bounded output and the virtualized view for browsing a full diff
- Desktop browsers only, per spec 13.11. No mobile memory ceiling to design around
- Do not start the Java binding, which is Phase 6, or the SIMD parser, which is
  Phase 7

Working notes:

- `python3 scripts-and-commands/run_phase4_checks.py` runs the whole gate and must
  keep passing. `cd js/packages/core && node --test "src/*.test.ts"` is the binding
  suite and needs nothing installed; the view and react packages use vitest, which
  is already installed, because a component test needs a DOM
- Relative imports in TypeScript name the `.ts` file they resolve to, and `tsc`
  rewrites them to `.js` on emit. That is deliberate, and
  `js/tsconfig.base.json` says why
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved under
  `scripts-and-commands/`, and a handoff file at the end of the phase

Three assumptions are implemented but unconfirmed and are described in handoff
section 8: the asymmetric ragged row rule, duplicate key row numbers being record
based rather than line based, and `VARCHAR(n)` being counted in characters rather
than bytes. Flag them rather than re-deciding them.
