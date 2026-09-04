# Handoff: Phase 0 complete

Date: 2026-08-03
Author: previous session
Next phase: Phase 1, the RFC 4180 parser and the columnar index

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/01-solution-design.md` | The original problem statement and requirements |
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier in that document where they conflict |
| `specs/02-solution-proposal.md` sections 2.6, 3, 5, 6 | Architecture, data layout, parser and matching design |
| `core/README.md` | Build instructions and the measured Phase 0 baseline |

Section 13 exists because the eleven open questions were all answered. Do not
re-derive those decisions; sections 1 to 12 of the proposal contain superseded
reasoning that section 13 corrects in several places.

## 2. Naming, fixed

| Thing | Name |
|---|---|
| Repos | `ibha-csvdiff-core`, `ibha-csvdiff-js`, `ibha-csvdiff-java` |
| Submodule paths in this repo | `core/`, `js/`, `java/` |
| C header / symbol prefix | `ibha_csvdiff.h` / `ibha_csvd_` |
| Static library / CLI | `libibha_csvdiff.a` / `ibha-csvdiff` |
| npm packages | `@ibhatech/csvdiff-core`, `@ibhatech/csvdiff-view`, `@ibhatech/csvdiff-react` |
| Java group / artifact / package | `com.ibhatech` / `ibha-csvdiff-java` / `com.ibhatech.csvdiff` |
| CSS class prefix | `ibha-csvd-`, configurable per component |

`-wasm` was deliberately dropped from the family name: WASM is one of four build
targets and the native builds serve the batch profile.

## 3. What is done and verified

### `core/` - Phase 0 complete

Everything below was run and passed on Apple M series, macOS, Apple clang 21.

- **53 unit assertions pass.** `make test`
- **Clean under AddressSanitizer and UndefinedBehaviorSanitizer.** `make asan`
- **Compiles warning free** under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith
  -Wwrite-strings`
- **CLI works end to end** against a generated 14.95 MB fixture, and the
  `TOO_LARGE` path returns exit code 2 with the specified message

Implemented:

| File | Contents |
|---|---|
| `include/ibha_csvdiff.h` | Public ABI. Status enum, limits, context, pull reader, ingest |
| `src/arena.c` | Chunked bump allocator, growable byte buffer |
| `src/ctx.c` | Context lifecycle, first-error-wins discipline, freestanding formatter (no libc `snprintf`) |
| `src/reader.c` | Pull reader adapters: fixed buffer, POSIX fd |
| `src/ingest.c` | Phase 0 ingest, limit enforced on arrival, size hint |
| `src/sys_libc.c`, `src/sys_wasm.c` | System memory layer, one per target |
| `cli/main.c` | `ibha-csvdiff ingest`, `diff(1)` exit codes |
| `tests/test_core.c`, `tests/tap.h` | 53 assertions, dependency free harness |
| `tests/fuzz_ingest.c` | libFuzzer target, randomizes chunk stride |
| `bench/bench_ingest.c` | Throughput baseline |
| `fixtures/gen_fixtures.py` | 12 edge case files plus sized source/target pairs with a known edit script |

### `js/` - source layer done, engine binding not started

- **36 tests pass** across three packages, all typecheck under
  `exactOptionalPropertyTypes` and `noUncheckedIndexedAccess`, `pnpm -r build`
  succeeds, and `dist/` correctly excludes test files.
- `packages/core/src/source.ts` **fully implements the spec 13.7 fetch contract**
  and is the most substantive JS code so far: source normalization for
  `Uint8Array` / `ArrayBuffer` / `Blob` / `ReadableStream` / callback-returning-
  `Response`, Content-Type ignored entirely including `application/octet-stream`,
  the HTML-and-JSON-error-page sniff, `maxBytes` enforced on arrival, optional
  `DecompressionStream`, and `Content-Length` used as a hint but ignored when
  `Content-Encoding` is present.
- `packages/core/src/index.ts` pins the `DiffRow` / `DiffCursor` / `DiffSummary`
  contracts. Fixed now because the emitters, the view and the Java binding all
  have to agree, and agreeing later is more expensive.
- `packages/view/src/index.ts` has the class name contract and `computeWindow`,
  the virtualization math, with the scroll clamping cases covered.
- `packages/react/src/index.ts` has `themeToCssVars` and the prop contract.

### `java/` - build configuration only

`build.gradle.kts` and `settings.gradle.kts` exist. **No Java sources and no JNI
glue yet.** This is where the previous session stopped.

## 4. Measured Phase 0 baseline, and how to read it

15 MB CSV shaped in-memory buffer, `-O3`, best of five:

| | Time | Throughput | Arena |
|---|---|---|---|
| Scan only | 5.40 ms | 2776 MB/s | 0.31 MB |
| Scan and retain | 5.57 ms | 2693 MB/s | 16.31 MB, 1.09x input |
| CLI, same file from disk | 8.3 ms | 1810 MB/s | 1.09x |

**Do not treat 2776 MB/s as a parser prediction.** The scan is two branchless
counters that clang auto-vectorizes on NEON. The RFC 4180 state machine has data
dependent branching and will not vectorize the same way; expect materially lower.
The only sound conclusion is that ingest and the memory subsystem are nowhere near
the constraint, which keeps SIMD in Phase 7 as a batch CPU cost lever rather than a
latency fix. **Phase 1 produces the number the SIMD decision actually rests on.**

One defect found and fixed during Phase 0, worth knowing about because the same
trap exists elsewhere: retaining N bytes originally cost **4.25x N** because the
byte buffer doubled and the arena never reclaims abandoned copies. Adding a
`size_hint` parameter took it to **1.09x**. Since spec 2.6.5 makes per-worker
memory the thing that sets batch concurrency, that ratio is now asserted in the
test suite so it cannot silently regress. Any future growable arena structure
needs the same treatment.

## 5. Blockers and prerequisites needing Manas's action

1. **No WebAssembly toolchain.** Apple clang has no WebAssembly backend and macOS
   ships no `wasm-ld`, so `make wasm` cannot work. It fails with explicit
   instructions rather than a confusing compiler error. Needs either
   `brew install llvm` or wasi-sdk. **CLAUDE.md requires asking before running
   `brew`, so this was not installed.** wasi-sdk is preferable for CI because it
   pins a toolchain version. Not blocking Phase 1: the parser is portable C and is
   developed and tested natively.
2. **JDK 21 is not installed; JDK 17 is.** Gradle's toolchain is set to require 21,
   so it will either auto-provision or fail clearly. Blocking for Phase 6 only.
   Note this reinforces the JNI decision: FFM is a preview feature on 21 and is
   absent entirely on 17.
3. **Repos are not git initialized.** Manas is creating the three remotes and
   wiring them as submodules.

## 6. Two assumptions awaiting confirmation

Both are recorded in spec 13.5 and 13.10 and were flagged rather than silently
decided. Phase 1 implements the stated default; changing them later is cheap.

1. **Ragged rows are an error, except that a row differing only by empty trailing
   fields is normalized.** Excel routinely emits trailing empty columns and
   rejecting those files would be a support burden, whereas a genuinely wrong field
   count means the file is broken and a diff of it would mislead.
2. **Added and removed columns are errors by default.** This follows from "whatever
   order the source file has must be continued" plus fail-fast. Open question:
   should a salesman appending a column be a warning instead?

## 7. Phase 1 scope

Deliverable: parse and index correctly, at a measured throughput, in one shot or
streamed. No diffing yet.

1. **Resumable RFC 4180 state machine.** Table driven, 256 entry transition table
   indexed by `(state, byte_class)`, five states: `FIELD_START`, `UNQUOTED`,
   `QUOTED`, `QUOTE_IN_QUOTED`, `AFTER_QUOTED`.
   **Resumability is a Phase 1 requirement, not a later optimization.** Parser
   state plus any partial field must be carried across calls so a chunk boundary
   may split a multi byte UTF-8 sequence, a quoted field, a `""` escape pair or a
   CRLF. Retrofitting this means rewriting the parser. Spec 2.5 point 2.
2. **Columnar index** exactly as laid out in spec 3.1: `field_off` / `field_len` /
   `field_flags` / `row_first_field` / `row_key_hash` / `row_full_hash`. No string
   materialization anywhere.
3. **Field flags** `FIELD_QUOTED`, `FIELD_HAS_ESCAPE`, `FIELD_HAS_NEWLINE`,
   `FIELD_EMPTY`. `FIELD_HAS_ESCAPE` set only when a `""` was actually seen, since
   that is what selects the slow comparison path.
4. **Four header row model plus target auto-detection** per spec 13.8. The source is
   schema-authoritative. For the target, locate the column name row by matching
   against the source's known column names within the first 8 rows; if the match is
   at row 1 the file is names-only and inherits metadata from the source; if no row
   matches, fail with `IBHA_CSVD_ERR_NO_HEADER`.
5. **xxHash3 subset** and the **logical field comparator**, including
   `ibha_csvd_field_cmp_escaped` which collapses `""` to `"` while walking, without
   allocating. Spec 5.2. **Quoting must never surface as a difference.**
6. **Golden corpus and property tests.** The 12 edge case fixtures already exist
   under `core/fixtures/`. Add: parse any fixture whole versus in random chunks and
   assert byte identical indexes; re-emit any fixture under three different quoting
   policies and assert an empty diff.
7. **Extend the fuzzer** from the ingest path to the state machine. This is the
   only untrusted input surface and is the main reason spec 2.3 judged C acceptable.
8. **Replace `bench_ingest`** with the real parser benchmark. This is the number
   that decides whether SIMD stays in Phase 7.

Do not start the diff engine, the matcher, the emitters or the view. Those are
Phases 2 and 3.

## 8. Invariants the code must keep

- No global mutable state. Every entry point takes `ibha_csvd_ctx *`.
- Arena allocation only. Nothing freed individually.
- One error per context, first one wins. No error accumulation.
- Streaming is the only ingest path; one shot is an adapter over it.
- No string materialization in the engine.
- Deterministic output regardless of thread count or chunk size.
- Validation findings are output, not errors. Structural problems are errors.

---

## 9. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project in this repository. Phase 0 is
complete and Phase 1 is next.

Start by reading, in this order:

1. `docs/HANDOFF-phase0.md` - full state, what is done, what is next, and the
   invariants to preserve
2. `specs/02-solution-proposal.md` **section 13** - the authoritative locked
   decisions. It overrides earlier sections of that document where they conflict,
   so do not re-derive decisions from sections 1 to 12
3. `specs/02-solution-proposal.md` sections 3, 5 and 6 - the columnar index layout,
   the parser design and the matching design
4. `core/README.md`, `core/include/ibha_csvdiff.h` and `core/src/internal.h` - the
   existing ABI and internals you will build on

Then implement **Phase 1: the RFC 4180 parser and the columnar index**, as scoped
in section 7 of the handoff. Summary: a resumable table driven state machine, the
columnar index from spec 3.1, field flags, the four header row model with target
header auto-detection per spec 13.8, an xxHash3 subset, the escape aware logical
field comparator, golden and property tests, an extended fuzz target, and a real
parser benchmark to replace `bench_ingest`.

Constraints:

- Resumability across arbitrary chunk boundaries is a Phase 1 requirement, not a
  later optimization. A chunk may split a multi byte UTF-8 sequence, a quoted
  field, a `""` escape pair or a CRLF.
- Quoting must never surface as a difference. Comparison operates on logical
  values.
- Keep the invariants in handoff section 8: no global state, arena only,
  first-error-wins, streaming only, no string materialization, deterministic
  output.
- Do not start the diff engine, matcher, emitters or view. Those are Phases 2 and 3.

Working notes:

- `cd core && make test` runs 53 existing assertions; `make asan` runs them under
  AddressSanitizer and UBSan; `make bench` is the throughput harness;
  `make fixtures` regenerates test data. All currently pass and must keep passing.
- The core compiles warning free under a strict flag set. Keep it that way.
- The wasm target cannot build on this machine: Apple clang has no WebAssembly
  backend and there is no `wasm-ld`. `make wasm` fails with instructions. Phase 1
  is portable C developed and tested natively, so this is not blocking. Ask before
  running `brew`.
- `cd js && pnpm -r test` runs 36 JS tests, all passing. Phase 1 does not touch
  the JS packages.
- Per CLAUDE.md: write python scripts rather than bash with quoted variables, save
  them under `scripts-and-commands/`, and write a handoff file at the end of the
  phase.

Two open assumptions are recorded in handoff section 6 (ragged row handling and
added/removed columns). Implement the stated defaults and flag them rather than
asking up front.

The single most important output of Phase 1 is the **measured parser throughput**,
because that is what the SIMD decision in Phase 7 rests on. The Phase 0 figure of
2776 MB/s is an auto-vectorized byte counter and is an upper bound only, not a
parser prediction.
