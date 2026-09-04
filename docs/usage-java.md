# Using ibha-csvdiff from Java

`com.ibhatech:ibha-csvdiff-java` compares two tabular sources, cell by cell, with
key based row matching, schema validation and four report formats. It is a binding
over the C engine, not a second implementation: a CSV file, a JDBC `ResultSet`, a
`CLOB` and a JSON array all reach the same parser and are compared under the same
rules, which is what lets a server side reconciliation and a browser preview agree
byte for byte.

This is the task oriented guide. `java/README.md` is the design rationale and the
measured costs. The javadoc is the reference.

---

## 1. Install

**Not published yet.** Until it is, build it locally:

```bash
python3 core/fixtures/gen_fixtures.py     # once, the tests need the fixtures
cd java && mvn install
```

After publishing:

```xml
<dependency>
  <groupId>com.ibhatech</groupId>
  <artifactId>ibha-csvdiff-java</artifactId>
  <version>0.1.0</version>
</dependency>
```

Java 21 or newer. The jar carries the native library and extracts it on first use;
there is nothing to install and no `java.library.path` to set.

**On JDK 24 and newer, add `--enable-native-access=ALL-UNNAMED`** to the JVM that
loads it. Without it the JVM prints a warning today and a future release will refuse
the load outright.

---

## 2. Quick start

```java
import com.ibhatech.csvdiff.*;
import java.nio.file.Path;

try (Diff d = CsvDiff.compare(Path.of("original.csv"), Path.of("uploaded.csv"))) {
    DiffSummary s = d.summary();
    System.out.printf("%d modified, %d added, %d deleted%n",
            s.rows().modified(), s.rows().added(), s.rows().deleted());

    if (s.identical()) return;

    d.rows(RowOptions.changes()).forEach(row -> {
        for (DiffCell c : row.cells()) {
            if (c.changed()) {
                System.out.printf("%s  %s: %s -> %s%n",
                        String.join("|", row.key()), c.name(),
                        c.source().orElse(""), c.target().orElse(""));
            }
        }
    });
}
```

---

## 3. The three things that will bite you

Read these before writing a consumer. Each one is a real defect if you get it wrong,
and none of them throws.

**A cell carries `source()` exactly when it differs.** `c.source()` being empty means
the two sides are byte identical, not that the value was empty. A consumer that
renders an absent `source()` as `""` shows every unchanged cell as a deletion.
`target()` is the current value and is absent only on a deleted row.

**`row.cells().size()` is not the file's column count.** Under the column policy the
report carries the columns the two sides share. Read the width from the row and the
names from `Diff.columns()`, never from your own header object.

**Findings are output, not errors.** A `REQUIRED` column with an empty cell, or a
value longer than its `VARCHAR(n)`, is the point of running the comparison. It never
aborts, and `changesOnly` never drops a row whose only news is a finding.

One more, less costly but confusing when it happens: **`moved()` is a flag on the
row, not a kind.** A row can be moved and modified in the same edit, so `kind()` and
`moved()` are separate facts.

---

## 4. Where the data comes from

`DiffSource` is the input side. Every factory ends as bytes in the same staging
buffer, so the choice is about what you hold, not about how it is compared.

| You have | Use | Notes |
|---|---|---|
| A file | `DiffSource.ofFile(path)` | Reads its own size |
| A byte array | `DiffSource.ofBytes(bytes)` | |
| A stream | `DiffSource.ofStream(in, size)` | **Pass the size whenever you know it**, see below |
| A stream of unknown length | `DiffSource.ofStream(in)` | |
| A `String` | `DiffSource.ofString(text)` | Wrapped, not copied |
| A `Reader` | `DiffSource.ofReader(in)` or `ofReader(in, size)` | Closed by you |
| A `CLOB` or `NVARCHAR` column | `DiffSource.ofClob(clob)` | Opens and closes its own character stream, does not free the `Clob` |
| A JDBC `ResultSet` | `DiffSource.ofResultSet(rs, header)` | Data rows only, header supplied by you |
| Any row iterator | `DiffSource.ofRows(header, iterator)` | A queue, a Parquet reader, a `List` |
| A JSON array of objects | `DiffSource.ofJsonRows(header, reader\|stream\|clob)` | |
| Data with no header row | `DiffSource.withHeader(header, source)` | Writes the header rows in front |

### Size hints

The engine's buffers double as they grow and the arena does not reclaim the
abandoned copies. On the 15 MB per side reference pair the whole comparison reserves
**86 MB with a size hint against 181 MB without one**, so on a batch worker the hint
roughly doubles how many concurrent diffs fit.

A multipart part, an HTTP `Content-Length` and an S3 `ContentLength` all know the
size. Pass it. It is a hint and never a limit: it does not truncate the read and is
not checked against what arrives, so a stale figure costs a little performance and
nothing else. The enforced ceiling is `DiffOptions.maxBytes`.

### Character data

Never reach for `clob.getAsciiStream()`, which replaces every non ASCII character
with a question mark: two sides corrupted the same way compare equal and the report
says clean. `ofClob` reads through `getCharacterStream` and encodes UTF-8 a chunk at
a time straight into the staging buffer, so no intermediate array of the whole text
exists. The alternative that is correct,
`ofBytes(rs.getString(n).getBytes(UTF_8))`, holds the text three times at peak.

---

## 5. Headers, and who declares them

The engine's model is up to four header rows above the data: `KEY` markers,
`REQUIRED` markers, declared types, and column names. A file carries them inside its
own bytes. A result set, a row iterator and a JSON array do not, so **you** supply a
`Header`.

```java
Header header = Header.builder()
    .column("account_id", "VARCHAR(20)").key().required()
    .column("period",     "CHAR(7)").key().required()
    .column("premium",    "DECIMAL(12,2)")
    .column("status",     "VARCHAR(10)")
    .build();
```

Three ways to build one, all producing the same rows:

- `Header.builder()`, above. `.key()` and `.required()` apply to the column just
  added; `.keys("a", "b")` marks several at once
- `Header.ofRows(List<List<String>>)` when you already hold the four rows
- `Header.namesOnly(List<String>)` for a **target** side, which inherits keys,
  required flags and types from the source

`Header.of(Column...)` and `Column.of(name, type).asKey().asRequired()` are there
when you are assembling columns programmatically.

### The header row count contract

**A row source states its own header row count** through `DiffSource.header()`, and
that is what its side is parsed with. `DiffOptions.headerRows` is for a **byte**
source, whose header is inside bytes that nothing outside them can measure.

Setting both to different numbers is **rejected** with a message naming the two,
rather than resolved in favour of either. That is deliberate. The unresolved version
of this ate a name row plus the first three data rows, reported the fourth data row
as the column names, and said `identical=true` with no error at all.

The target side's count is always auto detected against the source, because the
source is authoritative and an uploader may have kept only the column name row.

---

## 6. Options

`DiffOptions.defaults()` is a sensible reconciliation configuration. Build a variant
only for what you actually need to change.

```java
DiffOptions options = DiffOptions.builder()
    .delimiter(';')
    .headerRows(1)              // a names-only source file
    .trimWhitespace(true)
    .numeric(true)
    .detectMoves(true)
    .maxBytes(200L * 1024 * 1024)
    .build();
```

**Dialect:** `delimiter` (`,`), `quote` (`"`), `stripBom` (on).

**Header:** `headerRows` (4; also accepts 1 or 0), and `keyRow`, `requiredRow`,
`typeRow`, `nameRow` when your file orders the header rows differently.

**Comparison:** `trimWhitespace` (on), `charIgnorePad` (on, so `CHAR(n)` padding does
not count as a difference), `numeric` (on, so `1.50` equals `1.5` in a `DECIMAL`
column), `booleans` (on) with `booleanWords(trueWords, falseWords)` to extend the
truth sets, `allowAddedColumns` and `allowRemovedColumns` (both off, so a schema
change is refused rather than quietly ignored).

**Matching:** `detectMoves` (on), `sourceOrdered` (on), `deletedRowPlacement`
(`ANCHORED` or `END`), `similarityPercent` and `similarityCandidates` for pairing
keyless rows, `requireKey` (off) to fail rather than fall back to all-keys matching.

**Validation and counting:** `validate` (on, the findings of section 3),
`countSuppressed` (on, counting cells equal only because normalization suppressed a
difference).

**Limits:** `maxBytes`, `maxRows`, `maxColumns`, all 0 for unlimited, and
`stagingBytes` (1 MB) for the row feed buffer.

---

## 7. Reading the result

### The summary

Constant memory whatever the size of the diff, and produced by the same emitter that
writes the summary report, so the two cannot disagree.

```java
DiffSummary s = d.summary();
s.identical();                      // nothing to report at all
s.rows();                           // unchanged, modified, added, deleted, moved, report
s.cells();                          // changed, suppressed
s.findings();                       // total, rows, requiredEmpty, tooLong, notNumeric, precision
s.matching();                       // allKeys, pairedBySimilarity, pairingTruncated
s.columns();                        // compared, added, removed
s.schemaFindings();                 // columnAdded, columnRemoved, metadataDisagreement
s.json();                           // the emitter's own bytes, for forwarding verbatim
```

`s.json()` is there so a caller who wants to put the summary on a queue or in an
email does not re-encode a record back into JSON.

### The rows

`rows()` is a lazy `Stream<DiffRow>` over engine memory. It is the interface whose
memory does not grow with the diff, and it is the one to use by default.

```java
RowOptions read = RowOptions.defaults()      // values on, all rows, whole values, 1000 per batch
        .withChangesOnly(true)               // skip rows that are unchanged, unmoved, finding free
        .withIncludeValues(false)            // the edit script alone, which costs almost nothing
        .withMaxCellBytes(4096);             // truncate long values at a UTF-8 boundary

try (Stream<DiffRow> rows = d.rows(read)) {
    rows.filter(r -> r.kind() == ChangeKind.MODIFIED).forEach(this::apply);
}
```

`RowOptions.changes()` is the common case: changed rows, with values.

Turning `includeValues` off is worth knowing about. A pass/fail check or a row count
does not need cell text, and decoding 1.7 million cells into Java strings is the
single largest cost in the reference measurements: 0.023 seconds without values
against 0.064 with them.

A `DiffRow` carries `kind()` (`UNCHANGED`, `MODIFIED`, `ADDED`, `DELETED`),
`moved()` and `moveDistance()`, `sourceRow()` and `targetRow()` as `OptionalInt`
1 based record numbers, `changedCells()`, `suppressedCells()`, `cells()`,
`findings()` and `key()`. `isQuiet()` is the exact rule `changesOnly` applies, if
you need to filter yourself.

A `DiffCell` carries `column()`, `name()`, `changed()`, `suppressed()`, `source()`,
`target()`, `truncated()` and `invalidUtf8()`.

Row numbers are **record** based, not line based: a file with blank lines or
multiline quoted fields will not agree with a text editor's line numbers.

### Intra cell highlighting

```java
List<TextSegment> segs = d.cellSegments(row, columnIndex, CellDiffMode.WORD_THEN_CHARACTER);
```

Modes are `NONE`, `WORD`, `CHARACTER` and `WORD_THEN_CHARACTER`. Each segment is an
op with a `start` and `len` in **bytes** into the logical, unescaped value, so slice
on byte offsets and not on `String` indexes. `EQUAL` and `DELETE` index the source
value, `INSERT` indexes the target value; `TextSegment.applyTo(byte[])` does that
slicing for you.

---

## 8. Writing a report

```java
byte[] jsonl = d.emit(EmitFormat.JSONL, EmitOptions.changes());

try (OutputStream out = Files.newOutputStream(Path.of("report.jsonl"))) {
    long written = d.emitTo(out, EmitFormat.JSONL, EmitOptions.changes());
}
```

Formats: `JSONL` (one row per line, the same field set the streamed rows carry),
`CSV`, `HTML` (a table with the class names the browser view uses), `SUMMARY` (the
JSON object behind `DiffSummary`).

`EmitOptions.defaults()` gives values on, all rows, no intra cell diff, the CSV
formula guard on. `withChangesOnly`, `withCellDiff`, `withMaxRows`, `withMaxCellBytes`,
`withCsvDelimiter` and `withClassPrefix` shape it.

Two things worth knowing:

- **The CSV formula guard is on by default and should stay on.** A cell starting
  with `=`, `+`, `-`, `@`, a tab or a CR is a formula to Excel, so a diff report of
  untrusted data is a script delivery mechanism without it. Guarded values get a
  leading single quote, Excel's own text marker.
- **`emitTo` bounds what Java holds, not what the engine does.** The engine produces
  the whole report in one call and this hands it out in pieces. For a full 90 MB
  report, stream `rows()` and write it yourself, or accept the peak. A resumable
  emitter belongs in the engine and is not built.

The HTML emitter is for bounded output: `changesOnly`, or `maxRows`, or a page at a
time. A 90,000 row diff as one HTML string is tens of megabytes of DOM.

---

## 9. Recipes

### Two files

```java
try (Diff d = CsvDiff.compare(Path.of("a.csv"), Path.of("b.csv"))) { ... }
```

### An upload against a database original

```java
try (Diff d = CsvDiff.compare(
        DiffSource.ofClob(rs.getClob("original_csv")),
        DiffSource.ofStream(uploadedPart.getInputStream(), uploadedPart.getSize()))) {
    ...
}
```

### Two result sets

```java
Header header = Header.builder()
    .column("account_id", "VARCHAR(20)").key().required()
    .column("premium", "DECIMAL(12,2)")
    .build();

try (Diff d = CsvDiff.compare(
        DiffSource.ofResultSet(sourceRs, header),
        DiffSource.ofResultSet(targetRs, Header.namesOnly(header.names())))) {
    ...
}
```

The binding reads exactly one thing from the driver, `getColumnCount()`, and only to
refuse a header whose width disagrees with the query before parsing a row. Everything
else about the schema comes from your `Header`.

Values are rendered by explicit rules rather than by `rs.getString`, because drivers
disagree about exactly the types a reconciliation cares about:

```
SQL NULL      -> an empty field          BOOLEAN       -> TRUE / FALSE
BigDecimal    -> toPlainString()         DATE          -> 2026-01-31
double, float -> plain decimal           TIME          -> 14:22:05
CLOB          -> its text                TIMESTAMP     -> 2026-01-31T14:22:05
BLOB, byte[]  -> refused
```

**NULL and the empty string are the same field**, because CSV cannot express the
difference and a file side could never agree with a JDBC side that made it. If your
CSV exports write a sentinel such as `\N`, that is a mismatch to know about now
rather than to discover from a report where every NULL cell is a change.

### Data in a column, header in a metadata table

```java
try (Diff d = CsvDiff.compare(
        DiffSource.withHeader(headerFromMetadata, DiffSource.ofClob(original)),
        DiffSource.ofBytes(uploaded))) {
    ...
}
```

`withHeader` refuses to wrap a source that already declares a header. One trap: a BOM
inside the wrapped source is no longer at offset 0 once the header sits in front of
it, so it becomes part of the first field. Strip it before wrapping, or read that
side through `ofReader`, where it is one character rather than three bytes.

### A JSON array of objects

```java
try (Diff d = CsvDiff.compare(
        DiffSource.ofJsonRows(header, originalJsonClob),
        DiffSource.ofJsonRows(header, uploadedJson))) {
    ...
}
```

Reads `[{"id":"1","name":"a"}, ...]` one row at a time. The `header` decides column
order, because JSON objects are unordered and two rows of one export can list their
keys differently.

**An array of objects, and only that.** An array of arrays fails with a message
naming the shape this reads. Four things are errors, each naming the row and the
document position: a declared key the object does not carry, a key the header does
not declare, the same key twice, and a nested object or array as a value.

Values render through the same code the JDBC path uses, so JSON `null` is an empty
field and `true` is `TRUE`. **A number keeps its literal text** and never passes
through a `double`, so `1.50` stays `1.50` and a 23 digit integer survives.

On 150,000 rows and 16 MB, `ofJsonRows` completes both sides and the whole diff in a
16 MB heap. The same array read into a `Map`/`List` tree needs 256 MB for one side
with no diff performed.

---

## 10. Errors

Everything the binding refuses throws `CsvDiffException`, a `RuntimeException`.

```java
try (Diff d = CsvDiff.compare(source, target)) {
    ...
} catch (CsvDiffException e) {
    log.warn("diff refused: {} ({})", e.getMessage(), e.status());
}
```

`status()` is the engine's error name, for example `IBHA_CSVD_ERR_RAGGED_ROW` or
`IBHA_CSVD_ERR_DUPLICATE_KEY`, and `CsvDiffException.NOT_ENGINE` (`"CALLER"`) when
the binding itself refused the call. **Switch on `status()`, not on the message
text**, which names user data and is not a stable contract.

The engine holds one error and the first one wins, so a comparison fails at its first
problem rather than accumulating a list. Validation findings are the opposite: they
are output and never abort.

---

## 11. Runtime notes

**Always close the `Diff`.** It owns native memory that the garbage collector does
not manage. Use try-with-resources. `bytesReserved()` tells you what one comparison
holds, which is how a batch worker sizes its concurrency.

**Threading.** One `Diff` is not thread safe; do not stream its rows from two
threads. Separate `Diff` instances are independent and can run concurrently.

**Backend selection.** `Diff` is an interface and the backend is discovered with
`ServiceLoader`. The JNI backend ships today; an FFM backend for JDK 22+ can be added
later as a separate artifact and will win the selection wherever it can run, with no
change visible here. `-Dibha.csvdiff.provider=jni` pins the choice.

**The native library.** Extracted from the jar to a temporary file on first use.
`-Dibha.csvdiff.nativeLibPath=<dir>` loads from a directory instead, which is what
the build does with a library it has just compiled. A read only or `noexec` temporary
directory is the one deployment that needs this flag set deliberately.

**Memory in one line:** hint your sizes, leave `includeValues` off when you do not
need cell text, prefer `rows()` over `emit()` for large reports, and close the handle.
