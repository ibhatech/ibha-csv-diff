# ibha-csvdiff-core

The C11 diff engine. Compiles to a native static library for server side bindings
and to freestanding `wasm32` for browsers, from one source tree.

Design rationale lives in `specs/02-solution-proposal.md` in the parent project.
Section 13 of that document is authoritative for locked decisions.

## What exists today

Phases 0 to 4 are complete.

**Phase 0** is the plumbing: context lifecycle, arena allocator, the pull based
reader abstraction, the ingest path with limit enforcement, the test harness, the
fuzz harness and the benchmark.

**Phase 1** is the front end: a resumable RFC 4180 state machine, the columnar
index of spec 3.1, the four header row model with target header auto-detection,
an XXH3 subset verified against the reference implementation, and the logical
field comparator that makes quoting invisible to comparison.

**Phase 2** is the engine: the declared type comparators of spec 5.3, keyed
matching over an open addressing bucket map, duplicate keys as a hard error,
move detection by longest increasing subsequence, the all-keys similarity
pairing, anchored placement of deleted rows, and the pull cursor that every
emitter will be built on.

**Phase 3** is the output layer: the `jsonl`, `csv`, `html` and `summary`
emitters, each a loop over the cursor into a caller supplied sink; the versioned
row contract they and their consumers agree on; the validation findings of spec
13.5 flowing through the cursor as cell flags; and the intra cell diff of spec 7.

**Phase 4** is the JavaScript binding, which lives in `js/packages/core`. It adds
three functions to this ABI, all of them things a host that cannot form a C
function pointer or reach inside the diff needs, and the generated struct offset
table that keeps such a host from hardcoding a layout. See the wasm build section.

The view packages are Phase 5, the Java binding is Phase 6 and the hand written
SIMD parser is Phase 7.

## Build

```sh
make            # static library, CLI, tests, benchmark
make test       # 463 assertions, including the golden and property suites
make asan       # the same under AddressSanitizer and UBSan
make ubsan      # undefined behaviour only, trapping, needs no runtime library
make valgrind   # memory safety under memcheck, needs no runtime library
make bench      # parser throughput
make wasm       # freestanding wasm32 modules, scalar and SIMD
make wasm-check # the same, then instantiate and drive them under node
make fixtures   # generate test and benchmark CSV files
make fuzz-native  # fuzz the parser, the matcher, the emitters and ingest
make fuzz         # the same targets under libFuzzer, built with FUZZ_CC
```

Or the whole gate at once, which reports which steps the local toolchain can run:

```sh
python3 ../scripts-and-commands/run_phase3_checks.py
```

```sh
build/ibha-csvdiff diff source.csv target.csv    # match two files and count the report
build/ibha-csvdiff diff a.csv b.csv --format summary          # counts as one JSON object
build/ibha-csvdiff diff a.csv b.csv --format jsonl | jq .     # one JSON object per row
build/ibha-csvdiff diff a.csv b.csv --format html \
                   --changes-only --cell-diff both > report.html
build/ibha-csvdiff parse path/to/file.csv        # index a real file and report it
build/ibha-csvdiff parse file.csv --header 1     # a names-only file
```

### Toolchain gaps, and what stands in for each

Each fails with an explicit message rather than a confusing compiler error, and
each has a fallback that needs nothing extra.

**`make fuzz` is the strong one and it needs clang.** libFuzzer is a clang
runtime with no GCC equivalent, so the fuzz targets build with `FUZZ_CC`, which
defaults to `clang` and leaves `CC` alone for every other target. On Fedora, RHEL
and Oracle Linux the runtime comes with the `clang` package; Apple's bundled
clang does not ship one, so point at one that does with
`make fuzz FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang`.

```sh
make fuzz
python3 ../scripts-and-commands/run_libfuzzer.py --seconds 90
```

That script seeds a corpus from the fixtures, runs all four targets, and fails on
any `crash-`, `leak-`, `timeout-` or `oom-` artifact rather than trusting the exit
status. The corpus lives in `core/corpus` and is kept between runs, because
coverage feedback is what grows it and throwing it away throws away the
interesting inputs.

`make fuzz-native` runs the same four entry points under a self contained driver
with a dumb generator and no coverage feedback. It exists so the untrusted input
surface is exercised on a machine with no libFuzzer, not because it is
equivalent: on the emitters, libFuzzer reaches about 4,500 executions a second
against the native driver's blind sweep. Keep both.

The emitter target is not only a crash hunt: it re-checks every output with the
independent checkers in `tests/emitkit.h`, so an escaping regression fails the
run rather than producing plausible looking markup.

`make asan` and `make fuzz-native` both need the AddressSanitizer runtime, which
is a separate package on Fedora and RHEL (`sudo dnf install libasan libubsan`).
Without it, `make ubsan` still catches every undefined behaviour by trapping
instead of reporting, `make valgrind` still catches memory safety, and
`make fuzz-native-notrap` still runs the fuzz targets and their invariants.

One platform difference worth knowing: **LeakSanitizer is enabled by default
under ASan on Linux and disabled on macOS.** A leak that a Mac run reports as
clean will fail the Linux run, so the Linux run is the one to gate CI on.

### The wasm build

`make wasm` produces two freestanding `wasm32` modules, scalar and SIMD, from the
same sources as the native build with a different system layer. They need a clang
with the WebAssembly backend plus `wasm-ld`; on Oracle Linux and Fedora that is
`dnf install clang lld`, and Apple's bundled clang has neither, so point the build
at one that does:

```sh
brew install llvm
make wasm WASM_CC=/opt/homebrew/opt/llvm/bin/clang \
          WASM_LD=/opt/homebrew/opt/llvm/bin/wasm-ld
```

`wasi-sdk` also works and pins a known toolchain version, which is preferable for
reproducible CI builds. `make wasm` fails with an explicit message rather than a
confusing compiler error when neither is present.

Two properties of the artifact, both of which took a fix to become true and both
of which `make wasm-check` asserts by instantiating the modules and running a
parse, a diff and a cursor drain inside them:

**It exports exactly the public ABI, and imports nothing.** 47 functions plus its
memory. The export surface is the set of declarations carrying `IBHA_CSVD_API` in
`include/ibha_csvdiff.h` and nothing else, so it cannot drift from the header;
`scripts-and-commands/mark_public_api.py` checks that every declaration carries
it. Zero imports means the host supplies no runtime: notably the engine computes
its own 64x64 to 128 multiply on this target rather than importing `__multi3`
from compiler-rt, which a `-nostdlib` module has no source of, and which would
have been a second implementation of the arithmetic under every digest.

**The engine owns exactly the pages it grew itself.** Not everything above
`__heap_base`, which is the obvious rule and is wrong as soon as there is a host:
a JavaScript binding has to put the CSV bytes somewhere, it does that with
`memory.grow`, and an engine that assumes it owns everything above `__heap_base`
hands those addresses out again. Host and engine allocations can never overlap
under the rule as it stands, because `memory.grow` gives each caller a disjoint
range whoever calls it.

| | scalar | SIMD |
|---|---|---|
| module size | 120 KB | 138 KB |
| exports | 48 | 48 |
| imports | 0 | 0 |

The SIMD module is `-msimd128` over the same sources; the hand written SIMD
parser of spec 3.3 is Phase 7, and the scalar build is retained permanently as
its differential oracle.

**The two builds and the native build produce byte identical output**, which is
what spec 3.2 asks for and what `node scripts-and-commands/check_determinism.mjs`
checks: 160 comparisons over eight fixture pairs and ten emitter configurations,
including an 86 MB JSONL report. Instantiating a module proves it runs; only
comparing the bytes proves a browser preview and a server side batch report of the
same pair are the same report.

`make wasm` also builds `build/abi_offsets.wasm` from `tools/abi_offsets.c`, which
exports the wasm32 size, alignment and field offsets of every public struct. The
bindings read `ibha_csvd_table` and `ibha_csvd_row` straight out of linear memory,
because an accessor call per cell would cost more than the diff, so they need those
offsets and a hand written table of them is a silent correctness hazard: a field
that moves is not a compile error, it is a binding that reads `n_columns` out of
the middle of a pointer. `scripts-and-commands/gen_abi.mjs` turns that module into
the JS binding's generated `abi.ts`, and `--check` is in the gate.

Three functions exist for hosts rather than for C callers, and each closes a hole
that made part of the ABI unreachable from wasm:

- `ibha_csvd_buffer_sink_bind` fills an `ibha_csvd_sink` with the buffer sink's
  write function. A function pointer on this target is an index into the module's
  indirect call table, the table is not exported, and JavaScript cannot portably
  put a host function into one, so without this the entire emitter layer was
  unreachable: every symbol it needs was exported and the one value that ties them
  together could not be constructed.
- `ibha_csvd_diff_table` and `ibha_csvd_diff_schema` return the tables the diff is
  actually comparing, which under the column policy of spec 6.6 are the projected
  ones rather than the parsed ones. The emitters have always used these
  internally; a consumer decoding cells out of the parsed table reads the wrong
  column, and only when the caller allowed an added or removed column, which makes
  it a bug that passes every test written against the defaults.

## Architecture

The columnar index is the core, not the parser. The parser is one front end; a row
feed fed from a JDBC `ResultSet` is another. The diff engine only ever sees the
index, which is why a relational source never has to be serialized to CSV text.

```
       +-- CSV bytes ---> RFC 4180 parser --+
       |                                    |
inputs +-- ResultSet / CLOB --> row feed ---+--> columnar index --+
       |                                    |                     |
       +-- pre-built digest ----------------+                     v
                                                      diff engine --> cursor --> emitters
```

Rules the code holds to:

- **No global mutable state.** Every entry point takes an `ibha_csvd_ctx *`, so
  concurrent diffs on different threads never share allocator state. Parallelism
  across file pairs is the caller's job.
- **Arena allocation only.** Nothing is freed individually; a context releases
  everything at once. There is no lock to contend and no fragmentation to manage.
- **One error per context, first one wins.** A later symptom never overwrites the
  original cause.
- **Streaming is the only ingest path.** One shot buffers are an adapter over the
  pull callback, not the other way round.
- **No string materialization.** The index stores offsets into the original bytes.
  Values become strings only at a rendering or export boundary.

## Layout

```
include/ibha_csvdiff.h   public ABI, the only installed header
src/arena.c              chunked bump allocator and growable byte buffer
src/ctx.c                context lifecycle, error discipline, freestanding formatter
src/reader.c             pull reader and push sink adapters: buffer, POSIX fd
src/ingest.c             Phase 0 ingest and limit enforcement
src/parse.c              the resumable RFC 4180 state machine and the index
src/schema.c             the four header row model, target header auto-detection
src/hash.c               XXH3 subset, logical field comparator, hashing, unescape
src/normalize.c          declared types and the comparators of spec 5.3
src/match.c              bucket map, keyed matching, duplicate keys, move detection
src/diff.c               report ordering and the pull cursor
src/validate.c           the validation findings of spec 13.5
src/segment.c            the intra cell diff of spec 7
src/emit.c               the jsonl, csv, html and summary emitters
src/sys_libc.c           system memory layer, native
src/sys_wasm.c           system memory layer, freestanding wasm32
cli/main.c               ibha-csvdiff, diff(1) exit codes
tests/                   unit, golden and property suites, four fuzz targets
bench/                   parser throughput
fixtures/                generator for golden and benchmark CSV files
```

## The parser, in one paragraph

A table driven state machine over six states, indexed by a 256 entry byte class
table built from the dialect. It carries its state and one field offset across
calls, so a chunk boundary may fall anywhere: inside a multi byte UTF-8 sequence,
inside a quoted field, between the two halves of a `""` escape pair, between the
CR and the LF of a CRLF, or inside the BOM. Bytes accumulate contiguously and the
index stores offsets into them, which is what makes that cheap: by the time a
split field ends, both halves are adjacent. Comparison operates on logical
values, so `abc`, `"abc"` and `"ab""c"` versus `ab"c` all compare and hash
correctly and requoting a file is never a difference.

## The engine, in one paragraph

Header row 3 selects a comparator per column, so `1.50` equals `1.5` and `007`
equals `7`, and **the row digests are folded from the same normalized bytes the
comparators walk**. That is the one coupling the whole engine rests on: the
unchanged-row fast path decides a matched row is unchanged from its digest alone
and never looks at a cell, which is only sound because the digest means exactly
what the comparator means. Matching is a bucket map over the key digest, with
every hash match verified against the key bytes, so a collision costs a
comparison rather than producing a wrong pairing. Rows that moved are found by
the longest increasing subsequence, so dragging one row to the top of a 146,000
row file reports one moved row rather than 146,000. Output is a pull cursor:
nothing is accumulated, no report array is built, and which cells changed is
worked out one row at a time as the consumer asks.

## The output layer, in one paragraph

There is exactly one output primitive, the pull cursor, and the four emitters are
loops over it into a caller supplied sink, so the library never decides the
destination and never needs the diff in memory to reach one. The row shape they
write is a **versioned contract**: `schemaVersion` rides on every JSONL row, on
every CSV row and on the HTML container, because the emitter and the consumer are
separate components that have to agree. The HTML emitter's escaping is a security
requirement rather than a formatting one: cell content is untrusted and the output
is injected through `dangerouslySetInnerHTML`, so every value goes through one
escaper, class names come from a fixed compiled-in set, no caller data reaches an
attribute name or a URL, and ill formed UTF-8 is replaced rather than passed
through, because an overlong encoding of `<` is not the byte `0x3C` and would slip
past a byte oriented escaper untouched.

### The row contract

```json
{"schemaVersion":1,"kind":"modified","sourceRow":108,"targetRow":109,
 "moved":false,"moveDistance":0,
 "cells":[{"name":"account_id","target":"ACC-00000103"},
          {"name":"premium_amount","source":"9684.26","target":"9685.76","changed":true}],
 "findings":[{"column":4,"name":"premium_amount","kind":"precision","precision":12,"scale":2}]}
```

Row numbers are 1 based records counting header records, `kind` is one of
`unchanged`, `modified`, `added` and `deleted`, and the one rule worth stating
outright: **a matched row carries `source` on a cell exactly when that cell
differs in bytes from the target.** Its absence means the two sides are byte
identical, which is what keeps an unchanged 90,000 row report from being written
out twice over. `changed`, `suppressed`, `truncated` and `invalidUtf8` appear only
when true.

### The column policy

Spec 13.10 locks column order: the uploaded file must carry the source's columns
in the source's order. Whether a column *added* or *removed* is an error or a
finding is the caller's decision, per kind, at the call site:

```c
ibha_csvd_compare_opts cmp;
ibha_csvd_compare_opts_init(&cmp);
cmp.allow_added_columns = 1;   /* a salesman appending a column is a finding */
cmp.allow_removed_columns = 0; /* a column that has gone missing is still an error */
```

```sh
build/ibha-csvdiff diff a.csv b.csv --allow-added-columns
```

The two are separate because the two cases are not equally serious. An appended
column means the data you asked for is all still there; a missing one is data
loss, and tolerating it means quietly not comparing something.

When a difference is allowed, the diff compares the columns the two files have in
common, **in the source's order**, and reports the rest: `stats.columns_added`,
`stats.columns_removed`, and named `columnAdded` and `columnRemoved` entries in
the summary emitter. Report rows carry the compared columns and no others, so a
consumer never renders a column the two files do not share.

Three things the flags never relax, each with an assertion in
`tests/test_columns.c`:

- **Reordering stays a hard error.** Spec 13.10 has no flag for it and these do
  not add one. A column that moved comes out of the name matcher as a removal in
  one place and an addition in another, so it is told apart from a genuine
  addition explicitly rather than silently dropping out of the comparison.
- **A missing KEY column is always an error.** The key is what row matching is
  built on, so tolerating its absence would not give a lenient diff, it would give
  a meaningless one.
- **A file with no column name row cannot use the flags at all.** Without names
  there is nothing to tell an added column from a shifted one, and guessing would
  compare the wrong pairs of cells.

These live in `ibha_csvd_compare_opts` rather than with the parse options because
they change which cells the row digests are folded from. That makes them part of
`compare_id`, so a pair parsed under different policies is refused rather than
silently compared.

The implementation is a **projection rather than a column map threaded through
the engine**. Column *c* means the same thing on both sides in a dozen places,
several of which compare two rows of the same side, and a per side map would have
to reach all of them correctly. Instead each side gets a table holding exactly the
compared columns in the source's order, and everything downstream keeps working
unchanged because it is still true that column *c* means the same thing on both
sides. The cost is one rebuilt index per side, about 12 bytes per compared cell,
and the default policy never reaches that code.

### Validation findings

Spec 13.5's distinction is load bearing: **errors abort the diff, findings are
output**. A `REQUIRED` column with an empty cell, a value over its `VARCHAR(n)`, a
value that does not parse as its `DECIMAL(p,s)` or that exceeds its declared
precision are the point of running the comparison, so they flow through the cursor
as flags on the affected cell and are counted in the summary. They are measured
against the source file's schema, which spec 13.8 makes authoritative, and
"does not parse as a number" is decided by the same canonical form the comparators
of spec 5.3 use, so a cell can never be reported as unparseable in the findings
and compared as a number in the same report.

This is the one thing that makes the cursor read cells it would otherwise skip: a
digest settles whether a row changed and cannot settle whether it satisfies the
schema. A per column pre-filter keeps that from costing anything when there is
nothing to check, and `diff_opts.validate = 0` turns it off entirely.

## Measured throughput

Two machines, so read each column against itself rather than across.

| | M1 Pro, Phase 1 | x86-64, Phase 1 | x86-64, Phase 2 |
|---|---|---|---|
| parse, zero copy | 793 MB/s | 682 MB/s | 648 MB/s |
| parse and digest, zero copy | 454 MB/s | 615 MB/s | 484 MB/s |
| arena, zero copy | 1.36x | 1.36x | 1.42x |

15 MB CSV shaped buffer, 20 columns, in memory, `-O3`, best of 5. `make bench`.
Run to run variance is about 3%. The x86-64 machine is GCC 11 on Linux; the two
Phase 1 columns are the same code, which is what makes the third column a
like-for-like measurement of what Phase 2 cost.

**Normalization costs 21% of the digest path and 5% of the arena**, 615 to 484
MB/s (472 to 492 across runs) and 1.36x to 1.42x. It buys the correctness the engine is for: without it
the digests and the comparators disagree and the fast path reports formatting
changes as edits. The arena difference is the third digest array, 8 bytes per
row, which is what makes suppressed-by-normalization countable without walking
every unchanged row. The parse only row moved 682 to 648, which is the extra
digest array's allocation and reserve path, not the state machine.

At 484 MB/s the p90 15 MB file is indexed and digested in 31 ms and a 150 MB file
extrapolates to about 320 ms, both far inside the one second requirement, and the
scalar parser still clears spec 3.3's 300 MB/s gate by 60%. **SIMD stays in
Phase 7.**

End to end on the generated 15 MB pair, both sides, through the CLI:

```
parse 0.105 s, match 0.013 s, cursor 0.002 s, 250 MB/s end to end
```

Matching 146,580 rows against 146,580 rows takes **13 ms**, and draining the
whole report through the cursor takes **1.7 ms**. The diff is not the expensive
part of a diff; the parse is.

### What the output layer costs

`python3 ../scripts-and-commands/measure_emitters.py`, 29.9 MB of input across
both sides, best of 3, whole pipeline including both parses.

| emitter | seconds | output | MB/s of input |
|---|---|---|---|
| counts only, no emitter | 0.135 | 593 B | 221 |
| summary | 0.138 | 480 B | 217 |
| jsonl, changes only | 0.139 | 1.3 MB | 216 |
| html, changes only, cell diff | 0.139 | 1.5 MB | 215 |
| csv | 0.197 | 21.3 MB | 152 |
| html | 0.250 | 79.5 MB | 120 |
| jsonl | 0.268 | 86.2 MB | 112 |

Two things to read out of that table.

**The bounded outputs are free.** Summary and changes-only cost 3 to 4 ms over
the bare drain, so a pass/fail check on a 15 MB pair is as cheap as the diff
itself. Memory is flat in every row: the emitter's own state is one 8 KB buffer
and one report row.

**The unbounded ones are dominated by the writing, not the diffing.** A full JSONL
report of this pair is 86 MB from 30 MB of input, and 130 ms of the 268 is
producing those bytes. This is the concrete form of spec 13.3's rule that the HTML
emitter is for bounded output: a full HTML render of a 90,000 row diff is tens of
megabytes of DOM and will not scroll acceptably, which is what the virtualized
view of section 8 exists for.

**Validation costs 13 ms of the drain**, taking the cursor from 1.7 ms to 14.9 ms
on 1.76 million cells. That is the price of reading cells an unchanged row's
digest let the matcher skip, it is about 11% of the parse, and `--no-validate`
removes it.

Two things worth reading deliberately:

**Hashing still costs more than parsing.** Digests take the throughput from 648
to 484 MB/s. Spec 3.3 ranked the structural scan first for SIMD payoff and
hashing third; on this evidence the ranking is the other way round, and Phase 7
should re-measure before spending its effort on the scan.

**The normalizer is called across a translation unit boundary once per cell.**
The build has no LTO, so that call is not inlined, and a fair share of the 21%
is likely to be call overhead rather than work. Measuring that is a cheap
first move for Phase 7, ahead of any SIMD.
