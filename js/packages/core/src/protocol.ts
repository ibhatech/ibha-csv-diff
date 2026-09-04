/**
 * The message protocol between the main thread and the worker.
 *
 * Spec 4.3 decided message passing by default and `SharedArrayBuffer` as an opt
 * in fast path, because zero copy access to the worker's wasm memory needs the
 * embedding site to send `Cross-Origin-Opener-Policy: same-origin` and
 * `Cross-Origin-Embedder-Policy: require-corp`, and an npm library cannot require
 * that of every consumer. So the main thread asks for the rows it is about to
 * paint and the worker serializes that page. For a 50 row page that is a few
 * kilobytes and about a tenth of a millisecond.
 *
 * Two things keep that cheap. Pages of the compact form travel as transferable
 * typed arrays rather than as row objects, and emitter output travels as one
 * transferred `ArrayBuffer` rather than as a string.
 *
 * **Every message carries a sequence number and replies are matched on it.** The
 * worker starts each request as soon as it arrives rather than finishing one
 * before reading the next, which is the point: the chunk pump has to keep moving
 * while the comparison it is feeding is still running. So replies come back out
 * of order, and a queue would pair the comparison's reply with the pump's.
 *
 * One worker holds exactly one comparison, so there is no comparison id here.
 * That is a consequence of instantiating the engine per diff: linear memory only
 * grows, so a worker shared between comparisons would carry the high water mark
 * of all of them.
 */

import type {
  CellDiffMode,
  CsvDiffOptions,
  DiffRow,
  DiffSummary,
  EmitFormat,
  EmitOptions,
  RowReadOptions,
  TextSegment,
} from './types.ts';
import type { CompactRowPage } from './handle.ts';
import type { LoadOptions } from './module.ts';

export type Side = 'source' | 'target';

/**
 * The options that survive `postMessage`.
 *
 * `signal` does not: an `AbortSignal` is not structured cloneable, and it would
 * be the wrong thing to send anyway because aborting is the main thread stopping
 * its pump. It is dropped explicitly rather than silently by the clone algorithm.
 */
export type WireOptions = Omit<CsvDiffOptions, 'signal'>;

export type RequestBody =
  | { t: 'open'; options: WireOptions; load: LoadOptions; sizeHints: Partial<Record<Side, number>> }
  | { t: 'chunk'; side: Side; bytes: ArrayBuffer }
  | { t: 'end'; side: Side }
  | { t: 'fail'; side: Side; message: string }
  /** Resolves when the comparison has finished, which is what the pump waits on. */
  | { t: 'ready' }
  | { t: 'summary' }
  | { t: 'index'; changesOnly: boolean }
  | { t: 'rows'; offset: number; count: number; read: RowReadOptions }
  | { t: 'compact'; offset: number; count: number }
  | { t: 'segments'; row: number; col: number; mode: CellDiffMode; maxBytes: number }
  | { t: 'emit'; format: EmitFormat; options: EmitOptions }
  | { t: 'drain'; read: RowReadOptions; batch: number }
  | { t: 'dispose' };

export type ResponseBody =
  | { t: 'ready'; columns: string[]; bytesReserved: number }
  | { t: 'summary'; summary: DiffSummary }
  | { t: 'index'; rowCount: number; columns: number; bytesRetained: number }
  | { t: 'rows'; rows: DiffRow[] }
  | { t: 'compact'; page: CompactRowPage }
  | { t: 'segments'; segments: TextSegment[] }
  | { t: 'emit'; bytes: ArrayBuffer }
  | { t: 'batch'; rows: DiffRow[]; done: boolean }
  | { t: 'ack' }
  | { t: 'error'; code: string; message: string; status?: string };

export type Request = RequestBody & { seq: number };
export type Response = ResponseBody & { seq: number };

/** Narrows a reply to the shape the request asked for. */
export type ReplyOf<T extends ResponseBody['t']> = Extract<ResponseBody, { t: T }>;

/** The transferable parts of a compact page, so the structured clone moves them
 *  instead of copying them. */
export function pageTransfers(page: CompactRowPage): Transferable[] {
  return [
    page.kinds.buffer,
    page.moved.buffer,
    page.moveDistance.buffer,
    page.sourceRows.buffer,
    page.targetRows.buffer,
    page.cellFlags.buffer,
  ] as Transferable[];
}
