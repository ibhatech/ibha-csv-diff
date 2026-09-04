# Handoff: Phase 5 complete, the diff renders in a browser and matches the report

Date: 2026-08-14
Next phase: Phase 6, the Java binding

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier where they conflict. 13.6 is the one Phase 6 lives inside: JDK 21 means JNI, not FFM |
| `specs/02-solution-proposal.md` section 9 | Server side and cross language use, which is what the Java binding is for |
| `js/packages/core/README.md` | The binding's API. The Java side has to make the same decisions against the same ABI |
| `core/include/ibha_csvdiff.h` | The ABI. Unchanged in this phase |
| This file, sections 4 and 6 | The contracts Phase 5 established, and what Phase 6 should take from them |

## 2. The headline results

**The HTML emitter and the virtualized view agree, and it is now checked rather
than argued.** Spec 13.3 says they agree by construction because they consume the
same cursor. That is a claim about a design, and the two decode paths are
separate code in two languages: the emitter writes bytes out of engine memory in
C, the view decodes cells through the binding and lays them out in TypeScript.
`view/src/parity.test.ts` drives the real engine over the real fixtures and
compares them row for row and cell for cell, including at the p90.

```
2,198 changed rows, twelve columns: same row classes, same row numbers,
same cell classes, same data-finding, same values, same old values
```

**Escaping is proved a third time, independently, and the third checker is
stronger than the first two had to be.** The C engine has `core/tests/emitkit.h`,
the binding has `htmlSafetyViolation` in `core/src/testkit.ts`, and the view now
has its own in `view/src/testkit.ts`, restated from the rules of HTML rather than
translated. It had to gain a rule the other two do not need; see section 5.4.

**The view never materializes the diff, and there is a test that scrolls 90,000
rows to prove it.** Value pages are capped at four, 200 rows; structure pages at
64, which is 38 KB at twelve columns.

**111 new assertions**, all passing, plus the whole Phase 4 gate unchanged.

```
[     ok] shipped stylesheets are current            4 stylesheets are current
[     ok] view suite, including emitter parity       7 files, 83 tests
[     ok] react suite, including the XSS corpus      2 files, 28 tests
[     ok] package build
[     ok] typescript typecheck
gate passed
```

## 3. What is done and verified

`python3 scripts-and-commands/run_phase5_checks.py` runs everything: the Phase 3
C gate, the Phase 4 binding gate, and the Phase 5 additions. All of it passes.
`--view-only` runs Phase 5 alone in about eight seconds.

### New in `js/packages/view/src/`

| File | Contents |
|---|---|
| `classes.ts` | The class contract, the prefix validation, the cell flag bits, the composed class lists |
| `theme.ts` | The CSS custom property contract and `themeToCssVars`, moved here from the React package so every binding shares it |
| `stylesheet.ts` | The default stylesheet as a function of the prefix, plus the three themes |
| `virtual.ts` | `computeWindow`, `slotsPerRow`, `pagesFor`, `scrollToShow` |
| `source.ts` | `DiffRowSource` and the two adapters, `localRowSource` and `remoteRowSource` |
| `rowModel.ts` | The four presentation modes of spec 8.4, and `cellPieces` over the engine's segments |
| `viewModel.ts` | The model: paging, the two caches, filtering, focus, column widths, next change |
| `testkit.ts` | **Exported as `@ibhatech/csvdiff-view/testkit`.** The independent safety checker, the XSS corpus, and `fakeRowSource` |
| `*.test.ts` | 83 assertions across seven suites, including the parity test |
| `../styles/*.css` | **Generated.** Four files. Do not edit; run `gen_stylesheet.mjs` |

### New in `js/packages/react/src/`

| File | Contents |
|---|---|
| `useDiffView.ts` | The store subscription, the prop sync, the ResizeObserver, the keyboard handler |
| `CsvDiffTable.tsx` | The component: colgroup widths, sticky header, spacer rows, the four layouts |
| `CsvDiffToolbar.tsx` | The "show unchanged rows" checkbox and the layout picker, controlled |
| `*.test.tsx` | 28 assertions. Most through `react-dom/server`; `interaction.test.tsx` uses jsdom |

### New in `scripts-and-commands/`

| File | Contents |
|---|---|
| `gen_stylesheet.mjs` | Generates `styles/*.css`; `--check` fails when they are stale |
| `run_phase5_checks.py` | The whole gate. `--view-only`, `--js-only`, `--c-only`, `--quick` |

## 4. The contracts Phase 5 established

### 4.1 The class set is the emitter's, and it is closed

Spec 8.3 sketches a BEM-ish list, `.csvd-row--modified` and so on, written before
the emitter existed. The emitter writes eighteen flat suffixes behind a validated
prefix, and section 13 overrides sections 1 to 12 where they conflict. One
stylesheet must style a saved report and the live view untouched, so the flat set
wins:

```
report table th num row cell
unchanged modified added deleted moved
changed suppressed finding old new del ins
```

Everything the view needs to express beyond that is a `data-` attribute:
`data-layout`, `data-variant`, `data-pane`, `data-old-value-position`,
`data-virtualized`, `data-change`, `data-moved`, `data-column`, `data-key`,
`data-pending`. Three view-only classes exist, `mark`, `spacer` and `pane`,
because they are elements the emitter has no equivalent of rather than states of
one it writes.

`classes.test.ts` reads `core/src/emit.c` and compares the two sets. That is the
only way a contract shared by a C file and a TypeScript file that cannot see each
other stays true, and the failure it prevents is silent: a class the emitter
writes and the sheet does not style looks like a bug in the consumer's CSS.

### 4.2 `DiffRowSource` is the seam, and the two adapters are separate on purpose

The view talks to a `DiffRowSource`, never to a handle. `localRowSource` wraps a
`DiffHandle`, `remoteRowSource` wraps a `RemoteDiffHandle`, and both are typed
structurally, so the headless package has no runtime import of the engine except
`sliceByBytes` and a test can substitute a fake.

They are not one function that sniffs the handle. `DiffHandle.index` takes
`{ changesOnly }` and `RemoteDiffHandle.index` takes a bare boolean, so a sniffing
adapter that guessed wrong would post `changesOnly: { changesOnly: true }` across
the worker boundary, which is truthy, which silently filters a report the caller
asked not to filter.

### 4.3 What the view retains, and the two caches

Four decoded value pages, 200 rows. Sixty four compact structure pages, one byte
per cell, 38 KB at twelve columns. `retainedPages` reports both and a test
scrolls a 90,000 row report end to end asserting they do not grow.

### 4.4 The filter is a new source, not a predicate

Filtering rebuilds the report index in the engine, per spec 8.4, so the
virtualizer only ever sees the filtered count. A consumer memoizes a new source
when `changesOnly` changes; `setSource` drops every cache, because a position in
the filtered report means a different row.

## 5. Decisions made in this phase, and why

1. **The stylesheet is a function, and its output for the default prefix is
   checked in.** The prefix is configurable per component, so a consumer who
   changes it would otherwise hand write the sheet; a bundler imports a file, not
   a function, so both have to exist. Generating one from the other and diffing
   it in the gate is the same argument `gen_abi.mjs` makes about the struct
   offsets: two copies of a thing that must agree, where neither is a compile
   error when it drifts.

2. **The view binds to `getRowsCompact` and to `getRows`, and keeps sixteen times
   as many of the former.** The obvious reading of "bind to the compact form" is
   that it replaces the value form, and it cannot: a table shows text. What it
   actually buys is that a page whose decoded values have been evicted still
   knows its kinds, its row numbers and its per cell flags, so it repaints in the
   right colours the instant it scrolls back and only its text is re-fetched. One
   byte per cell is what makes keeping sixty four of them free.

3. **Fixed row height, so the two-row layouts give every report row two slots.**
   Expanding only changed rows in `stacked` and `unified` would need the measured
   offset table that spec 8.5 puts in 0.3 and calls the hard one: the scrollbar
   would have to know how many rows above the viewport were changed, which is a
   prefix sum over the whole report. It costs nothing in practice, because those
   layouts are read with `changesOnly` on.

4. **The safety checker gained an attribute name rule.** The C emitter never puts
   file data in an attribute, so its checker rejects a raw `&` there outright. The
   view puts a column name in `data-column` and cell values in `aria-label`, so
   that rule had to become the correct HTML one, that a `&` must open a character
   reference. That alone accepts `" onload="x`, which contains no angle bracket
   and produces well formed markup. So the checker parses an open tag as name and
   value pairs and requires every name to be structural, `data-` or `aria-`. Both
   directions are asserted, and every corpus value is checked to be rejected when
   inserted raw, because a checker nothing can fail is decoration.

5. **The corpus split in two.** `&lt;script&gt;` as a cell value is not an attack:
   written through unescaped it renders as the text `<script>`, which is a report
   that lies about its input rather than one that executes. Keeping it in the XSS
   corpus would have forced either a weaker checker or a test asserting something
   false, so it lives in `ENTITY_VALUES` and is checked for correct display
   instead.

6. **Build runs before typecheck in the gate now.** `dist` is gitignored and the
   view package's declarations are what the React package typechecks against, so
   on a clean checkout the Phase 4 order reported a missing module as a type
   error.

7. **The React tests mostly do not use jsdom.** `renderToStaticMarkup` produces
   the markup string the safety checker wants and needs no DOM, and everything
   about scroll math lives in the headless package. jsdom is used for exactly two
   things, a scroll event and a keydown, which is what it is actually for.

8. **`theme` still emits custom properties and never inline styles**, and there is
   now a test asserting no cell carries a `style` attribute. Inline styles beat
   the consumer's own stylesheet on specificity, and a JS object cannot express
   `:hover`, dark mode, print or container queries.

## 6. What Phase 6 needs to know

1. **The ABI did not change in this phase.** `core/include/ibha_csvdiff.h` is
   exactly what Phase 4 left, 51 marked declarations, and the JNI binding sees
   the same surface the JS binding does.

2. **Read `js/packages/core/src/engine.ts` before writing the JNI decode path.**
   It is the worked example of the decision Phase 6 has to make again: decode
   cells by reading the columnar arrays directly rather than through an accessor
   call per cell, because at 1.8 million cells the call overhead dominates the
   diff. Spec 13.6 makes the same point about JNI and is the reason the row feed
   ABI takes a batch rather than a field.

3. **The row contract has two independent implementations now and they are
   checked against each other.** A third, in Java, should be checked the same
   way: `engine.test.ts` asserts the binding's decoded rows against the JSONL
   emitter's own output on the p90 pair, field for field. That test is the model.

4. **The determinism check is the harness to extend, not to copy.**
   `check_determinism.mjs` already compares native against both wasm builds over
   a fixture and emitter matrix. A Java build is a fourth producer of the same
   bytes and belongs in the same comparison.

5. **`summary()` runs the summary emitter and parses its JSON rather than reading
   the stats struct**, because cell level counters accumulate as a cursor
   advances. The Java binding should do the same, for the same reason.

6. **The three flagged assumptions are unchanged.** See section 8.

7. **JDK 21 is still not installed.** See section 9.

## 7. What is deliberately not done

1. **Column virtualization, and variable row height for multiline cells.** Spec
   8.5 puts both in 0.3. Column virtualization matters at 200+ columns; variable
   height needs a measured offset table, which is also what non-uniform stacked
   rows need, so the two arrive together.

2. **Column resize by drag.** The view model owns widths, applies them as custom
   properties and has `setColumnWidth`, which is the 0.2 requirement. What is
   missing is the drag handle in the React component, which is a pointer event
   handler and no new state.

3. **Column pinning, hide and show, reorder in view, and sort.** Spec 8.5 puts
   them in 0.4. Sorting in particular overrides the target order constraint and
   is meant to be opt in and clearly indicated, which is a design decision this
   phase had no reason to make.

4. **`@ibhatech/csvdiff-element`, the custom element wrapper.** Spec 8.1 lists it
   as optional and 8.2 as a secondary artifact. Everything it needs is in the
   headless package; it is a rendering layer, and writing it now would be
   guessing at a consumer who has not appeared.

5. **Export of the visible or full diff from the view.** Spec 8.5 puts it in 0.4
   and says streamed from wasm, never materialized. `handle.emit` already does
   the work; what is missing is a button and a download, and the honest version
   waits for the resumable emitter in section 7 of the Phase 4 handoff.

6. **A resumable emitter.** Unchanged from Phase 4. `ibha_csvd_emit` is all or
   nothing, so `emitStream` produces the whole report first and then hands it out
   in pieces.

## 8. Assumptions still flagged

The same three, unchanged since Phase 3. All are implemented and none is
confirmed. **Flag them, do not re-decide them.**

1. **The asymmetric ragged row rule.** A row with extra fields that are all empty
   is normalized and counted in `stats.ragged_normalized`; a row with missing
   fields is `IBHA_CSVD_ERR_RAGGED_ROW`. Three line change in `row_end` in
   `src/parse.c` if a short row should be padded instead.

2. **Duplicate key row numbers are record based, not line based.** Every row
   number in every emitter, in every binding row and now in the view's row number
   gutter is the same 1 based record number, which differs from the physical line
   when the file has blank lines or multiline quoted fields.

3. **`VARCHAR(n)` is counted in characters, not bytes.** `café` is four characters
   in five bytes and does not violate `VARCHAR(5)`. One line in `char_count` in
   `src/validate.c` if the destination column is declared in bytes.

## 9. Blockers needing Manas's action

1. ~~No WebAssembly toolchain.~~ Resolved 2026-08-12.
2. ~~No libFuzzer runtime.~~ Resolved 2026-08-12.
3. ~~No pnpm and no installed JS dependencies.~~ Resolved 2026-08-13.
4. ~~No React types and no DOM for component tests.~~ **Resolved 2026-08-14** with
   your approval: `pnpm add -w -D @types/react @types/react-dom jsdom`. React and
   react-dom 19.2.8 were already present as auto-installed peers.

5. **JDK 21 is not installed; JDK 17 is.** This now blocks the next phase rather
   than a later one. Spec 13.6 pins the binding to JDK 21 with hand written JNI,
   because on 21 the FFM API is a preview feature and a library cannot require
   `--enable-preview` of its consumers. Building against 17 would mean either
   dropping to a lower bytecode target than the spec decided on, or discovering
   the difference at the end.

6. **Repos are not git initialized as submodules.** `core/`, `js/` and `java/` are
   ordinary directories in one repository. This got more expensive again: three
   npm packages now depend on each other by workspace protocol, and the view
   package's tests read `core/src/emit.c` and `core/fixtures/generated/` by
   relative path to check contracts across the boundary. Splitting the JS repo out
   would break those two paths and they are load bearing. **Worth deciding before
   the Java repo is written**, because it is about to become the same question
   with one more answer.

## 10. Working notes

- `python3 scripts-and-commands/run_phase5_checks.py` runs everything. `--quick`
  skips the 15 MB pair in the determinism check, `--view-only` runs Phase 5 alone,
  `--c-only` and `--js-only` split the halves.
- `cd js/packages/core && node --test "src/*.test.ts"` still needs nothing
  installed. The view and react suites need `pnpm install`, because a component
  test needs a DOM and vitest is where jsdom is wired up.
- Both vitest configs alias the workspace packages to their **sources**, not their
  `dist`, so the suites run on a checkout that has never been built.
- **Run `node scripts-and-commands/gen_stylesheet.mjs` after any change to
  `stylesheet.ts`**, or the gate will tell you to. Same for `gen_abi.mjs` after
  any change to a public struct.
- `pnpm -r build` must run before `pnpm -r typecheck` on a clean checkout.
- The C side is unchanged in this phase and unchanged in its verification: 465
  assertions, three sanitizers, valgrind, four fuzz targets, warning free under
  the strict flag set. Keep it that way.

---

## 11. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project. Phases 0 through 5 are complete,
committed, and the tree is clean. Phase 6 is next: the Java binding,
`com.ibhatech:ibha-csvdiff-java`.

**One toolchain blocker is open and you should confirm it before starting.** Spec
13.6 pins this binding to JDK 21, and the machine has JDK 17. clang 21.1.8, gcc,
Node 24 and pnpm 9.12 are all present. Ask before installing anything.

Start by reading, in this order:

1. `docs/HANDOFF-phase5.md` - full state. Section 6 is the seven things Phase 6
   specifically needs, section 7 is what was deliberately left undone, section 9
   is the JDK question
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. **13.6 is the one this phase lives inside**: JDK 21 means hand
   written JNI rather than FFM, and the row feed ABI takes a batch of rows in a
   direct `ByteBuffer` rather than a field, because at 1.8 million cells the JNI
   call overhead would dominate the diff. It overrides earlier sections where
   they conflict, so do not re-derive decisions from sections 1 to 12
3. `specs/02-solution-proposal.md` **section 9** - server side and cross language
   use, which is what this binding is for
4. `core/include/ibha_csvdiff.h` - the ABI, unchanged since Phase 4
5. `js/packages/core/src/engine.ts` and `js/packages/core/README.md` - the worked
   example of every decision this phase has to make again, in a different
   language

Then implement **Phase 6: the JNI binding and the Java API**.

Constraints:

- **One implementation of diff semantics.** The parser, the matcher, the
  comparators and the emitters are in the C core. The Java side feeds it bytes and
  decodes the rows it hands back, and decides nothing about the diff
- **Decode cells out of the columnar arrays, not through an accessor call per
  cell.** That is what `engine.ts` does and why, and spec 13.6 makes the same
  point about the JNI boundary specifically
- **The row contract needs a third implementation and it must be checked against
  the first two.** `engine.test.ts` asserts the binding's decoded rows against the
  JSONL emitter's own output on the p90 pair, field for field. That test is the
  model, not a review
- **The Java build is a fourth producer of the same bytes.** Extend
  `scripts-and-commands/check_determinism.mjs` rather than writing a second
  comparison: native against scalar wasm against SIMD wasm against Java must be
  byte identical
- **The public Java API is an interface with the implementation selected at
  runtime**, per 13.6, so an FFM implementation can be added for JDK 22+ later
  without a change visible to consumers
- Do not start the SIMD parser, which is Phase 7, and do not add view features
  deferred to 0.3 and 0.4 in handoff section 7

Working notes:

- `python3 scripts-and-commands/run_phase5_checks.py` runs the whole gate and must
  keep passing. `--quick` skips the 15 MB pair; `--view-only` skips everything
  before Phase 5
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved
  under `scripts-and-commands/`, and a handoff file at the end of the phase

Three assumptions are implemented but unconfirmed and are described in handoff
section 8: the asymmetric ragged row rule, duplicate key row numbers being record
based rather than line based, and `VARCHAR(n)` being counted in characters rather
than bytes. Flag them rather than re-deciding them.
