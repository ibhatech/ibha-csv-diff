# CSV Diff Library: Solution Proposal and Design Choices

Response to `01-solution-design.md`. This document answers every question you raised, states a
recommendation for each, records the alternatives I rejected and why, and ends with a phase wise
implementation plan.

Nothing has been built yet. Performance numbers below are engineering targets derived from known
throughput of comparable systems, not measurements. Phase 0 of the plan exists specifically to
turn them into measurements before we commit to the rest.

**Revision 2, 2026-08-01.** You answered the sizing question: **p90 is 15 MB.** That resolves open
question 1, confirms the engine choice with room to spare, and moves the binding constraint from CPU
to network transfer. Sections 2.1, 2.2, the new section 2.5, the phase plan in section 11 and the
open questions in section 12 are revised accordingly. The language, data structure, API and view
decisions are unchanged.

**Revision 3, 2026-08-01.** Correction of emphasis, and it changes the architecture rather than just
the priorities. Revisions 1 and 2 treated the browser preview as the primary use case and the server
as a binding to be added later. That is wrong. The server side batch profile, files arriving by S3 or
FTP, source data coming from an RDBMS table or a CLOB, hundreds of files reconciled in an end of day
run, is a first class profile with different constraints, and in some respects it is the more
demanding one. The engine is now specified engine-first, with the browser as one of several front
ends.

Four consequences, developed in the new section 2.6:

1. The parser is demoted from "the core" to "one front end to the core". The columnar index is the
   core, and an RDBMS `ResultSet` feeds it directly without ever being serialized to CSV text.
2. The core ABI is a pull based byte source plus a row feed, not the JavaScript flavored union type
   that section 4.2 describes. That union becomes a thin adapter in the JS binding.
3. The core must be thread agnostic with all state in a caller supplied context, because
   parallelism across files is worth far more than any single threaded optimization.
4. Output gains a streaming writer and a summary only mode, so peak memory does not scale with diff
   size when no human is looking at a table.

SIMD moves back onto the roadmap proper, for a reason revision 2 missed: in batch, CPU time is a cost
line rather than a latency budget. Section 2.5 is retained but is now explicitly scoped to the
browser interactive profile only.

---

## 1. Executive summary of decisions

| #   | Question                  | Decision                                                                                                                                                                                                                                                            | Confidence  |
| --- | ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------- |
| 1   | Language for the engine   | C11, compiled to `wasm32` for browsers and to a native static library for servers. Zero third party dependencies, no libc dependency in the wasm build.                                                                                                             | High        |
| 2   | Why not JavaScript        | Not the CPU cost, the memory cost. At the p90 of 15 MB a JS parse produces roughly 130 to 200 MB of live heap per file, and the resulting GC pauses are what actually destroy the user experience. This happens to one interaction in ten, not to a rare tail case. | High        |
| 3   | Why not Go                | Runtime plus GC in every wasm binary, roughly 2 MB even after trimming, mandatory `wasm_exec.js` glue, and no zero copy path for handing 50 MB into the module. TinyGo fixes size but not the copy or the GC.                                                       | High        |
| 4   | Data layout               | Struct of arrays, columnar, built as an index over the untouched original file bytes. No string materialization anywhere in the engine.                                                                                                                             | High        |
| 5   | CSV parsing               | Hand written RFC 4180 state machine, scalar first, SIMD accelerated in a later phase behind runtime feature detection. No external parser.                                                                                                                          | High        |
| 6   | Quotes and escaping       | Never unescape during parsing. Each field carries a `needs_unquote` bit, and comparison uses a logical comparator that walks escapes on the fly. Quoting differences must never surface as a diff.                                                                  | High        |
| 7   | Library input interface   | Bytes in, not strings: `ArrayBuffer`, `Uint8Array`, `File`, `Blob`, `ReadableStream`, or a push based session for streamed sources.                                                                                                                                 | High        |
| 8   | Library output interface  | An opaque lazy result handle backed by wasm memory, with paged accessors. Explicitly not a materialized JSON diff object.                                                                                                                                           | High        |
| 9   | Row matching              | Keyed hash matching, plus longest increasing subsequence to identify the minimal set of moved rows, plus similarity pairing for the all keys case.                                                                                                                  | Medium high |
| 10  | Report ordering           | Target file order for everything that exists in the target, with deleted rows anchored to their surviving source neighbour. Configurable.                                                                                                                           | High        |
| 11  | Column reordering         | Match columns by name, default to rejecting reorder as a validation error, allow it behind a flag.                                                                                                                                                                  | High        |
| 12  | Packaging                 | Three packages: `@csvdiff/core` (engine plus TS API), `@csvdiff/view` (headless view model), `@csvdiff/react` (components). Optional custom element wrapper.                                                                                                        | High        |
| 13  | Styling contract          | Stable class names plus a documented CSS custom property contract, with a `theme` JS object that compiles down to those custom properties. No shadow DOM by default.                                                                                                | High        |
| 14  | Server side               | Same C core behind bindings: Node, Java via the FFM API, Python, Go via cgo, plus a CLI. Do not reimplement diff semantics in SQL.                                                                                                                                  | High        |
| 15  | What the core actually is | The columnar index, not the CSV parser. The parser is one front end; a row feed from a `ResultSet` or CLOB is another. Added in revision 3, section 2.6.1.                                                                                                          | High        |
| 16  | Core ingest ABI           | A pull based `csvd_read_fn` callback. Every source, including `mmap`, S3, JDBC and `ReadableStream`, is an adapter over it. Streaming is the only path, one shot is a special case. Section 2.6.2.                                                                  | High        |
| 17  | Concurrency               | Thread agnostic core, zero global state, no internal thread pool. Parallelism is across file pairs and belongs to the caller. Deterministic output regardless of thread count. Section 2.6.3.                                                                       | High        |
| 18  | Output modes              | Three: random access handle for the UI, streaming writer to JSONL or CSV for batch, and summary only in constant memory. Section 2.6.4.                                                                                                                             | High        |
| 19  | Memory strategy for batch | Two pass digest mode as the server default, 16 bytes per source row instead of a resident index, which is what sets concurrency per machine. Section 2.6.5.                                                                                                         | Medium high |

---

## 2. Language and runtime choice

### 2.1 The real constraint is memory, not clock speed

Your stated worry is latency: 30 seconds is unacceptable. Agreed, but the mechanism that produces
that 30 seconds in a JavaScript implementation is worth spelling out, because it also rules out the
obvious fix of "write tighter JavaScript".

The p90 of 15 MB is what settles this. Had the distribution been "usually 500 KB, occasionally
50 MB", a TypeScript implementation with a documented size limit and a spinner for the rare big file
would have been a defensible engineering trade. At a p90 of 15 MB, **one preview in ten is a large
file**, and the tail above it is larger still. The big file path is the product, not an exception to
it, so it has to be the path that is designed for.

Take the p90 case first: 15 MB, 20 columns, roughly 90,000 rows, so roughly 1.8 million cells.

A JavaScript parser such as Papa Parse or `d3-dsv` produces an array of objects or an array of
arrays of strings. Cost per cell in V8:

- A short string is a `SeqOneByteString`: 16 byte header plus payload, rounded to 8 bytes, so a
  9 character cell costs about 32 bytes rather than 9.
- An array of 6 million small strings costs 8 bytes per pointer slot on top of that.
- If rows are objects rather than arrays, add hidden class plus property backing store overhead.

That lands between 130 MB and 200 MB of live heap for one 15 MB file. You need two files, so 260 to
400 MB, and then the diff result is a third graph of objects on top. At the 50 MB tail the same
arithmetic gives 400 to 700 MB per file.

On a laptop with other tabs open you are now in major GC territory, and major GC on a multi hundred
megabyte heap with millions of live pointers costs hundreds of milliseconds per cycle and happens
repeatedly. Worse, it is not steady: it is exactly the stuttering, unpredictable,
sometimes-fine-sometimes-terrible profile that produces the negative reviews you described. On a
mobile device it is an out of memory crash. And on the raw parse alone, a JS CSV parser sustains
roughly 10 to 20 MB/s, so two 15 MB files cost 1.5 to 3 seconds of parsing before any diff work
starts or any garbage is collected.

The WASM design avoids the entire category. The file bytes sit in one `ArrayBuffer` inside wasm
linear memory. The parse produces flat `u32` arrays of offsets into that buffer. Total footprint is
the file size plus roughly 1.6x the file size of index, with zero garbage collected objects and zero
string allocation. Strings get created only when a cell is actually painted on screen, which for a
virtualized table is a few hundred cells at a time.

That is the argument. It is not "C is faster than JS in a microbenchmark". It is "the JS object
model cannot represent this workload at this size without a heap that GC cannot service smoothly".

### 2.2 Throughput targets

| Stage                      | Approach                          | Target throughput | 2 x 15 MB, the p90 | 2 x 50 MB, the tail |
| -------------------------- | --------------------------------- | ----------------- | ------------------ | ------------------- |
| Parse and index            | Scalar table driven state machine | 300 to 500 MB/s   | 60 to 100 ms       | 200 to 330 ms       |
| Parse and index            | SIMD accelerated, deferred        | 1.0 to 1.5 GB/s   | 20 to 30 ms        | 65 to 100 ms        |
| Key hashing                | xxHash3 over key fields           | over 5 GB/s       | under 15 ms        | under 50 ms         |
| Hash index build and probe | Open addressing, power of two     | 30 to 60 M rows/s | 3 to 6 ms          | 10 to 20 ms         |
| Cell comparison            | Byte compare of matched rows      | memory bound      | 15 to 50 ms        | 50 to 150 ms        |

Budget, wall clock, in a Web Worker, excluding transfer time: **under 250 ms at the p90, under
700 ms at the 50 MB tail**, both with the scalar parser. Small files, 1 KB to 1 MB, land in single
digit milliseconds and are dominated by module instantiation.

**This retires SIMD from the critical path**, which is the most useful consequence of knowing the
p90. The scalar parser already clears the p90 by a wide margin and clears the 50 MB tail with room
to spare, so the WASM missing-carry-less-multiply problem in section 3.3 is no longer a risk to the
product requirement, just a possible later optimization. Section 11 demotes it accordingly. If
Phase 0 measures the scalar parser slower than 250 MB/s, SIMD comes back on the critical path, and
that is the number to watch in Phase 0.

Module instantiation deserves a note because it kills a tempting idea. A 60 to 100 KB wasm module
compiles in roughly 1 to 5 ms with `WebAssembly.instantiateStreaming`, and can be cached in the HTTP
cache or IndexedDB. So there is **no need for a hybrid "use JS for small files" path**. One code
path, always, which is worth a lot in testing and in confidence that the preview matches the
approval.

### 2.3 Alternatives considered

**Pure JavaScript or TypeScript.** Rejected for the reasons in 2.1. It would be the cheapest thing
to build and would be perfectly adequate up to about 2 MB. If the 50 MB requirement were dropped
this would be the right answer, so it is worth confirming that the requirement is real.

**Go with `GOOS=js GOARCH=wasm`.** Rejected. Three problems. First, binary size: the Go runtime,
scheduler and GC are always linked in, and even with `-ldflags="-s -w"` plus `wasm-opt` you land
around 2 MB, roughly 25x the C build, which matters for a library other people embed. Second, the
boundary: Go wasm cannot hand JavaScript a view into its own heap safely, so 50 MB has to come in
through `js.CopyBytesToGo`, an unavoidable copy, and results have to go back the same way. Third,
Go's `encoding/csv` allocates a `[]string` per record, which recreates the JS problem in a different
language. Writing a columnar zero allocation parser in Go is possible but you are then fighting the
language's grain while still paying for the runtime.

**TinyGo.** Fixes binary size, roughly 100 to 300 KB, but keeps a GC, has an incomplete reflect and
goroutine story, and the toolchain is much less predictable than clang for a library you intend to
maintain for years. Rejected.

**Rust.** Genuinely a peer of C here: same performance profile, better safety, `wasm-bindgen` and
`wasm-pack` are the best tooling in this space, and `simd-json` style crates exist. Rejected only
because you prefer C, it is not installed, and the safety benefit is smaller than usual for this
workload. The engine is one arena allocator, no free calls during a diff, no concurrency inside the
module, and a single fuzz target that covers the only untrusted input surface. That is close to the
best case for writing safe C. If you later change your mind, the interface in section 4 is
unchanged, only the implementation language moves.

**C11.** Recommended. Roughly 60 to 100 KB wasm, full control of memory layout, zero copy in and
out, `wasm_simd128.h` intrinsics available, and the same source compiles natively for every server
side binding you listed. Also the easiest target for `clang -fsanitize=fuzzer,address` which is how
we will get confidence in the parser.

### 2.4 Build targets

```
src/            portable C11 core, no libc beyond memcpy/memset which we provide
  arena.c       bump allocator over linear memory
  parse.c       RFC 4180 state machine, scalar
  parse_simd.c  v128 accelerated variant, compiled only when -msimd128
  hash.c        xxHash3 subset
  index.c       row key hash table
  match.c       keyed match, LIS move detection, similarity pairing
  cells.c       logical field comparator, type aware normalizers
  textdiff.c    intra cell word and character diff (Myers)
  api.c         exported ABI

targets:
  wasm32 baseline    clang --target=wasm32 -nostdlib -O3 -flto        ~70 KB
  wasm32 simd        same plus -msimd128                              ~85 KB
  native static      libcsvdiff.a for server bindings
  native cli         csvdiff binary for CI use
  fuzz               clang -fsanitize=fuzzer,address
```

Two wasm binaries, selected at load time by feature detecting SIMD with a 9 byte probe module. The
baseline build is the fallback and also what runs in any environment where SIMD is unavailable.

### 2.5 Browser interactive profile only: the bottleneck moves to the network

**Scope note added in revision 3.** Everything in this subsection applies to the browser preview
profile and to nothing else. In the server batch profile there is no download, no
`Content-Encoding`, and no user waiting on a spinner, so none of it applies and none of it should
shape the engine. Section 2.6 covers that profile. Kept here because the browser preview is still a
real deliverable and this is still the dominant cost in it.

Within the browser profile, this is the most important thing that changes now that the p90 is known,
and it is worth acting on before any browser-side micro-optimization work.

In the business scenario from your spec, the preview requires the **source CSV to be downloaded from
the server**. The user's edited copy is local and free to read. So the p90 preview involves pulling
15 MB across the wire, and:

| Effective throughput                        | Transfer of 15 MB uncompressed |
| ------------------------------------------- | ------------------------------ |
| 100 Mbps, good broadband to a nearby origin | about 1.2 s                    |
| 25 Mbps, typical real world single origin   | about 4.8 s                    |
| 10 Mbps, constrained or mobile              | about 12 s                     |

Against a diff that costs 250 ms, transfer is somewhere between 5x and 50x the cost of the thing we
are optimizing. Shaving the parse from 100 ms to 30 ms with SIMD is invisible next to that. Fixing
the transfer is where the user-perceived latency actually lives.

Four mitigations, in order of payoff against cost:

**1. Verify `Content-Encoding` is on for the CSV endpoint. Cheapest 10x available.** CSV is unusually
compressible: repeated delimiters, low cardinality columns, numeric text, and highly correlated
adjacent rows. gzip typically gets 5x to 8x on real tabular data and brotli 8x to 12x, so 15 MB
becomes roughly 1.5 to 2 MB and the p90 transfer drops to well under a second on ordinary broadband.
This needs zero client code and no library support. It is worth checking rather than assuming,
because endpoints that stream a generated CSV out of a database frequently bypass the compression
middleware, and this is a common and quiet source of exactly this problem. When the browser handles
`Content-Encoding`, the `ReadableStream` we consume already yields decompressed bytes, so it composes
with the next point for free.

**2. Parse during the download instead of after it.** Pipe the `fetch` response body straight into
`DiffSession` so that parsing overlaps transfer. Parsing 15 MB costs 30 to 50 ms spread across a
transfer measured in hundreds of milliseconds or seconds, so it disappears entirely: the diff is
ready within a few milliseconds of the last byte arriving.

This has a real design consequence and it is why it belongs in this document rather than in a tuning
note later. It promotes `ReadableStream` from a convenience input to **the primary input path**,
which means **the parser state machine must be resumable across arbitrary chunk boundaries** from the
first line of code. A chunk can split a multi byte UTF-8 sequence, a quoted field, a `""` escape
pair, or a CRLF. Designing that in during Phase 1 is nearly free, because the state machine already
has explicit states and simply needs its state plus a partial field carried across calls.
Retrofitting it later means rewriting the parser. Section 11 moves this into Phase 1.

**3. Cache the source by version.** The source CSV is server owned and immutable for a given version
or ETag, so it belongs in the Cache API or IndexedDB keyed on that version. This matters more than it
looks, because the real usage loop is not one preview: it is preview, spot a mistake, fix the
spreadsheet, preview again. Every iteration after the first should cost zero transfer.

**4. A digest protocol, and an honest assessment that it is less of a win than it first appears.**
The idea is that the server sends only a per row digest, an 8 byte key hash plus an 8 byte row hash,
and the client requests the handful of full source rows it needs in order to display old values. At
90,000 rows that is 1.4 MB. But hashes are incompressible, so the comparison is 1.4 MB of digest
against roughly 1.7 MB of brotli compressed CSV. Once compression is on, the transfer saving is
marginal, and it costs a second round trip plus a new server side endpoint that must compute hashes
using **exactly** our normalization rules or the whole thing silently produces wrong answers.

Its genuine benefits are client memory and not having to serve the full file at all, so within the
browser profile it stays in the plan but is demoted: worth building only if the tail above the p90
turns out to be very large, or if mobile browsers are in scope and their memory ceiling binds.

**Revision 3 correction.** The transfer argument above is sound but it is the wrong argument for the
server profile, where the two pass digest mode turns out to be the mechanism that sets batch
concurrency. Section 2.6.5 promotes it back for that reason. Demoted in the browser, default on the
server.

For contrast, the client side memory picture at the p90 is comfortable and is not what forces this
decision: 15 MB of bytes plus about 16 MB of field index plus about 1.4 MB of digests is roughly
33 MB per side, so about 66 MB for both, against the 260 to 400 MB a JavaScript implementation would
need for the same pair.

### 2.6 The two execution profiles, and what the batch profile changes

There are two profiles with genuinely different constraints, and the engine has to be designed for
both rather than for one with the other bolted on.

|                        | Browser interactive                         | Server batch                                       |
| ---------------------- | ------------------------------------------- | -------------------------------------------------- |
| Trigger                | One user, one preview, waiting              | Scheduled run, hundreds of file pairs              |
| What "fast" means      | Time to first paint of the diff             | Total wall clock and CPU-seconds per GB            |
| Dominant cost          | Network transfer, per section 2.5           | CPU and disk, and the diff itself                  |
| Address space          | wasm32, hard ceiling, no threads by default | 64 bit, many cores, no ceiling worth naming        |
| Source of the old data | HTTP download of a CSV                      | RDBMS table, a CLOB, S3 object, FTP drop           |
| Consumer of the result | A virtualized React table                   | A file, a table, an exception report, an exit code |
| Failure of one input   | Show the user a diagnostic                  | Quarantine that pair, the batch must not abort     |
| Best parallelism lever | One worker thread                           | Embarrassingly parallel across file pairs          |

Six design consequences follow. Numbers 1, 2 and 3 are structural and I would build them this way
regardless of how you answer the questions in section 12.

#### 2.6.1 The parser is a front end, not the core

The core of this engine is the columnar index of section 3, not the CSV parser. That was true in
revision 1 but it was not stated, and the API in section 4 quietly assumed CSV text was the only way
in. It is not, and your RDBMS case proves it.

If the old data is a relational table, serializing a million row `ResultSet` into CSV text so that our
parser can immediately parse it back is pure waste: a full serialize pass, a full parse pass, and a
transient copy of the whole table, all to reach a structure the caller could have built directly. So
the index gets a second front end:

```
                       +-- CSV bytes ---> RFC 4180 parser -----+
                       |                                       |
input sources ---------+-- ResultSet / CLOB / any row iterator +--> columnar index --+
                       |      ---> row feed API ---------------+                     |
                       +-- pre-built index (cached digest) ----+                     |
                                                                                     v
                                                                        diff engine --> report
```

The row feed is three calls and writes straight into the index arena:

```c
void csvd_feed_begin_row(csvd_builder *b);
void csvd_feed_field(csvd_builder *b, const uint8_t *bytes, uint32_t len, uint8_t flags);
void csvd_feed_end_row(csvd_builder *b);   // computes key_hash and full_hash for the row
```

Because the diff engine only ever touched the index and never touched CSV text, it works unchanged.
This is the payoff of the "index over immutable bytes" decision, and it is worth noting that it only
works because comparison was defined on logical values in section 5.2 rather than on raw CSV bytes. A
`ResultSet` field and a CSV field that represent the same value hash identically.

One subtlety the row feed has to handle: the caller's field bytes may not outlive the call, whereas
CSV parsing points into a buffer that does. So the builder needs a `copy_fields` mode that appends
into an arena owned string pool, at a cost of one `memcpy` per field, versus the zero copy parser
path. Worth exposing as a flag rather than always copying, since a JDBC caller reading into a reused
buffer needs the copy and an `mmap` caller does not.

#### 2.6.2 The core ABI is a pull based byte source, and streaming is the only path

Section 4.2's `CsvSource` union is a JavaScript convenience. The C core should know nothing about
`Blob`, `File` or `ReadableStream`. It takes a pull callback:

```c
typedef int64_t (*csvd_read_fn)(void *ctx, uint8_t *dst, size_t cap);  // bytes, 0 = EOF, <0 = error
```

Every real source is then an adapter: a fixed buffer, a POSIX `fd`, an `mmap`ed region, a JS
`ReadableStream` pumped from the host, an S3 `GetObject` body, a JDBC `Reader`, an FTP socket. The
"give me the whole buffer" case becomes the trivial adapter rather than the primary interface, which
is the right way round. Combined with the resumable state machine that revision 2 already moved into
Phase 1, this makes streaming the only path in the core and one-shot a special case of it.

Server side this unlocks `mmap` for local files, which is the fastest possible ingest: no `read`
syscalls, no user space copy, the OS page cache does the work, and the field offsets in our index
point directly into mapped pages. The index-over-immutable-bytes design is close to ideal for `mmap`,
and this is a real advantage the native build has over the wasm build.

#### 2.6.3 Thread agnostic core, parallelism across pairs

For a batch of hundreds of independent file pairs, the highest value optimization by a wide margin is
running them concurrently. Eight cores is 8x. No amount of SIMD or cache tuning competes with that,
so the engine must not stand in its way:

- No global mutable state. No static buffers, no lazily initialized globals, no `errno` style
  reporting. Every entry point takes a `csvd_ctx *`.
- All allocation from the context's arena, so two concurrent diffs never touch shared allocator state
  and there is no lock to contend.
- No internal thread pool, and no threading in the core at all. Concurrency is the caller's job,
  where the caller already has a thread pool, an executor, a goroutine pool or a process per file.
  This also keeps the wasm build trivially valid, since wasm threads need shared memory that we
  cannot count on.
- Deterministic output that does not depend on thread count or scheduling, so batch results are
  reproducible and comparable run to run.

Intra-file parallel parsing, splitting one file into chunks parsed concurrently, is a separate and
much harder thing, because you cannot find a record boundary in a CSV containing quoted multiline
fields without knowing the quote parity at the split point. It is solvable, either by a cheap quote
counting prepass or by speculatively parsing each chunk under both parity hypotheses and discarding
the one that fails to reconcile, but it is only worth the complexity if a **single** file is large
enough that one core cannot chew it in acceptable time. That is question 5 in section 12.

#### 2.6.4 Output: streaming writer and summary only mode

`DiffHandle` with paged random access exists to feed a virtualized table. In batch there is no table,
and holding the whole diff so that nobody reads it is waste. Two additional output modes:

- **Streaming writer.** Diff rows are emitted to a sink as they are produced, so peak memory is
  bounded by the source-side index rather than by the diff size. Formats: CSV, and JSONL as the
  default because it is line delimited, so it streams, appends, greps, splits and loads into anything.
  In this mode the report index array of section 6.5 is never built at all.
- **Summary only.** Counts per change kind, plus schema differences, plus validation exceptions, and
  no per row output retained. Many reconciliation jobs genuinely only need "did it change, how much,
  and which rows violated the rules". This mode runs in constant memory with respect to diff size.

For the CLI, `diff(1)` style exit codes so it composes in a shell or a scheduler: 0 identical,
1 differences found, 2 error. And per section 2.6's table, a batch driver must be able to quarantine
one bad pair and keep going, so every failure mode is a returned typed error against a specific pair,
never a process abort and never a longjmp out of the middle of a parse.

#### 2.6.5 The digest mode is promoted, for a different reason than it was demoted

Section 2.5 demoted the two pass digest mode because its transfer saving evaporates once
`Content-Encoding` is on. That reasoning holds in the browser and does not transfer to the server,
where there is no transfer to save. Server side the relevant number is **per worker memory, because
that is what sets concurrency per machine**:

| Per side, 15 MB and 90,000 rows | Full index resident | Digest only                                        |
| ------------------------------- | ------------------- | -------------------------------------------------- |
| Footprint                       | about 33 MB         | about 1.4 MB, at 16 bytes per row                  |
| Concurrent pairs in 4 GB        | roughly 60          | roughly 1,000, then bounded by target side instead |

That is more than an order of magnitude more concurrency per box, which in a batch context is
directly proportional to throughput per machine. And the objection that killed it in the browser, that
fetching old values for changed rows costs a second round trip, does not apply: server side the source
is an `mmap`ed file, a local object or a database the process is already connected to, so the second
pass is a seek into the page cache or a keyed re-query. Nearly free.

So: **digest mode becomes the default for the server batch profile** and stays optional in the
browser. The two pass structure is the same one sketched in revision 2, it simply changes
from a speculative optimization to a specified mode.

#### 2.6.6 SIMD comes back, as a cost lever rather than a latency lever

Revision 2 retired SIMD on the grounds that the scalar parser already clears the browser latency
budget. That is still true, and it was the wrong frame for batch. In a nightly run, parse throughput
is not a latency budget, it is a **CPU-seconds bill**, whether that is billed as cloud instance hours
or as a batch window that has to finish before the business day starts. A 3x parse speedup is a 3x
reduction in the machine time the batch consumes, on the largest single CPU consumer in the pipeline.

So SIMD returns to the plan proper rather than sitting behind a measurement gate. Still after
correctness, because a fast wrong diff is worthless, and still with the honest caveat from section 3.3
that WASM's missing carry less multiply caps the wasm variant nearer 1 to 1.5 GB/s. The **native**
build has no such limit and can use `pclmulqdq` and AVX2 or NEON directly, so the native SIMD parser
is meaningfully faster than the wasm one and the batch profile gets the better of the two. That
asymmetry is a point in favor of the native build for batch, independent of the wasm startup cost.

#### 2.6.7 Sanity check on the batch scenario you described

Worth doing the arithmetic, because it reframes where the batch engineering effort should go. Take 500
salesmen at the 15 MB p90, compared against equivalent source side data, so roughly 15 GB to process
in total:

| Configuration                     | Parse and diff CPU time                          |
| --------------------------------- | ------------------------------------------------ |
| Scalar, single core, 400 MB/s     | about 40 s of parsing, roughly 60 to 90 s all in |
| Scalar, 8 cores across pairs      | roughly 10 to 15 s                               |
| Native SIMD, 8 cores across pairs | roughly 4 to 6 s                                 |

An end of day reconciliation of that size is a **seconds long job**, not an overnight window. Which
means that once the engine is built as specified, the batch will be bound by S3 or FTP fetch
concurrency, by database read throughput on the source side, and by however the report is persisted.
None of those are the diff. If a batch like this currently takes hours, the diff engine is not what is
costing the hours, and it is worth confirming where that time actually goes before optimizing this
component further.

This is also the strongest argument for the digest mode of section 2.6.5 and the summary mode of
section 2.6.4: at these speeds the engine is fast enough that memory footprint and I/O shape, not
instructions retired, decide how much hardware the batch needs.

---

## 3. Data structure design: struct of arrays, columnar, over immutable bytes

Your instinct about struct of arrays is right, and for this workload the more important property is
not vectorization, it is that **we never build a string**.

### 3.1 Layout

```c
typedef struct {
    const uint8_t *bytes;   // the original file, untouched, owned by the arena
    size_t         len;

    // Field index. One entry per cell, in row major order.
    uint32_t *field_off;    // byte offset into bytes, points past the opening quote
    uint32_t *field_len;    // logical byte length, excludes the closing quote
    uint8_t  *field_flags;  // FIELD_QUOTED | FIELD_HAS_ESCAPE | FIELD_HAS_NEWLINE | FIELD_EMPTY
    uint32_t  n_fields;

    // Row index. Explicit rather than row*ncols, so ragged rows are representable
    // and reported as a diagnostic rather than a crash.
    uint32_t *row_first_field;   // n_rows + 1 entries, last is a sentinel
    uint32_t  n_rows;

    // Per row digests, filled during parse.
    uint64_t *row_key_hash;      // hash of the key columns only, normalized
    uint64_t *row_full_hash;     // hash of all compared columns, normalized

    // Open addressing map from key_hash to row index. Power of two, 50% load.
    uint32_t *bucket;
    uint32_t  bucket_mask;
} csvd_table;
```

Cost per cell is 9 bytes of index, so roughly 1.6x file size for a typical 20 column CSV, plus 16
bytes per row of digests. For a 50 MB file: 50 MB of bytes plus about 55 MB of index plus about
5 MB of digests, so about 110 MB per side, 220 MB for both, comfortably inside a wasm32 heap.

A note on the memory budget, since it constrains the roadmap: wasm32 tops out at 4 GB
architecturally but browsers in practice fail `memory.grow` well before that, often around 2 GB, and
`ArrayBuffer` allocation of hundreds of megabytes can fail on 32 bit or memory constrained mobile.
The API therefore exposes a `maxBytes` budget and fails fast with a typed error rather than
crashing the tab. Memory64 is not yet portable enough to rely on.

### 3.2 Why offsets and lengths rather than pointers or a copied field buffer

Two `u32` per cell rather than a `char*` halves the index on 64 bit native builds and keeps the
layout identical between wasm32 and native, which matters because we are going to assert that both
produce byte identical diff output.

I considered a single `u32 offsets[n+1]` array with length derived by subtraction, which saves
4 bytes per cell. Rejected: it breaks as soon as a field is quoted, because the logical field ends
before the closing quote and the delimiter, so the derived length would be wrong. Storing length
explicitly keeps the hot comparison loop branch free.

`field_len` is `u32` rather than `u16` deliberately. A 64 KB cap on cell size would be fine 99.99%
of the time and then fail on someone's base64 blob column, and the failure mode would be silent
truncation. Not worth 4 bytes per cell.

### 3.3 Where SIMD actually helps, and where it does not

Three places, in order of payoff:

1. **Structural scan.** Find all delimiter, quote, CR and LF positions 16 bytes at a time with
   `wasm_i8x16_eq` plus `wasm_i8x16_bitmask`. This is the big win and is the bulk of the 3 to 5x.
2. **Field comparison.** 16 byte chunked `memcmp` on matched rows. The compiler mostly does this
   already given `-O3`, so hand vectorizing is low value.
3. **Hashing.** xxHash3 is already designed around wide loads.

One honest caveat that shapes the plan. The simdjson style trick for deciding which bytes are inside
quoted regions uses a carry less multiply (`pclmulqdq`) to compute a prefix XOR of the quote mask in
one instruction. **WASM SIMD has no carry less multiply.** The prefix XOR has to be emulated, either
with a short shift and XOR ladder over the 16 bit mask, about 4 operations, or by carrying a parity
bit across lanes scalar-ly. That is fine, it still beats a byte loop comfortably, but it means the
realistic WASM ceiling is nearer 1 to 1.5 GB/s than the 2 GB/s+ that native simdcsv papers report.
I would rather state that now than have it show up as a missed target later.

This is also why the plan does scalar first. A table driven state machine, 256 entry transition
table indexed by `(state, byte_class)`, gets us to 300 to 500 MB/s, which already meets the under
one second requirement for 50 MB and clears the 15 MB p90 by a wide margin. SIMD is Phase 7, after
correctness and with the scalar parser retained as its oracle, and is justified by the batch CPU cost
argument of section 2.6.6 rather than by browser latency. It is never a Phase 1 risk.

---

## 4. The library interface

**Revision 3 scope note.** This section describes the **JavaScript binding**, which is one binding
over the core ABI rather than the core itself. The core C ABI is the pull based byte source and row
feed of sections 2.6.1 and 2.6.2, and the `CsvSource` union below is a set of adapters onto it. The
two rules in 4.1 remain correct for the JS binding, and their C analogues are "pull bytes through a
callback" and "never materialize the diff".

### 4.1 Principle: bytes in, handles out

Two rules drive the whole API.

**Rule 1: accept bytes, never require strings.** If the consumer hands us a JS string we have to
encode it to UTF-8, which for 50 MB is a copy plus a transient 50 MB allocation. `File`, `Blob`,
`fetch` responses and Node streams are all already bytes. So bytes are the primary currency and
strings are an accepted-but-slower convenience.

**Rule 2: never return a materialized diff.** This is the single most important API decision. The
tempting signature is `compare(a, b) => DiffResult` where `DiffResult` is a plain object full of
arrays of row objects. That signature recreates in the result exactly the heap explosion we
eliminated in the parse. A 300,000 row diff would be a million JS objects.

Instead `compare` returns an opaque handle over wasm memory with paged accessors. The React view
asks for the 50 rows it is about to paint, gets them decoded on demand, and lets them be collected.

### 4.2 Core API sketch

```ts
// ---------- input ----------
type CsvSource =
  | ArrayBuffer
  | Uint8Array // zero copy fast path
  | Blob
  | File // read via arrayBuffer(), still one copy
  | ReadableStream<Uint8Array> // chunked, no full buffer in JS
  | string // convenience, encoded internally
  | { url: string; init?: RequestInit }; // library does the fetch, streams it in

// ---------- options ----------
interface CsvDiffOptions {
  header?: {
    rows?: number; // default 4
    keyRow?: number | null; // default 1, cell containing KEY marks a key column
    requiredRow?: number | null; // default 2
    typeRow?: number | null; // default 3
    nameRow?: number; // default 4
  };
  // Explicit overrides win over in-file markers.
  keyColumns?: string[] | number[] | "all-columns" | "auto";

  dialect?: {
    delimiter?: "," | ";" | "\t" | string; // default ',' with optional sniffing
    quote?: '"' | "'";
    escape?: "double" | "backslash"; // default 'double' per RFC 4180
    // newline handling is always permissive: LF, CRLF and CR are all accepted
    bom?: "strip" | "keep"; // default 'strip'
  };

  columns?: {
    allowReorder?: boolean; // default false, mismatch is a validation error
    allowAdded?: boolean; // default false
    allowRemoved?: boolean; // default false
    detectRenames?: boolean; // default false, heuristic
    ignore?: string[]; // columns excluded from comparison entirely
  };

  comparison?: {
    trimWhitespace?: boolean; // default true
    caseInsensitive?: boolean; // default false
    emptyEqualsNull?: boolean; // default true
    useDeclaredTypes?: boolean; // default true, uses header row 3
    numericEquality?: "exact" | "value"; // default 'value': 1.50 equals 1.5
    dateEquality?: "exact" | "value"; // default 'exact', opt in per column
    perColumn?: Record<string, Partial<ColumnComparison>>;
  };

  matching?: {
    deletedRowPlacement?: "anchored" | "end" | "sourceOrder"; // default 'anchored'
    detectMoves?: boolean; // default true
    pairUnmatched?: boolean; // default true when there are no key columns
    similarityThreshold?: number; // default 0.5 fraction of matching cells
    duplicateKeys?: "error" | "pair-in-order"; // default 'pair-in-order' + diagnostic
  };

  cellDiff?: {
    mode?: "none" | "word" | "character" | "word-then-character"; // default 'none'
    maxCellBytes?: number; // default 4096, longer cells report whole cell changed
    // computed lazily per visible cell, never for the whole table
  };

  limits?: { maxBytes?: number; maxRows?: number; maxColumns?: number };
  signal?: AbortSignal;
  onProgress?: (p: { phase: DiffPhase; bytesDone: number; bytesTotal: number }) => void;
}

// ---------- one shot ----------
declare function compare(source: CsvSource, target: CsvSource, options?: CsvDiffOptions): Promise<DiffHandle>;

// ---------- streamed / incremental ----------
interface DiffSession {
  pushSource(chunk: Uint8Array): void;
  pushTarget(chunk: Uint8Array): void;
  endSource(): void;
  endTarget(): void;
  finish(): Promise<DiffHandle>;
  abort(): void;
}
declare function createSession(options?: CsvDiffOptions): DiffSession;

// ---------- result ----------
interface DiffHandle {
  readonly summary: DiffSummary; // small, safe to hold: counts per change kind
  readonly schema: SchemaDiff; // column level: added, removed, reordered, renamed, retyped
  readonly diagnostics: Diagnostic[]; // ragged rows, duplicate keys, REQUIRED violations, type violations
  readonly rowCount: number; // rows in the report, after filtering

  // Paged access. Decodes on demand out of wasm memory.
  getRows(offset: number, count: number, opts?: { include?: ChangeKind[] }): DiffRow[];
  getRow(index: number): DiffRow;

  // Compact form for virtualized rendering: parallel typed arrays, no per row objects.
  getRowsCompact(offset: number, count: number): CompactRowPage;

  // Intra cell segments, computed on first request and cached in wasm memory.
  getCellSegments(rowIndex: number, columnIndex: number): TextSegment[];

  // Export without ever materializing the whole diff in JS.
  toCsvStream(opts?: ExportOptions): ReadableStream<Uint8Array>;
  toJsonStream(opts?: ExportOptions): ReadableStream<Uint8Array>;

  dispose(): void; // frees the arena. Also wired to FinalizationRegistry as a safety net.
}

type ChangeKind = "unchanged" | "modified" | "added" | "deleted" | "moved";

interface DiffRow {
  kind: ChangeKind;
  targetIndex: number | null; // row number in the modified file, 1 based, null when deleted
  sourceIndex: number | null;
  key: string[] | null;
  moveDistance?: number;
  cells: DiffCell[];
}

interface DiffCell {
  column: number;
  changed: boolean;
  newValue: string | null; // null when the row is deleted
  oldValue: string | null; // null when the row is added
  segments?: TextSegment[]; // only when cellDiff is enabled and requested
  violations?: CellViolation[]; // REQUIRED empty, type or length violation
}
```

Answering your question directly: **not file handles, not JS collections, not callbacks.** Bytes, or
a stream of bytes, or a URL we will stream for you. Callbacks that return CSV text would force us
back into JS strings, which is the thing we are avoiding. The push based `DiffSession` covers the
case that a callback interface was reaching for, without the string cost.

### 4.3 Worker execution and the SharedArrayBuffer question

Doing a 400 ms diff on the main thread is a 400 ms frozen UI, so the worker is not optional. Default
entry point runs the engine in a Web Worker.

That creates a real problem worth surfacing, because it has a deployment consequence. The result
lives in the worker's wasm memory, and the React table on the main thread needs to read pages of it.
Two options:

- **`SharedArrayBuffer`**: main thread reads wasm memory directly, zero copy, best possible. But it
  requires the embedding site to send `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`. We cannot require that of every consumer of an npm
  library, and many will not be able to set it.
- **Message passing pages**: main thread requests rows `[offset, offset+n)`, worker serializes that
  page into a transferable `ArrayBuffer`. For a 50 row page that is a few kilobytes and about
  0.1 ms. Works everywhere.

Recommendation: **message passing by default, SharedArrayBuffer as an opt in fast path** that we
feature detect and use when the headers happen to be present. Also offer `compareSync` in the same
thread for Node and for tiny inputs. The API is identical in all three modes, which is why
`getRows` is synchronous-looking but the handle is obtained from a promise: the view model prefetches
pages slightly ahead of the scroll position.

---

## 5. Parsing: multiline fields, quotes and escapes

### 5.1 The parser

Hand written, RFC 4180 compliant with the usual permissive extensions. No external library. Reasons:
the C options are all callback-per-field designs that allocate or copy, none of them build the
columnar index we need, and the parser is roughly 600 lines that we will fuzz anyway. Writing it is
cheaper than adapting one.

Scalar core is a table driven state machine with five states: `FIELD_START`, `UNQUOTED`,
`QUOTED`, `QUOTE_IN_QUOTED`, `AFTER_QUOTED`. Handled:

- Quoted fields containing the delimiter.
- **Quoted fields containing newlines**, which is your multiline question. The state machine simply
  does not treat LF or CR as a record terminator while in `QUOTED`. The field's `FIELD_HAS_NEWLINE`
  flag is set so the view knows the cell needs multi line rendering and so row height measurement can
  be triggered for it.
- `""` as an escaped quote inside a quoted field, and optionally backslash escapes as a dialect
  option.
- Mixed LF, CRLF and bare CR line endings, including a file that mixes them.
- UTF-8 BOM stripped by default.
- Trailing newline or no trailing newline.
- Ragged rows: reported as a `Diagnostic` with the row number, missing cells treated as empty,
  extra cells retained and reported. Not a hard error by default, because real files have them and
  a hard error means the user sees nothing.
- Bytes that are not valid UTF-8: the engine is byte oriented and does not care. Only the decode at
  the view boundary cares, and it uses `TextDecoder` with replacement characters plus a diagnostic.
  This is important: a diff tool that refuses to open a Latin-1 file is useless.

### 5.2 Quotes and escaping in the diff engine, which is the subtle question

Your second quoting question is the one that decides whether this library is trustworthy. If the
source file has `"O""Brien, Ltd"` and the user's spreadsheet round trip emits `O'Brien, Ltd` in a
different quoting style, or quotes every field where the original quoted only the ones that needed
it, a naive byte comparison reports the entire file as modified. That library gets uninstalled.

So the rule is: **comparison operates on logical values, never on raw bytes, and quoting is never a
difference.**

Implementation, without giving up the no-allocation property:

1. Parsing records `field_off` and `field_len` for the region **inside** the quotes, and sets
   `FIELD_QUOTED`, and sets `FIELD_HAS_ESCAPE` only if a `""` was actually seen.
2. The fast path, which is the overwhelming majority of cells, has `FIELD_HAS_ESCAPE` clear. Then
   the logical value is exactly the bytes in `[off, off+len)` and comparison is a `memcmp`. Whether
   the field was quoted is irrelevant and invisible.
3. The slow path, `FIELD_HAS_ESCAPE` set, uses `csvd_field_cmp_escaped`, a comparator that walks
   both fields simultaneously, collapsing `""` to `"` as it goes, and never allocates. Same for
   hashing: `csvd_field_hash` feeds the unescaped byte stream into xxHash3 incrementally.
4. Only when a cell is rendered or exported do we materialize the unescaped string, and then only
   for the visible window.

The consequence is that `"abc"`, `abc`, and `"ab""c"` versus `ab"c` all compare correctly, and the
hash of a row is independent of how that row was quoted. That property is worth a dedicated test
suite: for every fixture, re-emit it with three different quoting policies and assert the diff
against the original is empty.

### 5.3 Normalization, which is where the real false positive risk lives

Quoting is one source of noise. The bigger one, in the exact business scenario you described, where
a user downloads a CSV and opens it in Excel, is that Excel silently rewrites data. `00123` becomes
`123`. `1.50` becomes `1.5`. `2024-01-05` becomes `1/5/2024`. `TRUE` becomes `True`. A long number
becomes `1.23457E+14`. If the diff reports all of that, the preview is useless.

This is where header row 3, the declared types, earns its place. It is not just validation metadata,
it selects a comparator:

| Declared type                  | Comparison                                                                                                                                                                  |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `VARCHAR(n)`, `CHAR(n)`        | byte equality after optional trim. `CHAR(n)` optionally ignores trailing pad. Length over `n` is a violation diagnostic.                                                    |
| `DECIMAL(p,s)`, `NUMERIC(p,s)` | parse to a fixed point integer at scale `s` and compare numerically, so `1.5`, `1.50` and `1.500` are equal. Scale or precision overflow is a violation.                    |
| `INTEGER`, `BIGINT`            | integer value equality, so `007` equals `7`.                                                                                                                                |
| `BOOLEAN`                      | case insensitive against a configurable truth set.                                                                                                                          |
| `DATE`, `TIMESTAMP`            | `exact` by default. Opt in to `value` with an explicit input format list, because guessing between `1/5/2024` and `5/1/2024` is not something a library should do silently. |
| unknown or absent              | byte equality after trim.                                                                                                                                                   |

Every normalization that suppresses a difference is recorded in the summary as
`suppressedByNormalization`, so the UI can say "3 cells differ only in formatting" and let the user
see them. Silent suppression is as dangerous as noise.

Numeric comparison is implemented as integer arithmetic on a scaled `int64`, not floating point, so
that money values compare exactly. `DECIMAL(38,x)` beyond `int64` range falls back to a normalized
string comparison, which is correct if slower.

---

## 6. Row matching, reordering and report order

### 6.1 Keyed matching, the normal case

1. Parse both files, computing `row_key_hash` from the key columns using the normalized logical
   comparator from section 5, and `row_full_hash` from all compared columns.
2. Build an open addressing table over the source rows keyed by `row_key_hash`.
3. Walk the **target rows in target order**. For each:
   - No matching key: `added`.
   - Matching key and equal `row_full_hash`: `unchanged`, and we do not compare cells at all. This
     is the fast path that makes the common "user changed 5 rows out of 300,000" case nearly free.
   - Matching key, different `row_full_hash`: compare cells to find which changed, mark `modified`.
     Hash collisions are handled by verifying key bytes on match, so a collision costs a comparison,
     never a wrong answer.
4. Source rows never matched: `deleted`.

This makes row reordering a non issue for keyed files, which answers your first sub question. A row
that moved is found by key regardless of where it now sits.

### 6.2 Move detection without flagging everything as moved

If we naively marked every row whose position changed as `moved`, then moving one row from the
bottom to the top would mark all 300,000 rows as moved. Wrong and useless.

The fix is the standard one: take the sequence of source indices in target order, compute the
**longest increasing subsequence**, and treat the LIS as "in place". Only rows outside it are
`moved`. Moving one row to the top then reports exactly one moved row. `O(n log n)` with a patience
style algorithm, which at 300,000 rows is a few milliseconds.

`moveDistance` is reported so the UI can choose to ignore small jitter.

### 6.3 Duplicate keys

Real files have them, including files where the declared key is not actually unique. Default
behaviour is `pair-in-order`: rows with the same key are matched first-to-first, second-to-second in
file order, and a `DuplicateKey` diagnostic is emitted with the count. `duplicateKeys: 'error'` is
available for callers who want to treat it as an upload validation failure, which for your approver
flow is probably the right setting.

### 6.4 The all keys case, where every column is part of the key

Here a modified row cannot be recognized as modified by key, because changing any cell changes the
key. Naively it becomes one `deleted` plus one `added`, which is technically correct and practically
unhelpful.

Two stage approach:

1. **Exact multiset match.** Match identical rows by `row_full_hash`, respecting multiplicity, so
   three identical rows on each side match three to three. Anything left over is a candidate.
2. **Similarity pairing over the leftovers.** Unmatched rows are usually a small fraction. For those:
   - Build a candidate index: for each unmatched source row, index it under a small set of cheap
     signatures, for example the hash of each individual column value, or the hash of the first
     few non empty columns.
   - For each unmatched target row, gather candidates from those buckets, cap the candidate list at
     a configurable `k` (default 16), and score each candidate by the fraction of matching cells.
   - Greedily accept the best pair above `similarityThreshold`, default 0.5.
   - Unpaired remainders stay as `added` and `deleted`.

That is `O(n)` with bounded constants rather than the `O(n^2)` that a full pairwise similarity search
would cost. For pathological inputs, hundreds of thousands of unmatched rows with no discriminating
column, we cap total pairing work and emit a diagnostic saying pairing was truncated, rather than
hanging.

**Alternative considered:** run a patience or histogram diff over the sequence of `row_full_hash`
values, which is what git does for lines. This gives a natural interleaved ordering and handles
insertions and deletions elegantly, and since row hashes are mostly unique it behaves well. I would
like to build this too, as `matching.algorithm: 'patience'`, because for files with no key at all it
produces a more intuitive report than similarity pairing. Recommendation: similarity pairing first
because it composes with the keyed path, patience as an alternative in a later phase, benchmark both
on real files before picking the default.

### 6.5 Report ordering, and where deleted rows go

Your constraint is that the report follows the modified file's row order. That fully determines the
position of `unchanged`, `modified`, `added` and `moved` rows. It says nothing about `deleted` rows,
which by definition have no position in the target.

Three options, `deletedRowPlacement`:

- **`anchored`, the default and recommended.** While walking the source file in order, track the
  target position of the most recently matched source row. A deleted source row is emitted
  immediately after that position. So a row deleted from the middle of the file appears in the
  middle of the report, next to its former neighbours, which is what a reviewer expects. Consecutive
  deletions stay grouped in source order.
- **`end`.** All deletions in a block at the bottom. Simplest to explain, easy to scan when you only
  care about deletions.
- **`sourceOrder`.** Emit the report in source order instead, with additions anchored. The inverse
  view, useful for an approver reviewing what was lost.

The report is itself a flat index array in wasm memory, `u32` per report row pointing at either a
target row or a source row plus a tag, so building it and re-building it under a different filter is
cheap and allocation free.

### 6.6 Column reordering

Columns are matched by name from header row `nameRow`. Then:

- Same names, same order: normal.
- Same names, different order: if `columns.allowReorder` is false, which is the default, this is a
  `ValidationError` reporting expected versus actual order, with the permutation shown. Your
  reasoning is right, data destined for a table with a fixed schema should usually reject reordering
  at the door.
- Same names, different order, `allowReorder: true`: build a permutation map, compare logically. The
  reorder itself is reported in `schema.reorderedColumns` so the UI can show it.
- Added or removed columns: gated by `allowAdded` and `allowRemoved`, reported in `schema`. When
  allowed, an added column shows every cell as added rather than the whole row as modified.
- Renamed columns: `detectRenames` is off by default. When on, a heuristic pairs an unmatched source
  column with an unmatched target column at the same position whose value fingerprint matches, and
  reports it as a rename with a confidence value. Heuristics that silently reinterpret schema should
  always be opt in.
- Header metadata differences, a column that gained or lost `KEY` or `REQUIRED`, or changed type,
  are themselves diffs and appear in `schema.retypedColumns`. A key set that differs between the two
  files is a hard error, because it means the two files are not comparable in the way the caller
  assumed.

### 6.7 When the source has no row order at all

Added in revision 3, and this is the one place where the RDBMS source case breaks an assumption the
rest of section 6 was built on. **A relational table has no inherent row order.** A `SELECT` without
an `ORDER BY` may return rows in a different sequence on every execution, depending on the plan, on
parallel scan, on physical reorganization or on a vacuum.

Three things in this document silently assume the source side has a meaningful order:

- **Move detection, section 6.2.** The longest increasing subsequence is computed over source indices,
  so against an unordered source it would report an arbitrary and unreproducible set of rows as
  `moved`. Non-deterministic diff output is worse than no move detection.
- **`deletedRowPlacement: 'anchored'`, section 6.5.** Anchoring a deleted row next to its former
  neighbours is meaningless if "former neighbour" is an artifact of the query plan.
- **`duplicateKeys: 'pair-in-order'`, section 6.3.** First-to-first pairing in file order is
  arbitrary without an order.

So the row feed builder carries an explicit flag, and it is the caller's assertion rather than
something we can detect:

```c
csvd_builder_set_source_ordered(csvd_builder *b, bool ordered);
```

When `ordered` is false, `detectMoves` is forced off and reported as forced off in the diagnostics
rather than silently ignored, `deletedRowPlacement` degrades to `'end'`, and duplicate key pairing
falls back to matching on the full row hash first so that at least identical duplicates pair
deterministically. The CSV front end defaults to `ordered = true`, since a file does have an order.

The alternative, which is available to the caller and is better when they can afford it, is to supply
an `ORDER BY` on the key columns and set `ordered = true`. Then everything in section 6 works
normally, at the cost of a sort on the database side. Worth documenting as the recommended pattern,
because a deterministic reconciliation report is usually worth a sort.

---

## 7. Cell level, intra text diffing

Your example, `Accident violation code` becoming `Accident Violation code(s)`, is exactly the case
where highlighting the whole cell loses the information the reviewer needs.

Four modes:

- **`none`**, default. Cell is changed or not. Fastest, and correct for ID and code columns where
  partial highlighting is noise.
- **`word`**. Tokenize on whitespace and punctuation boundaries, run Myers diff over tokens. On the
  example this yields: `Accident` unchanged, `violation` replaced by `Violation`, `code` replaced by
  `code(s)`.
- **`character`**. Myers over bytes, grapheme cluster aware at the boundaries so we never split a
  UTF-8 sequence or an emoji. Best for short cells and single character edits.
- **`word-then-character`**, recommended default when any cell diff is on. Word diff first, then for
  each adjacent replaced token pair, refine with a character diff inside it. On the example this
  highlights just the `v` to `V` and just the added `(s)`. This is what GitHub's intra line
  highlighting does and it reads far better than either level alone.

Three things keep this cheap:

1. **Computed lazily, per cell, on request.** `getCellSegments(row, col)` is called by the view only
   for cells in the visible window. Results are memoized in the arena. A 300,000 row diff never
   computes 300,000 cell diffs.
2. **Capped.** Cells over `maxCellBytes`, default 4096, skip refinement and report as wholly
   changed. Myers is `O(ND)`, which is fine for short strings and quadratic-ish for long dissimilar
   ones.
3. **Returned as compact segment arrays**, `[op, start, len]` triples in a typed array, not as
   objects or as pre-built HTML. The view decides the markup.

Segments are returned in terms of byte offsets into the logical, unescaped value, with a helper to
convert to UTF-16 code unit offsets for DOM ranges. Getting this boundary wrong is a classic source
of mangled non ASCII text, so it is explicit in the API rather than implied.

---

## 8. The view component

### 8.1 Package structure

Three layers, so that the consumer takes only what they need and so that a future Vue or Svelte
binding is cheap:

```
@csvdiff/core      engine, wasm, TS API. Zero dependencies. Framework agnostic. Works in Node.
@csvdiff/view      headless view model: virtualization math, filter and sort state,
                   column widths, selection, keyboard nav. No DOM, no React. Testable in isolation.
@csvdiff/react     React components over @csvdiff/view. Peer dependency on react.
@csvdiff/element   optional custom element wrapper for non React consumers.
```

The headless middle layer is the part people usually skip and then regret. It is what lets us ship a
Vue binding later without reimplementing scroll math, and it is what makes the view logic unit
testable without a browser.

### 8.2 Web component or React component

You asked whether the view should be a web component. Recommendation: **React as the primary
binding, with a custom element wrapper as a secondary artifact, and no shadow DOM by default.**

The reasoning is your own styling requirement. Shadow DOM's entire purpose is to prevent the host
page's stylesheet from reaching inside the component. You have stated that the consumer must supply
the stylesheet. Those are in direct conflict. A shadow DOM component can only be styled through an
explicitly exposed `::part()` surface and inherited custom properties, which means every new styling
need becomes a version bump of our library. That is a bad bargain here.

So: light DOM, stable class names, consumer CSS reaches everything. `@csvdiff/element` exists for
consumers who are not on React, and it also renders into light DOM by default, with
`shadow="open"` available for those who specifically want isolation and accept the `::part()`
contract.

### 8.3 Styling contract

Three mechanisms, in order of what I expect people to actually use:

**1. Stable, documented class names on every element.** Prefixed, BEM-ish, and treated as public
API covered by semver:

```
.csvd-table  .csvd-header  .csvd-header-cell  .csvd-body  .csvd-row
.csvd-row--modified  .csvd-row--added  .csvd-row--deleted  .csvd-row--moved  .csvd-row--unchanged
.csvd-cell  .csvd-cell--changed  .csvd-cell--key  .csvd-cell--violation
.csvd-value--new  .csvd-value--old
.csvd-seg--added  .csvd-seg--removed  .csvd-seg--unchanged
```

Plus `data-*` attributes for state that CSS should be able to select on, `data-change="modified"`,
`data-column="premium_amount"`, `data-row-kind`. This is the escape hatch that means the consumer is
never blocked on us.

**2. A documented CSS custom property contract**, consumed by the optional default stylesheet:

```css
:root {
  --csvd-font-family: system-ui, sans-serif;
  --csvd-font-size: 13px;
  --csvd-font-mono: ui-monospace, monospace;
  --csvd-row-height: 28px;
  --csvd-cell-padding-x: 8px;
  --csvd-border-color: #e2e2e2;

  --csvd-added-bg: #e8f5e9;
  --csvd-added-fg: #1b5e20;
  --csvd-deleted-bg: #ffebee;
  --csvd-deleted-fg: #b71c1c;
  --csvd-modified-bg: #fff8e1;
  --csvd-modified-fg: #6d4c00;
  --csvd-old-value-fg: #8a8a8a;
  --csvd-old-value-decoration: line-through;
  --csvd-seg-added-bg: #a5d6a7;
  --csvd-seg-removed-bg: #ef9a9a;
}
```

Three shipped themes, `light`, `dark` and `high-contrast`, are just files that set these variables,
and they are optional imports. `@csvdiff/react` with no stylesheet imported renders unstyled
semantic markup, which is the right default for a library.

**3. A `theme` JS object**, which is the configuration object you asked about:

```tsx
<CsvDiffTable
  handle={handle}
  theme={{ fontSize: 13, addedBg: "#e8f5e9", rowHeight: 28 }}
  classNames={{ row: "my-row", cell: (c) => (c.changed ? "my-changed" : "") }}
/>
```

One deliberate design note here. The `theme` object does **not** produce inline styles. It emits a
scoped block of CSS custom properties on the container. Inline styles would win over the consumer's
own stylesheet on specificity and make the component harder to theme, not easier, and a JS object
fundamentally cannot express `:hover`, `@media (prefers-color-scheme: dark)`, print styles, or
container queries. So the object exists because it is convenient for the 80% case, and custom
properties plus class names remain the real contract underneath. `classNames` accepts functions for
the cases where the styling depends on the data, which is how Tailwind users will want to drive it.

### 8.4 Row presentation modes

All four of the layouts you described come from the same row model, selected by a prop:

- **`inline`**: one row per report row. A changed cell shows the new value in the emphasis colour
  with the old value above or below it in gray strike-through, position controlled by
  `oldValuePosition: 'above' | 'below' | 'tooltip'`.
- **`stacked`**: two physical rows per changed row, new on top, old below, with unchanged cells in
  the lower row rendered blank so the eye goes straight to the differences. This is your fourth
  bullet and it falls out naturally.
- **`sideBySide`**: two synchronized panes, source left, target right, with aligned rows and gap
  filler rows for additions and deletions.
- **`unified`**: git style, deleted row then added row, useful for the all keys case.

Filtering, your first UI bullet, is a prop plus a built in control: `filter: { changesOnly: true }`
by default, with `<CsvDiffToolbar>` exposing the "show unchanged rows" checkbox. Filtering happens in
wasm by rebuilding the report index array, not by hiding DOM nodes, so filtering 300,000 rows is
instant and the virtualizer only ever sees the filtered count.

### 8.5 Table mechanics and how they phase in

Virtualization is not optional at 300,000 rows, so it is in from the first version. Everything else
phases in, as you suggested:

| Feature                                                    | Version | Notes                                                                                 |
| ---------------------------------------------------------- | ------- | ------------------------------------------------------------------------------------- |
| Windowed vertical virtualization, fixed row height         | 0.1     | Only visible rows in the DOM.                                                         |
| Sticky header, sticky key columns                          | 0.1     | `position: sticky`, no JS scroll sync, no jitter.                                     |
| Single scroll container, both axes                         | 0.1     | Avoids the classic desynchronized header on fast scroll.                              |
| Keyboard navigation, focus management                      | 0.2     | Arrow keys, page keys, jump to next change.                                           |
| Column resize by drag, persisted widths                    | 0.2     | Widths in the view model, applied via CSS variables.                                  |
| Column virtualization                                      | 0.3     | Needed for very wide files, 200+ columns.                                             |
| Variable row height for multiline cells                    | 0.3     | Measured and cached, the hard one, hence later.                                       |
| Column pinning, hide and show, reorder in view             | 0.4     | View only, does not affect diff semantics.                                            |
| Sort and filter by column and by change kind               | 0.4     | Sorting overrides the target order constraint, so it is opt in and clearly indicated. |
| Export visible or full diff to CSV, XLSX, Markdown         | 0.4     | Streamed from wasm, never materialized.                                               |
| Print stylesheet                                           | 0.5     | Approvers print things.                                                               |
| Accessibility pass, ARIA grid, screen reader announcements | 0.5     | Ongoing from 0.1, formally audited at 0.5.                                            |

On accessibility: the markup is a real `<table>` with `role="grid"` semantics, and a changed cell
carries an `aria-label` like "premium amount, changed from 1200.00 to 1350.00" so that the
information conveyed by colour is also available non visually. Colour alone must never be the only
signal, so change kinds also carry an icon or text marker by default, which is a correctness issue
for colour blind reviewers, not a nicety.

---

## 9. Server side and cross language use

The same C core, one implementation of diff semantics, several bindings:

| Target                  | Mechanism                                               | Notes                                                                                                                                                               |
| ----------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Node and Bun and Deno   | The same wasm module, or an optional N-API native addon | Default to wasm so `npm install` never needs a compiler. The native addon is an opt in `optionalDependency` for throughput sensitive server use, roughly 1.3 to 2x. |
| Java                    | `java.lang.foreign` FFM API on JDK 22+                  | No JNI glue to hand write or maintain. JNI fallback for JDK 17.                                                                                                     |
| Python                  | `cffi` against the static library, shipped as wheels    | Avoids requiring a compiler at install time.                                                                                                                        |
| Go                      | `cgo` against the static library                        | Or the wasm module via `wazero` if a pure Go, cgo free build matters more than the last 30% of speed.                                                               |
| CLI                     | `csvdiff old.csv new.csv --format=json`                 | For CI checks and for debugging customer files. Also our own test harness.                                                                                          |
| Kotlin and JVM services | Same FFM binding as Java                                |                                                                                                                                                                     |

One strong recommendation on your approver flow. You mentioned the server side comparison might be
done in SQL since both versions are in the database. That will work, and it will also **disagree with
the browser preview**, because SQL comparison inherits the database's collation, its numeric
coercion, its null and empty string semantics, and its trailing space handling. The user will then
see one set of differences in the preview and the approver will see a different set, and someone will
file a bug that is very unpleasant to explain.

So: use the same C core on the server, via the FFM or cffi binding, for the approver diff. Keep SQL
for what SQL is good at, cheaply detecting _whether_ anything changed, and for set based validation.
Diff semantics should live in exactly one place, and the golden test suite should assert that the
wasm build, the native build, and the CLI produce byte identical output for every fixture.

---

## 10. Correctness strategy

The parser and the matcher are the two places where a subtle bug produces a confidently wrong answer,
which is worse than a crash. Four layers:

1. **Golden corpus.** Handwritten fixtures for every quirk: multiline quoted fields, escaped quotes,
   mixed line endings, BOM, ragged rows, empty file, header only file, one column file, duplicate
   keys, Latin-1 bytes, 100,000 identical rows, unicode and emoji, plus the `csv-spectrum` suite.
2. **Differential testing against Python's `csv` module** for pure parse equivalence, and against
   a naive `O(n^2)` reference diff implementation, written in Python for clarity rather than speed,
   for diff equivalence on small inputs. The naive implementation is the oracle.
3. **Property based testing.** Generate a random CSV, apply a random edit script consisting of
   inserts, deletes, cell edits, row moves and column reorders, then assert the diff recovers exactly
   that edit script. This catches matcher bugs that fixtures never will. Also the quoting property
   from section 5.2: re-emit any fixture with a different quoting policy and assert an empty diff.
4. **Fuzzing.** `clang -fsanitize=fuzzer,address,undefined` on the parser entry point, run in CI with
   a persistent corpus. This is the main reason C is acceptable here rather than merely tolerable.

Plus a benchmark suite with 1 KB, 100 KB, 1 MB, 10 MB and 50 MB fixture pairs, run in CI on both
native and wasm, with a regression gate that fails the build on a greater than 10% slowdown. Since
performance is a stated product requirement, it needs a test, not a hope.

---

## 11. Phase wise implementation plan

Each phase ends with a handoff file in `docs/handoff/`, per the project convention.

**Phase 0, foundation and proof.** Repo scaffolding, clang wasm build, arena allocator, the JS glue
that moves bytes into linear memory with zero copy, benchmark harness, fixture generator producing
the 1 KB to 50 MB pairs with 15 MB as the headline case. Deliverable: a wasm module that ingests
15 MB and 50 MB and reports its byte count, with a measured end to end time. This is the phase that
validates or refutes the throughput assumptions in section 2.2 before we build on them. **The number
to watch is scalar parse throughput: at or above 300 MB/s the plan below holds and SIMD stays
deferred to Phase 7; below 250 MB/s it moves up ahead of the view work.**

**Phase 1, parser and index, resumable from the start.** Scalar RFC 4180 state machine, columnar
index, four row header model, field flags, xxHash3, logical field comparator including the escape
aware path. Fuzz target and golden corpus.

Moved into this phase from the old Phase 6, per section 2.5: **the state machine must be resumable
across arbitrary chunk boundaries.** Parser state plus any partial field is carried between calls, so
a chunk may split a multi byte UTF-8 sequence, a quoted field, a `""` escape pair or a CRLF. This is
nearly free to design in now and requires a parser rewrite if deferred. The fuzz target feeds input
in randomly sized chunks specifically to exercise it, and a property test asserts that parsing any
fixture in one shot and in random chunks yields byte identical indexes.

Deliverable: parse and index correctly at the target throughput, in one shot or streamed.

**Phase 2, keyed diff engine, the JS API, and streaming ingest.** Key hash table, keyed matching,
cell comparison, report index construction, LIS move detection, `compare` and `DiffHandle` with paged
accessors, Web Worker wrapper, TypeScript types.

Also moved up from the old Phase 6: the `DiffSession` push API and a `fetch`-to-parser pipeline, so
that download and parse overlap. Given section 2.5 this is the single highest leverage latency work
in the project, and it is cheap once Phase 1 made the parser resumable.

Deliverable: `@csvdiff/core` at 0.1, usable and benchmarked, with the p90 preview measured end to end
over a throttled network rather than just in isolation.

**Phase 3, React view, minimum viable.** Headless view model, virtualized table, sticky header,
inline mode, changes only filter, class name and custom property styling contract, default themes.
Deliverable: `@csvdiff/react` at 0.1 plus a demo app that diffs two dropped files.

**Phase 4, schema and validation.** Column matching by name, reorder detection and gating, added and
removed columns, declared type comparators and the normalization suppression report, REQUIRED and
type violation diagnostics, duplicate key handling. Deliverable: the false positive problem from
section 5.3 is solved and demonstrated against a real Excel round tripped file.

**Phase 5, all keys matching and cell level diff.** Exact multiset matching, similarity pairing,
optional patience diff over row hashes, Myers word and character diff with lazy per cell
computation, stacked and side by side and unified view modes. Deliverable: no key CSVs produce a
useful report, and intra cell highlighting works on your `Accident violation code` example.

**Phase 6, the server profile.** Revised and substantially expanded in revision 3, and promoted ahead
of view polish because it is a product requirement rather than an optimization. Native static library,
the row feed front end of section 2.6.1 with its `copy_fields` mode and `source_ordered` flag,
`mmap` ingest, streaming JSONL and CSV writers plus summary only mode from section 2.6.4, the two pass
digest mode from section 2.6.5, CLI with `diff(1)` exit codes and per pair error isolation, N-API
addon, Java FFM binding, Python wheels, Go cgo binding, and the cross target identical output test.

Deliverable: a batch driver that reconciles 500 pairs against an RDBMS source with bounded per worker
memory, plus the measured numbers from section 2.6.7 to replace the estimates there.

**Phase 7, native and wasm SIMD.** Promoted out of the old measurement gate for the reason in section
2.6.6: in batch, parse throughput is a CPU cost line, not a latency budget. SIMD structural scan, with
`pclmulqdq` and AVX2 or NEON on native and the emulated prefix XOR on wasm, selected by runtime
feature detection. The scalar parser is retained permanently as the differential oracle, and a
property test asserts the two produce identical indexes on every fixture and every fuzz input.

Deliverable: measured 2 to 4x parse speedup, native ahead of wasm, with identical output.

**Phase 8, view polish.** Everything in the 0.2 to 0.5 rows of the table in section 8.5: column
resize, column virtualization, variable row heights, sort and filter, export, print, accessibility
audit.

**Phase 9, remaining gated work.** Each item still waits on evidence:

- _Intra-file parallel parsing_, per section 2.6.3. Gated on a single file being large enough that one
  core cannot handle it in acceptable time, which is question 5 in section 12. Needs either a quote
  parity prepass or speculative dual hypothesis parsing, so it is real work for a narrow payoff.
- _SharedArrayBuffer fast path_ for worker to main thread page reads. Gated on the message passing
  path showing up in a scroll profile, and on consumers being able to set COOP and COEP at all.
- _Out of core operation_ for inputs that exceed available RAM even in digest mode, by spilling the
  target side index to disk. Gated on question 5.

---

## 12. Open questions for you

Revision 3 rewrote this section. The questions below are ordered by how much the answer changes the
design, and each carries the default I will build if you do not want to decide now. Questions 1 to 4
are the ones that actually change something.

**1. For the RDBMS source, do you want the row feed, or will the server always hand us CSV text?**
The row feed of section 2.6.1 lets a `ResultSet` populate the columnar index directly, skipping a
serialize pass and a parse pass over the whole table. It is the difference between reading a million
row table once and writing it out, parsing it back and holding a third copy. It costs a second front
end to the index plus its own tests.
_Default if unanswered: build the row feed._ It is the main structural consequence of your batch
framing and it is much cheaper to design in now than to add once the parser is assumed everywhere.

**2. Does the source side have a defined row order?** Per section 6.7, a `SELECT` without `ORDER BY`
has no reproducible order, which makes move detection non-deterministic and anchored deletion
placement meaningless. Either you can afford an `ORDER BY` on the key columns, in which case
everything in section 6 works normally, or you cannot, in which case those features degrade
explicitly rather than silently.
_Default if unanswered: expose the `source_ordered` flag, default it to false for the row feed and
true for the CSV front end, and force the affected features off with a diagnostic when it is false._

**3. What does the batch actually need to emit?** Three quite different answers: a full persisted
diff, a summary plus an exceptions list, or just a pass/fail signal per salesman. This decides
whether the streaming writer or the summary mode is the primary path, and whether we need a
**serialized diff format** that can be stored and re-rendered later, which is a real deliverable not
currently scoped. Re-rendering a stored diff in the approver UI months later is a different
requirement from producing one on demand.
_Default if unanswered: build the streaming JSONL writer and the summary mode, and do not build a
stable serialized diff format until you confirm a diff has to be persisted and re-displayed._

**4. Is there a single-file-is-enormous case, or only many-medium-files?** These need different
engineering. Hundreds of 15 MB files is solved by running pairs concurrently, which is free. One
5 GB file needs intra-file parallel parsing with the quote parity problem of section 2.6.3, and
possibly out of core operation, and that is Phase 9 work with real complexity. The p99 question from
revision 2 was a clumsy way of asking this; this is the version that changes the design.
_Default if unanswered: assume many-medium-files, build for concurrency across pairs, and leave
intra-file parallelism gated in Phase 9._

**5. How malformed is the real input?** In a browser preview a bad file means one diagnostic shown to
one user. In a 500 file batch it means that pair is quarantined and the other 499 must still complete,
which makes the error model a first class design concern rather than an afterthought. If salesmen are
producing these files by hand in Excel, I would expect a meaningful rate of wrong column counts,
stray delimiters, mixed encodings and CP1252 smart quotes.
_Default if unanswered: no failure ever aborts a batch, every error is a typed value returned against
a specific pair, and encoding problems produce a diagnostic rather than a rejection._

**6. Which binding first, and which JVM or runtime version?** Phase 6 is sequenced by the binding you
actually need. The Java answer also decides FFM versus JNI: `java.lang.foreign` on JDK 22+ needs no
hand written glue, JDK 17 does.
_Default if unanswered: CLI first, since it is our own test harness anyway, then Java FFM._

**7. Browser profile only, and worth a five minute check rather than a decision:** is
`Content-Encoding` enabled on the endpoint that serves the source CSV? Per section 2.5 it is worth
roughly a 10x reduction in the dominant cost of the browser preview, needs no code from us, and is
frequently missing on endpoints that stream generated CSV out of a database. 8. **Do the four header rows appear in both files, always?** Specifically, will the file the user
downloads, edits and re-uploads still carry rows 1 to 3, or will Excel or the user strip them? If
they can go missing, the caller must be able to supply the key and type metadata out of band. The
API supports that, but I want to know whether it is the common case or the exception. This matters
more in the batch profile, where nobody is present to correct a stripped header. 9. **Are key columns guaranteed unique in practice?** This decides whether `duplicateKeys` defaults
to `error` or to `pair-in-order`. For the batch profile I would lean toward `error` plus
quarantine, since a non unique key means the reconciliation is not well defined. 10. **Is column reordering ever legitimate in your domain?** You said it usually should not be
allowed. If it is never allowed, the schema logic gets simpler and I would make it a hard error
with no flag. 11. **Which environments must the browser build support?** Any non evergreen browser, any mobile
browsers with tight memory limits, and can the embedding sites plausibly set COOP and COEP. Browser
profile only, and it does not affect the engine.

Answers to open questions
Q1: Row Feed
Q2: Order of the CSV records is as per its insertion order . So always ordered.
Q3: Users of the library are free to decide at the call site what to do with the result, Store it in a file and render it on web UI when clients ask for it, or store partial diff or complete diff to a file, and send emails, or supply it to a HTML renderer which generates HTML reports and saves it. In other worrds, the diff emitter, and diff consumer components agree on the data format, and keep it a lazy stream. The library user may choose to transform it into a HTML table stream and then add some custom iterator to write it to a file or choose the default iterator that generates a DIV containing a table with standard style classes that can be injected into a React component

Q4: At present a single-file-is-enormous case can simply fail with an error message "File too large. Max file size allowed is (decide the limit youself) MB". No implementation needed. I don't see any files exceeding 10x case 150 MB any time soon.

Q5: Agree with your default. Diff fails with the first error message. No need to enumerate all errors.

Q6: Java 21 bindings needed.

Q7. How a browser fetches the content is not the library's concern. The library should give a clear guideline on how to use it on the browser. Consider the case where a user is attaching a file. In this case one file is on the client laptop. The source file needs to be fetched from server and used to run the diff. Decide whether you want accept a callback in which client assembles all the API call , headers etc required to send a GET a call. What Content-Type do you support? If the server sends a application/octet-stream can you handle it? Or, do you want CSV content in the response body and you need to be able to get the response handle to download the response in a lazy fashion, or do you want all the response content at one shot? Or, or do you want to throw an error when the response is application/octet-stream. Decide what you can handle and cannot handle

Q8. Header rows always appear in general, but they should be present in the source file definitely. Sales users may forget to be diligent and may just keep the table column headers and forget the other headers.

Q9. Duplicate keys should throw an error

Q10. Do not allow Column reordering when processing new input files. Whatever order the source file has must be continued.

Q11. It is sufficient to support desktop browsers.

---

## 13. Revision 4: decisions locked from your answers

All eleven questions answered on 2026-08-01. This section is authoritative where it conflicts with
anything above it, and the implementation follows this section. Two answers corrected mistakes in
earlier revisions and are called out as corrections rather than refinements.

### 13.0 Naming and repository layout

| Thing | Name |
|---|---|
| C engine repo | `ibha-csvdiff-core` |
| JS repo | `ibha-csvdiff-js` |
| Java repo | `ibha-csvdiff-java` |
| Submodule paths in this repo | `core/`, `js/`, `java/` |
| C public header | `ibha_csvdiff.h` |
| C public symbol prefix | `ibha_csvd_` |
| Static library | `libibha_csvdiff.a` |
| CLI binary | `ibha-csvdiff` |
| npm packages | `@ibhatech/csvdiff-core`, `@ibhatech/csvdiff-view`, `@ibhatech/csvdiff-react` |
| Java group and artifact | `com.ibhatech` : `ibha-csvdiff-java` |
| Java package | `com.ibhatech.csvdiff` |
| CSS class prefix | `ibha-csvd-`, configurable at the component level |

`-wasm` is dropped from the family name. WASM is one of four build targets and the native builds
serve the batch profile, which section 2.6 established as first class.

### 13.1 Q1, row feed: build it

Confirmed. Section 2.6.1 stands as written. The columnar index has two front ends, the RFC 4180
parser and the row feed, and the diff engine sees only the index.

### 13.2 Q2, source rows are always ordered

Your answer: CSV record order is insertion order, so the source side always has a meaningful order.

Consequence: everything in section 6 works in its full form everywhere. Move detection by longest
increasing subsequence and `deletedRowPlacement: 'anchored'` are both live in every profile, and
section 6.7's degraded mode never triggers in practice.

The `source_ordered` flag stays in the row feed ABI, defaulting to **true**, because a future caller
feeding an unordered `SELECT` needs a way to say so and silently producing non-reproducible move
reports would be worse than making them declare it. It is a safety valve, not a mode we expect anyone
to use.

### 13.3 Q3, output is a lazy row stream with pluggable emitters

This is the answer with the largest design consequence and it replaces section 2.6.4.

Your requirement, restated: the emitter and the consumer agree on a row format and keep it a lazy
stream. The caller decides at the call site what to do with it, whether that is writing JSONL to a
file, generating an HTML report, emailing a summary, or feeding a React component. So the library
must not decide the destination, and must never require the whole diff in memory to reach any
destination.

**The single output primitive is a pull cursor.** Everything else is an emitter built on it.

```c
// Advances to the next report row. Returns 0 at end of stream, 1 on a row, <0 on error.
int ibha_csvd_cursor_next(ibha_csvd_cursor *cur);

// The current row. Field values are pointers into engine memory, valid until the next call to
// cursor_next. Zero copy, zero allocation, and nothing accumulates.
const ibha_csvd_row *ibha_csvd_cursor_row(const ibha_csvd_cursor *cur);
```

Peak memory in cursor mode is bounded by the two indexes plus one row, never by the diff size. The
report index array of section 6.5 is not built at all unless a consumer asks for random access.

Emitters shipped with the library, each a thin loop over the cursor writing into a caller supplied
sink:

| Emitter | Output | Use |
|---|---|---|
| `jsonl` | One JSON object per report row | Default machine format. Streams, appends, greps, splits. |
| `csv` | A CSV diff report | Loading the report back into a spreadsheet or a table. |
| `html` | A `<div>` containing a `<table>` with the standard classes | Saved HTML reports, and injection into a React component. |
| `summary` | Counts, schema findings, validation findings | Constant memory. Pass/fail and email bodies. |
| custom | Whatever the caller writes | The caller implements the sink and consumes the cursor directly. |

Three things about the HTML emitter, because it is the one with sharp edges.

**Escaping is mandatory and is a security requirement, not a formatting one.** Cell content is
untrusted, arriving from a salesman's spreadsheet, and the output is injected into a page through
`dangerouslySetInnerHTML` or written to a file someone opens in a browser. A cell containing
`<script>` or `" onload="` must not become live markup. The emitter escapes `&`, `<`, `>`, `"` and
`'` in every value, emits only class names from a fixed compiled-in set, never interpolates caller
data into an attribute name or a URL, and this is covered by explicit XSS fixtures in the test
corpus. Getting this wrong turns a diff preview into stored XSS against the approver.

**The HTML emitter is for reports, not for interactive browsing of a large diff.** A 90,000 row diff
rendered to an HTML string is tens of megabytes of DOM and will not scroll acceptably. The rule:
HTML emitter for bounded output, which in practice means changes-only or a page at a time; the
virtualized React component of section 8 for interactive browsing of a full diff. Both consume the
same cursor, so they agree by construction.

**The row schema is a versioned public contract.** Since the emitter and the consumer are separate
components that must agree, the JSONL and HTML row shapes carry a `schemaVersion` and are covered by
compatibility tests. Changing them is a breaking change.

The random access `DiffHandle` of section 4.2 remains, and is now defined as a cursor consumer that
retains an index so the React view can seek. It is one emitter among several rather than the primary
interface.

### 13.4 Q4, hard size limit instead of an enormous-file implementation

Your answer: fail with a clear message, and nothing is expected above roughly 150 MB.

**Default limit: 150 MB per input file, both profiles, configurable.** Chosen to sit exactly at your
stated ceiling so no legitimate file is rejected, and not higher because of what it costs in the
browser. At 150 MB one side is roughly 150 MB of bytes plus 160 MB of index plus 14 MB of digests,
about 325 MB, so a pair with both sides resident is about 650 MB. That is acceptable on a desktop
browser, which Q11 confirms is the only browser target, and it is the reason the ceiling is not
raised further without the digest mode.

The limit is enforced **as bytes arrive**, not after buffering, so an oversized stream fails at the
byte that crosses the threshold rather than after consuming memory to discover the size. Error:

```
IBHA_CSVD_ERR_TOO_LARGE: "File too large. Maximum allowed is 150 MB, received at least 151 MB."
```

Consequence for the roadmap: **Phase 9's intra-file parallel parsing and out of core operation are
dropped entirely.** No quote parity prepass, no speculative dual hypothesis parsing, no spilling the
index to disk. That removes the single most complex piece of unbuilt work in the plan.

### 13.5 Q5, fail fast with one error

Agreed and adopted. No error accumulation, no error list. The context holds one error, the first one
wins, and the diff aborts. This removes a whole layer of machinery.

It does require one distinction to be explicit, because collapsing it would be wrong:

- **Errors abort the diff.** Malformed structure, and the answers to Q4, Q9 and Q10. Unterminated
  quote at end of file, file too large, duplicate key, column order mismatch, missing key column,
  column header row not locatable.
- **Validation findings are output, not errors.** A `REQUIRED` column with an empty cell, a value
  longer than its `VARCHAR(n)`, a value that does not parse as its declared `DECIMAL(p,s)`. These are
  *the point of running the comparison*, so aborting on the first one would hide the other 400 and
  make the feature useless. They flow through the cursor as flags on the affected cell and are
  counted in the summary.

**Assumption flagged for your confirmation:** ragged rows are treated as an error, except that a row
differing only by *empty* trailing fields is normalized rather than rejected. Excel routinely emits
trailing empty columns, and rejecting those files would be a support burden for no benefit, whereas a
row with a genuinely wrong field count means the file is broken and a diff of it would be misleading.

### 13.6 Q6, Java 21 means JNI, not FFM. Correction

**This corrects an earlier claim.** Section 9 and an earlier answer recommended `java.lang.foreign`.
That was written assuming JDK 22 or later. On **JDK 21 the FFM API is a preview feature** under
JEP 442, so using it requires `--enable-preview` on every consuming application, and the API was not
frozen until JEP 454 in JDK 22. A library cannot reasonably force its consumers to enable preview
features or pin an unstable API.

So the Java 21 binding is **hand written JNI**. The public Java API is defined as an interface with
the implementation selected at runtime, so an FFM implementation can be added for JDK 22+ later
without any change visible to consumers.

One JNI specific design point that affects the row feed. A naive binding would call across JNI once
per field, and at roughly 1.8 million cells for a 15 MB table the call overhead plus per-string UTF-16
to UTF-8 conversion would dominate the diff itself. Instead the Java side fills a **direct
`ByteBuffer` with a length prefixed batch of rows** and makes one JNI call per batch, default 1,000
rows. The native side reads the batch straight out of the mapped buffer with no copy. This keeps the
`ResultSet` path close to the throughput of the CSV path, and it is the reason the row feed ABI takes
a batch rather than a field.

The multi platform jar bundles `linux-x86_64`, `linux-aarch64`, `darwin-aarch64`, `darwin-x86_64` and
`windows-x86_64`, extracted to a temp path and loaded on first use.

### 13.7 Q7, the browser fetch contract, decided

You asked me to decide what the library handles. The decisions:

**The library never performs the fetch.** Authentication, headers, cookies, tokens, retries, CORS
and base URLs are application concerns that differ per deployment and change over time. The library
accepts what the application already has:

```ts
type IbhaCsvSource =
  | Uint8Array | ArrayBuffer | Blob | File        // the local attachment case
  | ReadableStream<Uint8Array>                    // response.body
  | (() => Promise<Response | ReadableStream<Uint8Array> | Uint8Array>);
```

The callback form is the recommended one for the server side file, because the caller assembles the
whole request and simply hands us the `Response`. We read `response.body` and pull lazily.

**Content-Type is ignored entirely, and `application/octet-stream` is fully supported.** Throwing on
a Content-Type would be actively harmful: S3 presigned URLs commonly serve `application/octet-stream`,
and plenty of endpoints send `text/plain` or `application/vnd.ms-excel` for a CSV. The library reads
bytes, so the declared type carries no information it needs.

**But the first bytes are sniffed for one specific, common failure.** If a response begins with
`<!DOCTYPE`, `<html`, or a JSON object containing an `error` key, the library fails immediately with
"expected CSV, received what appears to be an HTML error page or a JSON error response". This is the
single most common integration bug, an expired session or a 401 returning a login page with a 200
status, and without the check it surfaces as a bizarre parse error hundreds of lines in. This is a
diagnostic aid, not Content-Type enforcement, and it can be disabled.

**Lazy streaming is the primary path**, one shot bytes are supported and are simply the degenerate
case. `Content-Length` is optional; the size limit from 13.4 is enforced as bytes arrive, which is
also what protects against a chunked response with no declared length.

**Optionally, gzip in the body rather than in `Content-Encoding`.** For a `.csv.gz` object served
from S3, the caller sets `decompress: 'gzip'` and we pipe through `DecompressionStream`. Transparent
`Content-Encoding` is already handled by the browser and needs nothing from us.

### 13.8 Q8, the source file is schema-authoritative and the target header count is auto-detected

Your answer: all four header rows are always present in the source, but a sales user may upload a
file that kept only the column name row.

**The source file is authoritative for all schema metadata**: which columns are `KEY`, which are
`REQUIRED`, and the declared types. Metadata in the target is never used to drive comparison.

**The target header row count defaults to `'auto'`.** Detection is not a guess, because we already
know what the column names are from the source:

1. Read the first 8 rows of the target.
2. Find the row whose cell values equal the source's column name row.
3. Rows above it are header metadata and are skipped. If the match is at row 1, the file is
   names-only and inherits everything from the source.
4. If no row in the first 8 matches, fail with `IBHA_CSVD_ERR_NO_HEADER`: "could not locate the
   column header row in the uploaded file; expected a row matching the source columns".

Where the target does carry metadata rows and they disagree with the source, the source wins and the
disagreement is reported as a finding, not an error. The uploaded file's opinion about which column
is a key is not something we should act on.

`header.target` can be pinned to `1` or `4` explicitly to skip detection.

### 13.9 Q9, duplicate keys are an error

Adopted, and `duplicateKeys: 'pair-in-order'` is removed rather than kept as an option. Checked on
both sides. The error names the offending key and both row numbers, because "duplicate key" without
the location is useless in a 90,000 row file:

```
IBHA_CSVD_ERR_DUPLICATE_KEY: "duplicate key (ACC-10231, 2026-01) in uploaded file at rows 4,812 and 60,144"
```

Section 6.3's pairing machinery is deleted from the plan. With a unique key guaranteed by validation,
the matcher gets simpler.

### 13.10 Q10, column reordering is never allowed

Adopted as a hard error with no flag. The target's column order must equal the source's column order
exactly. `columns.allowReorder` is removed from the API.

**Assumption flagged for your confirmation:** added and removed columns are also errors by default,
on the same reasoning, since "whatever order the source file has must be continued" implies the same
set in the same positions. A genuine schema change is then handled by the source file changing first,
which is the correct order of operations for data destined for a fixed table. Section 6.6's
`allowAdded` and `allowRemoved` flags are retained but default to false. Tell me if a salesman
appending a column should instead be a warning.

### 13.11 Q11, desktop browsers only

Consequences:

- WASM SIMD is baseline in every evergreen desktop browser, so the SIMD build is the expected runtime
  path rather than an enhancement. The scalar build is retained as a compile target because it is the
  **differential oracle** for testing the SIMD parser, not because we expect to ship it.
- No mobile memory ceiling to design around, which is what makes the 150 MB limit in 13.4 tenable.
- `SharedArrayBuffer` remains opt-in and feature detected. Desktop-only does not imply the embedding
  sites can set COOP and COEP.

### 13.12 Net effect on the plan

Dropped entirely: intra-file parallel parsing, out of core operation, error accumulation, duplicate
key pairing, column reorder support, the FFM binding for now, and the browser digest mode.

Added: the cursor plus emitter output layer with an HTML emitter and its XSS test corpus, target
header auto-detection, the JNI batch feed, and the response sniffing diagnostic.

The plan is meaningfully smaller than it was in revision 3.

### 13.13 The five value semantics, answered 2026-09-04

These five were implemented during phases 3 to 6 as **flagged assumptions**: the
code made a choice, the choice was recorded rather than decided, and every handoff
from Phase 3 onward carried the same list forward with the instruction to flag them
rather than re-decide them. That was correct while the engine was being built and
stopped being correct once it was.

They are answered now, and they are decisions rather than assumptions. The evidence
each was answered against is in `docs/ASSUMPTIONS-B.md`, produced by
`scripts-and-commands/confirm_assumptions.py`, which runs the real engine on the
smallest input that makes each rule observable. Four were right as built. One was a
defect.

**13.13.1 SQL NULL is an empty field.** CSV cannot express the difference between a
null and an empty string, so making the JDBC and JSON sides distinguish them would
report differences the file side could never agree with. A null in the last column
is written as a trailing delimiter, and the absence of content between a delimiter
and the line ending is an empty cell, under LF and CRLF alike.

**13.13.2 A TIMESTAMP's fractional seconds carry no significant trailing zeros.**
`14:22:05` equals `14:22:05.000`, and `14:22:05.100` equals `14:22:05.1`. This is
the rule `numeric` already applies to DECIMAL, for the same reason: a driver
rendering seconds only and an export written to fixed millisecond precision
describe the same instant, and calling them different reports every row of a table
as modified.

This one was **a defect, not a decision**, and was fixed in `normalize.c` rather
than documented. It is not gated on an option, because it is what the type means.
It rides on `IBHA_CSVD_DATE_EXACT`, whose meaning is now byte equality on the
canonical form, so it required no ABI change. **The date is still not parsed**:
`31/01/2026` still differs from `2026-01-31`, which remains 13.5's position and
remains `IBHA_CSVD_DATE_VALUE`, still unimplemented.

**13.13.3 Ragged rows are asymmetric.** A row whose only excess is empty trailing
fields is normalized and counted; a row with too few fields is
`IBHA_CSVD_ERR_RAGGED_ROW`. Excel emits trailing empty columns routinely, and
nothing is lost by dropping an empty field past the declared width. A short row
means the file is broken, and padding it would hide the corruption a diff exists to
surface. The two halves describe one well formed file from opposite sides: a
producer that writes a null last field as a trailing delimiter never emits a short
row.

**13.13.4 Row numbers are 1 based record numbers, not physical line numbers.** A
record spanning several physical lines is one row. The count includes the header
records, so the first data row of a four row header is record 5. This holds in
every error message, in both bindings' row records, and in all four emitters.

**The justification is Excel, not semantics.** These CSVs are viewed and edited in
Excel, Excel parses the format properly, and so a quoted field containing a newline
is displayed as one grid row with a line break inside the cell. Excel's row numbers
*are* record numbers, header rows included. The engine's numbering therefore already
matches the tool the reports are read against, and reporting a physical line number
would mean reporting the number that does not match what the reader is looking at.
That is why 13.13.4 is a decision and not a compromise, and why the physical line
number proposal (`specs/03-remaining-tasks.md` B4a) was rejected rather than
deferred.

**One documented exception.** A blank line in the middle of a file makes the two
numberings drift by one, and each further blank line adds another, because the
engine treats a wholly empty line as not a record while Excel shows it as an empty
grid row. This cannot be resolved by counting blank lines as records: a blank line
is one empty field, which in a file of three or more columns is a short row, and
13.13.3 refuses short rows. A trailing blank line at end of file causes no drift,
since no record follows it. `stats.blank_lines` is public, so a caller that meets
this can explain it without an engine change.

**13.13.5 `VARCHAR(n)` counts characters, not bytes.** `café` is four characters in
five bytes and does not violate `VARCHAR(5)`. The data these schemas describe is
UTF-8 text and a byte limit is not what `VARCHAR(5)` means to the people reading
the report. `char_count` in `core/src/validate.c` counts UTF-8 lead bytes, and both
bindings inherit it because both run this engine.

Databases disagree about this and the disagreement is the reason it was worth
asking: Oracle defaults to BYTE semantics unless the column was declared with CHAR
semantics, while SQL Server `NVARCHAR(n)` and PostgreSQL `varchar(n)` count
characters. A source that counted bytes would make the diff pass a value the target
then refuses on insert.
