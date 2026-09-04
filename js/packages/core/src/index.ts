/**
 * @ibhatech/csvdiff-core
 *
 * Public surface of the JavaScript binding. This package is one binding over the
 * C ABI, not the engine itself: see specs/02-solution-proposal.md sections 2.6.2
 * and 13.7 in the parent project.
 *
 * Two rules the API is built on, both of which exist to avoid recreating in
 * JavaScript the heap explosion the WASM engine was chosen to prevent:
 *
 *   1. Bytes in, never strings. A JS string input costs an encode pass plus a
 *      transient copy of the whole file.
 *   2. Never return a materialized diff. `compare` yields a handle over engine
 *      memory whose primary interface is a lazy cursor. A 90,000 row diff as
 *      plain JS objects would be a million allocations, which is the thing we are
 *      avoiding. Random access is available and is opt in, because it is the one
 *      thing here whose memory grows with the diff.
 */

export {
  toByteSource,
  sniffNotCsv,
  IbhaCsvError,
  DEFAULT_MAX_BYTES,
  type IbhaCsvSource,
  type IbhaCsvErrorCode,
  type SourceOptions,
  type ByteSource,
} from './source.ts';

export {
  DIFF_ROW_SCHEMA_VERSION,
  CHANGE_KINDS,
  CELL_DIFF_MODES,
  SEGMENT_OPS,
  EMIT_FORMATS,
  type ChangeKind,
  type CellDiffMode,
  type ComparisonOptions,
  type CsvDiffOptions,
  type DiffCell,
  type DiffRow,
  type DiffSummary,
  type EmitFormat,
  type EmitOptions,
  type FindingKind,
  type HeaderOptions,
  type MatchingOptions,
  type RowFinding,
  type RowReadOptions,
  type SchemaFinding,
  type SegmentOp,
  type TextSegment,
} from './types.ts';

export { C as ENGINE_CONSTANTS } from './abi.ts';
export { Engine, simdSupported, type LoadOptions } from './module.ts';
export { DiffIndex, sliceByBytes, type CompactRowPage } from './handle.ts';
export { PushSource } from './push.ts';
export { compareInWorker, RemoteDiffHandle } from './workerClient.ts';

import { DiffEngine } from './engine.ts';
import { DiffIndex } from './handle.ts';
import { Engine } from './module.ts';
import type { LoadOptions } from './module.ts';
import type { IbhaCsvSource } from './source.ts';
import type {
  CsvDiffOptions,
  DiffRow,
  DiffSummary,
  EmitFormat,
  EmitOptions,
  RowReadOptions,
} from './types.ts';

/* ------------------------------------------------------- module loading -- */

let loadOptions: LoadOptions = {};
let engineCache: Promise<Engine> | null = null;

/**
 * Where to find the wasm module, when the default is wrong.
 *
 * The default resolves `../wasm/<name>` relative to this module, which is what
 * bundlers understand and what works from the published package. A host that
 * serves the module from a CDN path, or that already has the bytes, says so here
 * once instead of at every call site. Call before the first `compare`.
 */
export function configure(options: LoadOptions): void {
  loadOptions = options;
  engineCache = null;
}

/** Compiles the module, once. Instances are per diff; see `Engine`. */
export function loadEngine(): Promise<Engine> {
  engineCache ??= Engine.load(loadOptions);
  return engineCache;
}

/* -------------------------------------------------------------- compare -- */

/**
 * The result of one comparison.
 *
 * `dispose` frees the engine context. Forgetting to is not a leak in the way it
 * would be with a shared instance: this handle owns a whole `WebAssembly.Instance`
 * and its linear memory, so dropping the last reference lets the garbage
 * collector reclaim all of it. That is a consequence of instantiating per diff
 * rather than sharing one instance, and it is why there is no FinalizationRegistry
 * here. Disposing is still worth doing, because it returns the memory at a moment
 * you choose rather than one the collector chooses.
 */
export class DiffHandle {
  private readonly engine: DiffEngine;
  private indexCache: DiffIndex | null = null;
  private indexChangesOnly = false;

  /** @internal */
  constructor(engine: DiffEngine) {
    this.engine = engine;
  }

  /** The compared columns, in the source's order. Under the column policy of
   *  spec 6.6 this is the columns the two files share, which is not always the
   *  source file's column list; read the width from here, not from the header. */
  get columns(): readonly string[] {
    return this.engine.columns;
  }

  /** Bytes the engine has reserved, so a batch driver can size its concurrency
   *  per spec 2.6.5. */
  get bytesReserved(): number {
    return this.engine.bytesReserved;
  }

  /** Counts, findings and column findings. Constant memory whatever the size of
   *  the diff, and computed by the same emitter that writes the summary report,
   *  so the two cannot disagree. */
  summary(): DiffSummary {
    return this.engine.summary();
  }

  /**
   * The report, one row at a time.
   *
   * This is the primary interface and the only one whose memory does not grow
   * with the diff: the engine's row buffer is reused and each decoded row is
   * garbage as soon as the consumer is done with it.
   */
  rows(read?: RowReadOptions): Generator<DiffRow> {
    return this.engine.rows(read);
  }

  /**
   * Builds and keeps the report index, so a view can seek.
   *
   * Costs one drain of the cursor and about 14 bytes plus one byte per compared
   * column per report row. Cached: asking twice with the same filter returns the
   * same index rather than draining again.
   */
  index(read: { changesOnly?: boolean } = {}): DiffIndex {
    const changesOnly = read.changesOnly ?? false;
    if (!this.indexCache || this.indexChangesOnly !== changesOnly) {
      this.indexCache = DiffIndex.build(this.engine, { changesOnly });
      this.indexChangesOnly = changesOnly;
    }
    return this.indexCache;
  }

  /**
   * Runs one of the engine's emitters and returns its bytes.
   *
   * The HTML emitter is for bounded output: `changesOnly`, or `maxRows`, or a
   * page at a time. A 90,000 row diff rendered to one HTML string is tens of
   * megabytes of DOM and will not scroll acceptably, which is what the index
   * above and the virtualized view of spec section 8 are for. Both consume the
   * same cursor, so they agree by construction.
   */
  emit(format: EmitFormat, options?: EmitOptions): Uint8Array {
    return this.engine.emit(format, options);
  }

  /**
   * The same report as a stream, for piping to a file or a response body.
   *
   * Honest about what this is: the report is produced whole inside the engine
   * first and then handed out in pieces, so it bounds what JavaScript holds and
   * not what the engine does. `ibha_csvd_emit` drains the whole diff in one call
   * and there is no way for a host to pull a bounded piece of it; a resumable
   * emitter is the fix and it belongs in the engine.
   */
  emitStream(format: EmitFormat, options?: EmitOptions): ReadableStream<Uint8Array> {
    const bytes = this.emit(format, options);
    const CHUNK = 256 * 1024;
    let off = 0;
    return new ReadableStream<Uint8Array>({
      pull(controller) {
        if (off >= bytes.length) {
          controller.close();
          return;
        }
        const end = Math.min(off + CHUNK, bytes.length);
        controller.enqueue(bytes.subarray(off, end));
        off = end;
      },
    });
  }

  dispose(): void {
    this.indexCache = null;
    this.engine.dispose();
  }
}

/**
 * Compares two CSV sources.
 *
 * Both sides are streamed into the engine as they arrive, so parsing overlaps the
 * download rather than following it: at the p90 the transfer costs seconds and
 * the parse costs tens of milliseconds, so the diff is ready within a few
 * milliseconds of the last byte landing.
 *
 * The source file is authoritative for all schema metadata, and the uploaded
 * file's header row count is detected against it rather than guessed, per spec
 * 13.8. A sales user who kept only the column name row is the case that exists
 * for.
 */
export async function compare(
  source: IbhaCsvSource,
  target: IbhaCsvSource,
  options: CsvDiffOptions = {},
): Promise<DiffHandle> {
  const engine = await loadEngine();
  return new DiffHandle(await DiffEngine.compare(engine, source, target, options));
}
