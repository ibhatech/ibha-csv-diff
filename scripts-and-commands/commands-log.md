# Commands log

Chronological record of shell commands run in this repo by the agent, per project convention.

## 2026-08-01, session 1, solution proposal

```
ls -la && ls -la specs/ && cat AGENTS.md && cat .gitignore && go version; clang --version | head -2
python3 scripts-and-commands/check_dashes.py && wc -l specs/02-solution-proposal.md
```

Scripts created:
- `check_dashes.py`, scans `specs/` and `docs/` markdown for em dash and en dash characters.

## 2026-08-01, session 1, revisions 2 and 3 of the proposal

Revision 2 applied after p90 of 15 MB was confirmed. Revision 3 applied after the server side batch
profile was raised as a first class use case. Both edited `specs/02-solution-proposal.md` in place via
the edit tool, plus these verification and cross-reference-repair commands:

```
grep -n "^## \|^\*\*Phase" specs/02-solution-proposal.md
grep -n "phase [0-9]\|Phase [0-9]" specs/02-solution-proposal.md
python3 scripts-and-commands/check_dashes.py
```

Inline python heredocs were used to repair stale `Phase N` cross-references and to renumber
section 12, since those edits touched several non-adjacent spots with identical surrounding text.

## 2026-08-03, session 1, Phase 0 implementation

Created the three library subfolders `core/`, `js/`, `java/`. Verification commands:

```
clang --target=wasm32 -nostdlib -c t.c -o t.o      # confirmed: no WebAssembly backend
cd core && make test                                # 53 assertions
cd core && make asan                                # AddressSanitizer + UBSan
cd core && make bench                               # throughput baseline
cd core && make all && python3 fixtures/gen_fixtures.py
./build/ibha-csvdiff ingest fixtures/generated/p90_target.csv --repeat 3
./build/ibha-csvdiff ingest fixtures/generated/p90_target.csv --max-mb 5   # exit 2
cd js && pnpm install && pnpm -r build && pnpm -r typecheck && pnpm -r test
java -version; gradle --version                     # JDK 17 present, 21 required
```

Correction logged: a mistyped Write path created a sibling directory of the project
with a typo in its name, containing one stray file. Inspected with `find`, then
removed with a single `rm -rf` after the contents were confirmed. Lesson recorded:
never bundle a destructive command with its own verification in one approval step.

Inline python heredocs were used for multi-site source patches (threading the
`size_hint` parameter through all callers, fixing TypeScript strictness errors).

## 2026-08-06, session 1, Phase 1 implementation

The RFC 4180 parser, the columnar index, the header model and the hash.

```
xxhsum --version; printf 'abc' | xxhsum -H3        # reference hash available
python3 scripts-and-commands/gen_xxh3_vectors.py   # writes core/tests/xxh3_vectors.h
cd core && make                                    # warning free under the strict flag set
cd core && make test                               # 200 assertions
cd core && make asan                               # the same under ASan and UBSan
cd core && make bench                              # 814 MB/s parse, 454 MB/s with digests
cd core && make fuzz                               # fails: Apple clang has no libFuzzer runtime
cd core && make fuzz-native                        # 200k inputs per target, no toolchain needed
./build/fuzz_parse_native 4000000 987654321        # 4M inputs, no invariant violated
./build/ibha-csvdiff parse fixtures/generated/p90_source.csv --repeat 3
./build/ibha-csvdiff parse fixtures/generated/ragged.csv          # exit 2, names the row
./build/ibha-csvdiff parse fixtures/generated/names_only_header.csv --header 1
```

Scripts created:
- `gen_xxh3_vectors.py`, regenerates the golden XXH3 vectors from `xxhsum`. The
  engine's hash is verified bit for bit against the reference at 44 lengths,
  because a hash that is merely "a good hash" would silently produce wrong diffs
  once spec 2.5's digest protocol has a server computing them independently.

Two defects found and fixed while measuring rather than by inspection:

- The index arrays reserved 4.19 million cells for a 2.06 million cell file,
  because the extrapolated size was fed through the doubling growth path and
  rounded up to the next power of two. The arena reservation was 2.58x the input
  instead of 1.36x. Fixed by giving the reserve path an exact mode.
- `fixtures/gen_fixtures.py` wrote `xss.csv` with backslash escaped quotes, which
  is not valid RFC 4180, so the file failed to parse with `BAD_CONTENT`. The
  payload has to survive parsing to reach the HTML emitter that must neutralize
  it, so the row is now built through `csv_escape`.

Inline python heredocs were used for multi-site source patches (threading the
`exact` parameter through the reserve call sites, splitting the TAP counters
across translation units).

## 2026-08-12, session 1, Phase 2 implementation

The declared type comparators, the matcher, move detection and the pull cursor.

```
cd core && make                                   # warning free under the strict flag set
cd core && make test                              # 321 assertions, up from 200
cd core && make ubsan                             # trapping UBSan, needs no runtime library
cd core && make valgrind                          # memcheck, clean
sudo dnf install -y libasan libubsan               # run by Manas; needs root
cd core && make asan                              # ASan, LSan and UBSan, clean
cd core && make fuzz-native FUZZ_ITERS=3000000    # 3M inputs per target, clean
cd core && make fuzz-native-notrap FUZZ_ITERS=3000000   # the no-runtime fallback, clean
cd core && make bench                             # 648 MB/s parse, 484 MB/s with digests
./build/ibha-csvdiff diff fixtures/generated/p90_source.csv fixtures/generated/p90_target.csv
python3 scripts-and-commands/run_phase2_checks.py       # the whole gate in one command
git worktree add /tmp/ibha-phase1 HEAD             # Phase 1 baseline on the same machine
git worktree remove /tmp/ibha-phase1 --force
```

Scripts created:
- `run_phase2_checks.py`, the whole verification gate in one command. Sanitizer
  steps that the local toolchain cannot run report "skipped" with the install
  line rather than failing the gate, because `ubsan` and `valgrind` between them
  already cover undefined behaviour and memory safety with no extra package.

Three environment portability defects found and fixed, none of them design:

- `fixtures/gen_fixtures.py` used `Path.write_text(newline=...)`, which needs
  Python 3.10; this machine has 3.9. The fixtures carry deliberate CR, LF and
  CRLF endings, so `newline=""` is not optional and the call became an explicit
  `open`.
- `cli/main.c` and `bench/bench_parse.c` call `clock_gettime` under `-std=c11`,
  which hides POSIX behind glibc's feature test macros. Both now define
  `_POSIX_C_SOURCE` rather than the build loosening the standard for the library.
- `fixtures/gen_fixtures.py` recorded an edit script whose entries overlapped: a
  row could be modified and then deleted, and an added row could then be removed.
  No correct diff could reproduce that script, so the generator now applies
  disjoint edits and writes them as a sorted TSV the property test reads.

A Phase 1 worktree was built and benchmarked on this machine to get a
like-for-like measurement of what normalization cost: 615 to 484 MB/s on the
digest path and 1.36x to 1.42x on the arena. Comparing against the README's M1
figures would have measured the hardware, not the change.

Inline python heredocs were used for multi-site patches (adding the new sources
to three Makefile variable lists, adding the sanitizer fallback targets).

Installing the sanitizer runtime paid for itself immediately: LeakSanitizer
found that `test_index_memory` in `tests/test_property.c` never freed its fixture
buffer. A Phase 1 defect in the harness rather than the engine, and it survived
Phase 1's `make asan` because **LeakSanitizer is off by default on macOS and on
by default on Linux**. Worth remembering when reading a green ASan run on a Mac.

## 2026-08-12, session 2, Phase 3 implementation

```sh
cd core
make test               # 432 assertions, up from 321
make asan               # ASan, LSan and UBSan, now with detect_stack_use_after_return=1
make ubsan
make valgrind
make fuzz-native FUZZ_ITERS=1000000   # four targets: parse, diff, emit, ingest
make bench
build/ibha-csvdiff diff a.csv b.csv --format summary
build/ibha-csvdiff diff a.csv b.csv --format jsonl --changes-only
build/ibha-csvdiff diff a.csv b.csv --format html --changes-only --cell-diff both

cd ..
python3 scripts-and-commands/run_phase3_checks.py --fuzz-iters 500000
python3 scripts-and-commands/measure_emitters.py
python3 scripts-and-commands/check_dashes.py <every file touched>
```

New scripts: `run_phase3_checks.py` is the gate, which now also drives the CLI end
to end in each of the four formats, because an emitter that works in a unit test
and not through the public entry point is not working. `measure_emitters.py`
produces the cost table in `core/README.md`, so the table is re-run rather than
edited by hand.

Two bugs found by the tooling rather than by reading, both worth recording.

**A test helper handed `ibha_csvd_parse_borrow` a stack buffer.** The parser
borrows rather than copies, so the index pointed into a frame that died when the
helper returned. It read correctly at `-O3`, where the stack happened to still
hold the bytes, and wrongly under ASan, which is the worst way for that to
surface: four assertions failed only in the sanitizer build. Fixed by making the
buffers static, and `make asan` now sets `detect_stack_use_after_return=1` so the
next one is reported at the point of the read rather than inferred from a wrong
answer.

**The declared type row is a CSV row too.** A test header wrote
`VARCHAR(4),VARCHAR(5),DECIMAL(5,2),INTEGER`, and the comma inside `DECIMAL(5,2)`
made it a five field row against a four column file, so the fixture failed with
RAGGED_ROW rather than testing anything. The type cell has to be quoted like any
other value containing the delimiter.

Inline python heredocs were used again for multi-site patches: adding the three
new sources to the native and wasm source lists, the three new suites to the test
list, and the emitter fuzz target to three Makefile rules.

## 2026-08-12, session 2, the column policy

```sh
cd core
make test               # 463 assertions, up from 432
make asan && make ubsan && make valgrind
make fuzz-native FUZZ_ITERS=300000
build/ibha-csvdiff diff a.csv b.csv --allow-added-columns --format summary
```

Manas's answer to the flagged assumption: added and removed columns are a config
setting, per kind, at the call site. `compare.allow_added_columns` and
`compare.allow_removed_columns`, both defaulting to 0, which leaves spec 13.10
exactly as it was for anyone who does not ask.

The implementation choice worth recording. Column *c* means the same thing on
both sides in a dozen places in the engine, and several of them compare two rows
of the *same* side, so threading a per side column map through all of them would
have been correct only by review. Instead, when the two files do not carry the
same columns, each side gets a **projected table** holding exactly the compared
columns in the source's order, with its row digests refolded over them. Everything
downstream keeps working unchanged because it is still true that column *c* means
the same thing on both sides. The row digest fold moved out of `parse.c` into
`normalize.c` so the projection reuses it rather than reimplementing it.

Three assertions the tests added before the feature was called done, because each
is a way the flags could have quietly produced a wrong report rather than a failed
one: a reordered header stays an error even with both flags on, a missing KEY
column stays an error whatever the policy, and a file with no column name row
cannot use the flags at all. The first needed real work: the name matcher is an
in-order merge, so a column that moved comes out of it as a removal in one place
and an addition in another, and it has to be told apart from a genuine addition
explicitly.

## 2026-08-12, session 2, the wasm toolchain and the first working module

```sh
sudo dnf install -y clang lld    # Oracle Linux 9.8, clang 21.1.8, provides wasm-ld
cd core
make wasm                        # scalar and SIMD freestanding wasm32 modules
make wasm-check                  # the same, then run them under node
cd ..
python3 scripts-and-commands/mark_public_api.py --write
node scripts-and-commands/wasm_smoke.mjs core/build/ibha_csvdiff.wasm
python3 scripts-and-commands/run_phase3_checks.py
```

The wasm build had never once compiled, so everything below was found in the
first ten minutes of it being able to. Each is the kind of defect that only
exists on a target nobody has built for, and each was silent rather than loud.

**The module was 292 bytes and exported nothing.** It linked cleanly. The
Makefile has compiled with `-fvisibility=hidden` and linked with `--gc-sections`
since Phase 0, which is the right pair of flags for "export the public API only",
but the other half of that design, marking the public API, was never written, so
everything was hidden and then collected. Fixed with an `IBHA_CSVD_API` macro on
every declaration in the public header, applied and checked by
`mark_public_api.py`, which is now in the gate: on this target an unmarked public
function is not a link error, it is a function missing from the module.

**The module imported `env.__multi3`.** clang defines `__SIZEOF_INT128__` on
wasm32 and then lowers a 128 bit multiply to a compiler-rt call that a `-nostdlib`
build has no runtime for. `--allow-undefined` let it link, so the failure would
have been at instantiation in the browser. Worse than the failure: the fix a
binding author would reach for is to implement the multiply in JavaScript, which
puts a second implementation of the arithmetic under every digest in the engine
and invites the wasm and native builds to disagree about a hash. Phase 1 had
already written a portable 32 bit halves version behind an `#else` that no build
had ever selected; it is now selected on wasm, and `test_hash.c` asserts the two
agree over 200,000 random pairs plus the edge cases.

**The allocator would have corrupted any host that used the memory.** It took
`__heap_base` to the current memory size as its own, which is fine for a module
nobody talks to and wrong the moment a JavaScript binding stages CSV bytes with
`memory.grow`: the engine hands the same addresses out again. Now it owns exactly
the pages it grew itself, which cannot overlap with the host's by construction,
because `memory.grow` gives each caller a disjoint range.

`wasm_smoke.mjs` drives a parse, a diff and a cursor drain inside the module and
is deliberately free of struct layouts, so nothing in it has to be revised when
Phase 4 works out the wasm32 field offsets. It reports the same four report rows
the native build reports for the same inputs, which is a down payment on the
spec 3.2 determinism check rather than the check itself: that one has to compare
emitter output byte for byte, and it is Phase 4's.

## 2026-08-12, session 2, libFuzzer

```sh
make fuzz                                  # FUZZ_CC defaults to clang now
python3 scripts-and-commands/run_libfuzzer.py --seconds 90
```

The clang installed for the wasm build brought its compiler-rt with it, so the
second standing blocker is gone too. The Makefile gained `FUZZ_CC`, defaulting to
clang, rather than moving `CC` to clang for the whole build: libFuzzer is a clang
runtime with no GCC equivalent, and the warning free claim is measured against
GCC, so only the fuzz targets change compiler.

First coverage guided run, 90 seconds per target, no crash, leak, timeout or oom
artifact on any of the four. The corpus grew to about 2,100 inputs and is kept in
`core/corpus` between runs, because coverage feedback is what produced it.

Worth recording for anyone deciding whether `make fuzz-native` is still needed:
it is, and it is not equivalent. With feedback the emitter target runs about
4,500 executions a second and added 3,269 new corpus entries in 90 seconds; the
native driver's blind generator has no way to build on what it found. The native
one exists so the untrusted input surface is exercised on a machine with no
clang, which is the case the fallback was written for and still covers.

`run_libfuzzer.py` checks for artifact files rather than the exit status. A
libFuzzer target that finds a crash writes `crash-<hash>` and exits non zero, but
a run that is merely killed by a timeout also exits non zero, and the two mean
opposite things.

## 2026-08-13, Phase 4, the JS binding

```sh
make -C core wasm                                        # now also builds abi_offsets.wasm
node scripts-and-commands/gen_abi.mjs                    # writes js/packages/core/src/abi.ts
node scripts-and-commands/check_determinism.mjs          # spec 3.2, native vs both wasm builds
node scripts-and-commands/measure_binding.mjs            # the table in the JS package README
cd js/packages/core && node --test "src/*.test.ts"       # 62 assertions, nothing installed
python3 scripts-and-commands/run_phase4_checks.py        # the whole gate, C and JS
```

Toolchain, on Manas's approval: `corepack enable pnpm` then `pnpm install` in
`js/`. That is what `pnpm -r typecheck` and `pnpm -r build` need and nothing else
does; the rest of the JS gate runs on a checkout with no `node_modules` at all.

Three decisions worth recording, because each was a fork in the road.

**The binding's tests run under `node --test` rather than vitest, and every
relative import names the `.ts` file it resolves to.** Node 22 and later execute
TypeScript by stripping types, but resolve the specifier exactly as written, so
`./source.js` fails where `./source.ts` runs. Turning on
`rewriteRelativeImportExtensions` (TypeScript 5.7) makes `tsc` rewrite those to
`.js` on emit, so the published package is unchanged. What it buys is that the
determinism check and the whole binding suite run with nothing installed, which
is the property the C gate already had and the one I did not want to give up. It
cost one real change: parameter properties are the single piece of TypeScript
that stripping cannot erase, and `source.ts` had two.

**Three functions were added to the C ABI.** `ibha_csvd_buffer_sink_bind` exists
because a wasm host cannot form a C function pointer: the indirect call table is
not exported and JavaScript cannot portably add to it, so the whole emitter layer
was unreachable even though every symbol it needs was exported.
`ibha_csvd_diff_table` and `ibha_csvd_diff_schema` exist because under the column
policy of spec 6.6 the diff compares projected tables, and a consumer decoding
cells out of the parsed ones reads the wrong column, silently and only when the
caller allowed a column difference.

**The struct offsets are generated by the compiler that builds the module.**
`core/tools/abi_offsets.c` compiles to its own wasm32 module exporting a name blob
and a parallel value array; `gen_abi.mjs` instantiates it and writes `abi.ts`; the
gate regenerates and diffs. A hand written offset table is not a compile error
when a field moves, it is a binding that reads `n_columns` out of the middle of a
pointer, and that is exactly the class of failure the wasm target has already
produced twice in this project.

## 2026-08-14, session 8, Phase 5: the view and React packages

One install, with approval, because the React package could not otherwise
typecheck against React's own types and two of its tests need a DOM:

```
pnpm add -w -D @types/react @types/react-dom jsdom
```

Verification, run repeatedly through the phase and green at the end:

```
node scripts-and-commands/gen_stylesheet.mjs
node scripts-and-commands/gen_stylesheet.mjs --check
cd js/packages/view && npx vitest run
cd js/packages/react && npx vitest run
cd js && pnpm -r build && pnpm -r typecheck
python3 scripts-and-commands/run_phase5_checks.py --view-only
python3 scripts-and-commands/run_phase5_checks.py --quick
```

Scripts created:
- `gen_stylesheet.mjs`, generates `js/packages/view/styles/*.css` from
  `src/stylesheet.ts`; `--check` fails when they are stale.
- `run_phase5_checks.py`, the whole gate. `--view-only` runs Phase 5 alone.

Four decisions worth recording.

**The class set is the emitter's, not spec 8.3's.** Section 8.3 sketches a BEM-ish
list, `.csvd-row--modified` and so on, written before the emitter existed. The
emitter writes eighteen flat suffixes behind a validated prefix, and spec 13
overrides sections 1 to 12 where they conflict. One stylesheet has to work on a
saved report and on the live view untouched, so the flat set wins and everything
the view needs to express beyond it is a `data-` attribute rather than a new
class. `classes.test.ts` reads `core/src/emit.c` and compares the two sets, which
is the only way a contract shared by a C file and a TypeScript file that cannot
see each other stays true.

**The stylesheet is generated, and both forms ship.** It is a function of the
prefix because the prefix is configurable per component, but a bundler imports a
file, so the default prefix's output is checked in and regenerated in the gate.
Same argument as `gen_abi.mjs` makes about the struct offsets.

**The view binds to both `getRowsCompact` and `getRows`, and keeps sixteen times
as many of the former.** Sixty four structure pages at one byte per cell is 38 KB;
four value pages is two hundred rows of objects. A page whose values were evicted
still repaints in the right colours with the right row numbers the moment it
scrolls back, and only its text is re-fetched. That is the concrete reason to bind
to the compact form rather than a preference about typed arrays.

**The safety checker gained an attribute name rule the engine's does not have.**
The C emitter never puts file data in an attribute, so its checker can reject a
raw `&` there outright. The view puts a column name in `data-column` and cell
values in `aria-label`, so the rule had to become the correct HTML one, and that
alone would have accepted `" onload="x`, which produces well formed markup. The
fix is to parse the open tag as name and value pairs and require every name to be
structural or `data-` or `aria-`. It is asserted in both directions: every corpus
value is rejected when inserted raw, because a checker nothing can fail is
decoration.

## 2026-08-14, session 9, Phase 6: the Java binding

```sh
python3 scripts-and-commands/build_jni.py                # javac -h, then engine + glue -> one .so
cd java && mvn test                                      # 61 assertions, builds the .so first
cd java && mvn -B test -DskipNative=true                 # the Java half alone
node scripts-and-commands/check_determinism.mjs --require-java   # 240 comparisons, four builds
python3 scripts-and-commands/measure_java_binding.py     # the table in java/README.md
python3 scripts-and-commands/run_phase6_checks.py        # the whole gate, C, JS and Java
python3 scripts-and-commands/run_phase6_checks.py --java-only --quick
git rm --cached java/build.gradle.kts java/settings.gradle.kts   # replaced by pom.xml
```

Nothing was installed. The Phase 5 handoff recorded JDK 21 as a blocker; the
machine actually has JDK 25 and Maven 3.9.12, so `--release 21` gives the target
spec 13.6 asks for with nothing to add.

Four decisions worth recording.

**Maven replaced the Gradle scaffolding**, on Manas's choice, because Gradle is not
installed and Maven is. Java 21 now comes from `<release>21</release>` rather than a
toolchain pin, which is the stronger guarantee: it compiles against the JDK 21 API
signatures so a class file cannot reference something newer, and it does not require
JDK 21 to be present.

**The JDBC header is supplied by the caller and derived from nothing**, settled by
Manas. A result set is data rows only, and nothing in it says which columns are
keys. `Header` is a separate argument to `DiffSource.ofResultSet`, its rows are
written into the byte stream ahead of the first data row, and the result set and a
file therefore reach the same parser. The only thing read from the driver is the
column count, and only to refuse a header of the wrong width before anything is
parsed.

**The row feed is CSV encoding into a direct buffer rather than a new C ABI.**
Spec 13.6 refers to a row feed ABI that was never built. Adding one means a second
front end into the columnar index, with its own fuzzing and determinism coverage,
and reopens a core unchanged since Phase 4. Encoding rows as RFC 4180 into the
staging buffer gets the JDBC path at the cost of one escape pass and keeps exactly
one implementation of diff semantics. It sits behind `DiffSource`, so a native feed
can replace it invisibly.

**The batch size claim was measured and it was smaller than advertised.** One JNI
crossing per row instead of one per thousand is inside the noise on a 147,000 row
report, 0.020 seconds against 0.023. The comments claiming otherwise were corrected
and the measurement kept in the README, because the decision that actually carries
the throughput is a different one: cell values never cross the boundary at all,
and there are 1.76 million cells to 147 thousand rows.

## 2026-08-15, session 11, the ingestion API: character sources and `withHeader`

Items 2 and 3 of the track opened by `specs/input-options.md`. No new script was
needed: the work is Java and the existing gate runs it.

```sh
python3 scripts-and-commands/run_phase6_checks.py --java-only     # 80 tests before, 110 after
python3 scripts-and-commands/run_phase6_checks.py --require-java  # the whole gate, C, JS and Java
cd java && mvn -o -q test-compile -DskipNative=true               # the compile errors, without the .so build
cd java && mvn -o -q javadoc:javadoc -DskipNative=true            # the new doc comments build clean
```

Three decisions worth recording.

**The character sources encode in chunks and materialize nothing.** `ofString`
wraps the sequence in a `CharBuffer` view rather than copying it, `ofReader` and
`ofClob` read 8,192 chars at a time, and every one of them encodes straight into the
direct staging buffer. The alternative, reading a `String` and calling
`getBytes(UTF_8)`, is what a caller already has and is precisely the three copies at
peak these exist to remove.

**An unpaired surrogate is replaced rather than refused**, because
`String.getBytes(UTF_8)` replaces it and so does `CsvWriter` on a row value. The same
text has to become the same bytes through every factory, or a comparison depends on
which one the caller reached for. The pair that straddles a chunk boundary is the
real hazard and it is held back with `CharBuffer.compact`; the test sweeps it across
the boundary and also feeds it through a reader that returns one char per call.

**`withHeader` inherits the header row count contract rather than reopening it.** It
returns its `Header` from `header()`, so the rows written are the rows parsed, and a
conflicting `headerRows` is rejected by the rule built for the defect in the previous
commit. It also refuses to wrap a source that already declares a header, since that
would write two and parse one.

## 2026-08-15, session 11, the ingestion API: the JSON row source

Item 4, the last of the track. All five use cases in `specs/input-options.md` are now
delivered.

```sh
python3 scripts-and-commands/measure_json_rows.py       # new: the heap each mode needs
python3 scripts-and-commands/run_phase6_checks.py --java-only     # 132 tests
python3 scripts-and-commands/run_phase6_checks.py --require-java  # 26 checks, all ok
```

Three answers from Manas, and one decision taken by symmetry.

**Array of objects only.** The array of arrays shape was not built: the deployment
does not hold it, and a shape nobody has is a parser nobody tests. An array of arrays
fails with a message naming the shape this reads rather than a parse error about a
bracket.

**A missing key is an error, not an empty cell.** An empty cell would report as a
changed value against a file that carries the real one. **A key the header does not
declare is also an error**, which was not asked: it is the same disagreement in the
other direction and is the JSON form of an added column, which the CSV path refuses
by default. One line to relax if a real export carries extra fields.

**One shared renderer with `SqlValues`, literally.** `JsonRows` produces the types
`SqlValues` already renders, a `String`, a `Boolean` or null, and calls it. So JSON
null is an empty field and true is TRUE, and the test compares a JSON side against a
JDBC side of the same rows so that the day they disagree a test fails. A number keeps
its literal text because it is never parsed, and that is tested against `VARCHAR` and
not `DECIMAL`: under `DECIMAL` the engine compares by value, 1.50 equals 1.5, and the
test would pass whether or not the literal survived.

**The streaming claim was measured.** 150,000 rows, 16.0 MB of JSON: the row source
diffs both sides inside a **16 MB** heap in 379 ms; the same array as a `Map`/`List`
tree needs **256 MB** for one side with no diff performed, and fails outright at 128.
That is the behaviour `Json.java` was ruled out for, and it is now a number rather
than an assertion.

## 2026-09-04, session 12, closing out group A

The task was to read the two handoffs and figure out what came next. What came
next was not what either of them said.

**Both handoffs end with "Phase 7, the SIMD parser, is next" and both are stale on
that point.** `specs/03-remaining-tasks.md`, written 2026-09-03, ranks Phase 7
last of eight in its section I, and `docs/PUBLISHING-PLAN.md` section 9 records
that Phase 7 is on hold behind publishing, per Manas. The working list is the
newer document and it wins.

### The environment, probed rather than recalled

```bash
for c in cc gcc clang wasm-ld node pnpm mvn java python3 make; do ... done
```

gcc 11.5.0, clang 21.1.8, wasm-ld LLD 21.1.8, Node v24.13.0, pnpm via corepack,
Maven 3.9.12, java 25.0.1, Python 3.9.25, GNU Make 4.3. `JAVA_HOME` unset, which
is fine. No virtualenv is present, and none is needed.

**The finding that collapsed most of group A: this is WSL2 Linux**
(`6.6.87.2-microsoft-standard-WSL2`, Oracle Linux 9), not native Windows. A3, A4
and A5 were all written about MSVC, the LLVM for Windows distribution and
`/dev/null` not existing, and none of them apply. The August build tree is intact
on the same filesystem: `core/build`, `js/node_modules`, both `.wasm` modules and
`core/fixtures/generated` are all present and dated 13 to 15 August.

### The gate, and the baseline

```bash
python3 scripts-and-commands/run_phase6_checks.py --quick
```

`gate passed`, exit 0, in all three sections. 465 C assertions under four
configurations, seven fuzz targets at 200,000 inputs, 62 binding, 83 view, 28
react and 132 Java tests, and 210 byte identical comparisons across native, wasm,
wasm+simd and Java. 210 rather than 240 because `--quick` skips the 15 MB pair.

Parse throughput, which is the A8 deliverable because Phase 7's D6 is measured
against it: 717 MB/s zero copy, 576 streamed, 502 with digests zero copy, 405 with
digests streamed. **A8 expected these not to match the handoff's numbers, on the
grounds of different hardware. They match to within a few percent**, so it is the
same hardware and the August figures carry over. It also confirms why group D is
ordered last: 717 against a spec floor of 300.

### A1, answered by reading rather than by installing

```bash
grep -hE '^\s*(import|from) ' scripts-and-commands/*.py core/fixtures/*.py | sort -u
```

Standard library only, every one of them. There is nothing to install, so there is
no venv to create and nothing to activate. The item reopens if a script ever grows
a third party import.

### A9, and the one thing it did not anticipate

```bash
git ls-files | grep -iE 'fixture|golden|expected'   # -> two .py/.java sources, no data
python3 scripts-and-commands/check_line_endings.py  # -> 202 files, no CRLF
```

No fixture or golden data file is tracked at all: every one is generated into a
gitignored directory. So the corruption A9 feared had no tracked file to happen
to, and all 202 tracked files are already LF.

`.gitattributes` now pins that rather than leaving it to `core.autocrlf` on the
next machine. **It carries one rule A9 did not anticipate:** `*.csv` and
`core/fixtures/generated/**` are `-text !eol`, taking no conversion at all,
because `core/fixtures/generated/crlf.csv` carries CRLF as the data under test and
a blanket `text=auto` would normalize away the thing that fixture exists to prove.
Verified with `git check-attr text eol`, which reports `text: unset, eol:
unspecified` for the CSV and `text: auto, eol: lf` for the sources, and with
`git add --renormalize . --dry-run`, which produces no content change.

### New scripts

- `scripts-and-commands/check_line_endings.py`, the A9 check. Exits 1 and names
  the offending files if a checkout ever introduces CRLF.

### What is now blocking

Not a task in `specs/03-remaining-tasks.md`. The five decisions in
`docs/PUBLISHING-PLAN.md` section 8 gate every task in groups F and G, and only
Manas can answer them.

## 2026-09-04, session 12 continued, the decisions and group B

### Decisions recorded

Manas answered three of the five publishing decisions and the repository layout
question. **Public distribution, Apache-2.0, the repository goes public**, and
**one repository, no submodule split**. Recorded in `docs/PUBLISHING-PLAN.md`
section 0 as a table at the head of the decisions they answer, in section 8, in
`specs/03-remaining-tasks.md` H2, and on F1.1, F1.2, G1.1 and G1.3.

Still open, and both noted as such: **0.3**, one platform or five, which is now the
only thing gating J3; and the `developers` block for the pom, which blocks nothing
until J1 is ready to commit one.

The submodule variant proposed was better than the one H3 was written against: a
private umbrella with public submodules keeps the determinism gate runnable,
because the umbrella has all three checked out at pinned SHAs, and the relative
paths survive because submodules land where the directories already are. **What
sank it is that `java` is not independently buildable**: `build_jni.py` compiles
core's C sources and asserts its list against `core/Makefile`, so a standalone
clone of `java` cannot build, which is the entire point of splitting. The recorded
reasoning is in H2.

### Group B: the five assumptions, made answerable

```bash
python3 scripts-and-commands/confirm_assumptions.py          # all five
python3 scripts-and-commands/confirm_assumptions.py b1 b2    # only some
```

New `java/src/test/java/com/ibhatech/csvdiff/AssumptionsMain.java`, in the main test
package rather than in `tools` because B1 and B2 need `FakeResultSet` and
`SqlValues`, both package private. It drives the real engine through the real
binding on the smallest input that makes each rule observable, and prints what
happened beside what the alternative would have produced. `docs/ASSUMPTIONS-B.md`
is the write up.

**It decides nothing on purpose.** Each of the five ends in a question whose answer
is a fact about the data a deployment holds, and that fact is not in this
repository. What was in the repository, the behaviour, is now visible rather than
described.

Two things the run turned up that were not in the written statements of the five:

**B2 is worse than "a different format breaks it".** A CSV carrying
`2026-01-31T14:22:05.000`, the same instant in the same ISO layout, still reports
as changed, because `SqlValues` drops a zero fraction while a fixed precision
export writes three digits. That is the likeliest way to hit B2 while everything
looks correct to the eye.

**B4 has a concrete failure rather than a theoretical one.** In a file whose first
data record spans two physical lines, the duplicate on physical line 8 is reported
as `duplicate key (1) in the source file at rows 5 and 7`, and line 7 holds a
different row with a different key. The record count also includes the header rows,
which is why a first data row reports as row 5, and it is why B3's ragged messages
say "row 5" for a first data row too.

The other three behaved exactly as documented: NULL agrees only with an empty
field and disagrees with `\N`, `NULL` and `(null)`; excess empty fields are
normalized while a non-empty excess or a short row is refused; and `ééééé`, five
characters in ten bytes, is accepted into a `VARCHAR(5)`.

## 2026-09-04, session 12 continued, all five answered and B2 fixed

Manas answered the remaining questions. **Four of the five were right as built.
B2 was a defect and is fixed.**

- **B1 confirmed.** Empty field, and the trailing delimiter detail verified against
  the CLI: `1,ann,` under LF, under CRLF, and as an explicit `""` all give three
  columns and one unchanged row.
- **B2 rejected, general rule confirmed.** Trailing zeros in fractional seconds are
  not significant at any width.
- **B3 confirmed, both halves.** Forgive extra trailing empties, refuse short rows.
- **B4 confirmed.** A record spanning two lines is one row. The optional extra,
  also reporting the physical line, is scoped as B4a and not built.
- **B5 confirmed.** Characters, and the answer to "does this matter to the C
  engine" is that it **is** the C engine.

### The B2 fix

`ibha_canonical_timestamp` in `core/src/normalize.c`, beside
`ibha_canonical_decimal`, plus a `TIMESTAMP` branch in `ibha_normalize` next to the
decimal and boolean ones, plus 13 assertions in `core/tests/test_normalize.c`.

**No ABI change, which was the thing worth getting right.** The obvious
implementation, a new flag in `ibha_csvd_compare_opts`, would have added a field to
a public struct and become the first ABI change since Phase 3, dragging in
regenerated wasm offsets and a JNI accessor. It is not needed: `date_compare` was
already a field in that struct and already folded into `ibha_compare_id`, and the
behaviour is not a preference anyway. Two timestamps differing only in trailing
fractional zeros are the same instant, which is what the type means, so it rides on
`IBHA_CSVD_DATE_EXACT` whose documented meaning is now byte equality on the
canonical form.

The function declines more than it accepts, on purpose. It requires a terminal run
of digits after a `.`, so `14:22:05.000+05:30` is left exactly as it arrived rather
than half understood, and it returns 0 when the value is already canonical so the
caller compares the original bytes with nothing copied. It does not parse the date:
`31/01/2026` still differs from `2026-01-31`, which stays `DATE_VALUE` and stays
unimplemented.

Known gap, recorded rather than fixed: a `TIME` column gets none of this, because
`TIME` is not one of the engine's declared types and resolves to `UNKNOWN`.
`SqlValues` never renders a fraction for a `java.sql.Time`, so the JDBC side cannot
produce the mismatch, but a CSV carrying `14:22:05.000` in a `TIME` column would.
Adding a `TIME` type would change the type enum and therefore `compare_id`.

### The gate, with the engine change in

```bash
cd core && make test                                   # 478 assertions, was 465
python3 scripts-and-commands/run_phase6_checks.py      # full, not --quick
```

`gate passed` in all three sections, exit 0. **240 byte identical comparisons
across native, wasm, wasm+simd and Java**, and `abi.ts` reports 185 entries up to
date, which is the check that would have caught an accidental ABI change. Parse
throughput 712 MB/s zero copy, within noise of the 717 recorded in A8 before the
change.

### B6 done

`specs/02-solution-proposal.md` gains **13.13**, the five value semantics as locked
decisions rather than flagged assumptions, so phase 8's handoff does not inherit
the list a seventh time. `docs/ASSUMPTIONS-B.md` keeps the evidence and the
reasoning; the spec keeps the rule.

## 2026-09-04, session 12 continued, B4a rejected on a better argument

Manas closed B4a: not needed, because people view and edit these CSVs in Excel, and
in Excel line numbers and record numbers coincide.

**That is a stronger argument than the one the spec had**, and it was worth
rewriting 13.13.4 for. The original justification was semantic, that a record is
the unit a person means by "row". The real one is checkable: Excel parses the CSV
properly, so a quoted field containing a newline is one grid row with a line break
inside the cell, and a four row header occupies Excel rows 1 to 4 with the first
data record at Excel row 5, which is what the engine already calls record 5. The
engine reports the number the reader is looking at, and adding a physical line
number would have added the number that does *not* match. So B4a is rejected rather
than deferred, and the day of ABI work it would have cost is avoided.

**One exception found by checking rather than assuming.** The original statement of
B4 named blank lines alongside multiline fields, and only the multiline half is
covered by the Excel argument. Verified on a file with four header rows, a data
row, a blank line, then two more data rows, the duplicate sitting on physical
line 8:

```
engine  ->  duplicate key (1) in the source file at rows 5 and 7
Excel   ->  shows that row at row 8, its row 6 being the blank one
```

The engine skips a wholly empty line deliberately, `core/src/parse.c` line 517, and
counts it in `stats.blank_lines`; a genuinely empty single column row can still be
written as `""`. Excel shows the blank line as an empty grid row, so the two drift
by one per blank line.

**Not fixable by counting blank lines as records:** a blank line is one empty field,
which in a file of three or more columns is a short row, and B3 refuses short rows.
So the options are skipping them, which is what happens, or refusing any file
containing one. Documented in 13.13.4 and in `docs/ASSUMPTIONS-B.md` instead.
`blank_lines` is already public in the C ABI and already in `abi.ts`, so a caller
that ever meets this can explain the drift with no engine change.

A trailing blank line at end of file, which is what a trailing newline produces,
causes no drift at all, because no record follows it.

## 2026-09-04, session 12 continued, J1 and N1

Manas answered the last two open decisions: developer name "Manas Marthi", email
manas@ibhatech.com, and created the `ibhatech` npm organization. That unblocked
both local publishing phases.

### J1, making the Java build releasable

**The javadoc jar did not build, and the number of warnings was hidden.**
`mvn package` failed on one hard error, `DiffSource.java:320: unexpected heading
used: <H2>, compared to implicit preceding heading: <H3>`. A member's javadoc sits
at an implicit `<h3>`, so a heading inside it starts at `<h4>`; `<h3>` is still a
level skip, which the first attempt at the fix discovered.

**javadoc caps reported warnings at 100 by default.** So "100 warnings" was a
ceiling, not a count, and the hard error was being pushed off the end of the
output. Passing `-Xmaxwarns 10000` showed the real number: **218**. That is now in
the pom, along with `doclint=all` and `failOnWarnings=true`, so the javadoc is
gated rather than advisory. It had already regressed once, silently, because a
stale jar sat in `target/` and looked like evidence it still built.

`scripts-and-commands/javadoc_warnings.py` runs the build and groups the warnings
by file and by kind, which turns them into a work list.

**A codemod was tried, damaged prose, and was withdrawn.** A general tag appender
handled the repetitive cases well but mishandled a javadoc block written as
`/** text\n *  more text */`, eating the final continuation line. It also ran over
package private classes javadoc never documents, inserting empty tags into
`SqlValues`, `NativeEngine`, `Json` and others.

Recovering from that is worth recording. The five non public files were reverted
with `git checkout`. Fifteen lost prose lines were restored by diffing each file
against `HEAD`, finding lines present there and absent now, and re-inserting each
after the predecessor it still had. Then an audit re-ran the same comparison and
came back with only three differences, all of them intentional rewrites. The
script was deleted rather than fixed: the remaining 80 warnings were written by
hand, which is what the published reference documentation deserved anyway.

**218 to 0.** `javadoc builds clean`.

The rest of J1: the Central required `url`, `licenses`, `developers`, `scm`,
plus `organization`, `inceptionYear` and `issueManagement`; `LICENSE` and `NOTICE`
at the repository root, carried into the jar under `META-INF/`; and a `release`
profile with `maven-gpg-plugin` and `central-publishing-maven-plugin` at
**`autoPublish=false`**, so a first deployment lands as a validated but unpublished
bundle that can be inspected and dropped. Central releases are permanent.

`mvn -DskipTests package` now produces all three jars, and the jar carries
`META-INF/LICENSE` (11,358 bytes) and `META-INF/NOTICE`.

**The licence text was copied, not recited.** `/usr/share/licenses/cups-libs/LICENSE`
is the canonical Apache-2.0: 11,358 bytes, md5 `3b83ef96387f14655fc854ddc3c6bd57`,
which is the published file's own checksum. Reciting 11 KB of licence text from
memory would have been the wrong way to produce a legal document.

**The NOTICE needed a correction before it was true.** The first draft said "no
xxHash source is copied into this project". Checking `core/src/hash.c` showed that
the 192 byte XXH3 default secret constant *is* reproduced from the reference
implementation, deliberately, because bit exact agreement with stock xxHash is a
functional requirement of the digest protocol. The NOTICE now says so and credits
xxHash and its BSD-2-Clause licence. **This is worth a human's eye**, since it is
the only third party attribution question in the repository.

### N1, npm metadata and the guard

All three manifests: `license: Apache-2.0`, `author`, `homepage`, `bugs`,
`repository` with a `directory`, `keywords`, and
**`publishConfig: { access: public }`**, because a scoped package is restricted by
default and a forgotten `--access public` on a first publish is the difference
between published and not. `LICENSE` and `NOTICE` copied into each package, since
npm does not inherit them from a parent.

**`js/scripts/check-wasm.mjs`, wired as `prepack` on the core package.** This is
the guard for the most likely way to ship a broken package: the `.wasm` modules
are gitignored build output, so a publish from a fresh clone that skipped
`pnpm wasm` produces a tarball that installs cleanly, type checks cleanly, and
throws at the consumer's first `compare()`. Tested both ways: it passes with the
modules present and exits 1 with one of them moved aside.

`pnpm pack` on core produces a 156 KB tarball containing exactly `dist`, both
`.wasm`, `LICENSE`, `NOTICE`, `README.md` and `package.json`, with **no `src` and
no tests**.

READMEs gained an install line and a licence footer, and core gained a section on
where the `.wasm` actually comes from in a consumer's build, with the three cases
that need `configure()`. The two option names were verified against
`src/module.ts` rather than assumed: `wasmUrl` and `wasmBinary` both exist, and
the default really is `new URL('../wasm/<name>', import.meta.url)`.

## 2026-09-04, session 12 continued, 0.3 answered and N2 rehearsed

**0.3 answered: two platforms, `linux-x86_64` and `darwin-aarch64`.** The machines
Manas runs and can therefore test. Neither option A nor option B of the plan, and
better than both for a 0.1.0: A ships something no Mac can load, B spends a day or
two on Windows and two more targets nobody has asked for and nobody can verify by
hand. The CI matrix is two runners, `ubuntu-latest` and `macos-14`.

It makes one existing bug live, recorded as G2.0: `NativeLibrary`'s failure message
names all five platforms as bundled, so shipping two while claiming five sends a
Mac Intel or Windows consumer looking for a packaging fault that is really an
unsupported platform. Must be corrected before release.

### N2, the npm rehearsal

```bash
cd js && pnpm -r build && pnpm -r test
cd packages/<p> && pnpm pack --pack-destination <dir>     # for core, view, react
```

Build and tests green: 62 core, 83 view, 28 react. Three tarballs, 156 KB, 59 KB
and 19 KB, holding exactly `dist`, the assets each needs, `LICENSE`, `NOTICE`,
`README.md` and the manifest. **No `src` and no tests in any of the three.**

**`workspace:*` is rewritten by `pnpm pack`, confirmed by reading the packed
manifests.** `npm publish` would not do this and would publish a manifest naming a
protocol the registry cannot resolve.

**It rewrites to an exact pin, which contradicted the plan.** Plan section 2 rule 1
asks for a caret range, and `workspace:*` produces `"0.1.0"` rather than
`"^0.1.0"`. With an exact pin a bug fixed in core 0.1.1 stays invisible to every
`csvdiff-react` consumer until react is republished. Both manifests now say
`workspace:^`, and the packed output was re-read to confirm `^0.1.0`.

### The two scratch consumers, outside the repository

**Plain Node, core alone.** Proves the `exports` map and that
`new URL('../wasm/<name>', import.meta.url)` resolves with no bundler. Runs a real
diff and asserts the typed comparison: `10.50` against `10.5` under `DECIMAL` must
not be a changed cell.

One thing the run showed that is worth keeping: **that row is still reported under
`changesOnly`**, because it is equal only after normalization, so it carries a
suppressed cell and is not quiet. The contract behaving correctly through a
published tarball rather than only in the workspace.

The first attempt failed with `row 3 has 4 fields, expected 3`, and the fault was
the fixture, not the package: `DECIMAL(10,2)` carries a comma and the type cell has
to be quoted. The engine refusing it is the ragged row rule of 13.13.3 working.

**Vite plus React, all three packages.** Proves what the Node test cannot: that a
bundler follows the wasm URL into the dependency, and that the React peer range and
the `./styles.css` export resolve. `npm ls` shows all three at 0.1.0 and deduped,
and the build emits **both modules as hashed assets**,
`ibha_csvdiff-*.wasm` at 121 KB and `ibha_csvdiff.simd-*.wasm` at 139 KB, plus the
stylesheet and the worker chunk.

### One real defect the rehearsal caught

`workerClient.ts` chose its entry with a ternary over two `new URL(literal, ...)`
calls, one naming `./worker.ts`. The runtime choice was always right, because the
condition is false in a published package. **But a bundler analyses both arms
statically without evaluating the condition**, so every consumer's build went
looking for a TypeScript file no published package contains:

```
new URL('./worker.ts', import.meta.url) doesn't exist at build time,
it will remain unchanged to be resolved at runtime.
```

Harmless, and a warning in someone else's build that they cannot act on is a
support question rather than a bug report. The published `.js` path stays a
literal, which is what bundlers must see and rewrite; only the development path is
now built from a template expression, which keeps it out of static analysis and
costs nothing since that arm never runs outside this repository. Rebuilt,
repacked, reinstalled: the warning is gone and both wasm assets still emit.

The other two warnings are **expected and now documented in the core README**:
`node:fs/promises` and `node:worker_threads` are externalized for the browser.
Both imports are dynamic and behind a runtime check, so a browser evaluates
neither; a bundler still sees the specifier while tracing and says it stubbed it,
which is what should happen. Left alone rather than papered over, because the
alternative is a second browser-only build for a warning that is telling the truth.

## 2026-09-04, session 12 continued, the repo is public and J3 is built

**Thirteen commits had never been pushed.** Manas made the repository public and
found no README, which was not a missing README: `README.md`, `LICENSE`, `NOTICE`
and everything else were committed locally and `main` was thirteen commits ahead of
`origin/main`, including the four ingestion API commits from 2026-08-14 and -15.
Pushed after re-running the history scan.

The scan, run before the repository went public, because going public exposes all
history and not just the current tree:

```bash
git log --all --pretty=format: --name-only --diff-filter=A | grep -iE '\.env|\.pem$|\.key$|id_rsa|credentials|secret|\.npmrc|settings\.xml'
git grep -InE '(AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{36}|npm_[A-Za-z0-9]{36}|-----BEGIN [A-Z ]*PRIVATE KEY-----)' $(git rev-list --all)
git grep -IhoE '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' -- .
```

Nothing. Two addresses in tracked files, `manas@ibhatech.com` and the `git@github.com`
in the scm URL, both deliberate. The commit history carried a different, personal
address as its author, which is one of the reasons the history was later replaced
with a single commit authored by the published identity.

### J3

**The five platform overpromise is fixed**, which decision 0.3 made live.
`NativeLibrary` told a consumer the jar bundles all five while the build produces
one, so a Mac Intel or Windows user was sent looking for a packaging fault that was
really an unsupported platform. It now names exactly what is bundled, from a
`BUNDLED` constant beside a comment explaining that the **loader** is deliberately
wider than the bundle: `platform()` computes a tag for any host, so adding a
platform is a CI matrix entry rather than a code change.

Both READMEs now say the same thing, with the escape hatch for anything else.

**`.github/workflows/native.yml`**, the two runner matrix of 0.3: `ubuntu-latest`
for `linux-x86_64` and `macos-14` for `darwin-aarch64`. Each job builds its own
library, asserts it landed under the expected tag, generates the fixtures and
**runs the whole Java suite against it** before uploading. Linking is not evidence:
a `.dylib` that loads and then disagrees about a diff is worse than one that fails
to load. `fail-fast: false`, so one platform failing does not hide the other's
result. Exposed as `workflow_call` so the release workflow reuses this exact matrix
rather than a second copy that can drift.

**`.github/workflows/ci.yml`**, and this project had no CI at all until now.
It runs `run_phase6_checks.py --require-java` rather than restating any part of
what green means. `--require-java` is the flag that matters: without it the script
*skips* the Java half when Maven is absent and still reports success, which is
right for a developer without a JDK and exactly wrong for CI.

A second job covers the two failure modes the gate does not, both of which have
already happened once: a javadoc jar that will not build, which Central rejects,
and an npm tarball missing its engine. It also asserts that **no `workspace:`
specifier survives into any packed manifest**, which is the `npm publish` versus
`pnpm publish` trap turned into a check rather than a line in a runbook.

**`.github/workflows/release-java.yml`**, the collect step the matrix exists to
feed. Deliberately `workflow_dispatch` only rather than tag driven, until the
Central token and the GPG key exist: a deploy that fires on a tag before it can
authenticate fails at the worst possible moment. With `deploy: false` it builds the
fat jar, asserts both platforms are inside it, and uploads it as an artifact
without touching the network, which is the only way to see what a consumer would
get without burning a version number. `autoPublish` stays false in the pom, so even
a real deploy lands as a validated but unpublished bundle.

Java gate green after all of it: 132 tests, 240 byte identical comparisons.

## 2026-09-04, session 12 continued, the npm packages are published

All three are live at 0.1.0 under Apache-2.0, published by hand from a terminal.
Nothing in this repository holds an npm token and nothing in CI publishes: the
`packaging` job packs tarballs to prove the guard works and that no `workspace:`
specifier leaks, and stops there.

```bash
npm view @ibhatech/csvdiff-core  version dist-tags.latest license dependencies
npm view @ibhatech/csvdiff-view  version dist-tags.latest license dependencies
npm view @ibhatech/csvdiff-react version dist-tags.latest license dependencies
```

```
core   0.1.0  Apache-2.0  {}
view   0.1.0  Apache-2.0  { core: ^0.1.0 }
react  0.1.0  Apache-2.0  { core: ^0.1.0, view: ^0.1.0 }
```

The publish arrived in two parts: `core` first, then `view` and `react` after a
gap, which is why an intermediate check found the latter two returning 404.

### Verified from the registry rather than from the workspace

This is F5, and it exercises paths a workspace install never touches.

```bash
npm install @ibhatech/csvdiff-react      # in an empty project, nothing else named
npx vite build
```

Installing **only** react pulled view and core transitively through the caret
ranges and deduped them, which is the `workspace:^` decision paying off exactly as
intended: had it stayed `workspace:*`, those would be exact pins and a core patch
release would be invisible to every react consumer.

The Vite build succeeded against the published packages and emitted both wasm
modules as hashed assets, 121 KB and 139 KB, plus the stylesheet and the worker
chunk. All three packages carry `LICENSE` and `NOTICE`; core ships `wasm/`, view
ships `styles/`.

Two fixes were confirmed present in **what was actually published**, rather than
only in the working tree: the caret, which came out as `^0.1.0` rather than an
exact `0.1.0`, and the `workerClient` change, which is why a consumer build no
longer warns about a `worker.ts` that no published package contains. That warning
count is now zero against the registry copy.

A plain Node consumer was checked the same way, installing core alone from the
registry and running a real diff: 1 changed cell, with `10.50` against `10.5` under
`DECIMAL` correctly not counted, and the normalization-equal row still reported
because it carries a suppressed cell.

**0.1.0 is permanent for all three now.** Any correction is 0.1.1.
