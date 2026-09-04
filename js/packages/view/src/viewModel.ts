/**
 * The headless view model: everything a virtualized diff table does that is not
 * putting elements on a screen.
 *
 * **The invariant this file exists to hold.** The view never materializes the
 * diff. It works one page of fifty rows at a time, keeps a handful of pages, and
 * lets the rest be collected. Two caches, sized very differently on purpose:
 *
 *   - **decoded value pages, four of them, two hundred rows.** These are the
 *     expensive ones: a row object and a cell object per cell, which at 12
 *     columns is thirteen allocations a row. Keeping four pages means a scroll of
 *     any distance retains a bounded number of them, and it is why scrolling to
 *     the bottom of a 90,000 row report costs the same as scrolling to row 200.
 *   - **compact structure pages, sixty four of them.** One byte per cell and no
 *     strings at all, so 64 pages over 12 columns is 38 KB. They are what the
 *     kinds, the row numbers and the per cell flags come from, which means a page
 *     whose values have been evicted still repaints in the right colours with the
 *     right row numbers the instant it scrolls back, and only its text has to be
 *     fetched again. That is the reason `getRowsCompact` is the form to bind to
 *     rather than a nicety about typed arrays.
 *
 * A local source answers in the same tick, so a scroll paints complete rows with
 * no intermediate state at all; the placeholder path exists for the worker, where
 * the answer is a message away.
 */

import type { CellDiffMode, CompactRowPage, DiffRow, TextSegment } from '@ibhatech/csvdiff-core';

import { DEFAULT_CLASS_PREFIX } from './classes.ts';
import {
  presentRow,
  structureFromCompact,
  type OldValuePosition,
  type ViewRow,
} from './rowModel.ts';
import { isPromise, type DiffRowSource } from './source.ts';
import {
  computeWindow,
  pagesFor,
  scrollToShow,
  slotsPerRow,
  type RowLayout,
} from './virtual.ts';

export const PAGE_SIZE = 50;
const VALUE_PAGES = 4;
const COMPACT_PAGES = 64;
const SEGMENT_ENTRIES = 2048;

export interface ViewModelOptions {
  layout?: RowLayout;
  oldValuePosition?: OldValuePosition;
  /** Height of one physical row in pixels. In `stacked` and `unified` a report
   *  row occupies two of them; see `slotsPerRow`. */
  rowHeight?: number;
  overscan?: number;
  classPrefix?: string;
  /** Intra cell diffing. Off by default: it is a per cell call into the engine
   *  and most reports are read without it. */
  cellDiff?: CellDiffMode;
  /** Truncate a decoded value at this many logical bytes. A single 2 MB cell in a
   *  90,000 row file is otherwise a 2 MB string in the viewport. */
  maxCellBytes?: number;
  /** Column indices the schema declares as KEY, for the sticky columns of 8.5. */
  keyColumns?: readonly number[];
  /** Column width in pixels, by column index. Anything absent uses `columnWidth`. */
  columnWidths?: Readonly<Record<number, number>>;
  defaultColumnWidth?: number;
}

export interface ViewSnapshot {
  status: 'idle' | 'loading' | 'ready' | 'error';
  error: Error | null;
  /** Report rows after the filter the source was built with. */
  rowCount: number;
  columns: readonly string[];
  /** First and last report row in the window, and the geometry the scroll
   *  container needs. */
  start: number;
  end: number;
  offsetTop: number;
  totalHeight: number;
  rowHeight: number;
  /** The height of one report row's slot, which is two physical rows in
   *  `stacked` and `unified`. This is the unit the window is measured in. */
  slotHeight: number;
  /** Where the model believes the scroll container is. It differs from the
   *  element's own `scrollTop` only just after keyboard navigation moved it, and
   *  that difference is the signal for the renderer to move the element. */
  scrollTop: number;
  layout: RowLayout;
  oldValuePosition: OldValuePosition;
  /** The physical rows to render, in order. In `sideBySide` each report row
   *  contributes one row per pane. */
  rows: ViewRow[];
  /** True while any visible page is still waiting on the worker. */
  pending: boolean;
  focus: { row: number; column: number } | null;
}

type Listener = () => void;

/** A Map used as an LRU: re-inserting moves an entry to the end, so the oldest
 *  key is always the first one the iterator yields. */
function touch<K, V>(map: Map<K, V>, key: K, value: V, limit: number): void {
  map.delete(key);
  map.set(key, value);
  while (map.size > limit) {
    const oldest = map.keys().next();
    if (oldest.done) break;
    map.delete(oldest.value);
  }
}

export class DiffViewModel {
  private source: DiffRowSource;
  private listeners = new Set<Listener>();

  private compact = new Map<number, CompactRowPage>();
  private values = new Map<number, DiffRow[]>();
  private inFlight = new Set<string>();
  private segments = new Map<number, TextSegment[]>();

  private scrollTop = 0;
  private viewportHeight = 0;
  private rowCount = 0;
  private nColumns = 0;
  private status: ViewSnapshot['status'] = 'idle';
  private error: Error | null = null;

  private snapshotCache: ViewSnapshot | null = null;

  readonly classPrefix: string;
  private layout: RowLayout;
  private oldValuePosition: OldValuePosition;
  private rowHeight: number;
  private overscan: number;
  private cellDiff: CellDiffMode;
  private maxCellBytes: number;
  private keyColumns: Set<number>;
  private widths: Map<number, number>;
  private defaultWidth: number;
  private focus: { row: number; column: number } | null = null;

  constructor(source: DiffRowSource, options: ViewModelOptions = {}) {
    this.source = source;
    this.layout = options.layout ?? 'inline';
    this.oldValuePosition = options.oldValuePosition ?? 'below';
    this.rowHeight = options.rowHeight ?? 28;
    this.overscan = options.overscan ?? 8;
    this.classPrefix = options.classPrefix ?? DEFAULT_CLASS_PREFIX;
    this.cellDiff = options.cellDiff ?? 'none';
    this.maxCellBytes = options.maxCellBytes ?? 0;
    this.keyColumns = new Set(options.keyColumns ?? []);
    this.widths = new Map(
      Object.entries(options.columnWidths ?? {}).map(([k, v]) => [Number(k), v]),
    );
    this.defaultWidth = options.defaultColumnWidth ?? 160;
  }

  /* ------------------------------------------------------------ subscribe -- */

  subscribe = (fn: Listener): (() => void) => {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  };

  private changed(): void {
    this.snapshotCache = null;
    for (const fn of this.listeners) fn();
  }

  /* ----------------------------------------------------------------- data -- */

  /**
   * Builds the index and learns the row count.
   *
   * Safe to call on every render: the source memoizes, and a second call while
   * the first is in flight joins it rather than issuing another.
   */
  start(): void {
    if (this.status !== 'idle') return;
    const info = this.source.ready();
    if (!isPromise(info)) {
      this.rowCount = info.rowCount;
      this.nColumns = info.columns;
      this.status = 'ready';
      this.snapshotCache = null;
      return;
    }
    this.status = 'loading';
    info.then(
      (r) => {
        this.rowCount = r.rowCount;
        this.nColumns = r.columns;
        this.status = 'ready';
        this.changed();
      },
      (err: unknown) => {
        this.error = err instanceof Error ? err : new Error(String(err));
        this.status = 'error';
        this.changed();
      },
    );
  }

  /**
   * Swaps in a different source, which is how the changes-only filter is applied.
   *
   * Filtering happens by rebuilding the report index in the engine, per spec 8.4,
   * not by hiding rows: the virtualizer only ever sees the filtered count, so a
   * filter over 300,000 rows costs one drain rather than 300,000 DOM nodes. Every
   * cache is dropped because index positions mean something different in the
   * filtered report.
   */
  setSource(source: DiffRowSource): void {
    this.source = source;
    this.compact.clear();
    this.values.clear();
    this.segments.clear();
    this.inFlight.clear();
    this.rowCount = 0;
    this.nColumns = 0;
    this.status = 'idle';
    this.error = null;
    this.scrollTop = 0;
    this.focus = null;
    this.start();
    this.changed();
  }

  /* -------------------------------------------------------------- viewport -- */

  setViewport(scrollTop: number, viewportHeight: number): void {
    if (scrollTop === this.scrollTop && viewportHeight === this.viewportHeight) return;
    this.scrollTop = scrollTop;
    this.viewportHeight = viewportHeight;
    this.changed();
  }

  setLayout(layout: RowLayout): void {
    if (layout === this.layout) return;
    this.layout = layout;
    this.changed();
  }

  setOldValuePosition(position: OldValuePosition): void {
    if (position === this.oldValuePosition) return;
    this.oldValuePosition = position;
    this.changed();
  }

  setRowHeight(px: number): void {
    if (px <= 0) throw new RangeError('rowHeight must be positive');
    if (px === this.rowHeight) return;
    this.rowHeight = px;
    this.changed();
  }

  setCellDiff(mode: CellDiffMode): void {
    if (mode === this.cellDiff) return;
    this.cellDiff = mode;
    // Keyed by position alone, so a change of mode has to clear them. The engine
    // side index does the same thing for the same reason.
    this.segments.clear();
    this.changed();
  }

  /** Column widths live here and are applied as CSS custom properties, per spec
   *  8.5's 0.2 row, so a drag resizes without re-rendering a single cell. */
  setColumnWidth(column: number, px: number): void {
    this.widths.set(column, Math.max(24, Math.round(px)));
    this.changed();
  }

  columnWidth(column: number): number {
    return this.widths.get(column) ?? this.defaultWidth;
  }

  /** Compared columns, from the index rather than from the column name list:
   *  under the column policy of spec 6.6 the two agree only when the caller
   *  allowed neither an added nor a removed column. */
  get columnCount(): number {
    return this.nColumns;
  }

  isKeyColumn(column: number): boolean {
    return this.keyColumns.has(column);
  }

  /** Compares contents rather than identity, because the realistic call site is
   *  an effect with a fresh array literal in its dependency list, and notifying
   *  on every render is a render loop rather than a slow render. */
  setKeyColumns(columns: readonly number[]): void {
    if (columns.length === this.keyColumns.size && columns.every((c) => this.keyColumns.has(c))) {
      return;
    }
    this.keyColumns = new Set(columns);
    this.changed();
  }

  /** `{ '--ibha-csvd-col-3': '220px', ... }` for the container element. */
  columnWidthVars(): Record<string, string> {
    const p = this.classPrefix.replace(/-$/, '');
    const out: Record<string, string> = {};
    for (const [col, px] of this.widths) out[`--${p}-col-${col}`] = `${px}px`;
    return out;
  }

  /* ---------------------------------------------------------------- focus -- */

  setFocus(row: number, column: number): void {
    const r = Math.max(0, Math.min(row, Math.max(0, this.rowCount - 1)));
    const c = Math.max(0, Math.min(column, Math.max(0, this.nColumns - 1)));
    if (this.focus && this.focus.row === r && this.focus.column === c) return;
    this.focus = { row: r, column: c };
    this.scrollTop = scrollToShow(r, this.scrollTop, this.viewportHeight, this.slotHeight());
    this.changed();
  }

  /**
   * Arrow and page keys. Spec 8.5 puts keyboard navigation in 0.2; it is here
   * because it is scroll math with a different input and belongs in the layer
   * that owns scroll math rather than in each rendering binding.
   */
  moveFocus(rows: number, columns: number): void {
    const from = this.focus ?? { row: this.visibleStart(), column: 0 };
    this.setFocus(from.row + rows, from.column + columns);
  }

  pageRows(): number {
    return Math.max(1, Math.floor(this.viewportHeight / this.slotHeight()));
  }

  /**
   * Jump to the next row that is not unchanged.
   *
   * It scans forward a page at a time through the compact form and retains
   * nothing: at one byte per cell a scan of the whole 90,000 row report reads
   * about a megabyte through a fifty row window, and the alternative is an index
   * of change positions whose memory grows with the diff, which is the thing this
   * package is built not to do. With `changesOnly` on, which is the default, the
   * first row it looks at is always the answer.
   */
  async findNextChange(from: number, direction: 1 | -1 = 1, maxScan = 100_000): Promise<number | null> {
    let scanned = 0;
    let at = from + direction;
    while (at >= 0 && at < this.rowCount && scanned < maxScan) {
      const pageStart = Math.floor(at / PAGE_SIZE) * PAGE_SIZE;
      const page = this.compact.get(pageStart / PAGE_SIZE) ?? (await this.source.getRowsCompact(pageStart, PAGE_SIZE));
      for (; at >= pageStart && at < pageStart + page.count; at += direction) {
        const i = at - page.offset;
        if ((page.kinds[i] ?? 0) !== 0 || page.moved[i] === 1) return at;
        scanned++;
      }
      if (at < pageStart) at = pageStart - 1;
    }
    return null;
  }

  /* -------------------------------------------------------------- snapshot -- */

  private slotHeight(): number {
    return this.rowHeight * slotsPerRow(this.layout);
  }

  private visibleStart(): number {
    return computeWindow({
      scrollTop: this.scrollTop,
      viewportHeight: this.viewportHeight,
      rowHeight: this.slotHeight(),
      rowCount: this.rowCount,
      overscan: 0,
    }).start;
  }

  /**
   * The current window, its rows and its geometry.
   *
   * Cached on identity until something changes, because `useSyncExternalStore`
   * calls it on every render and compares the result by reference: returning a
   * fresh object each time is an infinite render loop rather than a slow one.
   */
  snapshot = (): ViewSnapshot => {
    if (this.snapshotCache) return this.snapshotCache;

    const win = computeWindow({
      scrollTop: this.scrollTop,
      viewportHeight: this.viewportHeight,
      rowHeight: this.slotHeight(),
      rowCount: this.rowCount,
      overscan: this.overscan,
    });

    const rows: ViewRow[] = [];
    let pending = false;

    for (const page of pagesFor(win.start, win.end, PAGE_SIZE)) {
      this.want(page);
    }

    for (let i = win.start; i < win.end; i++) {
      const pageNo = Math.floor(i / PAGE_SIZE);
      const structPage = this.compact.get(pageNo);
      if (!structPage) {
        pending = true;
        continue;
      }
      const valuePage = this.values.get(pageNo);
      if (!valuePage) pending = true;

      const struct = structureFromCompact(structPage, i - structPage.offset);
      const row = valuePage?.[i - pageNo * PAGE_SIZE] ?? null;
      rows.push(
        ...presentRow(struct, row, {
          columns: this.source.columns,
          layout: this.layout,
          oldValuePosition: this.oldValuePosition,
          keyColumns: this.keyColumns,
          segments: this.cellDiff === 'none' ? undefined : (col) => this.segmentsFor(i, col),
        }),
      );
    }

    this.snapshotCache = {
      status: this.status,
      error: this.error,
      rowCount: this.rowCount,
      columns: this.source.columns,
      start: win.start,
      end: win.end,
      offsetTop: win.offsetTop,
      totalHeight: win.totalHeight,
      rowHeight: this.rowHeight,
      slotHeight: this.slotHeight(),
      scrollTop: this.scrollTop,
      layout: this.layout,
      oldValuePosition: this.oldValuePosition,
      rows,
      pending,
      focus: this.focus,
    };
    return this.snapshotCache;
  };

  /* --------------------------------------------------------------- fetching -- */

  /** Requests a page's structure and its values, at most once each. */
  private want(pageNo: number): void {
    const offset = pageNo * PAGE_SIZE;
    if (!this.compact.has(pageNo)) this.fetch(`c${pageNo}`, () => this.source.getRowsCompact(offset, PAGE_SIZE), (page) => {
      touch(this.compact, pageNo, page, COMPACT_PAGES);
    });
    if (!this.values.has(pageNo)) this.fetch(
      `v${pageNo}`,
      () =>
        this.source.getRows(offset, PAGE_SIZE, {
          includeValues: true,
          ...(this.maxCellBytes ? { maxCellBytes: this.maxCellBytes } : {}),
        }),
      (rows) => {
        touch(this.values, pageNo, rows, VALUE_PAGES);
      },
    );
  }

  /**
   * One request, deduplicated, installed synchronously when the source can answer
   * synchronously.
   *
   * The synchronous path matters more than it looks: with a local handle every
   * page resolves in the same tick, so `snapshot()` returns complete rows on the
   * first call and the placeholder state never appears. Routing a local source
   * through a promise would cost a repaint per scroll for nothing.
   */
  private fetch<T>(key: string, get: () => T | Promise<T>, install: (value: T) => void): void {
    if (this.inFlight.has(key)) return;
    let result: T | Promise<T>;
    try {
      result = get();
    } catch (err) {
      this.error = err instanceof Error ? err : new Error(String(err));
      this.status = 'error';
      return;
    }
    if (!isPromise(result)) {
      install(result);
      return;
    }
    this.inFlight.add(key);
    result.then(
      (value) => {
        this.inFlight.delete(key);
        install(value);
        this.changed();
      },
      (err: unknown) => {
        this.inFlight.delete(key);
        this.error = err instanceof Error ? err : new Error(String(err));
        this.status = 'error';
        this.changed();
      },
    );
  }

  /**
   * Segments for one visible cell, or null while they are on their way.
   *
   * Keyed by report position exactly as the engine side memo is, and capped, so a
   * scroll through a large report retires the cells it has left behind instead of
   * accumulating one entry per cell ever looked at.
   */
  private segmentsFor(row: number, column: number): TextSegment[] | null {
    const key = row * this.nColumns + column;
    const hit = this.segments.get(key);
    if (hit) return hit;
    this.fetch(
      `s${key}`,
      () => this.source.getCellSegments(row, column, this.cellDiff, this.maxCellBytes),
      (segs) => {
        touch(this.segments, key, segs, SEGMENT_ENTRIES);
      },
    );
    return this.segments.get(key) ?? null;
  }

  /** What the caches are holding, for a test or a diagnostic panel. The point of
   *  the number is that it does not grow with the diff. */
  get retainedPages(): { values: number; compact: number; segments: number } {
    return { values: this.values.size, compact: this.compact.size, segments: this.segments.size };
  }
}
