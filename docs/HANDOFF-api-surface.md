# Handoff: the ingestion API against the client use cases

Date: 2026-08-15
Commits: `0d06053`, `64d90d6`, `c2ae454`, `cab38b6`, plus this session's, on `main`,
tree clean
Status: **the track is complete.** All five use cases in `specs/input-options.md` are
delivered, the whole gate passes, and the C core and the ABI were never touched.

**This is not a phase.** It was a track that opened when `specs/input-options.md`
arrived and competed with Phase 7 for what happened next. That competition is now
resolved by the track having finished: **Phase 7 is what comes next**, and its prompt
is in section 8.

---

## 1. Read these first

| File | Why |
|---|---|
| `specs/input-options.md` | The five client use cases, in the order they were delivered |
| `docs/api-surface-vs-input-options.md` | **The assessment.** Verdict per use case, the gaps, and sections 3 through 3d, which are the record of the five things built |
| `java/README.md` | The three new sections: character data, a header the data does not carry, and a table held as a JSON array |
| `docs/HANDOFF-phase6.md` section 11 | The Phase 7 prompt, still valid, see section 8 |

## 2. What was asked, and what it came to

The question was whether the public API covers the five ways a client will feed data
to the engine. It did not; it does now.

| # | Use case | Side | Verdict |
|---|---|---|---|
| 1 | React SPA: upload plus a callback fetching the server side original | JS | **Works**, and always did |
| 2 | Java: upload against a CLOB / NVARCHAR original, header supplied separately | Java | **Works**, sections 3.3 and 3.4 |
| 3 | Java: both sides from a column, CSV text or JSON arrays | Java | **Works**, CSV text through 3.3 and 3.4, JSON through 3.5 |
| 4 | Java: two file paths | Java | **Works.** S3 is the caller's fetch, by design |
| 5 | Java: ad-hoc `SELECT` against any of the first four | Java | **Works**, and the defect that was in its way is fixed |

Five items were built: a defect fix, a size hint, the character sources,
`withHeader`, and the JSON row source. Every one of them ends as bytes in the same
staging buffer feeding the same parser, which is what keeps spec section 9's promise
that the browser and the server cannot disagree.

## 3. What is done and verified

### 3.1 The defect: a row source states its own header row count

`JniDiff.parseSide` took the source side's header row count from `DiffOptions`
alone, so a `Header.namesOnly` row source parsed under the default of four ate its
name row plus the first three data rows. The fourth data row became the column
names, three of five rows vanished, and the summary said `identical=true`. No error
and no finding, because four header rows is an ordinary thing to ask for. Every test
used `namesOnly` on the target side, where the count is auto-detected, which is why
the gate stayed green.

A source declares what it writes through `DiffSource.header()`, and
`DiffOptions.headerLayout(Header)` reconciles that with the options: a declared
header beats the default and **conflicts with a stated `headerRows` rather than
overriding it**. `HeaderLayout` is the five numbers a parse opens with and is the one
place the row positions are derived. The rule still matches `engine.ts` line 434
exactly.

Everything built after this inherits that contract rather than reopening it, which is
why the two sources that write header rows came in small.

### 3.2 `DiffSource.ofStream(InputStream, long size)`

The unhinted overload delegates with 0. Negative rejected, 0 still unknown.

**The claim it exists for was measured and is smaller than the javadoc said.**
Through `Diff.bytesReserved()` on the p90 pair it is 86,250,336 hinted against
181,378,464 unhinted, **a factor of 2.1**, not the 4x claimed. Corrected in the
javadoc rather than dropped. See section 6.1 for the loose end this leaves.

### 3.3 The character sources

`DiffSource.ofString(CharSequence)`, `ofReader(Reader)`, `ofReader(Reader, long)`,
`ofClob(Clob)`, over one package private `Utf8Encoder`.

A CLOB is character data and every factory before this was byte oriented, so a
caller had `clob.getAsciiStream()`, which corrupts every non-ASCII byte, or
`rs.getString(n).getBytes(UTF_8)`, which is correct and holds the text three times at
peak. These encode 8,192 chars at a time straight into the direct staging buffer, and
`ofString` wraps rather than copies, so nothing is materialized.

**The surrogate pair is the whole difficulty and it is where the tests are.** A
character above U+FFFF is two Java chars whose four UTF-8 bytes cannot come from
either half, so a read ending between them holds the first back through
`CharBuffer.compact`. `CharacterSourceTest` sweeps the pair across the read boundary
and separately feeds it through a reader returning one char per call, which splits
every pair in the text at once. Bytes are asserted against `getBytes(UTF_8)`
directly, not through a report, because a character corrupted the same way on both
sides compares equal to itself. An unpaired surrogate is replaced rather than
refused, because that is what `String.getBytes` and `CsvWriter` do.

**Measured:** on the p90 pair `ofString` reserves 86,250,336 bytes, which is what
`ofBytes` reserves, to the byte, because it computes its exact encoded length in one
pass with nothing allocated. `ofClob` hints with `Clob.length()`, which is
characters: exact for ASCII, an undercount otherwise, and that is the right
direction.

Lifecycle, tested both ways: `ofClob` opens the character stream so it closes it, and
does not free the caller's `Clob`; `ofReader` closes nothing.

### 3.4 `DiffSource.withHeader(Header, DiffSource)`

Writes the header rows ahead of another source's bytes, for a caller whose CLOB holds
headerless data, or a name row only while the key and type information lives in a
metadata table. There is a `RowFeedOptions` overload for a non-default dialect.

It returns its `Header` from `header()`, so the rows written are the rows parsed, and
a conflicting `headerRows` is rejected by the section 3.1 rule. The header goes
through `CsvWriter`, so a column name containing the delimiter is quoted rather than
silently becoming two columns.

Refused, both tested: wrapping a source that already declares a header, which would
write two and parse one; and a null header or body.

**One caveat that is documented and not fixed:** a BOM inside the wrapped source is
no longer at offset 0 once the header is in front of it, so it stops being a BOM and
becomes part of the first field. Strip it before wrapping, or read that side through
`ofReader`, where it is one character rather than three bytes.

### 3.5 The JSON row source

`DiffSource.ofJsonRows(Header, Reader)`, with `InputStream` and `Clob` overloads and
one for `RowFeedOptions`, over a package private `JsonRows` that implements
`Iterator<List<String>>` and feeds the existing `ofRows` path, inheriting the batch
feed, the escaping and the width check.

The three questions that blocked it were answered by Manas and implemented as
answered:

1. **Array of objects only.** Array of arrays was not built. An array of arrays fails
   with a message naming the shape this reads, not a parse error about a bracket
2. **A missing key is an error**, not an empty cell
3. **JSON `null` is an empty field and a number keeps its literal text**, through one
   shared renderer

**One decision was taken by symmetry rather than asked, and is the one thing here
worth revisiting if a real export disagrees: a key the header does not declare is
also an error.** It is the same disagreement in the other direction, it is the JSON
form of an added column, and the CSV path refuses that by default. Relaxing it to
"ignored" is a one line change.

The shared renderer is literal: `JsonRows` produces the types `SqlValues` already
renders, a `String` for a string or a number, a `Boolean`, or null, and calls
`SqlValues.render`. There is no second rendering rule in the JSON path, and
`JsonRowsTest` compares a JSON side against a **JDBC** side of the same rows,
including a null and a boolean, so the day the two stop agreeing a test fails. A
number keeps the text it was written with because it is never parsed, tested against
a `VARCHAR` column deliberately and not a `DECIMAL` one: under `DECIMAL` the engine
compares by value and `1.50` equals `1.5`, so that test would prove nothing.

**The streaming claim was measured**, through
`python3 scripts-and-commands/measure_json_rows.py`, on 150,000 rows and 16.0 MB:

| | smallest heap that completes | time |
|---|---|---|
| `ofJsonRows`, both sides, whole diff | **16 MB** | 379 ms |
| the same array as a `Map`/`List` tree, one side, no diff | **256 MB** | 115 ms |

The tree does not miss 128 MB narrowly. It fails there, on one side, with no
comparison performed, which is exactly what `Json.java` was ruled out for.

### 3.6 The state of the gate

```
java binding suite    Tests run: 132  (61 at the end of Phase 6, 80 at the start of this session)
determinism           all 240 comparisons byte identical across native, wasm, wasm+simd and java
gate passed           26 checks: the C gate, both JS gates and the Java gate
```

`python3 scripts-and-commands/run_phase6_checks.py --require-java` runs all of it.
`--java-only` is about twenty seconds.

**The C core and the ABI are untouched.** Three phases and this whole track without
an ABI change.

## 4. What is next

Nothing in this track. Phase 7, the SIMD parser, is the next piece of work, and its
prompt is in section 8.

## 5. The decisions that were made along the way

Kept because the reasoning is the part that will be questioned later, not the code.

1. **Array of objects only**, decided by Manas. Array of arrays is what a compact
   export holds, and the deployment does not hold one. A shape nobody has is a parser
   nobody tests.
2. **A missing key is an error**, decided by Manas. A key absent from one object and
   present in the next usually means the header and the data disagree, and an empty
   cell would report as a changed value against a file that has the real one.
3. **JSON scalars render through `SqlValues`**, confirmed by Manas. `null` is an
   empty field, matching flagged assumption 4 in `docs/HANDOFF-phase6.md` section 8,
   and a number renders by its literal text, never through a `double`.
4. **A key the header does not declare is an error**, decided by symmetry with 2 and
   not asked. See section 3.5.
5. **An unpaired surrogate is replaced, not refused**, decided by consistency with
   `String.getBytes` and `CsvWriter`. See section 3.3.

## 6. Loose ends

1. **The 4x claim still stands in the JavaScript binding**, in
   `js/packages/core/src/source.ts` and `engine.ts`. It is a different allocator on a
   growing wasm memory and was not measured here, so correcting it from a native
   number would be the same mistake in the other direction. Whoever measures the wasm
   side should close it out. Small, and it needs `bytesReserved` on the JS handle,
   which already exists.

2. **The JS mirrors are deliberately not built.** `rowsSource(header, rows)`,
   `withHeader(header, source)` and a JSON row source would match the Java additions
   and are pure TypeScript over the existing `ByteSource`, needing no wasm change.
   Nothing in the five use cases asks for them: use case 1 is the only JS one and it
   is complete. If a browser is never asked to diff a JSON array, they should not be
   written.

3. **`StreamSource` still copies through a 64 KB heap array.** Reading straight into
   the direct buffer with `Channels.newChannel(in)` was looked at and dropped: that
   wrapper uses an internal heap array for a direct destination anyway. The
   *character* sources do not have this problem, since the encoder writes into the
   direct buffer itself.

4. **A BOM inside a `withHeader` body is not stripped**, per section 3.4. Documented
   rather than handled, because stripping it here would mean stripping it twice for a
   source that is at offset 0 after all.

5. **The five flagged assumptions of `docs/HANDOFF-phase6.md` section 8 stand**, and
   two of them, SQL NULL as an empty field and the timestamp text format, are now
   load bearing on two paths rather than one, since the JSON source renders through
   the same code. That is an argument for the shared renderer, which is what was
   built, and not for re-deciding either rule.

## 7. If a sixth ingestion shape turns up

The pattern to follow, in order:

1. If it produces **bytes**, implement `DiffSource` directly; `feed` plus
   `sizeHint` is a dozen lines and gets the streaming behaviour of everything here.
2. If it produces **characters**, wrap `Utf8Encoder`, which is the whole of
   `ofReader`.
3. If it produces **rows**, build an `Iterator<List<String>>` and hand it to
   `ofRows`, which is the whole of `ofJsonRows`. Render values through `SqlValues` or
   the sides will not compare equal.
4. If the data carries **no header**, do not concatenate one. Wrap with
   `withHeader`, or return the `Header` from `header()` if it is a row source.

None of these needs a C change, and none of them should add a second set of diff
semantics.

## 8. The prompt for the next conversation

Phase 7, the SIMD parser. Use the prompt in `docs/HANDOFF-phase6.md` section 11 as
written, with two amendments:

- the Java suite is **132 tests**, not 61
- also read `docs/HANDOFF-api-surface.md` section 3, because `HeaderLayout`,
  `DiffSource.header()`, the character sources, `withHeader` and `ofJsonRows` are all
  new public API since that prompt was written, and `docs/HANDOFF-phase6.md`
  section 4.3's list of backend surface members is out of date without it

Nothing in this track touched the C core, so the rest of that prompt is accurate. The
shared surface between the two is the determinism gate, which Phase 7 must keep
passing on all 240 comparisons.
