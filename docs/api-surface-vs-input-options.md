# The public API against the five ingestion use cases

Date: 2026-08-14
Source: `specs/input-options.md`
Assessed against: the Phase 6 tree (Java binding complete, JS binding complete)

---

## 1. Verdict per use case

| # | Use case | Side | Verdict |
|---|---|---|---|
| 1 | React SPA: upload plus a callback that fetches the server side original | JS | **Fully possible today.** No API change |
| 2 | Java: uploaded CSV against an original in a CLOB / NVARCHAR(max), header supplied separately | Java | **Fully possible as of 2026-08-15**, sections 3b and 3c |
| 3 | Java: both sides from CLOB columns, either CSV text or JSON arrays | Java | **Fully possible as of 2026-08-15**, CSV text through sections 3b and 3c, JSON through section 3d |
| 4 | Java: two file paths, headers inside the files | Java | **Fully possible today** for local and SAN paths. S3 is the caller's fetch by design |
| 5 | Java: ad-hoc `SELECT` against any of the first four | Java | **Possible**, and the best tested path. The defect in section 3 was in its way and is now fixed |

The ordering in `specs/input-options.md` is the delivery order, so the work splits
cleanly: use cases 1 and 4 need nothing, use case 2 needs three small additions plus
one bug fix, use case 3 needs one larger addition, use case 5 needs the bug fix.

**As of 2026-08-15 all five use cases are delivered.** The bug fix is section 3, the
size hint section 3a, the character sources section 3b, `withHeader` section 3c and
the JSON row source section 3d. Nothing in section 4 is outstanding on the Java side;
what remains is the JavaScript symmetry below, which nothing in the five use cases
asks for.

---

## 2. Use case by use case

### 2.1 Use case 1, the React SPA. Nothing missing

`IbhaCsvSource` already accepts a `File`/`Blob` from an upload input and a
`() => Promise<Response>` callback for the server side original, which is exactly
the shape described. The callback form is the deliberate one: per spec 13.7 rule 1
the library never performs the fetch, so authentication, cookies, base URLs and
retries stay with the application.

```ts
const diff = await compare(
  () => fetch(`/api/statements/${id}/original.csv`, { headers: auth }),
  uploadedFile,
);
```

Everything around it is present too: streaming so the parse overlaps the download,
the HTML/JSON error page sniff for an expired session returning a login page with a
200, `decompress` for a `.csv.gz` S3 object, `maxBytes`, and `compareInWorker` to
keep it off the main thread.

**One thing to get right, not an API gap:** the source argument is
schema-authoritative per spec 13.8. The server side original goes first, the upload
second. Reversed, the diff silently reconciles against the salesman's opinion of the
schema.

### 2.2 Use case 2, upload against a CLOB. Three gaps

The uploaded half is covered by `DiffSource.ofStream(part.getInputStream())`.

**Gap A: there is no character source. Closed 2026-08-15, see section 3b.** A CLOB is character data and every
`DiffSource` factory is byte oriented: `ofBytes`, `ofFile`, `ofStream`. The two
workarounds both cost something real.

- `DiffSource.ofStream(clob.getAsciiStream())` corrupts every non-ASCII byte. A
  customer name with an accent becomes mojibake and reports as a changed cell.
- `DiffSource.ofBytes(rs.getString(n).getBytes(UTF_8))` is correct but holds the
  text three times at peak: the driver's `String` (2 bytes per char), the encoded
  `byte[]`, and the engine's arena copy. For a 15 MB CLOB that is about 60 MB of
  transient heap on a server sized for concurrent diffs.

Note that `SqlValues` already reads a `Clob` correctly for a *cell value*, so the
rendering rules exist; what is missing is a whole-side source.

**Gap B: a header cannot be attached to a byte source. Closed 2026-08-15, see
section 3c.** `Header` is only reachable
through `ofResultSet` and `ofRows`. Use case 2 says the header is prepared
separately, which means the CLOB may hold headerless data, or hold a name row only
while the caller holds the key and type information elsewhere. Today there is no way
to say so. The caller has to build the header CSV text by hand and concatenate it,
which is the drift `Header.toRows()` was made public to prevent.

**Gap C: `ofStream` cannot carry a size hint. Closed 2026-08-14, see section 3a.**
A multipart part and a `Content-Length` both know their size, and the interface
supports a hint; the factory had no way to pass one, so every stream fed side paid
for growing the arrays it could have sized once.

### 2.3 Use case 3, both sides from a column, including JSON. One large gap

The CSV-text half is Gaps A and B again, twice, and is closed with them: both sides
are `withHeader(header, ofClob(clob))`, or `ofClob` alone where the column carries
its own header rows.

**Gap D: JSON arrays are not supported anywhere in either binding. Closed on the
Java side 2026-08-15, see section 3d.** Neither
`DiffSource` nor `IbhaCsvSource` accepts a JSON array of rows or of objects, and
there is no design note deferring it, so this is genuinely unbuilt rather than
deliberately omitted.

`Json.java` does not help here. It parses a whole document into `Map`/`List` objects
and exists to read a summary the library produced itself; running a 15 MB row array
through it would allocate millions of objects, which is precisely the heap behaviour
the engine exists to avoid. A JSON row source needs a streaming reader that pulls one
row at a time into the existing `CsvWriter` path.

The good news is that the destination already exists. `DiffSource.ofRows(Header,
Iterator<List<String>>)` is exactly the right target, so a JSON row source is a
streaming parser plus an `Iterator`, and it inherits the batch feed, the escaping and
the width check for free. Two shapes are worth supporting, and they need different
handling:

- **array of arrays** `[["1","a"],["2","b"]]`, positional, maps straight to the
  header's column order.
- **array of objects** `[{"id":"1","name":"a"}]`, which needs the `Header`'s names to
  project each object into column order, and needs a decision on a missing key
  (empty cell) versus an unknown key (error, because it usually means the header and
  the data disagree).

JSON scalars also need a rendering rule, and it must be the same rule `SqlValues`
uses or a JSON side and a JDBC side of the same data will not compare equal. In
particular JSON `null` should render as an empty field, matching flagged assumption
4 in the Phase 6 handoff, and a JSON number should render by its literal text rather
than through a `double`, because `1.50` must survive as `1.50` for the DECIMAL
comparison to mean anything.

### 2.4 Use case 4, two file paths. Nothing missing

`CsvDiff.compare(Path, Path)` is the two argument convenience overload, reading
through a `FileChannel` into a direct buffer so the bytes never touch the Java heap,
with `Files.size` supplying the hint automatically. A SAN path is just a path.

An S3 path is not a `Path`, and the library will not grow an S3 client: credentials,
regions, endpoints and retry policy are deployment concerns, which is the same rule
spec 13.7 already locked for the browser. The caller writes
`DiffSource.ofStream(s3.getObject(req))`, and once Gap C is closed they pass the
`ContentLength` alongside it and get the memory profile of the local file path.

### 2.5 Use case 5, ad-hoc SELECT against anything. Possible, with a caveat

`DiffSource.ofResultSet(rs, header)` is the most thoroughly tested path in the
binding, and mixing it with any other source works by construction: rows are encoded
as RFC 4180 into the staging buffer, so a result set and a file reach the same
parser. `aResultSetDiffsTheSameAsTheEquivalentCsv` is the test that says so.

Two things a caller must know, neither of them an API gap:

1. Put the `SELECT` on the **source** side if it is the authority for keys and types,
   because only the source side's header is read for them.
2. `ORDER BY` the key columns, or set `sourceOrdered(false)`. The option exists and
   its javadoc names this exact case. Without either, move detection reports noise
   against a file that has a meaningful row order.

Use case 5 is where the defect below was most likely to be hit, because the SELECT
side is where a caller reaches for `Header`. It is fixed, and a `SELECT` fed through
`Header.namesOnly` now parses as the one header row it writes.

---

## 3. A defect found while assessing this. Fixed 2026-08-14

**A `Header` on the source side of a row feed was ignored in favour of
`DiffOptions.headerRows()`, and the mismatch was silent.**

`JniDiff.parseSide` takes the source side's header row count from the options alone:

```java
int headerRows = isTarget ? HEADER_AUTO : options.headerRows();
```

The target side is auto-detected per spec 13.8, so it is fine. The source side is
not. A `Header.namesOnly(...)` row source writes one header row while the options
still say four, so the engine consumes the name row plus three data rows as a header.

Verified, comparing a five row feed against itself with a names-only header:

```
columns = [3, c]                       // the fourth data row became the column names
rows    = unchanged 2                  // three of five rows silently absorbed
```

No error, no finding, `identical=true`. The column names came out as a data row and
40% of the data disappeared. Every existing test used `namesOnly` on the target side
only, which is why the gate was green.

### The fix

A source now declares what header it writes, and that is what its side is parsed
with.

| Change | File |
|---|---|
| `DiffSource.header()`, empty by default, returning the `Header` for the two row sources | `DiffSource.java` |
| `HeaderLayout`, the five resolved numbers a parse opens with, and one copy of the derivation rule | `HeaderLayout.java` (new) |
| `DiffOptions.headerLayout()` and `headerLayout(Header)`, which reconcile the two | `DiffOptions.java` |
| `DiffOptions.headerRowsStated()`, so a stated 4 is distinguishable from the default 4 | `DiffOptions.java` |
| `parseSide` asks for the layout instead of reading `headerRows` directly | `jni/JniDiff.java` |

The policy: a declared header beats the default, and **conflicts with a stated
`headerRows` rather than overriding it**. A caller who wrote both down had one of the
two in mind, and guessing which is how this comes back.

```
the source writes 1 header row but headerRows(4) was set on the options. Drop one of
the two: a row source states its header rows through its Header, and headerRows is
for a byte source whose header is inside its own bytes
```

The derivation of the four row positions is unchanged and still matches
`engine.ts` line 434 exactly, so the two bindings cannot disagree about a file.

Ten assertions in `SourceHeaderLayoutTest` hold it: the regression itself, a
names-only row source checked cell for cell against the equivalent CSV bytes, the
four row and byte source paths unchanged, the conflict rejected, the agreement
accepted, and the layout rule as a unit. The Java suite is 71 tests, up from 61, and
the determinism gate is still 240 byte identical comparisons across native, both
wasm builds and Java.

Gap B in section 2.2 now has its contract already built: a `withHeader(Header,
DiffSource)` returns the header from `header()` and the row count follows.

---

## 3a. `ofStream` carries a size hint. Done 2026-08-14

`DiffSource.ofStream(InputStream, long size)`, alongside the unhinted overload,
which now delegates to it with 0. A negative size is rejected; 0 still means
unknown. Nine assertions in `StreamSourceTest`, which is also the first coverage
`ofStream` has had at all.

**The claim was measured and it is smaller than the javadoc said.** Through
`Diff.bytesReserved()` on the p90 pair:

| | bytes reserved |
|---|---|
| both sides hinted | 86,250,336 |
| both sides unhinted | 181,378,464 |
| ratio | **2.10** |

The interface's own javadoc claimed peak retention of "roughly 4x N rather than 1x".
The measured figure is a factor of 2.1 on this pair, so the claim was corrected in
`DiffSource.sizeHint()` and in the new overload rather than the measurement quietly
dropped, in the same spirit as the batch drain correction in handoff section 5.3.
The unmeasured 4x wording survives in the JavaScript binding, which is a different
allocator on a growing wasm memory and was not measured here; correcting it there on
the strength of a native number would just be the same mistake pointing the other
way.

The test asserts the direction and not the ratio. The exact figure depends on how
the arena happens to double for a given pair, and pinning it would fail on a fixture
change for no reason anyone could act on.

Two things checked because they are what makes a hint safe to give: a **wrong** hint,
far too small and far too large, produces a byte for byte identical report, since the
hint sizes a reservation and is never checked against what arrives; and the stream is
**not closed** by the source, which is documented and was untested.

---

## 3b. The character sources. Done 2026-08-15

`DiffSource.ofString(CharSequence)`, `ofReader(Reader)`, `ofReader(Reader, long)` and
`ofClob(Clob)`, over one package private `Utf8Encoder`. Gap A of section 2.2 is
closed: a CLOB no longer has to arrive through `getAsciiStream`, which corrupts
it, or through `getString(n).getBytes(UTF_8)`, which holds the text three times at
peak.

The encode is chunked, 8,192 chars at a time, straight into the direct staging
buffer. Nothing materializes: `ofString` wraps the sequence in a `CharBuffer` view
rather than copying it, and no `byte[]` of the whole text exists at any point.

**The surrogate pair is the difficulty and it is where the tests are.** A character
above U+FFFF is two Java chars whose four UTF-8 bytes cannot be produced from either
half alone, so a read that ends between them has to hold the first back;
`CharBuffer.compact` is what does it. `CharacterSourceTest` sweeps the pair across
the read boundary at five offsets and, separately, feeds it through a reader that
returns one character per call, which splits every pair in the text at once. The
bytes are asserted against `getBytes(UTF_8)` directly rather than through a report,
because a report can agree while the encoding is wrong: a character corrupted the
same way on both sides compares equal to itself.

**An unpaired surrogate is replaced, not refused.** That is what
`String.getBytes(UTF_8)` does and what `CsvWriter` does to a row value, and the same
text has to become the same bytes through every factory or the comparison depends on
which one the caller reached for.

Two things measured rather than asserted:

| | bytes reserved, p90 pair |
|---|---|
| both sides `ofBytes` | 86,250,336 |
| both sides `ofString` | 86,250,336 |

`ofString` computes its exact encoded length in one pass with nothing allocated, so
a text source reserves what the byte source reserves to the byte, which is the
section 3a profile rather than the doubling one. `ofClob` hints with
`Clob.length()`, which is characters: exact for ASCII, an undercount otherwise, and
that is the right direction, since an undercount grows a little while a 4x overcount
would reserve 60 MB for a 15 MB column.

Lifecycle, tested both ways: `ofClob` opens the character stream so `ofClob` closes
it, and does not free the caller's `Clob`; `ofReader` closes nothing, exactly as
`ofStream` leaves an `InputStream` open.

---

## 3c. `withHeader`. Done 2026-08-15

`DiffSource.withHeader(Header, DiffSource)` and the `RowFeedOptions` overload. Gap B
of section 2.2 is closed: a CLOB holding headerless rows, or a name row only while
the keys and types live in a metadata table, now becomes one ordinary CSV without the
caller concatenating strings.

It came in small, as section 3 predicted, because it inherits rather than reopens the
header row count contract: it returns its `Header` from `header()`, so the rows
written and the rows parsed are the same number, and a conflicting `headerRows` is
rejected with the message that contract already produces. The header rows go through
`CsvWriter`, so a column name containing the delimiter is quoted rather than silently
becoming two columns.

Two refusals and one caveat, all tested:

- wrapping a source that **already declares a header** is rejected at construction.
  Two headers would be written and one parsed, and the second would arrive as data
  rows
- a null header or body is rejected where it is given, not where it is read
- **a BOM inside the wrapped source stops being a BOM**, because it is no longer at
  offset 0 once the header sits in front of it. Documented on the method, in
  `java/README.md`, and the reason `ofReader` is the better door for text that might
  carry one

The size hint is the body's plus the header's, and unknown when the body's is:
a hint covering the header alone would be worse than none, since it would size the
reservation to a few hundred bytes and grow from there.

---

## 3d. The JSON row source. Done 2026-08-15

`DiffSource.ofJsonRows(Header, Reader)`, plus `InputStream` and `Clob` overloads and
one for `RowFeedOptions`, over a package private `JsonRows` that implements
`Iterator<List<String>>` and feeds the existing `ofRows` path. Gap D is closed and
the track is complete.

The three decisions of section 5 of the handoff were answered by Manas and
implemented as answered:

1. **Array of objects only.** The array of arrays shape was not built. An array of
   arrays now fails with a message naming the shape this reads, rather than with a
   parse error about a bracket
2. **A missing key is an error**, not an empty cell
3. **JSON `null` is an empty field and a number keeps its literal text**, through one
   shared renderer

One thing was decided by symmetry rather than asked, and is worth confirming: **a key
the header does not declare is also an error**. It is the same disagreement in the
other direction, it is the JSON form of an added column, and the CSV path refuses
that by default. Relaxing it to "ignored" is a one line change if a real export
carries extra fields.

**The shared renderer is literal.** `JsonRows` produces the types `SqlValues` already
renders, a `String` for a string or a number, a `Boolean`, or null, and calls
`SqlValues.render`. There is no second rendering rule anywhere in the JSON path, and
`JsonRowsTest` compares a JSON side against a **JDBC** side of the same rows,
including a null and a boolean, so the day the two stop agreeing a test fails rather
than a customer's report quietly changing.

**A number keeps the text it was written with**, because it is never parsed. Tested
against a CSV holding the same literals as `VARCHAR`, deliberately not as `DECIMAL`:
under `DECIMAL` the engine compares by value and `1.50` equals `1.5`, so the test
would pass whether or not the literal survived and would prove nothing.

The streaming claim was measured rather than asserted, through
`scripts-and-commands/measure_json_rows.py`, on 150,000 rows and 16.0 MB of JSON:

| | smallest heap that completes | time |
|---|---|---|
| `ofJsonRows`, both sides, whole diff | **16 MB** | 379 ms |
| the same array as a `Map`/`List` tree, one side, no diff | **256 MB** | 115 ms |

The tree does not miss 128 MB narrowly. It fails there, on one side, with no
comparison performed, which is the behaviour `Json.java` was ruled out for.

---

## 4. Proposed additions

### Java, in delivery order

| # | Addition | Serves | Size |
|---|---|---|---|
| ~~0~~ | ~~Fix the source side header row count, driven by the source's own `Header`~~ | 2, 5 | **done, section 3** |
| ~~1~~ | ~~`DiffSource.ofStream(InputStream, long sizeHint)`~~ | 2, 4 | **done, section 3a** |
| ~~2~~ | ~~`DiffSource.ofReader(Reader)`, `ofClob(Clob)`, `ofString(CharSequence)`, chunk encoding UTF-8 straight into the staging buffer~~ | 2, 3 | **done, section 3b** |
| ~~3~~ | ~~`DiffSource.withHeader(Header, DiffSource)`, writing the header rows ahead of the bytes~~ | 2, 3 | **done, section 3c** |
| ~~4~~ | ~~`DiffSource.ofJsonRows(Header, Reader \| InputStream, JsonRowShape)` over a streaming reader, feeding the existing `ofRows` path~~ | 3 | **done, section 3d.** No shape argument: array of objects only |

Item 3 was additive and touched no existing behaviour. Item 2 carried one decision
worth stating rather than assuming, and it was implemented as stated: a `Reader`
source encodes in fixed size chunks rather than materializing a `String`, which is
the heap cost it exists to avoid.

The JSON row source of item 4 should also declare its `Header` through `header()`,
so it inherits the section 3 contract rather than reopening it.

None of these touch the C ABI or the engine, and none of them add a second set of
diff semantics: every one of them ends up as bytes in the same staging buffer feeding
the same parser. That is what keeps spec section 9's promise intact.

### JavaScript

Nothing is required for use case 1, which is the only listed use case on that side.

Two are worth doing for symmetry once the Java work lands, and only then:

- `rowsSource(header, rows)`, mirroring `DiffSource.ofRows`, for a SPA that holds a
  JSON array from an API and wants to diff it against an upload without serializing
  CSV by hand.
- `withHeader(header, source)`, mirroring the Java one.

Both are pure TypeScript over the existing `PushSource` and `ByteSource` and need no
change to the wasm module. If a browser is never asked to diff a JSON array, neither
is needed, and the honest answer is to wait for the caller to appear rather than
build them speculatively.

---

## 5. What this does not change

The five flagged assumptions in `docs/HANDOFF-phase6.md` section 8 stand. Two of
them get *more* load-bearing under use case 3, because a JSON row source would need
to render `null` and timestamps by exactly the same rules `SqlValues` uses, or the
JSON and JDBC paths would disagree about the same underlying data. That is an
argument for one shared value renderer rather than for re-deciding either rule.
