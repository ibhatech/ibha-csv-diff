/**
 * Where the view gets rows from, and the two adapters that exist.
 *
 * The view never holds a diff. It holds a `DiffRowSource`, asks it for the fifty
 * rows it is about to paint, and lets them be collected when they scroll away.
 * What is behind the source is either the retained index in engine memory, which
 * is 14 bytes plus one byte per column per row and not one cell value, or the
 * same index inside a worker. Neither materializes anything.
 *
 * The two adapters are separate functions rather than one that sniffs the handle,
 * and that is deliberate: `DiffHandle.index` takes `{ changesOnly }` while
 * `RemoteDiffHandle.index` takes a bare boolean, so a sniffing adapter that
 * guessed wrong would post `changesOnly: { changesOnly: true }` across the worker
 * boundary, which is truthy, which means it would silently filter a report the
 * caller asked not to filter. Making the caller name which one they have costs a
 * word and removes the failure.
 *
 * Both adapters are typed structurally rather than against the imported classes,
 * so this package keeps no runtime import of the engine except `sliceByBytes`,
 * and so a test can substitute a fake without a wasm instance.
 */

import type {
  CellDiffMode,
  CompactRowPage,
  DiffRow,
  RowReadOptions,
  TextSegment,
} from '@ibhatech/csvdiff-core';

export type MaybePromise<T> = T | Promise<T>;

export interface DiffRowSourceInfo {
  /** Report rows after the filter the source was built with. */
  rowCount: number;
  /** Compared columns. Not the source file's column count: under the column
   *  policy of spec 6.6 a report row carries the columns the two files share. */
  columns: number;
  /** What the retained index costs, so a caller can report it. */
  bytesRetained: number;
}

export interface DiffRowSource {
  /** Compared column names, in the source file's order. */
  readonly columns: readonly string[];
  /**
   * Builds the retained index if it is not built, and reports what it cost.
   *
   * Idempotent and cheap after the first call. It is separate from construction
   * because it is the one thing in the whole pipeline whose memory grows with the
   * diff, per spec 13.3, and because in the worker case it is asynchronous.
   */
  ready(): MaybePromise<DiffRowSourceInfo>;
  getRows(offset: number, count: number, read?: RowReadOptions): MaybePromise<DiffRow[]>;
  getRowsCompact(offset: number, count: number): MaybePromise<CompactRowPage>;
  getCellSegments(
    row: number,
    col: number,
    mode: CellDiffMode,
    maxBytes: number,
  ): MaybePromise<TextSegment[]>;
}

/* ------------------------------------------------------------------ local -- */

/** The shape of `DiffIndex`, stated structurally. */
export interface DiffIndexLike {
  readonly rowCount: number;
  readonly columns: number;
  readonly bytesRetained: number;
  getRows(offset: number, count: number, read?: RowReadOptions): DiffRow[];
  getRowsCompact(offset: number, count: number): CompactRowPage;
  getCellSegments(row: number, col: number, mode?: CellDiffMode, maxBytes?: number): TextSegment[];
}

/** The shape of `DiffHandle`, stated structurally. */
export interface LocalDiffHandleLike {
  readonly columns: readonly string[];
  index(read?: { changesOnly?: boolean }): DiffIndexLike;
}

/**
 * A source over a handle in this thread.
 *
 * The index is built on the first `ready`, not here, so constructing a source in
 * a render pass costs nothing. `DiffHandle` caches the index per filter, so
 * rebuilding a source for the same filter does not drain the cursor again.
 */
export function localRowSource(
  handle: LocalDiffHandleLike,
  options: { changesOnly?: boolean } = {},
): DiffRowSource {
  const changesOnly = options.changesOnly ?? false;
  let index: DiffIndexLike | null = null;
  const idx = (): DiffIndexLike => (index ??= handle.index({ changesOnly }));

  return {
    columns: handle.columns,
    ready: () => {
      const i = idx();
      return { rowCount: i.rowCount, columns: i.columns, bytesRetained: i.bytesRetained };
    },
    getRows: (offset, count, read) => idx().getRows(offset, count, read),
    getRowsCompact: (offset, count) => idx().getRowsCompact(offset, count),
    getCellSegments: (row, col, mode, maxBytes) => idx().getCellSegments(row, col, mode, maxBytes),
  };
}

/* ----------------------------------------------------------------- remote -- */

/** The shape of `RemoteDiffHandle`, stated structurally. */
export interface RemoteDiffHandleLike {
  readonly columns: readonly string[];
  index(changesOnly?: boolean): Promise<DiffRowSourceInfo>;
  getRows(offset: number, count: number, read?: RowReadOptions): Promise<DiffRow[]>;
  getRowsCompact(offset: number, count: number): Promise<CompactRowPage>;
  getCellSegments(
    row: number,
    col: number,
    mode?: CellDiffMode,
    maxBytes?: number,
  ): Promise<TextSegment[]>;
}

/**
 * A source over a handle in a worker, which is the default for anything
 * interactive: a 400 ms diff on the main thread is a 400 ms frozen UI, and at the
 * 150 MB ceiling of spec 13.4 it is seconds.
 *
 * `ready` is memoized on the promise rather than on its result, so a component
 * that mounts, renders three times and asks each time issues one request.
 */
export function remoteRowSource(
  handle: RemoteDiffHandleLike,
  options: { changesOnly?: boolean } = {},
): DiffRowSource {
  const changesOnly = options.changesOnly ?? false;
  let pending: Promise<DiffRowSourceInfo> | null = null;

  return {
    columns: handle.columns,
    ready: () => (pending ??= handle.index(changesOnly)),
    getRows: (offset, count, read) => handle.getRows(offset, count, read),
    getRowsCompact: (offset, count) => handle.getRowsCompact(offset, count),
    getCellSegments: (row, col, mode, maxBytes) =>
      handle.getCellSegments(row, col, mode, maxBytes),
  };
}

/** True for a thenable, so the view model can install a synchronous result in the
 *  same tick instead of paying a microtask and a repaint for a local source. */
export function isPromise<T>(v: MaybePromise<T>): v is Promise<T> {
  return typeof (v as Promise<T>)?.then === 'function';
}
