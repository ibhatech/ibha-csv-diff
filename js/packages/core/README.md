# @ibhatech/csvdiff-core

```
npm install @ibhatech/csvdiff-core
```

The JavaScript binding over the `ibha-csvdiff` engine. One binding over the C ABI,
not a second implementation of it: the parser, the matcher, the comparators and
the emitters all live in the wasm module, and this package's job is to feed it
bytes and to decode the rows it hands back.

```ts
import { compare } from '@ibhatech/csvdiff-core';

const handle = await compare(
  () => fetch('/api/source.csv', { credentials: 'include' }),  // the server's copy
  fileInput.files[0],                                          // the salesman's copy
);

for (const row of handle.rows({ changesOnly: true })) {
  console.log(row.kind, row.key, row.cells.filter((c) => c.changed));
}
handle.dispose();
```

## Two rules the API is built on

**Bytes in, never strings.** A JS string input costs an encode pass plus a
transient copy of the whole file. `File`, `Blob`, a `fetch` response body and a
`ReadableStream` are all already bytes.

**Never a materialized diff.** `compare` returns a handle over engine memory whose
primary interface is a lazy cursor. A 90,000 row diff as plain JS objects is a
million allocations, which is the thing the wasm engine was chosen to prevent.
Random access exists, and it is opt in, because it is the one thing here whose
memory grows with the diff.

## What it costs

The p90 pair, 15 MB a side, on Node 24. Regenerate with
`node scripts-and-commands/measure_binding.mjs` rather than editing this table.

```
what                          seconds   note
compare, parse and match        0.160   82 MB reserved
summary()                       0.035   one drain, constant memory
rows(), values off              0.057   146,946 rows
rows(), values decoded          0.296   1,763,352 cells
index({changesOnly})            0.053   55 KB retained, 2,198 rows
index(), whole report           0.093   3.6 MB retained
100 pages of 50 rows            0.019   decoded from the index
emit jsonl, changes only        0.044   1367 KB, two passes
emit jsonl, whole report        0.473   86 MB, two passes
compareInWorker, end to end     0.221   includes the chunk pump
```

Read the gap between `rows(), values off` and `rows(), values decoded` as what
decoding 1.7 million cells into JS strings costs, and the two `index` rows as what
a view pays to be able to seek. The index keeps 14 bytes plus one byte per compared
column per report row and not a single cell value.

## The API

### `compare(source, target, options?) => Promise<DiffHandle>`

`source` and `target` are anything in `IbhaCsvSource`: a `Uint8Array`, an
`ArrayBuffer`, a `Blob` or `File`, a `ReadableStream<Uint8Array>`, or a callback
returning a `Response`.

**The library never performs the fetch.** Authentication, headers, cookies,
tokens, retries, CORS and base URLs are application concerns that differ per
deployment. The callback form is the recommended one for the server side file: the
caller assembles the whole request and hands over the `Response`, and this reads
`response.body` and pulls lazily, so parsing overlaps the download rather than
following it.

`Content-Type` is ignored entirely, because `application/octet-stream` is what S3
presigned URLs commonly serve. The first bytes are sniffed for one specific
failure only: a response that is actually an HTML login page or a JSON error body,
which is the most common integration bug by a wide margin.

### `DiffHandle`

| Member | What it does |
|---|---|
| `columns` | The compared columns, in the source's order |
| `summary()` | Counts, findings and column findings, in constant memory |
| `rows(read?)` | The report one row at a time. Nothing accumulates |
| `index(read?)` | Builds and keeps the report index, so a view can seek |
| `emit(format, opts?)` | Runs one of the engine's four emitters |
| `emitStream(format, opts?)` | The same bytes as a `ReadableStream` |
| `dispose()` | Frees the context and its whole arena |

### `compareInWorker(source, target, options?)`

The same thing with the engine in a worker, per spec 4.3, because a 400 ms diff on
the main thread is a 400 ms frozen UI and at the 150 MB ceiling it is seconds. The
handle's methods are asynchronous and everything else is identical. The fetch
still happens on the main thread, where the application's credentials are, and the
body is pumped across in chunks with backpressure.

## The row contract

Versioned, and shared with the JSONL emitter field for field, so a consumer can
move between the streamed rows and a saved report without a translation layer.
`DIFF_ROW_SCHEMA_VERSION` is taken from the engine rather than restated.

**The one rule that is easy to get wrong.** A matched row's cell carries `source`
**exactly when that cell differs in bytes from the target**. Its absence means the
two sides are byte identical; it never means the value was empty. This is what
keeps an unchanged 90,000 row report from being written out twice, and a consumer
that reads a missing `source` as an empty string renders every unchanged cell as a
deletion.

Three more things a consumer has to hold to:

- **`moved` is a flag on the row, not a kind.** A row can move and be modified in
  one edit, and a single enum loses one of the two facts.
- **`row.cells.length` is not always the source file's column count.** Under the
  column policy of spec 6.6 a report row carries the columns the two files share.
  Read the width from the row and the names from `handle.columns`.
- **Findings are output, not errors.** A `REQUIRED` column with an empty cell or a
  value over its `VARCHAR(n)` is the point of running the comparison, so it never
  aborts and `changesOnly` never drops a row whose only news is one.

## Loading the module

The wasm module is resolved from `../wasm/<name>` relative to the package, which
is what bundlers understand. Two builds ship: SIMD is feature detected and
preferred, and the scalar build is the differential oracle the determinism check
compares against. Override with `configure({ wasmUrl })` or
`configure({ wasmBinary })` before the first `compare`.

### Where the `.wasm` file comes from in your build

The default resolution is `new URL('../wasm/<name>', import.meta.url)`, which Vite,
webpack, Rollup and Node all understand without configuration: each of them sees
the asset reference, copies the file into the build output and rewrites the URL.
This is the path that works if you do nothing.

Three cases need `configure()` before the first `compare`, and all three fail the
same way without it, with the module failing to fetch:

| Situation | What to do |
|---|---|
| Serving the engine from a CDN or a versioned asset host | `configure({ wasmUrl: 'https://cdn.example.com/ibha_csvdiff.simd.wasm' })` |
| A bundler that does not follow `new URL(..., import.meta.url)`, or an inlining build | `configure({ wasmBinary })` with the bytes you fetched yourself |
| A strict CSP without `wasm-unsafe-eval` | Nothing here helps: the page needs that directive, since instantiating a module is what it governs |

`configure` is global and takes effect for every later `compare`, so call it once
at startup rather than per comparison.

### Two build warnings that are expected

A browser build of this package prints:

```
Module "node:fs/promises" has been externalized for browser compatibility
Module "node:worker_threads" has been externalized for browser compatibility
```

**Both are correct and neither needs action.** This package runs under Node and in
a browser from one build. Reading the `.wasm` from a `file:` URL needs
`node:fs/promises`, and spawning a worker where there is no `Worker` global needs
`node:worker_threads`. Both imports are dynamic and sit behind a runtime check, so
a browser never evaluates either one; a bundler still sees the specifier while
tracing the module and says it has stubbed it, which is exactly what should happen.

Verified in a scratch Vite plus React app installing the published tarballs: the
build succeeds, and both `.wasm` modules are emitted as hashed assets.

One `WebAssembly.Instance` is created per comparison and discarded with it.
Linear memory only ever grows and the engine's allocator frees nothing
individually, so a shared instance would carry the high water mark of every
comparison it had ever run. It also means a handle that is dropped without
`dispose` is collected rather than leaked; disposing returns the memory at a
moment you choose instead of one the collector chooses.

## Running the tests

```
node --test "src/*.test.ts"
```

No install required: the tests run the TypeScript sources directly under Node,
which is why every relative import names the `.ts` file it resolves to and
`tsconfig` sets `rewriteRelativeImportExtensions` to turn those into `.js` on
build. `pnpm build` and `pnpm typecheck` are the two things that do need the
toolchain.

## Licence

Apache-2.0. See `LICENSE` and `NOTICE` in this package.

The full guide is [docs/usage-javascript.md](https://github.com/ibhatech/ibha-csv-diff/blob/main/docs/usage-javascript.md).
