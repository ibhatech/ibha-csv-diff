# com.ibhatech:ibha-csvdiff-java

The Java binding over the `ibha-csvdiff` engine. One binding over the C ABI, not a
second implementation of it: the parser, the matcher, the comparators and the
emitters all live in the C core, and this artifact's job is to feed it bytes and to
decode the rows it hands back.

```java
try (Diff d = CsvDiff.compare(Path.of("source.csv"), Path.of("uploaded.csv"))) {
    DiffSummary s = d.summary();
    if (s.identical()) return;

    d.rows(RowOptions.changes()).forEach(row -> {
        for (DiffCell c : row.cells()) {
            if (c.changed()) {
                System.out.println(row.key() + " " + c.name() + ": "
                        + c.source().orElse("") + " -> " + c.target().orElse(""));
            }
        }
    });
}
```

## Why this exists

Spec section 9's argument, restated: if the server side reconciliation is done in
SQL and the browser preview is done by the engine, the two **will** disagree,
because SQL comparison inherits the database's collation, its numeric coercion, its
null and empty string semantics and its trailing space handling. The user sees one
set of differences and the approver sees another, and somebody files a bug that is
very unpleasant to explain. So the server uses the same engine, and the
`check_determinism` gate asserts that this binding's output is byte identical to the
native CLI's and to both wasm builds' on every fixture and every emitter.

## Two rules the API is built on

**Bytes in, never strings.** A `String` of a 15 MB file is 30 MB of char data plus
an encode pass to get back to the UTF-8 the engine reads. A `Path`, an
`InputStream` and a `ResultSet` are all already bytes, or can produce them without
going through one.

The rule is about what gets *materialized*, not about what a caller is allowed to
hold. A CLOB is character data and no amount of API design makes it bytes, so
`ofClob`, `ofReader` and `ofString` encode a chunk at a time straight into the
staging buffer: no intermediate `byte[]` of the whole text ever exists, and the
steady state cost is one chunk of chars. See below.

**Never a materialized diff.** `compare` returns a handle over engine memory whose
primary interface is a lazy `Stream<DiffRow>`. A 90,000 row diff as Java objects is
a few million allocations, which is the thing the C engine was chosen to prevent.

### Tell it the size when you know it

`DiffSource.ofFile` reads its own size and `ofBytes` has one. A stream does not, so
`ofStream(in, size)` is the overload to reach for whenever the length is knowable,
which for a multipart part, an HTTP `Content-Length` or an S3 `ContentLength` it is.
Without a hint the engine's byte buffer and index arrays double as they grow and the
arena does not reclaim the abandoned copies: on the p90 pair the whole comparison
reserves **86 MB hinted against 181 MB unhinted**, measured through
`Diff.bytesReserved()` in `StreamSourceTest`. That ratio is what sets how many
concurrent diffs fit on a batch worker.

It is a hint and never a limit. It does not truncate the read and is not checked
against what arrives, so a stale or approximate figure costs a little performance
and nothing else. The enforced ceiling is `maxBytes` in `DiffOptions`, which is a
different thing.

## A CLOB, or any other character data

The original side of a comparison often lives in a `CLOB` or an `NVARCHAR(max)`
column rather than in a file. Three factories read it as characters and encode as
they stream:

```java
DiffSource.ofClob(clob);        // opened and closed by the source; the Clob is not freed
DiffSource.ofReader(reader);    // any Reader, closed by the caller
DiffSource.ofString(text);      // a String you already hold, wrapped rather than copied
```

`ofClob` reads through `getCharacterStream`. **Never `getAsciiStream`**, which is
what a caller reaches for when the API offers only byte sources: it replaces every
non-ASCII character with a question mark, and a comparison of two corrupted sides
reports clean. The alternative that is correct, `ofBytes(rs.getString(n).getBytes(UTF_8))`,
holds the text three times at peak: the driver's `String`, the encoded array, and
the engine's arena copy.

`ofString` knows its exact encoded length and passes it as the size hint, so a text
source reserves what the equivalent byte source does, to the byte. `ofReader(in, size)`
takes one when the caller knows it; the hint is bytes, not characters, and a
character count is a fine undercount for ASCII-ish data.

The encode is chunked, including across the boundary that falls between the two
halves of a surrogate pair, which is the one thing a naive loop gets wrong and the
reason `CharacterSourceTest` sweeps the pair across that boundary rather than
placing it once.

## A header the data does not carry

A column holding data rows only, or a name row only while the keys and types live in
a metadata table, is the usual shape of use cases 2 and 3 in
`specs/input-options.md`. `withHeader` writes the header rows ahead of the bytes:

```java
try (Diff d = CsvDiff.compare(
        DiffSource.withHeader(header, DiffSource.ofClob(original)),  // header from metadata
        DiffSource.ofBytes(uploaded))) {                             // carries its own
    ...
}
```

It reports its `Header` from `DiffSource.header()`, so the number of header rows
written is the number the parse uses, under the same contract described below. It
refuses to wrap a source that already declares a header, and it does not check the
wrapped data's column count, which surfaces as the engine's own ragged row error.

One thing to watch: a byte order mark inside the wrapped source is no longer at
offset 0 once the header sits in front of it, so it stops being a BOM and becomes
part of the first field. Strip it before wrapping, or read that side through
`ofReader`, where it is one character rather than three bytes.

## A table held as a JSON array

`ofJsonRows` reads `[{"id":"1","name":"a"}, ...]` one row at a time into the same row
feed a `ResultSet` uses, so a JSON column and a file reach the same parser.

```java
DiffSource.ofJsonRows(header, clob);          // opens and closes its own stream
DiffSource.ofJsonRows(header, reader);        // closed by the caller
DiffSource.ofJsonRows(header, inputStream);   // UTF-8, per RFC 8259
```

**An array of objects, and only that.** The array of arrays shape was considered and
deliberately not built: the deployment does not hold it, and a shape nobody has is a
parser nobody tests.

The `header` decides the column order, because JSON objects are unordered by
definition and two rows of one export can list their keys differently. Each object is
projected by name.

**Four things are errors, each naming the row and the position in the document:** a
key the header declares that the object does not carry, a key the header does not
declare, the same key twice, and a nested object or array as a value. The first
would otherwise report as a changed value against a file that carries the real one,
which is a difference nobody made. The second is the JSON form of an added column,
which the CSV path also refuses by default. The third is undefined in JSON itself.
The fourth has no text form a CSV cell would agree with, exactly as binary has none.

Values render through `SqlValues`, the same code the JDBC path uses, because use
case 3 compares a JSON side against a JDBC side of the same data and two renderers
would eventually disagree. So JSON `null` is an empty field and `true` is `TRUE`.
**A number keeps its literal text** and never passes through a `double`: `1.50` has
to survive as `1.50` for a `DECIMAL(12,2)` comparison to mean anything, and
`12345678901234567890123` has to survive at all.

### What streaming it is worth

150,000 rows, 16.0 MB of JSON, through
`python3 scripts-and-commands/measure_json_rows.py`. The figure is the smallest heap
each one completes at, which is what decides how many concurrent diffs fit on a
worker:

```
what                                        heap     time
ofJsonRows, both sides, whole diff          16 MB    379 ms
the same array as a Map/List tree, one side 256 MB   115 ms
```

The tree does not fail at 128 MB by a little: it fails, and it is parsing *one* side
and doing no diff at all. Read the times with that in mind rather than as a race.

## Result sets, and where the header comes from

A JDBC `ResultSet` is data rows only. The engine's model wants up to four header
rows above the data: `KEY` markers, `REQUIRED` markers, declared types, and column
names. **Nothing in a result set carries the first**, and where the rest live, a
metadata table, a config file, a schema registry or `ResultSetMetaData`, differs per
deployment. So the caller supplies the header and this binding derives none of it.

```java
Header header = Header.builder()
    .column("account_id", "VARCHAR(20)").key().required()
    .column("period",     "CHAR(7)").key().required()
    .column("premium",    "DECIMAL(12,2)")
    .build();

try (Diff d = CsvDiff.compare(
        DiffSource.ofResultSet(sourceRs, header),                       // authoritative
        DiffSource.ofResultSet(targetRs, Header.namesOnly(names)))) {   // inherits the rest
    ...
}
```

Three ways to build a `Header`, all producing the same rows: the builder above,
`Header.ofRows(List<List<String>>)` for four rows you already hold, and
`Header.namesOnly(List<String>)` for a target side, which per spec 13.8 inherits
keys, required flags and types from the source.

`DiffSource.ofRows(header, iterator)` is the same feed without JDBC, for a caller
reading from a queue, a Parquet file or a list.

**A row source states its own header row count**, through `DiffSource.header()`, and
that is what the source side is parsed with. `DiffOptions.headerRows` is for a
*byte* source, whose header is inside its own bytes and whose length nothing outside
them can know. Setting both to different numbers is rejected with a message naming
the two, rather than resolved in favour of either: a caller who wrote both down had
one of them in mind and guessing which is how this went wrong the first time. It
went wrong as a `Header.namesOnly` source parsed under the default of four, which
ate the name row plus the first three data rows and reported the fourth data row as
the column names, with no error and `identical=true`. `SourceHeaderLayoutTest` is
what keeps it fixed.

The rows are encoded as RFC 4180 CSV into a direct `ByteBuffer` and handed over a
batch at a time, per spec 13.6, so a result set and a file reach the same parser and
cannot be compared under different rules.

### How a JDBC value becomes text

Not with `rs.getString`. Drivers disagree about exactly the types a reconciliation
cares about, and the determinism gate requires byte identical output, so the rules
are stated here and are the same on every driver:

```
SQL NULL      -> an empty field
BigDecimal    -> toPlainString(), so 1.50 stays 1.50 and never becomes 1.5E0
double, float -> plain decimal, never exponent notation
DATE          -> 2026-01-31
TIME          -> 14:22:05
TIMESTAMP     -> 2026-01-31T14:22:05, with a fraction only when it is non zero
BOOLEAN       -> TRUE / FALSE, which are in the engine's default truth sets
CLOB          -> its text
BLOB, byte[]  -> refused
```

**NULL and the empty string are the same field**, because CSV has no NULL. Making
the JDBC side distinguish them would report differences the file side could never
agree with. Binary is refused rather than silently base64'd, because any encoding
chosen here would be one the file did not use and every such cell would report as
changed.

## What it costs

The p90 pair, 15 MB a side, on Linux x86-64 with the JNI backend. Regenerate with
`python3 scripts-and-commands/measure_java_binding.py` rather than editing this
table.

```
what                                seconds   note
compare, parse and match              0.113   82 MB reserved
summary, one drain                    0.031   constant memory
rows(), values off                    0.023   146,946 rows
rows(), values decoded                0.064   1,763,352 cells
rows(), changes only                  0.062   2,198 rows
rows(), values off, batch 1           0.020   one crossing per row
rows(), decoded, batch 1              0.069   one crossing per row, cells decoded
emit jsonl, changes only              0.035   1,367 KB, two passes
emit jsonl, whole report              0.314   86 MB, two passes
row feed, 50,000 rows, batch 1000     0.058   encode plus parse plus match
row feed, 50,000 rows, batch 100      0.059   encode plus parse plus match
```

Read the gap between values off and values decoded as what decoding 1.7 million
cells into Java strings costs.

The two `batch 1` rows deflate a claim this binding could easily have made. Running
the same walk with one JNI crossing per row instead of one per thousand is inside
the noise here, so **the batch drain is cheap insurance, not the load bearing
decision**. The load bearing one is that cell values never cross the boundary at
all: they are decoded from the columnar arrays directly, and there are twelve times
as many cells as rows in this table, with the ratio growing as tables get wider.

## The row contract

Versioned, and shared with the JSONL emitter field for field, so a consumer can move
between streamed rows and a saved report without a translation layer.

**The one rule that is easy to get wrong.** A matched row's cell carries `source()`
**exactly when that cell differs in bytes from the target**. An empty `source()`
means the two sides are byte identical; it never means the value was empty. A
consumer that reads an absent source as an empty string renders every unchanged cell
as a deletion.

Three more things a consumer has to hold to:

- **`moved()` is a flag on the row, not a kind.** A row can move and be modified in
  one edit, and a single enum loses one of the two facts.
- **`row.cells().size()` is not always the source file's column count.** Under the
  column policy of spec 6.6 a report row carries the columns the two files share.
  Read the width from the row and the names from `Diff.columns()`.
- **Findings are output, not errors.** A `REQUIRED` column with an empty cell or a
  value over its `VARCHAR(n)` is the point of running the comparison, so it never
  aborts and `changesOnly` never drops a row whose only news is one.

## The backend is selected at runtime

Spec 13.6 pins this binding to JDK 21, where the Foreign Function and Memory API is
a preview feature under JEP 442 and was not final until JEP 454 in JDK 22. A library
cannot demand `--enable-preview` of its consumers, so the shipped backend is hand
written JNI. `Diff` is an interface and the backend is discovered with
`ServiceLoader`, so an FFM backend can be added later as a separate artifact and win
the selection wherever it can run, with no change visible here.

`-Dibha.csvdiff.provider=jni` pins the choice, which is what a differential test
between two backends would use.

A handful of members are public only because a backend lives in another package and
needs them: `ChangeKind.ofOrdinal`, `FindingKind.bit`, the `Finding` factories,
`TextSegment.Op.ofOrdinal`, `DiffSummary.parse`, and `HeaderLayout` with
`DiffOptions.headerLayout`. The last of those resolves the header row count from the
options and from what the source declares, and it lives here rather than in the
backend so that a second backend cannot resolve it differently.

## The native library

**Version 0.1.0 bundles two platforms: `linux-x86_64` and `darwin-aarch64`.**
Those are the two built and tested on real hardware by the `native` workflow, which
runs the full Java suite on each rather than only linking the library. A build that
links and is never run proves nothing: a `.dylib` that loads and then disagrees
about a diff is worse than one that fails to load.

Anywhere else, build it and point the loader at it:

```bash
python3 scripts-and-commands/build_jni.py
java -Dibha.csvdiff.nativeLibPath=java/target/native ...
```

The **loader** is not limited to those two. It computes an `<os>-<arch>` tag for any
host, including `linux-aarch64`, `darwin-x86_64` and `windows-x86_64`, so adding a
platform is a CI matrix entry rather than a code change. They are absent because
nobody has one to test on, not because anything here refuses them.

The jar carries one library per platform under `native/<os>-<arch>/` and extracts it
to a temporary file on first use. `-Dibha.csvdiff.nativeLibPath=<dir>` overrides that and
loads from a directory instead, which is what the build does with a library it has
just compiled.

**On JDK 24 and newer**, loading a native library is a restricted operation and the
JVM warns; a later release will refuse it. Pass
`--enable-native-access=ALL-UNNAMED`, or name this module in the equivalent manifest
entry, to silence it now and to keep working later.

## Building and testing

```
cd java && mvn test                      # builds the JNI library, then runs the suite
python3 scripts-and-commands/build_jni.py   # the library alone, no Maven needed
python3 scripts-and-commands/run_phase6_checks.py --java-only
```

Java 21 bytecode via `--release 21`, built by whatever JDK is installed. The native
build compiles the engine's C sources and the glue into one shared library, and it
runs `javac -h` first so the generated header is **included** by the glue: every
native signature is checked by the C compiler against its Java declaration, which
turns the classic hand written JNI failure into a build error.

The test suite needs the generated fixtures:

```
python3 core/fixtures/gen_fixtures.py
```
