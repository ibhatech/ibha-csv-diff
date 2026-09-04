# ibha-csvdiff

Compare two tables, cell by cell, and get the same answer everywhere.

One C engine. A WebAssembly build for the browser, a JNI binding for the JVM, and a
native CLI. The parser, the row matcher, the comparators and the four report
emitters live in the engine; the bindings feed it bytes and decode what it hands
back. They are not three implementations that happen to agree.

**That agreement is tested rather than asserted.** Every build is compared byte for
byte against every other build, across the fixture corpus and every emitter
configuration:

```
all 240 comparisons byte identical across native, wasm, wasm+simd and java
```

So a server side reconciliation and a browser preview of the same pair produce the
same report, as a matter of evidence.

---

## What it does

- **Key based row matching**, with move detection, and similarity pairing for
  tables that declare no key
- **Typed comparison**: `DECIMAL` compares by value, so `1.50` equals `1.5` and an
  Excel round trip does not report a whole file as changed. `TIMESTAMP` ignores
  trailing zeros in fractional seconds. `CHAR(n)` ignores its pad
- **Schema validation as output, not as failure**: a `REQUIRED` cell left empty or
  a value over its `VARCHAR(n)` is the point of the run, so it is reported rather
  than thrown
- **Intra cell diffing**, by word or by character
- **Four emitters**: JSONL, CSV, HTML and a summary
- **Streaming**: the engine holds a columnar index, not a document tree. On the
  JVM, 150,000 rows of JSON (16 MB) diff on both sides inside a **16 MB** heap;
  the same array parsed into a `Map`/`List` tree needs 256 MB for one side and no
  comparison at all

## Getting it

| | |
|---|---|
| Java, JDBC, CLOBs, JSON arrays | [`docs/usage-java.md`](docs/usage-java.md) |
| Browser, React, file uploads | [`docs/usage-javascript.md`](docs/usage-javascript.md) |

```xml
<dependency>
  <groupId>com.ibhatech</groupId>
  <artifactId>ibha-csvdiff-java</artifactId>
  <version>0.1.0</version>
</dependency>
```

```
npm install @ibhatech/csvdiff-react
```

**Neither is published yet.** See [`docs/PUBLISHING-PLAN.md`](docs/PUBLISHING-PLAN.md)
for where that stands.

The jar bundles a native library for **`linux-x86_64` and `darwin-aarch64`**, the two
platforms built and tested on real hardware. Elsewhere, build it with
`scripts-and-commands/build_jni.py` and point `-Dibha.csvdiff.nativeLibPath` at it.
The npm packages carry WebAssembly and run anywhere.

## Layout

```
core/    the C engine, the CLI, the fixtures and the fuzz targets
js/      three npm packages: csvdiff-core, csvdiff-view, csvdiff-react
java/    the JNI binding, com.ibhatech:ibha-csvdiff-java
specs/   the design authority. Section 13 of 02-solution-proposal.md is the
         locked decisions, and overrides everything earlier where they conflict
docs/    usage guides, the publishing plan, and one handoff per phase
scripts-and-commands/   the gate, the measurement scripts, and a log of what was
                        run and what it returned
```

**It is one repository on purpose**, and the reason is the determinism gate above:
it needs the native CLI, both wasm builds and the compiled Java classes in one
process at once. Splitting the three directories means that gate either moves to a
fourth repository or stops existing. The reasoning is written up in
`specs/03-remaining-tasks.md` section H.

## Building and testing

Needs a C compiler, a clang with the WebAssembly backend plus `wasm-ld`, Node 20+
with pnpm, and a JDK 21 or newer with Maven.

```bash
python3 scripts-and-commands/run_phase6_checks.py          # everything
python3 scripts-and-commands/run_phase6_checks.py --quick   # skips the 15 MB pair
python3 scripts-and-commands/run_phase6_checks.py --java-only
```

The C core is held to 478 assertions under four configurations (plain, trapping
UBSan, valgrind, and ASan/LSan/UBSan together), seven fuzz targets at 200,000
inputs each, and a strict warning set with `-Werror`.

## Status

0.x. The API can break on a minor bump, and the version you get from
`CsvDiff.engineVersion()` is the engine's, which is not the same number as the
binding's. A bug report wants both.

Phases 0 through 6 are built and green. Not built: the SIMD parser, the view polish
of spec 8.5, and a resumable emitter. The working list is
`specs/03-remaining-tasks.md`.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
