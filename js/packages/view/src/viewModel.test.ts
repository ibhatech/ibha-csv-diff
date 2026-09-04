import { describe, expect, it } from 'vitest';

import { CELL_CHANGED } from './classes.ts';
import { fakeRowSource, type FakeRow } from './testkit.ts';
import { DiffViewModel, PAGE_SIZE } from './viewModel.ts';

const COLUMNS = ['id', 'name', 'amount'];

/** A report where every hundredth row is modified and the rest are unchanged,
 *  which is roughly what a real 90,000 row comparison looks like. */
function report(n: number): FakeRow[] {
  const rows: FakeRow[] = [];
  for (let i = 0; i < n; i++) {
    const changed = i % 100 === 0 && i > 0;
    rows.push({
      kind: changed ? 'modified' : 'unchanged',
      sourceRow: i + 1,
      targetRow: i + 1,
      cells: [
        { target: `ACC-${i}` },
        changed
          ? { source: `name ${i}`, target: `NAME ${i}`, changed: true }
          : { target: `name ${i}` },
        { target: '12.00' },
      ],
    });
  }
  return rows;
}

describe('paging', () => {
  it('asks for the page it is about to paint, not for the report', async () => {
    const asked: Array<[string, number, number]> = [];
    const source = fakeRowSource(COLUMNS, report(90_000), {
      onRequest: (what, offset, count) => asked.push([what, offset, count]),
    });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    m.snapshot();

    expect(asked.every(([, , count]) => count === PAGE_SIZE)).toBe(true);
    expect(asked.map(([what]) => what).sort()).toEqual(['compact', 'rows']);
  });

  /**
   * The invariant the whole package is built around. A scroll from the top of a
   * 90,000 row report to the bottom must not accumulate rows, or the view has
   * quietly rebuilt in JavaScript the million object heap the wasm engine exists
   * to prevent.
   */
  it('retains a bounded number of pages however far it scrolls', async () => {
    const source = fakeRowSource(COLUMNS, report(90_000));
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    for (let top = 0; top < 28 * 90_000; top += 28 * 137) {
      m.setViewport(top, 560);
      m.snapshot();
    }
    const held = m.retainedPages;
    expect(held.values).toBeLessThanOrEqual(4);
    expect(held.compact).toBeLessThanOrEqual(64);
  });

  /**
   * The reason to bind to `getRowsCompact` rather than only to `getRows`. Sixty
   * four pages of one byte per cell is 38 KB at twelve columns, so a page whose
   * decoded values were evicted still knows its kinds and row numbers and repaints
   * in the right colours the moment it scrolls back; only its text is re-fetched.
   */
  it('keeps a page’s structure long after its values have been evicted', () => {
    const wanted: Array<[string, number]> = [];
    const source = fakeRowSource(COLUMNS, report(5_000), {
      onRequest: (what, offset) => wanted.push([what, offset]),
    });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    m.snapshot();
    for (let top = 0; top < 28 * 2_000; top += 28 * 50) {
      m.setViewport(top, 560);
      m.snapshot();
    }
    wanted.length = 0;
    m.setViewport(0, 560);
    m.snapshot();

    expect(wanted.some(([what, offset]) => what === 'rows' && offset === 0)).toBe(true);
    expect(wanted.some(([what]) => what === 'compact')).toBe(false);
  });

  it('produces complete rows on the first snapshot with a local source', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(200)), { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    const s = m.snapshot();
    expect(s.pending).toBe(false);
    expect(s.rows[0]?.cells[0]?.text).toBe('ACC-0');
  });

  it('returns the identical snapshot object until something changes', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(200)), { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    // useSyncExternalStore compares by reference and re-renders forever if this
    // is a fresh object each call.
    const first = m.snapshot();
    expect(m.snapshot()).toBe(first);
    m.setViewport(28, 560);
    expect(m.snapshot()).not.toBe(first);
  });
});

describe('a source in a worker', () => {
  it('paints the structure first and fills in the values when they arrive', async () => {
    const source = fakeRowSource(COLUMNS, report(200), { async: true });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    await Promise.resolve();
    m.setViewport(0, 560);

    const first = m.snapshot();
    expect(first.pending).toBe(true);
    expect(first.rows.length).toBe(0);

    await new Promise((r) => setTimeout(r, 0));
    const filled = m.snapshot();
    expect(filled.pending).toBe(false);
    expect(filled.rows[0]?.cells[0]?.text).toBe('ACC-0');
  });

  it('notifies once per arrival rather than once per row', async () => {
    let notes = 0;
    const source = fakeRowSource(COLUMNS, report(200), { async: true });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.subscribe(() => notes++);
    m.start();
    await new Promise((r) => setTimeout(r, 0));
    m.setViewport(0, 560);
    m.snapshot();
    await new Promise((r) => setTimeout(r, 0));
    // ready, then the compact page and the value page: three, not fifty.
    expect(notes).toBeLessThanOrEqual(4);
  });

  it('does not issue the same request twice while it is in flight', async () => {
    const asked: string[] = [];
    const source = fakeRowSource(COLUMNS, report(200), {
      async: true,
      onRequest: (what, offset) => asked.push(`${what}:${offset}`),
    });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    await new Promise((r) => setTimeout(r, 0));
    m.setViewport(0, 560);
    m.snapshot();
    m.snapshot();
    m.setViewport(1, 560);
    m.snapshot();
    await new Promise((r) => setTimeout(r, 0));
    expect(asked.filter((a) => a === 'rows:0').length).toBe(1);
  });
});

describe('the changes-only filter', () => {
  /**
   * Filtering happens in the engine by rebuilding the report index, per spec 8.4,
   * not by hiding rows: the virtualizer only ever sees the filtered count. So a
   * new filter is a new source, and every cache has to go, because a position in
   * the filtered report means a different row.
   */
  it('drops every cache when the source is replaced', () => {
    const all = fakeRowSource(COLUMNS, report(1_000));
    const m = new DiffViewModel(all, { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    m.snapshot();
    expect(m.retainedPages.compact).toBeGreaterThan(0);

    const changes = fakeRowSource(COLUMNS, report(1_000).filter((r) => r.kind !== 'unchanged'));
    m.setSource(changes);
    m.setViewport(0, 560);
    const s = m.snapshot();
    expect(s.rowCount).toBe(9);
    expect(s.rows.every((r) => r.kind === 'modified')).toBe(true);
  });
});

describe('keyboard navigation', () => {
  it('scrolls only as far as it must to keep the focused row visible', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(1_000)), { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    m.setFocus(5, 0);
    expect(m.snapshot().scrollTop).toBe(0);
    m.setFocus(25, 0);
    expect(m.snapshot().scrollTop).toBe(26 * 28 - 560);
  });

  it('clamps the focus to the report rather than scrolling into nothing', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(10)), { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    m.setFocus(9_999, 99);
    expect(m.snapshot().focus).toEqual({ row: 9, column: 2 });
  });

  it('finds the next change by scanning the compact form and retaining nothing', async () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(1_000)), { rowHeight: 28 });
    m.start();
    m.setViewport(0, 560);
    expect(await m.findNextChange(0, 1)).toBe(100);
    expect(await m.findNextChange(100, 1)).toBe(200);
    expect(await m.findNextChange(950, 1)).toBeNull();
    expect(await m.findNextChange(150, -1)).toBe(100);
    expect(m.retainedPages.values).toBeLessThanOrEqual(4);
  });
});

describe('column widths', () => {
  it('are view state applied as custom properties, not as a re-render of every cell', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(10)), { defaultColumnWidth: 160 });
    expect(m.columnWidth(1)).toBe(160);
    m.setColumnWidth(1, 240);
    expect(m.columnWidth(1)).toBe(240);
    expect(m.columnWidthVars()).toEqual({ '--ibha-csvd-col-1': '240px' });
  });

  it('refuses to shrink a column to nothing', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(10)));
    m.setColumnWidth(0, -100);
    expect(m.columnWidth(0)).toBe(24);
  });
});

describe('intra cell segments', () => {
  it('are off until asked for, then memoized by position', () => {
    const asked: string[] = [];
    const source = fakeRowSource(COLUMNS, report(200), {
      onRequest: (what, a, b) => asked.push(`${what}:${a}:${b}`),
      segments: () => [
        { op: 'equal', start: 0, len: 4 },
        { op: 'insert', start: 4, len: 4 },
      ],
    });
    const m = new DiffViewModel(source, { rowHeight: 28 });
    m.start();
    m.setViewport(28 * 95, 560);
    expect(m.snapshot().rows.some((r) => r.cells.some((c) => c.pieces))).toBe(false);

    m.setCellDiff('word-then-character');
    const s = m.snapshot();
    const changed = s.rows.find((r) => r.kind === 'modified');
    expect(changed?.cells[1]?.pieces?.length).toBe(2);

    const before = asked.filter((a) => a.startsWith('segments')).length;
    m.snapshot();
    expect(asked.filter((a) => a.startsWith('segments')).length).toBe(before);
  });

  it('asks only about changed cells, not about every cell in the viewport', () => {
    const asked: string[] = [];
    const source = fakeRowSource(COLUMNS, report(200), {
      onRequest: (what, a, b) => asked.push(`${what}:${a}:${b}`),
      segments: () => [{ op: 'equal', start: 0, len: 1 }],
    });
    const m = new DiffViewModel(source, { rowHeight: 28, cellDiff: 'word' });
    m.start();
    m.setViewport(28 * 95, 560);
    m.snapshot();
    const segmentCalls = asked.filter((a) => a.startsWith('segments'));
    // One changed cell in the band, not twenty rows times three columns.
    expect(segmentCalls.length).toBeLessThanOrEqual(2);
    expect(segmentCalls[0]).toBe(`segments:100:1`);
  });
});

describe('the flag byte reaches the rendered cell', () => {
  it('so a consumer’s classNames function can style from the same thing', () => {
    const m = new DiffViewModel(fakeRowSource(COLUMNS, report(200)), { rowHeight: 28 });
    m.start();
    m.setViewport(28 * 99, 560);
    const row = m.snapshot().rows.find((r) => r.index === 100);
    expect(row?.cells[1]?.flags).toBe(CELL_CHANGED);
    expect(row?.cells[0]?.flags).toBe(0);
  });
});
