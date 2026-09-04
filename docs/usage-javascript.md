# Using ibha-csvdiff from JavaScript and TypeScript

Three packages, one engine:

| Package | What it is | Depends on |
|---|---|---|
| `@ibhatech/csvdiff-core` | The wasm engine and its binding. Compare, stream rows, emit reports | nothing |
| `@ibhatech/csvdiff-view` | Headless view model: virtualization, paging, filtering, column widths. No DOM | core |
| `@ibhatech/csvdiff-react` | React components over the view model | core, view |

The engine is the same C core the Java binding runs, and the determinism gate asserts
that both produce byte identical reports. A browser preview and a server side
reconciliation of the same pair are the same report as a matter of evidence.

This is the task oriented guide. Each package README carries its design rationale and
its measured costs. The `.d.ts` files are the reference.

---

## 1. Install

**Not published yet.** Until then, build from the repository:

```bash
cd js && pnpm install && pnpm wasm && pnpm -r build
```

`pnpm wasm` builds the two wasm modules and copies them into the core package. It
needs a wasm capable clang. Without it, the package builds and installs but throws at
the first `compare`.

After publishing:

```bash
npm install @ibhatech/csvdiff-core
npm install @ibhatech/csvdiff-view @ibhatech/csvdiff-react   # only for the UI
```

ESM only. Node 20 or newer, or any modern bundler. There is no CommonJS build, so
`require()` will not work.

---

## 2. Quick start

```ts
import { compare } from '@ibhatech/csvdiff-core';

const handle = await compare(
  () => fetch('/api/source.csv', { credentials: 'include' }),  // the server's copy
  fileInput.files[0],                                          // the uploaded copy
);

const summary = handle.summary();
if (!summary.identical) {
  for (const row of handle.rows({ changesOnly: true })) {
    console.log(row.kind, row.key, row.cells.filter((c) => c.changed));
  }
}
handle.dispose();
```

In a browser, prefer `compareInWorker`, which is the same API off the main thread.
See section 6.

---

## 3. The three things that will bite you

**A cell carries `source` exactly when it differs.** `cell.source === undefined`
means the two sides are byte identical, not that the value was empty. A consumer that
renders a missing `source` as `''` shows every unchanged cell as a deletion.
`cell.target` is the current value and is absent only on a deleted row.

**`row.cells.length` is not the file's column count.** The report carries the columns
the two sides share. Read the width from the row and the names from
`handle.columns`.

**Findings are output, not errors.** A `REQUIRED` column with an empty cell, or a
value longer than its `VARCHAR(n)`, is the point of running the comparison. It never
aborts, and `changesOnly` never drops a row whose only news is a finding.

And one that costs less but confuses: **`moved` is a flag on the row, not a kind.** A
row can be moved and modified in one edit.

---

## 4. Sources

```ts
type IbhaCsvSource =
  | Uint8Array
  | ArrayBuffer
  | Blob                     // a File from an <input type="file"> is a Blob
  | ReadableStream<Uint8Array>
  | ByteSource               // your own puller
  | (() => Promise<Response | ReadableStream<Uint8Array> | Uint8Array | ArrayBuffer | Blob>);
```

**The library never performs the fetch.** Authentication, headers, cookies, tokens,
retries, CORS and base URLs differ per deployment, so the callback form is the
recommended shape for a server side file: you assemble the whole request and hand
over the `Response`, and the binding reads `response.body` and pulls lazily, so
parsing overlaps the download rather than following it.

`Content-Type` is ignored entirely, because `application/octet-stream` is what S3
presigned URLs commonly serve. The first bytes are sniffed for one specific failure:
a response that is really an HTML login page or a JSON error body, which is by a wide
margin the most common integration bug. That surfaces as an `IbhaCsvError` with code
`BAD_CONTENT` rather than as a parse error 40 lines into a file.

### Pushing bytes yourself

```ts
import { PushSource } from '@ibhatech/csvdiff-core';

const push = new PushSource(contentLength);   // size hint, optional
const done = compare(push, other);
for await (const chunk of someUpload) await push.write(chunk);   // applies backpressure
push.end();
const handle = await done;
```

`push.fail(err)` aborts the comparison with your error.

---

## 5. Options

```ts
const handle = await compare(source, target, {
  header: { rows: 1 },                 // a names-only source file; 4 is the default
  dialect: { delimiter: ';', quote: '"', stripBom: true },
  comparison: {
    trimWhitespace: true,              // default
    charIgnorePad: true,               // CHAR(n) padding is not a difference
    numeric: true,                     // 1.50 equals 1.5 in a DECIMAL column
    booleans: true,
    allowAddedColumns: false,          // a schema change is refused, not ignored
    allowRemovedColumns: false,
  },
  matching: {
    detectMoves: true,
    deletedRowPlacement: 'anchored',   // or 'end'
    similarityPercent: 0,              // pairing keyless rows
    requireKey: false,                 // fail rather than fall back to all-keys
  },
  validate: true,                      // the findings
  countSuppressed: true,
  limits: { maxBytes: 150 * 1024 * 1024, maxRows: 0, maxColumns: 0 },
  signal: abortController.signal,
});
```

`header.rows` describes the **source** side. The target's header row count is always
detected against the source, because the source is authoritative and an uploader may
have kept only the column name row. `header.keyRow`, `requiredRow`, `typeRow` and
`nameRow` are there when your files order the header rows differently.

---

## 6. Off the main thread

A 400 ms diff on the main thread is a 400 ms frozen UI, and at the size ceiling it is
seconds.

```ts
import { compareInWorker } from '@ibhatech/csvdiff-core';

const handle = await compareInWorker(() => fetch('/api/source.csv'), file);
const summary = await handle.summary();
for await (const row of handle.rows({ changesOnly: true })) { ... }
await handle.dispose();
```

Everything is identical except that the methods are asynchronous. The fetch still
happens on the main thread, where your credentials are, and the body is pumped across
in chunks with backpressure.

`RemoteDiffHandle` adds `getRows(offset, count, read?)`, `getRowsCompact(...)` and
`index(changesOnly?)` for a virtualized view, which is what the view package consumes
through `remoteRowSource`.

---

## 7. Reading the result

### Summary

Constant memory whatever the size of the diff, and produced by the same emitter that
writes the summary report, so the two cannot disagree.

```ts
const s = handle.summary();
s.identical;
s.rows;            // unchanged, modified, added, deleted, moved, report
s.cells;           // changed, suppressed
s.findings;        // total, rows, requiredEmpty, tooLong, notNumeric, precision, enabled
s.matching;        // allKeys, pairedBySimilarity, pairingTruncated, movesForcedOff
s.columns;         // compared, added, removed
s.schemaFindings;  // columnAdded | columnRemoved | metadataDisagreement
```

### Rows

`rows()` is a lazy generator over engine memory. Nothing accumulates, and it is the
right default.

```ts
for (const row of handle.rows({ changesOnly: true, includeValues: true, maxCellBytes: 4096 })) {
  row.kind;                 // 'unchanged' | 'modified' | 'added' | 'deleted'
  row.moved; row.moveDistance;
  row.sourceRow; row.targetRow;   // 1 based record numbers, or null
  row.key;                  // key column values, or null
  row.changedCells; row.suppressedCells;
  row.cells;                // column, name, source?, target?, changed, suppressed
  row.findings;             // column, name, kind, limit?, precision?, scale?
}
```

`includeValues: false` yields the edit script alone, which is what a pass/fail check
or a row count wants. On the 15 MB reference pair it is 0.057 seconds against 0.296
with values, because decoding 1.7 million cells into JS strings is the whole
difference.

Row numbers are **record** based, not line based: a file with blank lines or
multiline quoted fields will not agree with a text editor.

### Random access, for a view

```ts
const index = handle.index({ changesOnly: true });
index.rowCount;
index.getRows(offset, 50, { includeValues: true });
index.getRowsCompact(offset, 50);      // typed arrays, no strings, for scroll math
index.getCellSegments(row, col, 'word-then-character');
index.bytesRetained;
```

This is the one thing here whose memory grows with the diff, which is why it is opt
in: about 14 bytes plus one byte per compared column per report row, and not a single
cell value. On the reference pair that is 55 KB for the changed rows and 3.6 MB for
the whole report.

### Reports

```ts
const bytes = handle.emit('jsonl', { changesOnly: true });
const stream = handle.emitStream('csv', { csvFormulaGuard: true });
```

Formats are `'jsonl'`, `'csv'`, `'html'` and `'summary'`. Options: `changesOnly`,
`includeValues`, `cellDiff`, `maxCellBytes`, `maxRows`, `csvFormulaGuard`,
`csvDelimiter`, `classPrefix`.

**Leave `csvFormulaGuard` on.** A cell starting with `=`, `+`, `-`, `@`, a tab or a CR
is a formula to Excel, so a diff report of untrusted data is a script delivery
mechanism without it.

`emitStream` bounds what JavaScript holds, not what the engine does: the report is
produced whole inside the engine and then handed out in pieces. The HTML emitter in
particular is for bounded output, since a 90,000 row diff as one HTML string is tens
of megabytes of DOM.

### Disposing

`handle.dispose()` frees the context and its arena. Forgetting is not a classic leak,
because each comparison owns a whole `WebAssembly.Instance` that the collector can
reclaim, but disposing returns the memory at a moment you choose.

---

## 8. Loading the wasm module

The module resolves from `../wasm/<name>` relative to the package, which Vite,
webpack, Rollup and Node all understand. Two builds ship and SIMD is feature detected
and preferred.

Override before the first `compare` when your host serves the module from elsewhere:

```ts
import { configure, loadEngine, simdSupported } from '@ibhatech/csvdiff-core';

configure({ wasmUrl: 'https://cdn.example.com/ibha_csvdiff.simd.wasm' });
// or configure({ wasmBinary: await (await fetch(url)).arrayBuffer() });
await loadEngine();   // optional: compile up front instead of on first compare
```

One `WebAssembly.Instance` is created per comparison and discarded with it, because
linear memory only ever grows: a shared instance would carry the high water mark of
every comparison it had ever run.

---

## 9. Errors

```ts
import { IbhaCsvError } from '@ibhatech/csvdiff-core';

try {
  const handle = await compare(a, b);
} catch (e) {
  if (e instanceof IbhaCsvError) {
    e.code;    // 'TOO_LARGE' | 'BAD_CONTENT' | 'IO_ERROR' | 'INVALID_ARG' | 'ENGINE'
    e.status;  // the engine's error name when code is 'ENGINE'
  }
}
```

Switch on `code` and `status`, not on the message text, which names user data.
`BAD_CONTENT` is the login page case from section 4 and deserves its own message in
your UI: it means the fetch succeeded and returned something that is not a CSV.

---

## 10. The headless view model

`@ibhatech/csvdiff-view` owns scroll math, paging, filtering, column widths and
keyboard focus, with no DOM and no framework. Use it directly for a Vue or Svelte
binding, or through the React package.

```ts
import { DiffViewModel, localRowSource, remoteRowSource } from '@ibhatech/csvdiff-view';

const source = localRowSource(handle, { changesOnly: true });    // or remoteRowSource
const model = new DiffViewModel(source, { rowHeight: 28, layout: 'unified' });

const unsubscribe = model.subscribe(render);
model.start();
model.setViewport(scrollTop, viewportHeight);

function render() {
  const snap = model.snapshot();   // status, rows, start, end, offsetTop, totalHeight, focus
}
```

Two things it holds to, which a rendering layer must not undo:

- **It never materializes the diff.** It asks for the fifty rows it is about to
  paint. `model.retainedPages` exists so a test can assert that.
- **Filtering rebuilds the index rather than hiding rows.** `setSource` with a
  differently filtered source means the virtualizer only ever sees the filtered
  count, so filtering 300,000 rows costs one drain rather than 300,000 DOM nodes.

`findNextChange(from, direction)`, `moveFocus(rows, cols)`, `setColumnWidth`,
`columnWidthVars()` and `setCellDiff` are the rest of the surface.

---

## 11. React

```tsx
import { useMemo, useState } from 'react';
import { compareInWorker } from '@ibhatech/csvdiff-core';
import { CsvDiffTable, CsvDiffToolbar, remoteRowSource } from '@ibhatech/csvdiff-react';
import '@ibhatech/csvdiff-view/styles.css';

function Report({ handle }) {
  const [changesOnly, setChangesOnly] = useState(true);
  const source = useMemo(() => remoteRowSource(handle, { changesOnly }), [handle, changesOnly]);

  return (
    <>
      <CsvDiffToolbar changesOnly={changesOnly} onChangesOnlyChange={setChangesOnly} />
      <CsvDiffTable source={source} height={600} />
    </>
  );
}
```

`CsvDiffTable` takes everything `ViewModelOptions` takes, plus `theme`, `height`,
`classNames`, `loading` and `onRowClick`. `useDiffView(source, options)` is the hook
underneath, returning `{ model, snapshot, ref, onScroll, onKeyDown }` for a component
you write yourself.

**Nothing is styled by default.** A component with no stylesheet imported renders
unstyled semantic markup, which is the right default for a library. Four ways to
style it, in increasing order of effort: import
`@ibhatech/csvdiff-view/styles.css`, pass a `theme` object, set the CSS custom
properties yourself, or write your own rules against the class names.

**The class names are the HTML emitter's**, so one stylesheet styles both a saved
report and the live view. View only state is a `data-` attribute rather than a new
class, which is what keeps that true.

React 18 or 19, as a peer dependency. Server rendering paints a first window at
`SSR_VIEWPORT_HEIGHT` and hydrates to the real viewport height.

---

## 12. Recipes

### Upload compared against the server's copy

The primary browser case: the user uploads a file and the original is fetched with
the application's credentials.

```ts
const handle = await compareInWorker(
  () => fetch(`/api/reconciliations/${id}/source.csv`, { credentials: 'include' }),
  file,
  { header: { rows: 4 }, limits: { maxBytes: 150 * 1024 * 1024 } },
);
```

### Node, comparing two files

```ts
import { readFile } from 'node:fs/promises';
import { compare } from '@ibhatech/csvdiff-core';

const handle = await compare(await readFile('a.csv'), await readFile('b.csv'));
process.stdout.write(handle.emit('summary'));
handle.dispose();
```

For files large enough to matter, hand over a stream instead:
`Readable.toWeb(createReadStream(path))`.

### Saving the report the user is looking at

```ts
const bytes = handle.emit('csv', { changesOnly: true });
const url = URL.createObjectURL(new Blob([bytes], { type: 'text/csv' }));
```

The same cursor produced the table and the file, so they cannot disagree.

---

## 13. Sizing and limits

From the 15 MB per side reference pair on Node 24:

```
compare, parse and match        0.160 s    82 MB reserved
summary()                       0.035 s    constant memory
rows(), values off              0.057 s    146,946 rows
rows(), values decoded          0.296 s    1,763,352 cells
index({changesOnly})            0.053 s    55 KB retained
index(), whole report           0.093 s    3.6 MB retained
emit jsonl, whole report        0.473 s    86 MB
compareInWorker, end to end     0.221 s    includes the chunk pump
```

`handle.bytesReserved` is what one comparison holds, which is how a batch driver
sizes its concurrency.

`limits.maxBytes` defaults to 150 MB and applies **per side**, both to the reader,
which stops pulling, and to the engine, which refuses. A side over the ceiling fails
with `IbhaCsvError` code `TOO_LARGE`. That default is sized for a browser tab; raise
it deliberately in Node, where the tab is not the constraint.
