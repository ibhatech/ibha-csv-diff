# Handoff: Phase 6 complete, the Java binding runs the same engine and proves it

Date: 2026-08-14
Next phase: Phase 7, the SIMD parser

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/02-solution-proposal.md` **section 13** | **Authoritative locked decisions.** Overrides anything earlier where they conflict. 13.11 is the one Phase 7 lives inside |
| `core/include/ibha_csvdiff.h` | The ABI. **Unchanged again this phase**, 51 marked declarations |
| `java/README.md` | The binding's API, the JDBC header contract and the measured costs |
| `scripts-and-commands/check_determinism.mjs` | Now compares four builds. This is the harness Phase 7 exists to be caught by |
| This file, sections 4 and 6 | What Phase 6 established, and what Phase 7 should take from it |

## 2. The headline results

**The Java binding produces byte identical output to the native CLI and to both
wasm builds, on every fixture and every emitter configuration, including a 90 MB
JSONL report.**

```
all 240 comparisons byte identical across native, wasm, wasm+simd and java
```

That is spec section 9's actual promise under test. A server side reconciliation
and a browser preview of the same pair are now the same report as a matter of
evidence rather than of intent.

**The row contract has a third implementation and it is checked against the first.**
`RowContractTest` asserts the binding's decoded rows against the JSONL emitter's own
output on the p90 pair, field for field, plus the escaped, multiline, CRLF, BOM,
Latin-1 and XSS fixtures. 2,198 changed rows and twelve columns on the p90 alone.

**61 new assertions**, all passing, plus the whole Phase 5 gate unchanged.

```
[     ok] jni library builds warning free
[     ok] java binding suite, including the emitter row contract   61 tests
[     ok] determinism, native against both wasm builds and java    240 comparisons
gate passed
```

## 3. What is done and verified

`python3 scripts-and-commands/run_phase6_checks.py` runs everything: the Phase 3 C
gate, the Phase 4 and 5 JS gates, and the Phase 6 additions. `--java-only` runs
Phase 6 alone in about twenty seconds.

### New in `java/`

| File | Contents |
|---|---|
| `pom.xml` | **Maven, not Gradle.** See section 5.1 |
| `src/main/c/ibha_csvdiff_jni.c` | The glue. ~40 entry points, no diff semantics |
| `src/main/java/com/ibhatech/csvdiff/*.java` | The public API: `CsvDiff`, `Diff`, `DiffSource`, `Header`, `Column`, the row and summary records, the options, `CsvDiffProvider` |
| `.../csvdiff/Json.java` | A 200 line JSON reader, package private. See 5.4 |
| `.../csvdiff/jni/*.java` | The backend: `NativeEngine`, `NativeLibrary`, `JniProvider`, `JniDiff`, `TableView`, `Utf8` |
| `src/main/resources/META-INF/services/...` | Registers the JNI backend |
| `src/test/java/.../*.java` | 61 assertions across seven suites, including the row contract test |
| `src/test/java/.../tools/EmitMain.java` | The determinism driver, taking the CLI's flags |
| `src/test/java/.../tools/BenchMain.java` | The measured cost table in the README |
| `README.md` | The API, the JDBC header contract, the numbers |

### New in `scripts-and-commands/`

| File | Contents |
|---|---|
| `build_jni.py` | `javac -h`, then the engine and the glue into one shared library |
| `run_phase6_checks.py` | The whole gate. `--java-only`, `--quick`, `--require-java` |
| `measure_java_binding.py` | Regenerates the README's cost table |
| `check_determinism.mjs` | **Extended, not copied.** Java is a fourth producer |

## 4. The contracts Phase 6 established

### 4.1 A result set is data rows; the header is supplied separately

This was the design question of the phase and it was settled by Manas: **the binding
derives nothing from `ResultSetMetaData`.** A JDBC result set carries no `KEY`
markers, no `REQUIRED` markers and no declared types in the form the engine's four
row header model wants, and where those live differs per deployment. So the caller
hands over a `Header` alongside the `ResultSet`, and where they got it, a metadata
table, a config file, a schema registry, or the driver's metadata, is their
business.

`Header` has three constructors that all reduce to the same rows: a builder,
`ofRows(List<List<String>>)`, and `namesOnly(List<String>)` for a target side, which
per spec 13.8 inherits everything else from the source. The header rows are written
into the byte stream ahead of the first data row, so **a result set and a file reach
the same parser**. There is no second front end and no second set of diff semantics,
which is what makes `aResultSetDiffsTheSameAsTheEquivalentCsv` a test that can pass.

The only thing read from the driver is `getMetaData().getColumnCount()`, and only to
refuse a header whose width disagrees with the query before a single row is parsed.
`FakeResultSet` throws on any other metadata call, so that stays true by
construction.

### 4.2 JDBC values are rendered by explicit rules, not by the driver

`SqlValues` renders each type itself: `BigDecimal.toPlainString()`, ISO-8601 dates
and timestamps with seconds always present, `TRUE`/`FALSE`, plain decimals with no
exponent notation, and binary refused. `rs.getString` was rejected because drivers
disagree about exactly these types, and a diff that depends on which driver is on
the classpath is not a diff anyone can act on. **SQL NULL renders as an empty
field**, like an empty string, because CSV cannot express the distinction and a
source file could never agree with a JDBC side that made it.

Both of these are flagged as assumptions in section 8.

### 4.3 The backend is an SPI, and the public API never names it

`Diff` is an interface; `CsvDiffProvider` is discovered with `ServiceLoader`; the
available provider with the highest priority wins and
`-Dibha.csvdiff.provider=<name>` overrides. The JNI backend is priority 0. An FFM
backend for JDK 22+ ships later as a separate artifact, registers itself the same
way, and wins wherever it can run, with no change visible to consumers. That is
spec 13.6's requirement implemented rather than promised.

One consequence worth knowing: a handful of members are public that would otherwise
be package private, because a backend in another package builds rows with them:
`ChangeKind.ofOrdinal`, `FindingKind.bit`, the `Finding` factories,
`TextSegment.Op.ofOrdinal` and `DiffSummary.parse`. They are documented as the
backend surface.

### 4.4 The JNI signatures are checked by the compiler, not by hope

`build_jni.py` runs `javac -h` over `NativeEngine.java` and the glue **includes** the
generated header. A parameter added, removed or retyped on either side is a C
compile error. The classic hand written JNI failure, a signature that drifted and
surfaces as an `UnsatisfiedLinkError` or, worse, a silently misread argument, cannot
happen here. It is also what satisfies `-Wmissing-prototypes`, so the glue is held
to the engine's own warning flags and builds clean under them.

**This binding needs no generated struct offsets.** The JS binding has `abi.ts` and
`gen_abi.mjs` because a wasm host has no C compiler at the boundary. Here the
boundary is C, so a struct field is a struct field. There is nothing to keep in
step and nothing to regenerate.

## 5. Decisions made in this phase, and why

1. **Maven, not Gradle.** The scaffolding was `build.gradle.kts` and Gradle is not
   installed; Maven 3.9.12 is, with JUnit already in the local repository. Manas
   chose conversion over an install. The two Gradle files are deleted rather than
   left to rot. Java 21 bytecode now comes from `<release>21</release>` rather than
   from a toolchain pin, which is the stronger guarantee anyway: it compiles against
   the JDK 21 API signatures, so a class file cannot reference something newer, and
   it does not require JDK 21 to be installed.

2. **The row feed is CSV encoding into a direct buffer, not a new C ABI.** Spec 13.6
   describes the batch `ByteBuffer` feed as "the reason the row feed ABI takes a
   batch rather than a field", and there is no row feed in the ABI: it was never
   built. Adding one means a second front end into the columnar index, with its own
   tests, fuzz target and determinism coverage, and it reopens a core that has been
   unchanged since Phase 4. Encoding rows as RFC 4180 into the staging buffer gets
   the JDBC path that 13.6 exists to serve, at the cost of one escape pass, and
   keeps exactly one implementation of diff semantics. A native row feed can replace
   it later behind `DiffSource` with nothing visible to consumers.

3. **The batch claim was measured and it turned out smaller than advertised.**
   Running the report walk with one JNI crossing per row instead of one per thousand
   is inside the noise on the p90 pair, 0.020 against 0.023 seconds. The comments
   that claimed otherwise were corrected rather than the benchmark quietly dropped.
   What actually carries the throughput is that **cell values never cross the
   boundary**: 1.76 million cells against 147 thousand rows, and the ratio grows with
   table width. The `batch 1` rows are kept in the README table for that reason.

4. **A 200 line JSON reader instead of a dependency.** The summary must come from
   the summary emitter's own bytes, because the cell counters accumulate as a cursor
   advances and a struct read reports whatever the caller happened to have drained.
   That needs a JSON parser. Pulling in Jackson to read an object this library
   produced itself would impose a transitive dependency, and its version conflicts,
   on every consuming application, which for an artifact whose whole purpose is to
   be embedded in someone else's server is a poor trade.

5. **Invalid UTF-8 is detected before decoding, not after.** `Utf8.isValid` runs over
   the byte range and then `new String(...)` decodes it. Decoding first and looking
   for U+FFFD cannot tell a replacement from a replacement character the file
   genuinely contained, and a report that claims a customer's data was mojibake when
   it was not costs an afternoon.

6. **Error detail crosses as `byte[]`, not as `String`.** `NewStringUTF` takes
   *modified* UTF-8. The engine's error messages name the offending key, which is
   file data, which can be any UTF-8 at all, and handing a four byte sequence to
   `NewStringUTF` is undefined behaviour in the JVM. The one place it would bite is
   the duplicate key message for a CJK or emoji key, which is exactly when a caller
   most needs to read it.

7. **The compare options are one native allocation, made once.** The engine bakes
   them into every row digest and refuses a pair whose `compare_id` differs, so
   building them once and handing the same pointer to both parses and to the diff
   makes that agreement structural rather than something three call sites must
   remember. It also gives the two boolean word lists, which the engine borrows
   rather than copies, one owner with one lifetime.

8. **`changesOnly` is filtered in Java, not in the glue.** The glue holds no policy.
   The quiet row rule is restated in `DiffRow.isQuiet`, exactly as the JS binding
   restates it, and the row contract test is what keeps the restatement honest.

## 6. What Phase 7 needs to know

1. **The determinism check is now a four way comparison and it is the SIMD gate.**
   Phase 7 replaces the parser's inner loop with SIMD intrinsics. `check_determinism`
   already compares native, scalar wasm, SIMD wasm and Java over the fixture and
   emitter matrix, and it is what will catch a SIMD parser that disagrees about a
   quoted field straddling a 16 byte lane. It passes today, which is what makes its
   first failure mean something.

2. **The Java build compiles the engine's C sources directly**, listed in
   `build_jni.py` as `CORE_SOURCES`. It asserts that list against `CORE_SRC` in
   `core/Makefile` and fails if they diverge, so a source file added for the SIMD
   parser will stop the Java build until it is added there too. That is deliberate:
   the alternative is a Java binding quietly running a different engine.

3. **The ABI is still unchanged.** Two phases now. If Phase 7 adds a runtime feature
   detection entry point, it is the first ABI change since Phase 3 and both bindings
   will need it.

4. **The parser numbers to beat**, from this run of the Phase 3 gate: 723 MB/s zero
   copy, 569 MB/s streamed, 496 MB/s with digests zero copy, 385 MB/s with digests
   streamed. Spec 11 gates SIMD on scalar parse throughput and the scalar parser is
   comfortably above the 300 MB/s line, so SIMD is a throughput improvement rather
   than a rescue.

5. **The scalar parser stays permanently** as the differential oracle, per spec
   13.11. Nothing in this phase changes that.

## 7. What is deliberately not done

1. **A random access index in the Java binding.** The JS binding has `index()`
   because a virtualized view has to seek. A server side binding streams, emits and
   summarizes; a retained index is the one thing whose memory grows with the diff,
   and building it for a consumer who has not appeared would be speculative.

2. **A native row feed ABI.** See 5.2. The Java side of it exists and is behind
   `DiffSource`, so the replacement is invisible to consumers when it comes.

3. **A resumable emitter.** Unchanged from Phases 4 and 5. `ibha_csvd_emit` is all or
   nothing, so `emit` sizes with one pass and fills with a second, and a large report
   is produced twice. `emitTo(OutputStream)` therefore does not stream from the
   engine, and says so in its own javadoc rather than implying otherwise. The fix
   belongs in the engine.

4. **Cross platform jars.** Only `linux-x86_64` is built here, because it is the only
   platform this machine is. The layout the jar wants is in place and
   `NativeLibrary` looks for all five; a release needs one CI runner per target and
   a step that collects them into `target/native/` before packaging.

5. **JPMS.** The jar carries `Automatic-Module-Name: com.ibhatech.csvdiff` rather
   than a `module-info`. JNI plus extracted native resources is a poor first thing to
   learn JPMS on, and an automatic module costs consumers nothing.

6. **A published artifact.** `mvn deploy` has no repository configured and no signing
   key. Worth doing when there is somewhere to publish to.

7. **Kotlin.** Spec section 9 lists Kotlin and JVM services as using the same
   binding, which they do, with no Kotlin specific work. A Kotlin friendly wrapper
   (nullable types instead of `Optional`, `use` blocks) would be sugar over this and
   is not needed for it to work.

## 8. Assumptions still flagged

The first three are unchanged since Phase 3. **Flag them, do not re-decide them.**

1. **The asymmetric ragged row rule.** A row with extra fields that are all empty is
   normalized and counted in `stats.ragged_normalized`; a row with missing fields is
   `IBHA_CSVD_ERR_RAGGED_ROW`. Three line change in `row_end` in `core/src/parse.c`.

2. **Duplicate key row numbers are record based, not line based.** Every row number
   in every emitter, in every binding row and now in every Java `DiffRow` is the same
   1 based record number, which differs from the physical line when the file has
   blank lines or multiline quoted fields.

3. **`VARCHAR(n)` is counted in characters, not bytes.** `café` is four characters in
   five bytes and does not violate `VARCHAR(5)`. One line in `char_count` in
   `core/src/validate.c`.

New in this phase, both in the JDBC path and both easy to change in `SqlValues`:

4. **SQL NULL renders as an empty field**, indistinguishable from an empty string.
   CSV has no NULL, so a source file cannot express the difference either. If a
   deployment's CSV export writes a sentinel such as `\N`, this needs a `nullText`
   option to match it, and without one every NULL cell would report as changed
   against such a file.

5. **Timestamps render as `2026-01-31T14:22:05`**, with a fractional part only when
   it is non zero, and dates as `2026-01-31`. If the CSV files being compared carry
   `31/01/2026` or a space instead of the `T`, every timestamp cell will differ.
   Changing the format is a few lines; the alternative, comparing dates by value, is
   `IBHA_CSVD_DATE_VALUE` in the engine and is explicitly not implemented, so it
   would be engine work rather than binding work.

## 9. Blockers needing Manas's action

1. ~~No WebAssembly toolchain.~~ Resolved 2026-08-12.
2. ~~No libFuzzer runtime.~~ Resolved 2026-08-12.
3. ~~No pnpm and no installed JS dependencies.~~ Resolved 2026-08-13.
4. ~~No React types and no DOM for component tests.~~ Resolved 2026-08-14.
5. ~~JDK 21 is not installed; JDK 17 is.~~ **Resolved 2026-08-14, and it was stale.**
   The machine has JDK 25 (Oracle GraalVM) and Maven 3.9.12, so `--release 21` gives
   the spec's target with nothing to install. Nothing was installed for this phase.

6. **Repos are not git initialized as submodules.** `core/`, `js/` and `java/` are
   ordinary directories in one repository. **This got harder again, as predicted.**
   `build_jni.py` compiles `../core/src/*.c` and reads `../core/Makefile` to check it
   is compiling the right set; the Java tests read `../core/fixtures/generated` by
   relative path; `check_determinism.mjs` drives the C CLI, both wasm modules and the
   Java classes from one script. Splitting the three repos now means either
   submodules with pinned SHAs, which makes a cross cutting change a three commit
   dance, or publishing the fixtures as an artifact. **Worth deciding, and the cost
   of deciding it later keeps rising.**

7. **A publishing target for the Maven artifact**, if this is meant to be consumed
   outside this repository. Needs a repository URL and a signing key.

## 10. Working notes

- `python3 scripts-and-commands/run_phase6_checks.py` runs everything. `--quick`
  skips the 15 MB pair, `--java-only` runs Phase 6 alone, `--require-java` fails
  rather than skips when Maven is absent, which is what CI should pass.
- `cd java && mvn test` builds the JNI library first through `build_jni.py`, so there
  is one description of the native build rather than two.
- `python3 scripts-and-commands/build_jni.py` builds the library alone and needs no
  Maven, so the glue's warnings are checkable on any machine with a compiler.
- `-DskipNative=true` builds the Java half alone, for an IDE or a javadoc run. The
  tests then fail to load the engine rather than quietly passing.
- **On JDK 24 and newer the JVM warns about loading a native library.** The build
  passes `--enable-native-access=ALL-UNNAMED` through a profile keyed on the running
  JDK, because the flag does not exist on JDK 21. Consumers need the same flag; it is
  in the README rather than left to be discovered from a warning.
- `python3 scripts-and-commands/measure_java_binding.py` regenerates the README's
  cost table. Do not edit it by hand.
- The C side is unchanged in this phase and unchanged in its verification: 465
  assertions, three sanitizers, valgrind, four fuzz targets, warning free under the
  strict flag set. Keep it that way.

---

## 10a. Addendum, 2026-08-14, after the handoff was written

**`docs/HANDOFF-api-surface.md` is the handoff for the work summarized here**, and
carries the prompt for continuing it. The prompt in section 11 below is still the
Phase 7 one and is still valid; read section 7 of the other file for which of the two
to start.

An assessment of the public API against the five client ingestion use cases is in
`docs/api-surface-vs-input-options.md`. It found one defect in this phase's work,
which is **fixed**; the rest of that document is proposed work, not done work.

**The defect.** `JniDiff.parseSide` took the source side's header row count from
`DiffOptions` alone, so a `Header.namesOnly` row source parsed under the default of
four ate its name row plus the first three data rows. The fourth data row became the
column names, three of five rows vanished, and the summary said `identical=true`.
Every test in section 3 used `namesOnly` on the target side, where the count is
auto-detected, which is why the gate was green.

**The fix.** A source declares its own header through the new
`DiffSource.header()`, and `DiffOptions.headerLayout(Header)` reconciles that with
the options: a declared header beats the default and conflicts with a *stated*
`headerRows` rather than overriding it. New public type `HeaderLayout`, the five
numbers a parse opens with, which is also now the one place the row positions are
derived and still matches `engine.ts` line 434 exactly.

**Then item 1 of that document's proposed additions.**
`DiffSource.ofStream(InputStream, long size)`, so a stream fed side can say what it
knows. The unhinted overload delegates to it with 0; a negative size is rejected.

**And a correction that came out of measuring it.** `DiffSource.sizeHint()` claimed
that supplying a hint takes peak retention "from roughly 4x N rather than 1x".
Measured through `Diff.bytesReserved()` on the p90 pair it is 86 MB hinted against
181 MB unhinted, **a factor of 2.1, not 4**. The javadoc was corrected rather than
the measurement dropped, the same way the batch drain claim was in section 5.3. The
4x wording still stands in the JS binding, which is a different allocator on a
growing wasm memory and was not measured; correcting it there from a native number
would be the same mistake in the other direction. **Whoever measures the wasm side
should close that out.**

Consequences for the numbers in this file: the Java suite is **80 tests, was 61**,
and section 4.3's list of backend surface members gains `HeaderLayout` and
`DiffOptions.headerLayout`. The ABI is still unchanged, the C core is untouched, and
the determinism gate is still 240 byte identical comparisons across all four
producers.

---

## 11. Prompt for the next conversation

Copy everything below into a new conversation.

---

I am continuing work on the ibha-csvdiff project. Phases 0 through 6 are complete,
committed, and the tree is clean, plus two commits of ingestion API work on top of
them described in `docs/HANDOFF-api-surface.md`. Phase 7 is next: the SIMD parser.

No toolchain blocker is open. clang 21.1.8, gcc, Node 24, pnpm 9.12, JDK 25 and
Maven 3.9.12 are all present. Ask before installing anything.

Start by reading, in this order:

1. `docs/HANDOFF-phase6.md` - full state. Section 6 is the five things Phase 7
   specifically needs, section 7 is what was deliberately left undone, section 8 is
   the five flagged assumptions
2. `specs/02-solution-proposal.md` **section 13**, and **13.11 in particular**: WASM
   SIMD is baseline in every evergreen desktop browser, so the SIMD build is the
   expected runtime path rather than an enhancement, and **the scalar parser is
   retained permanently as the differential oracle**. Section 13 overrides earlier
   sections where they conflict, so do not re-derive decisions from sections 1 to 12
3. `specs/02-solution-proposal.md` **section 3.3**, where SIMD helps and where it
   does not, and section 2.6.6 on why this was promoted out of a measurement gate
4. `core/src/parse.c` - the scalar state machine, which stays
5. `scripts-and-commands/check_determinism.mjs` - the harness this phase will be
   caught by

Then implement **Phase 7: the SIMD structural scan**.

Constraints:

- **The scalar parser is not replaced, it is retained as the oracle.** A property
  test must assert that the scalar and SIMD parsers produce identical indexes on
  every fixture and every fuzz input
- **Identical output is the gate, not a goal.** `check_determinism.mjs` compares
  native, scalar wasm, SIMD wasm and Java byte for byte over the fixture and emitter
  matrix, and it passes today. It must still pass, and it is the reason a lane
  boundary bug cannot ship quietly
- **Runtime feature detection**, with `pclmulqdq` and AVX2 or NEON on native and the
  emulated prefix XOR on wasm. If this needs a new ABI entry point it is the first
  ABI change since Phase 3, and both bindings will need it
- **Deliverable: a measured 2 to 4x parse speedup**, native ahead of wasm, with
  identical output. The scalar numbers to beat are in handoff section 6
- Do not add view features deferred to 0.3 and 0.4, and do not start the native row
  feed described in handoff section 7

Working notes:

- `python3 scripts-and-commands/run_phase6_checks.py` runs the whole gate and must
  keep passing. `--quick` skips the 15 MB pair
- `build_jni.py` asserts its source list against `core/Makefile`, so a new C source
  file must be added to both
- Per CLAUDE.md: python scripts rather than bash with quoted variables, saved under
  `scripts-and-commands/`, and a handoff file at the end of the phase

Five assumptions are implemented but unconfirmed and are described in handoff
section 8: the asymmetric ragged row rule, duplicate key row numbers being record
based rather than line based, `VARCHAR(n)` counted in characters rather than bytes,
and two new ones in the JDBC path, SQL NULL rendering as an empty field and the
timestamp text format. Flag them rather than re-deciding them.
