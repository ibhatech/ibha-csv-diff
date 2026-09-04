# Handoff: Phase 1 complete

Date: 2026-08-06
Next phase: Phase 2, the diff engine and the matcher

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier in that document where they conflict |
| `specs/02-solution-proposal.md` sections 6, 7 | Row matching, move detection, report ordering, cell level diffing. This is Phase 2's specification |
| `specs/02-solution-proposal.md` section 5.3 | Normalization and the declared type comparators, which Phase 2 must build on the Phase 1 index |
| `docs/HANDOFF-phase0.md` | The invariants, the naming table and the two flagged assumptions |
| `core/README.md` | Build instructions, the parser design in one paragraph, and the measured throughput |
| `core/include/ibha_csvdiff.h` | The ABI Phase 2 extends. The columnar index and the schema are concrete public structs |

## 2. The headline number

**About 800 MB/s** for the state machine and the index, 793 to 814 across runs,
and **454 MB/s** with row digests, on an M1 Pro at `-O3` over a 15 MB 20 column
file. `make bench`.

The Phase 0 figure of 2776 MB/s was an auto-vectorized byte counter and only ever
an upper bound. This is the real figure and it clears the gate: spec 3.3 predicted
300 to 500 MB/s for a scalar table driven parser and set 300 as the threshold that
keeps SIMD in Phase 7. **SIMD stays in Phase 7.** The p90 15 MB file is fully
indexed with digests in 33 ms; a 150 MB file at the size limit extrapolates to
about 340 ms.

Two findings in that number that should shape Phase 7:

1. **Hashing costs more than parsing.** Digests take throughput from about 800 to
   454 MB/s, so 44% of the total is hash rather than state machine. Spec 3.3
   ranked the structural scan first for SIMD payoff and hashing third. On this
   evidence the ranking inverts for the batch profile. Re-measure before
   committing Phase 7's effort to the structural scan.
2. **The streamed path costs 4%, not 40%.** Copying bytes into the arena on the
   way past is nearly free next to the parse, so the `ReadableStream` primary
   path of spec 2.5 carries no meaningful penalty against the mmap path. The zero
   copy path exists and is used by the benchmark and the tests, but nothing needs
   to be redesigned around it.

## 3. What is done and verified

All run and passing on Apple M1 Pro, macOS, Apple clang 21.

- **200 assertions pass**, up from 53. `make test`
- **Clean under AddressSanitizer and UndefinedBehaviorSanitizer.** `make asan`
- **4.4 million fuzz inputs, no invariant violated.** `make fuzz-native`
- **Compiles warning free** under the same strict flag set, tests included
- **CLI parses the 14.95 MB fixture end to end** at 437 MB/s and reports the index

### New in `core/`

| File | Contents |
|---|---|
| `src/parse.c` | The resumable state machine, the columnar index, ragged handling, index sizing |
| `src/schema.c` | The four header row model, target header auto-detection, column order diagnosis |
| `src/hash.c` | XXH3 subset, the logical field comparator, field hashing, unescaping |
| `bench/bench_parse.c` | Replaces `bench_ingest.c`. Four measurements plus the Phase 0 reference |
| `tests/test_parse.c` | Quoting forms, line endings, EOF edges, blank lines, BOM, errors, ragged, limits |
| `tests/test_schema.c` | The four detection outcomes and the three ways detection can fail |
| `tests/test_hash.c` | 44 reference vectors, the comparator, chunked hashing of long values |
| `tests/test_property.c` | The two universal properties, over the fixture corpus |
| `tests/fuzz_parse.c` | Fuzzes the state machine, asserting index invariants and whole-versus-chunked equality |
| `tests/fuzz_main.c` | Standalone fuzz driver, so the targets run without libFuzzer |
| `cli/main.c` | Adds `ibha-csvdiff parse` |
| `scripts-and-commands/gen_xxh3_vectors.py` | Regenerates the golden hash vectors from `xxhsum` |

### The parser

Table driven, six states, indexed by a 256 entry byte class table built from the
dialect so a tab delimited or single quoted file needs no separate code path.

`FIELD_START`, `UNQUOTED`, `QUOTED`, `QUOTE_IN_QUOTED`, `AFTER_QUOTED` are the
five from the plan. **`AFTER_CR` is a sixth**, and it is not optional: a record
terminated by a bare CR has to carry one bit of state so that an LF arriving as
the first byte of the next chunk is recognized as the second half of a CRLF
rather than as an empty record. It is resolved before the transition table is
consulted, because unlike every other state it may need to leave its byte
unconsumed.

Runs of ordinary bytes are consumed by a skip loop rather than one table step
each, which is where most of the throughput comes from. The tables remain the
definition of the grammar; the skip loops only shortcut transitions that do
nothing.

**Resumability is structural.** Bytes accumulate contiguously and the index
stores offsets into them, so a field split across chunks needs no partial field
buffer: by the time the field ends, both halves are adjacent. This is why the
index holds offsets rather than pointers, per spec 3.2. The property is asserted
across every fixture at nine chunk strides including a randomized one, on every
possible split point of an escape pair, and by the fuzzer on every input.

### Three parser behaviours that were decisions, not defaults

Each is a place where real files disagree with RFC 4180, and where the choice
decides whether a legitimate file is rejected or a broken one silently misread.

1. **A quote inside a bare field is data.** `say "hi",b` parses. Rejecting it
   would fail on files no spreadsheet has trouble with.
2. **Padding between a closing quote and the delimiter is skipped, and anything
   else after a closing quote is an error.** `"ab" ,c` parses, `"ab"c,d` fails
   with `BAD_CONTENT` naming the row and field. That is what `AFTER_QUOTED` is
   for. The alternative, silently discarding bytes the user can see in their
   file, is the worst failure mode a diff tool can have.
3. **A wholly empty line is not a record.** Stray blank lines are common and a
   one field empty row would be reported as ragged. Nothing is lost: a
   deliberately empty single column row is written `""` and is still a record.
   Blank lines are counted in the parse stats.

### Header model

The source is schema-authoritative, per spec 13.8. Target detection reads rows as
they complete and stops at the first that matches the source's column names, so
it fails fast rather than after reading the file. All four outcomes are covered by
tests: detected at row 1 (names-only, inherits everything), detected at row 4,
detected at some row in between, and not found.

**Detection failure produces the most specific error it can.** "Could not find
the header" is useless when the header is plainly there and one column was
renamed or the file gained a column, so the diagnosis picks the row that most
resembles the header and reports what is actually wrong with it:
`COLUMN_ORDER` naming the column and both names, distinguishing a reorder from a
rename, and reporting a column count difference with both counts. Only a file
with no resemblance at all falls through to `NO_HEADER`.

### Hashing

`ibha_xxh3_64` is XXH3-64, one shot, unseeded, default secret, covering all four
length classes including the long path. **Verified bit for bit against the
reference implementation** at 44 lengths chosen to cross every branch, via
`xxhsum`. That matters because the digest protocol in spec 2.5 has a server side
computing row hashes, and a hash that is merely "a good hash" would silently
produce wrong diffs rather than failing.

Field hashing never allocates and never builds a string. Values longer than 4 KB
fold over chunks of the *logical* byte stream, which is what lets the escaped
path collapse `""` through a fixed stack window and land on the same value as the
unescaped path.

## 4. Invariants, all still held

- No global mutable state. Every entry point takes `ibha_csvd_ctx *`
- Arena allocation only. Nothing freed individually
- One error per context, first one wins
- Streaming is the only ingest path; one shot and zero copy are adapters over it
- No string materialization in the engine. `ibha_csvd_field_copy` is the single
  sanctioned boundary and exists for rendering and export
- Deterministic output regardless of chunk size, asserted rather than assumed

## 5. Things Phase 2 needs to know

1. **Row digests are byte exact on logical values, with no normalization.**
   `row_full_hash` and `row_key_hash` currently hash the unescaped bytes as they
   are. Spec 5.3 requires the comparators to normalize by declared type, so that
   `1.50` equals `1.5` and `007` equals `7`, and `comparison.trimWhitespace`
   defaults to true. **When Phase 2 adds those, the row digests must be computed
   through the same normalization or the fast path in spec 6.1 step 3 will report
   normalized-equal rows as modified.** The hook is `hash_row` in `src/parse.c`,
   which already has the schema in hand; the declared types are readable in place
   at `schema.type_row`. This is the single most important thing to get right in
   Phase 2 and it is why it is first in this list.

2. **The bucket map from spec 3.1 is not built.** `bucket` and `bucket_mask` are
   deliberately absent from `ibha_csvd_table` rather than present and empty,
   because the matcher owns that structure and Phase 2 should decide its shape.

3. **`row_key_hash` equals `row_full_hash` when no key columns are declared.**
   That is spec 6.4's all-keys case arriving at the index level. The matcher has
   to recognize it rather than assuming the two digests are independent.

4. **Header rows carry a zero digest** and are not compared. `schema.first_data_row`
   is the boundary.

5. **`IBHA_CSVD_ERR_MISSING_KEY_COLUMN` and `IBHA_CSVD_ERR_DUPLICATE_KEY` are
   defined and unused.** Duplicate key detection is spec 13.9 and belongs to the
   matcher, since it needs the bucket map.

6. **Both flagged assumptions from Phase 0 are implemented as stated.** See
   section 7 below; one of them needed a reading that is worth confirming.

## 6. Two things worth doing that Phase 1 did not

- **The `p90_edits.json` script is not yet asserted against.** The fixture
  generator writes the exact edit script it applied, and spec section 10 wants
  the diff to recover exactly that script. That is Phase 2's headline property
  test and the fixture is already waiting for it.
- **Cross-target determinism is unverified.** Spec 3.2 asserts the wasm32 and
  native builds produce byte identical output, and that cannot be checked until
  a wasm toolchain exists. The code is portable C with no target dependent
  behaviour, and `mul128_fold64` has a 32 bit fallback for exactly this reason,
  but it is unproven.

## 7. The two flagged assumptions, and how they were read

Both were implemented as the default and neither is confirmed. Changing either is
cheap.

1. **Ragged rows.** Implemented as: a row with *extra* fields that are all empty
   is normalized by dropping them and counted in `stats.ragged_normalized`; a row
   with *missing* fields is `IBHA_CSVD_ERR_RAGGED_ROW` naming the row and both
   counts. **The asymmetry is the reading worth confirming.** Excel emits trailing
   empty columns, which is the case spec 13.5 names, and those are extra fields.
   A short row means data is missing, and padding it would hide exactly the
   corruption a diff exists to surface. If a short row should instead be padded
   with empty fields, that is a three line change in `row_end` in `src/parse.c`.

2. **Added and removed columns are errors.** Both surface as
   `IBHA_CSVD_ERR_COLUMN_ORDER` with a message naming both column counts. The
   question from spec 13.10 stands: should a salesman appending a column be a
   warning instead? If so it becomes a finding rather than an error, and the
   finding infrastructure does not exist yet, so it is worth deciding before
   Phase 2 builds one.

## 8. Blockers needing Manas's action

Unchanged from Phase 0, plus one new one. None blocks Phase 2.

1. **No WebAssembly toolchain.** Apple clang has no WebAssembly backend and macOS
   ships no `wasm-ld`. Needs `brew install llvm` or wasi-sdk; wasi-sdk is
   preferable for CI because it pins a version. **CLAUDE.md requires asking
   before running `brew`, so this was not installed.** Blocks the cross-target
   determinism check and Phase 4.
2. **No libFuzzer runtime.** New this phase. Apple clang does not ship
   `libclang_rt.fuzzer`, so `make fuzz` cannot link. Mitigated rather than
   blocked: `make fuzz-native` runs the same two targets under a self contained
   driver with ASan and UBSan, which is what produced the 4.4 million inputs
   above. It has no coverage feedback, so a real libFuzzer run on CI is still
   worth having. Same `brew install llvm` fixes it.
3. **JDK 21 is not installed; JDK 17 is.** Blocks Phase 6 only.
4. **Repos are not git initialized as submodules.** Manas is creating the three
   remotes.

## 9. Working notes

- `cd core && make test` runs 200 assertions and generates the fixtures if they
  are missing. `make asan` runs the same under sanitizers. `make bench` is the
  parser benchmark. `make fuzz-native` runs 200,000 fuzz inputs per target;
  `FUZZ_ITERS=5000000 make fuzz-native` for a long run.
- The core and the tests compile warning free under
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings`.
  Keep it that way.
- `cd js && pnpm -r test` runs 36 JS tests. Phase 1 did not touch them.
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved
  under `scripts-and-commands/`, and a handoff file at the end of each phase.

---

## 10. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project in this repository. Phases 0 and 1
are complete and Phase 2 is next.

Start by reading, in this order:

1. `docs/HANDOFF-phase1.md` - full state, what is done, what is next, and the
   invariants to preserve. Section 5 lists what Phase 2 specifically has to know
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. It overrides earlier sections of that document where they conflict,
   so do not re-derive decisions from sections 1 to 12
3. `specs/02-solution-proposal.md` sections 6 and 5.3 - row matching, move
   detection, report ordering, and the normalization rules that select a
   comparator per declared type
4. `core/README.md`, `core/include/ibha_csvdiff.h` and `core/src/internal.h` -
   the ABI and internals you will build on. The columnar index and the schema are
   concrete public structs and Phase 2 extends them

Then implement **Phase 2: the diff engine, the matcher and the cursor**. Scope:
the open addressing bucket map over `row_key_hash`, keyed matching per spec 6.1,
duplicate key detection as a hard error per spec 13.9, move detection by longest
increasing subsequence per spec 6.2, the all-keys similarity pairing of spec 6.4,
report ordering with `deletedRowPlacement: 'anchored'` per spec 6.5, the pull
cursor of spec 13.3, and the type aware comparators of spec 5.3.

Constraints:

- **The row digests must be computed through the same normalization the
  comparators use.** They currently hash raw logical values with no
  normalization. If the comparator says `1.50` equals `1.5` but the digest does
  not, the unchanged-row fast path of spec 6.1 step 3 reports normalized-equal
  rows as modified. Handoff section 5 point 1 has the details and the hook.
- Peak memory in cursor mode must stay bounded by the two indexes plus one row.
  The report index array of spec 6.5 is not built unless a consumer asks for
  random access.
- Keep the invariants in handoff section 4: no global state, arena only,
  first-error-wins, streaming only, no string materialization, deterministic
  output.
- Do not start the emitters, the view or the bindings. Those are Phases 3 and
  later.

Working notes:

- `cd core && make test` runs 200 assertions; `make asan` runs them under
  AddressSanitizer and UBSan; `make bench` is the parser benchmark;
  `make fuzz-native` runs the fuzz targets without needing libFuzzer. All
  currently pass and must keep passing.
- The core and the tests compile warning free under a strict flag set. Keep it
  that way.
- `fixtures/generated/p90_edits.json` records the exact edit script the generator
  applied to produce `p90_target.csv` from `p90_source.csv`. Spec section 10 wants
  the diff to recover exactly that script; that is Phase 2's headline property
  test.
- Neither a wasm toolchain nor a libFuzzer runtime is available on this machine.
  Ask before running `brew`.
- Per CLAUDE.md: write python scripts rather than bash with quoted variables,
  save them under `scripts-and-commands/`, and write a handoff file at the end of
  the phase.

Two assumptions from Phase 0 are implemented but unconfirmed and are described in
handoff section 7: the asymmetric ragged row rule, and added or removed columns
being errors rather than warnings. Flag them rather than re-deciding them.
