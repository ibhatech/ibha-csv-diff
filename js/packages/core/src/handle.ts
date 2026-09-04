/**
 * The random access consumer of spec 4.2: a cursor consumer that retains an
 * index so a virtualized view can seek.
 *
 * It is deliberately opt in and separate from the streaming walk, because it is
 * the one thing in this package whose memory grows with the diff. Spec 13.3 puts
 * it this way: the report index array is not built at all unless a consumer asks
 * for random access. A JSONL export, a summary and a pass/fail check never ask.
 *
 * **What it retains, and why that is the whole design.** One drain of the cursor
 * fills a handful of typed arrays: the kind, the moved flag, the move distance,
 * the two row numbers, and the per cell flag bytes. That is 14 bytes plus one
 * byte per compared column per report row, so a 90,000 row diff over 12 columns
 * costs about 2.3 MB. What it deliberately does not retain is a single cell
 * value: those stay in engine memory and are decoded for the fifty rows the view
 * is about to paint, which is the difference between 2 MB and the million object
 * heap the engine exists to avoid.
 *
 * The alternative was to re-walk the cursor to reach row N, which is O(N) per
 * seek and turns a scroll to the bottom of a 90,000 row report into 90,000 row
 * comparisons.
 */

import { C } from './abi.ts';
import type { DiffEngine, RawRow } from './engine.ts';
import type {
  CellDiffMode,
  DiffRow,
  RowReadOptions,
  TextSegment,
} from './types.ts';

/**
 * The parallel array form, for a virtualized table that wants to bind straight to
 * typed arrays rather than allocate a row object per visible row. Spec 4.2 calls
 * this `CompactRowPage`.
 */
export interface CompactRowPage {
  offset: number;
  count: number;
  /** 0 unchanged, 1 modified, 2 added, 3 deleted. */
  kinds: Uint8Array;
  moved: Uint8Array;
  moveDistance: Int32Array;
  /** 0 means the row has no counterpart on that side; real numbers are 1 based. */
  sourceRows: Uint32Array;
  targetRows: Uint32Array;
  /** `count * columns` cell flag bytes, row major. */
  cellFlags: Uint8Array;
  columns: number;
}

/** How many segment results to keep. A viewport asks for the same cells over and
 *  over as it repaints, and a scroll retires the ones it has left behind, which
 *  is exactly the access pattern an LRU is for. */
const SEGMENT_CACHE_ENTRIES = 4096;

export class DiffIndex {
  private readonly engine: DiffEngine;
  readonly rowCount: number;
  readonly columns: number;

  private readonly kinds: Uint8Array;
  private readonly movedFlags: Uint8Array;
  private readonly moveDist: Int32Array;
  private readonly srcRows: Uint32Array;
  private readonly tgtRows: Uint32Array;
  private readonly flags: Uint8Array;

  /**
   * Memoized cell segments, keyed by report position.
   *
   * Phase 3 deliberately left the segment API unmemoized: a memo table keyed by
   * cell is state that grows with the number of cells looked at, which is what
   * the streaming path is forbidden to carry, and in a streaming emitter each
   * cell is computed once and used once so it would buy nothing. Here it is the
   * opposite. This consumer already retains an index and its viewport asks for
   * the same cells repeatedly, so this is the natural owner, and the key is the
   * report position rather than the cell contents.
   */
  private readonly segCache = new Map<number, TextSegment[]>();
  private segMode: CellDiffMode = 'none';
  private segMaxBytes = 0;

  private constructor(engine: DiffEngine, rows: RawRow[], columns: number) {
    this.engine = engine;
    this.columns = columns;
    this.rowCount = rows.length;

    const n = rows.length;
    this.kinds = new Uint8Array(n);
    this.movedFlags = new Uint8Array(n);
    this.moveDist = new Int32Array(n);
    this.srcRows = new Uint32Array(n);
    this.tgtRows = new Uint32Array(n);
    this.flags = new Uint8Array(n * columns);

    for (let i = 0; i < n; i++) {
      const r = rows[i]!;
      this.kinds[i] = r.kind;
      this.movedFlags[i] = r.moved ? 1 : 0;
      this.moveDist[i] = r.moveDistance;
      // 0 is the "no counterpart" sentinel here rather than the engine's
      // 0xFFFFFFFF, because these are 1 based record numbers and 0 is therefore
      // not a value any real row can have. That keeps the arrays readable in a
      // debugger and survives being posted to another thread.
      this.srcRows[i] = r.sourceRow ?? 0;
      this.tgtRows[i] = r.targetRow ?? 0;
      // The three cell counts are deliberately not stored. Each is a count of
      // set bits in the flag bytes this row already keeps, so storing them costs
      // 12 bytes a row, about a third of the index, to save an O(columns) loop
      // over the fifty rows a viewport is painting.
      this.flags.set(r.flags, i * columns);
    }
  }

  /**
   * Drains the cursor once and keeps the index.
   *
   * `changesOnly` filters at build time, so the index positions are report
   * positions in the filtered report. That is what a view scrolling a
   * changes-only report needs, and it means the index and the HTML emitter run
   * under the same filter and agree row for row.
   */
  static build(engine: DiffEngine, read: { changesOnly?: boolean } = {}): DiffIndex {
    const rows: RawRow[] = [];
    for (const raw of engine.rawRows({ changesOnly: read.changesOnly ?? false, copy: true })) {
      rows.push(raw);
    }
    return new DiffIndex(engine, rows, engine.nColumns);
  }

  private assertRange(i: number): void {
    if (!Number.isInteger(i) || i < 0 || i >= this.rowCount) {
      throw new RangeError(`report row ${i} is outside 0..${this.rowCount - 1}`);
    }
  }

  private rawAt(i: number): RawRow {
    const flags = this.flags.subarray(i * this.columns, (i + 1) * this.columns);
    let changed = 0;
    let suppressed = 0;
    let findings = 0;
    for (const f of flags) {
      if (f & C.CELL_CHANGED) changed++;
      if (f & C.CELL_SUPPRESSED) suppressed++;
      if (f & C.CELL_FINDING) findings++;
    }
    return {
      kind: this.kinds[i]!,
      moved: this.movedFlags[i] === 1,
      moveDistance: this.moveDist[i]!,
      sourceRow: this.srcRows[i] === 0 ? null : this.srcRows[i]!,
      targetRow: this.tgtRows[i] === 0 ? null : this.tgtRows[i]!,
      changedCells: changed,
      suppressedCells: suppressed,
      findingCells: findings,
      flags,
    };
  }

  /** One row, decoded on demand out of engine memory. */
  getRow(i: number, read: RowReadOptions = {}): DiffRow {
    this.assertRange(i);
    return this.engine.buildRow(this.rawAt(i), read.includeValues ?? true, read.maxCellBytes ?? 0);
  }

  /** The page the view is about to paint. Clamped rather than throwing, because a
   *  virtualized list routinely asks for a window that runs off the end. */
  getRows(offset: number, count: number, read: RowReadOptions = {}): DiffRow[] {
    const from = Math.max(0, Math.min(offset, this.rowCount));
    const to = Math.max(from, Math.min(from + count, this.rowCount));
    const out: DiffRow[] = [];
    for (let i = from; i < to; i++) out.push(this.getRow(i, read));
    return out;
  }

  /**
   * The same page as parallel typed arrays and no row objects at all.
   *
   * The arrays are copies rather than views into the index, so the page can be
   * transferred to another thread without handing over the index itself, and so
   * a later drain cannot change a page the view is still holding.
   */
  getRowsCompact(offset: number, count: number): CompactRowPage {
    const from = Math.max(0, Math.min(offset, this.rowCount));
    const to = Math.max(from, Math.min(from + count, this.rowCount));
    const n = to - from;
    return {
      offset: from,
      count: n,
      columns: this.columns,
      kinds: this.kinds.slice(from, to),
      moved: this.movedFlags.slice(from, to),
      moveDistance: this.moveDist.slice(from, to),
      sourceRows: this.srcRows.slice(from, to),
      targetRows: this.tgtRows.slice(from, to),
      cellFlags: this.flags.slice(from * this.columns, to * this.columns),
    };
  }

  /** The cell flag byte for one cell, without decoding anything. This is what a
   *  view styles from: changed, suppressed, and the four findings. */
  cellFlags(row: number, col: number): number {
    this.assertRange(row);
    return this.flags[row * this.columns + col] ?? 0;
  }

  /**
   * Intra cell segments for one visible cell, memoized by report position.
   *
   * The cache is keyed on position alone, so a change of mode or of cap has to
   * clear it. Letting the key carry the mode instead would keep entries nobody
   * will ask for again: the mode is a setting of the whole view, not of a cell.
   */
  getCellSegments(
    row: number,
    col: number,
    mode: CellDiffMode = 'word-then-character',
    maxBytes = 0,
  ): TextSegment[] {
    this.assertRange(row);
    if (mode === 'none') return [];
    if (mode !== this.segMode || maxBytes !== this.segMaxBytes) {
      this.segCache.clear();
      this.segMode = mode;
      this.segMaxBytes = maxBytes;
    }

    const key = row * this.columns + col;
    const hit = this.segCache.get(key);
    if (hit !== undefined) {
      // Re-inserting moves it to the end, which is what makes a Map an LRU.
      this.segCache.delete(key);
      this.segCache.set(key, hit);
      return hit;
    }

    const segs = this.engine.cellSegments(
      this.srcRows[row] === 0 ? null : this.srcRows[row]!,
      this.tgtRows[row] === 0 ? null : this.tgtRows[row]!,
      col,
      mode,
      maxBytes,
    );

    if (this.segCache.size >= SEGMENT_CACHE_ENTRIES) {
      const oldest = this.segCache.keys().next();
      if (!oldest.done) this.segCache.delete(oldest.value);
    }
    this.segCache.set(key, segs);
    return segs;
  }

  /** Approximate retained bytes, so a caller can decide whether asking for an
   *  index on this diff was a good idea. */
  get bytesRetained(): number {
    return (
      this.kinds.byteLength +
      this.movedFlags.byteLength +
      this.moveDist.byteLength +
      this.srcRows.byteLength +
      this.tgtRows.byteLength +
      this.flags.byteLength
    );
  }
}

/**
 * Converts a segment's byte offsets into UTF-16 code unit offsets for a decoded
 * string, so it can be handed to `slice` or to a DOM range.
 *
 * The engine's offsets are byte offsets into the logical value, which is what the
 * header says and what a byte oriented engine can produce cheaply. A JS string is
 * UTF-16, so for anything outside ASCII the two disagree, and slicing a decoded
 * string at a byte offset is the classic source of mangled non ASCII text. Doing
 * it once here is the cheapest place for the conversion to live.
 */
export function sliceByBytes(value: string, startByte: number, lenBytes: number): string {
  if (startByte === 0 && lenBytes >= utf8Length(value)) return value;
  let bytes = 0;
  let from = -1;
  for (let i = 0; i <= value.length; ) {
    if (from < 0 && bytes >= startByte) from = i;
    if (from >= 0 && bytes >= startByte + lenBytes) return value.slice(from, i);
    if (i === value.length) break;
    const code = value.codePointAt(i)!;
    const units = code > 0xffff ? 2 : 1;
    bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    i += units;
  }
  return from < 0 ? '' : value.slice(from);
}

function utf8Length(s: string): number {
  let n = 0;
  for (const ch of s) {
    const code = ch.codePointAt(0)!;
    n += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
  }
  return n;
}
