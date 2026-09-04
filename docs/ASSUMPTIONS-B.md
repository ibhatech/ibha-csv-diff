# The five flagged assumptions, made answerable

Date: 2026-09-04
Group B of `specs/03-remaining-tasks.md`
Status: **all five answered by Manas 2026-09-04. Four confirmed, one fixed.**

| | Assumption | Answer | Work |
|---|---|---|---|
| B1 | SQL NULL is an empty field | **CONFIRMED as built** | none |
| B2 | Timestamp text format | **REJECTED**, and fixed | C core, done |
| B3 | Asymmetric ragged rows | **CONFIRMED as built**, both halves | none |
| B4 | Record numbers, not line numbers | **CONFIRMED as built**, B4a rejected | none |
| B5 | `VARCHAR(n)` counts characters | **CONFIRMED as built** | none |

Four of the five were right as built. The one that was not, B2, was a defect rather
than a decision, and it is fixed in this same pass.

Run it yourself:

```bash
python3 scripts-and-commands/confirm_assumptions.py          # all five
python3 scripts-and-commands/confirm_assumptions.py b1 b2    # only some
```

It drives the real engine through the real binding, on the smallest input that
makes each rule observable, and prints what happened beside what the alternative
would have produced. Every output line quoted below came from that run.

---

## Why this is being asked now, and not earlier

Six phases of "flag it, do not re-decide it" was correct during construction. It is
not correct any more, for two reasons that both arrived recently.

**These five now fail silently on more paths than they used to.** B1 and B2 were
JDBC rules when they were written. The JSON row source added in the ingestion track
renders through the same `SqlValues`, deliberately, so that a JSON side and a JDBC
side of the same data compare equal. That was the right call, and it doubled the
number of ingestion paths each rule is load bearing on.

**The repository is about to become public.** `specs/03-remaining-tasks.md` states
in group B that five assumptions are unconfirmed and that each one "fails silently:
the diff returns a wrong answer that looks like a right answer". That sentence is
honest and should not be deleted. Confirming the five is what turns the most
alarming public paragraph in the repository into a settled decision.

---

## B1. SQL NULL renders as an empty field

**What it does.** `SqlValues.render` returns `""` for SQL NULL, so a null and an
empty string are the same field. CSV has no NULL, so the file side cannot express
the difference either.

**Evidence.** One row, `id=1`, `note = SQL NULL`, compared against a CSV export
that writes a null as:

```
  export writes ""       ->  identical
  export writes "\N"     ->  1 modified, 1 changed cells
  export writes "NULL"   ->  1 modified, 1 changed cells
  export writes "(null)" ->  1 modified, 1 changed cells
```

Only the empty field agrees. Against any sentinel, **every null cell in the table
reports as a changed value**, and the report is noise rather than a finding.

**ANSWERED 2026-09-04: an empty field. CONFIRMED as built, no work.**

Manas also specified the detail: a null in the last column is written as `,\n`, and
the absence of any content between the delimiter and `\n` or `\r\n` is an empty
cell. **Verified against the C CLI**, three files each holding one row whose third
column is empty:

```
1,ann,\n     ->  3 columns, 1 unchanged row
1,ann,\r\n   ->  identical to the LF file
1,ann,""\n   ->  identical to both
```

All three forms of an empty last cell agree, and CRLF makes no difference. The
trailing delimiter produces a third field rather than a short row, which is what
makes the rule work.

**This ties B1 to B3.** The statement "a null last field looks like `,\n`" is a
statement about what a correct producer emits. B3 is the question of what happens
when a producer instead emits `1,ann\n` for that same row, dropping the delimiter
along with the value.

---

## B2. The timestamp and date text format

**What it does.** Renders `2026-01-31T14:22:05`, with a fractional part only when
it is non zero, and then in groups of three digits.

```
  TIMESTAMP 2026-01-31 14:22:05.000  ->  "2026-01-31T14:22:05"
  TIMESTAMP 2026-01-31 14:22:05.100  ->  "2026-01-31T14:22:05.100"
  TIMESTAMP 2026-01-31 00:00:00.000  ->  "2026-01-31T00:00:00"
  DATE      2026-01-31               ->  "2026-01-31"
  TIME      14:22:05                 ->  "14:22:05"
```

**Evidence.** A JDBC `TIMESTAMP` compared against a CSV carrying:

```
  "2026-01-31T14:22:05"     ->  identical
  "2026-01-31 14:22:05"     ->  1 modified, 1 changed cells
  "31/01/2026 14:22:05"     ->  1 modified, 1 changed cells
  "2026-01-31T14:22:05.000" ->  1 modified, 1 changed cells
  "01/31/2026 02:22:05 PM"  ->  1 modified, 1 changed cells
```

**The fourth line is the one worth looking at twice.** `2026-01-31T14:22:05.000` is
the *same instant* in the *same ISO layout*, and it still reports as changed,
because this renderer drops a zero fraction and that export writes a fixed three
digits. An export configured for fixed precision is common, and this is the failure
mode most likely to be hit while looking correct to the eye.

A timestamp column is usually populated on every row, so a disagreement here does
not report a few changed cells. **It reports the entire table as modified.**

**ANSWERED 2026-09-04: absence of milliseconds equals `.000`. REJECTED as built.**

So `2026-01-31T14:22:05` and `2026-01-31T14:22:05.000` must compare equal, and the
fourth line above is a defect rather than a documented rule.

**What this is not.** It is not `IBHA_CSVD_DATE_VALUE`. That flag means comparing
by parsed value against an explicit list of input formats, which would also make
`31/01/2026` equal `2026-01-31`. That is a much larger change and it is not what
was asked for.

**What it is: a canonical form for TIMESTAMP, exactly like the one DECIMAL already
has.** The rule generalizes to "trailing zeros in the fractional part are not
significant, and an all zero fraction is the same as no fraction", which is the
`1.50 == 1.5` rule applied to fractional seconds.

The place for it already exists. `ibha_normalize` in `core/src/normalize.c`
dispatches on the declared type: `DECIMAL` and `INTEGER` go through
`ibha_canonical_decimal`, `BOOLEAN` through the truth sets, and everything else
falls through to trimmed byte equality, which is where `TIMESTAMP` lands today. A
`TIMESTAMP` branch sits alongside those two, and because normalization feeds the
row digests, the comparison and the suppressed cell count all at once, nothing
downstream needs to know about it.

**It is a C core change, the first since Phase 4.** That means the full C gate,
three sanitizers, valgrind and four fuzz targets, plus the determinism gate across
all four producers, and a new compare option carried by both bindings if it is opt
in rather than always on. `numeric` and `booleans` both default to on, so matching
them is the consistent choice.

**The general rule was confirmed 2026-09-04**, so `.100` equals `.1` as well.

### What was built

`ibha_canonical_timestamp` in `core/src/normalize.c`, sitting beside
`ibha_canonical_decimal`, and a `TIMESTAMP` branch in `ibha_normalize` next to the
decimal and boolean ones.

**It is not gated on an option.** Two timestamps differing only in trailing
fractional zeros are the same instant, which is what the type means rather than a
preference about how to compare it. That also avoided the alternative, a new field
in `ibha_csvd_compare_opts`, which is a public struct and would have been the first
ABI change since Phase 3.

**No ABI change was needed at all.** The behaviour rides on `IBHA_CSVD_DATE_EXACT`,
whose documented meaning is now "byte equality after trim, on the canonical form",
where the canonical form means one thing only: fractional seconds carry no trailing
zeros. `date_compare` was already folded into `compare_id`, so the digest safety
that refuses a pair parsed under different comparison settings already covers it.

**What it deliberately does not do.** It does not parse the date, so `31/01/2026`
still differs from `2026-01-31`; that remains `IBHA_CSVD_DATE_VALUE` and remains
unimplemented. And it declines any value whose fraction is not a terminal run of
digits, so `14:22:05.000+05:30` is left exactly as it arrived rather than half
understood. Trimming a fraction out of the middle of a shape the code does not
model is how a comparator silently starts matching values that differ.

**One consequence worth knowing.** The canonical form belongs to the declared type,
not to the text, exactly as `1.50 == 1.5` only holds in a `DECIMAL` column. Two
timestamps in a column declared `VARCHAR` are text and still differ. A `TIME`
column gets nothing either, because `TIME` is not one of the engine's declared
types and resolves to `UNKNOWN`; `SqlValues` never renders a fraction for a
`java.sql.Time`, so the JDBC side cannot produce that mismatch, but a CSV carrying
`14:22:05.000` in a `TIME` column would. That is a known gap, not an oversight, and
adding a `TIME` type would be a change to the type enum and therefore to
`compare_id`.

13 new assertions in `core/tests/test_normalize.c` cover it, including the cases it
must **not** change: a differing fraction, differing seconds, an unparsed date
format, the offset suffix, a bare trailing point, and the same two values under
`VARCHAR`.

---

## B3. The asymmetric ragged row rule

**What it does.** A three column file, one data row per case:

```
  exactly three fields                 "1,ann,NE"     ->  identical
  four fields, the extra one EMPTY     "1,ann,NE,"    ->  identical
  five fields, both extras EMPTY       "1,ann,NE,,"   ->  identical
  four fields, the extra one NOT empty "1,ann,NE,x"   ->  REFUSED: row 5 has 4 fields, expected 3
  two fields, one MISSING              "1,ann"        ->  REFUSED: row 5 has 2 fields, expected 3
```

Excess *empty* fields are normalized and counted in `stats.ragged_normalized`,
because Excel emits trailing empty columns routinely. Anything else is refused.

**ANSWERED 2026-09-04: both halves confirmed as built. No work.**

Forgiving extra trailing empty columns is agreed, and refusing short rows is
agreed. `core/src/parse.c` `row_end` is unchanged.

This is consistent with the B1 answer rather than merely compatible with it. A
producer that writes a null last field as a trailing delimiter is a producer that
never emits a short row, so the two halves of the rule describe the same
well formed file from opposite sides: the delimiter count is the row's declaration
of its own width, and a row that under-declares is broken rather than sparse.

---

## B4. Duplicate key row numbers are record based, not line based

**This is the one where the evidence is strongest that the current behaviour is a
problem.** The demonstration file, with physical line numbers:

```
    line 1 | KEY,
    line 2 | REQUIRED,
    line 3 | INTEGER,VARCHAR(40)
    line 4 | id,note
    line 5 | 1,"first line
    line 6 | second line"
    line 7 | 2,plain
    line 8 | 1,"duplicate of the first key"
```

The duplicate of key `1` is on **physical line 8**. What the engine reports:

```
    REFUSED: duplicate key (1) in the source file at rows 5 and 7
```

Row 7 is the *record* number. **A person who opens this file and goes to line 7
lands on `2,plain`, which is a different row with a different key.** The message is
internally consistent and externally misleading, and it gets worse the more
multiline fields a file has, because the drift accumulates.

Two details worth having in front of you. The count **includes the header rows**,
so the first data row of a four header file is row 5, which is why B3's messages
above also say "row 5" for a first data row. And this same numbering reaches
`DiffRow.sourceRow`, `targetRow` and every emitter, so it is not only the error
message that is affected.

**ANSWERED 2026-09-04: the record numbering is correct. CONFIRMED as built.**

Manas's CSVs do contain embedded newlines, and the answer is still that lines 2 and
3 together are record 2 and line 4 is record 3, which is exactly what the engine
counts today. **The record is the unit a person means by "row", even when it spans
several lines.** So nothing here is wrong and nothing has to change.

### B4a, the physical line number: DECIDED 2026-09-04, not needed

**The reason is better than the one this document originally gave, and it changes
the argument from a semantic one to a checkable one.**

People view and edit these CSVs in Excel. **Excel parses the CSV properly**, so a
quoted field containing a newline is displayed as one grid row with a line break
inside the cell, not as two rows. Excel's row numbers are therefore record numbers,
and the engine's numbering already matches the tool the reports are read against,
including the header rows: a four row header occupies Excel rows 1 to 4, and the
first data record is Excel row 5, which is exactly what the engine calls record 5.

So the original justification, that a record is the unit a person means by "row",
was true but weaker than it needed to be. The real answer is that **record numbers
are the numbers the reader's tool is showing them**, and adding physical line
numbers would have been adding the number that does *not* match Excel.

This also disposes of the cost, which was a day's work and would have been the
first ABI change since Phase 3: an array on the public `ibha_csvd_table`, four
bytes a row, regenerated wasm offsets, a JNI accessor and the number carried
through all four emitters and both bindings' row records. None of it is needed.

### The one case where Excel and the engine do disagree

Verified rather than assumed, because the original statement of B4 named blank
lines alongside multiline fields and only the multiline half is covered by the
Excel argument.

**A blank line in the middle of a file makes the two drift by one, and each further
blank line adds another.** The engine treats a wholly empty line as not a record at
all, deliberately: `core/src/parse.c` skips it and counts it in
`stats.blank_lines`, and a genuinely empty single column row can still be written
as `""` so nothing is lost. Excel, by contrast, shows a blank line as an empty grid
row.

Measured on a file with four header rows, a data row, a blank line, then two more
data rows, where the duplicate sits on physical line 8:

```
    engine  ->  duplicate key (1) in the source file at rows 5 and 7
    Excel   ->  shows that row at row 8, because its row 6 is the blank one
```

**Why this is documented rather than fixed.** Counting blank lines as records is
not available: a blank line is one empty field, which in a file of three or more
columns is a short row, and short rows are refused per B3. So the choice is to skip
them, which is what happens, or to refuse any file containing one, which would be
worse.

A trailing blank line at end of file, which is the common case and what a trailing
newline produces, causes no drift at all, because no record follows it. The drift
needs a blank line with data after it, which is a hand edited file or one
concatenated from several sources.

`blank_lines` is already public in the C ABI and already carried in the JS
binding's struct offsets, so a caller who cares can say "this file has N blank
lines, so Excel's row numbers run ahead of these by N from the first one onward"
without any engine change. Worth surfacing in a report only if it is ever actually
hit.

---

## B5. `VARCHAR(n)` counts characters, not bytes

**What it does.** Column 2 declared `VARCHAR(5)`:

```
  "abcde"  5 chars,  5 bytes  ->  no finding, accepted
  "abcdef" 6 chars,  6 bytes  ->  TOO_LONG
  "café"   4 chars,  5 bytes  ->  no finding, accepted
  "cafés"  5 chars,  6 bytes  ->  no finding, accepted
  "ééééé"  5 chars, 10 bytes  ->  no finding, accepted
  "éééééé" 6 chars, 12 bytes  ->  TOO_LONG
```

`ééééé` is the case to look at: five characters in **ten bytes**, accepted into a
`VARCHAR(5)`.

**ANSWERED 2026-09-04: characters. CONFIRMED as built, no work.**

The data is UTF-8 string values and a byte limit is not what anyone means by
`VARCHAR(5)`.

Manas asked whether this matters to the C engine. **It is the C engine**:
`char_count` in `core/src/validate.c` counts UTF-8 lead bytes, which is to say
characters, and that function is the whole of the rule. Both bindings inherit it
because both run this engine. So the answer that was wanted is the behaviour that
is already there, and nothing changes.

Kept for the record, since it is the reason the question was worth asking: Oracle
defaults to BYTE semantics unless the column was declared with CHAR semantics or
`NLS_LENGTH_SEMANTICS` says otherwise, while SQL Server `NVARCHAR(n)` and
PostgreSQL `varchar(n)` both count characters. Had the source counted bytes, the
diff would pass a value that the target system then refuses on insert, which is the
most expensive failure of the five to diagnose because the report said the row was
fine.

---

## Where this leaves things

All five are settled, so the paragraph that made this urgent, group B's statement
that five unconfirmed rules each fail silently, is no longer true and the
repository can go public without shipping it.

**Done in this pass:** the five answers recorded, B2 implemented and tested, and
B6, moving the settled rules into `specs/02-solution-proposal.md` section 13 as
decisions so a seventh phase does not inherit the same five items.

**Also decided:** B4a, the physical line number, is **rejected rather than
deferred**. Excel row numbers are record numbers, so the engine already reports the
number the reader is looking at. One exception is documented under B4: a blank line
in the middle of a file drifts the two by one.

One thing to keep in view. B1 and B2 are load bearing on both the JDBC path and the
JSON row source, because both render through `SqlValues`. That is an argument for
the shared renderer, which is what was built, and not for re-deciding either rule
twice. It also means B2's engine side fix and the binding's rendering have to keep
agreeing: `SqlValues` renders no fraction when there is none, and the engine now
treats that as equal to any all zero fraction, so the two meet in the middle rather
than one compensating for the other.
