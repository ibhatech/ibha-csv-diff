import { describe, expect, it } from 'vitest';
import type { DiffRow, TextSegment } from '@ibhatech/csvdiff-core';

import { CELL_CHANGED, CELL_TOO_LONG } from './classes.ts';
import { cellPieces, presentRow, structureFromCompact, type RowStructure } from './rowModel.ts';
import { fakeRowSource } from './testkit.ts';

const COLUMNS = ['id', 'name', 'amount'];

const opts = (over: Partial<Parameters<typeof presentRow>[2]> = {}) => ({
  columns: COLUMNS,
  layout: 'inline' as const,
  oldValuePosition: 'below' as const,
  ...over,
});

/** A modified row whose middle cell changed and whose last cell did not. */
function modified(): { struct: RowStructure; row: DiffRow } {
  const struct: RowStructure = {
    index: 3,
    kind: 'modified',
    moved: false,
    moveDistance: 0,
    sourceRow: 12,
    targetRow: 14,
    flags: new Uint8Array([0, CELL_CHANGED, 0]),
  };
  const row: DiffRow = {
    kind: 'modified',
    moved: false,
    moveDistance: 0,
    sourceRow: 12,
    targetRow: 14,
    key: ['ACC-1'],
    changedCells: 1,
    suppressedCells: 0,
    findings: [],
    cells: [
      { column: 0, name: 'id', target: 'ACC-1', changed: false, suppressed: false },
      // The contract: `source` is present exactly because it differs.
      { column: 1, name: 'name', source: 'Acme', target: 'Acme Ltd', changed: true, suppressed: false },
      // No `source`, which means byte identical, not empty.
      { column: 2, name: 'amount', target: '12.00', changed: false, suppressed: false },
    ],
  };
  return { struct, row };
}

describe('inline layout', () => {
  it('shows the new value with the old one beside it, and only where it changed', () => {
    const { struct, row } = modified();
    const [r] = presentRow(struct, row, opts());
    expect(r?.variant).toBe('single');
    expect(r?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme Ltd', '12.00']);
    expect(r?.cells.map((c) => c.old)).toEqual([undefined, 'Acme', undefined]);
  });

  /**
   * The rule the README singles out as the one a consumer gets wrong. A cell whose
   * `source` is absent is byte identical to the target, not empty, and a view that
   * read it as an empty string would render every unchanged cell of every modified
   * row as a deletion.
   */
  it('does not treat an absent source as an empty old value', () => {
    const { struct, row } = modified();
    const [r] = presentRow(struct, row, opts());
    expect(r?.cells[2]?.old).toBeUndefined();
    expect(r?.cells[2]?.text).toBe('12.00');
  });

  it('shows the source side on a deleted row, which has no target at all', () => {
    const struct: RowStructure = {
      index: 0, kind: 'deleted', moved: false, moveDistance: 0,
      sourceRow: 7, targetRow: null, flags: new Uint8Array(3),
    };
    const row: DiffRow = {
      kind: 'deleted', moved: false, moveDistance: 0, sourceRow: 7, targetRow: null,
      key: null, changedCells: 0, suppressedCells: 0, findings: [],
      cells: [
        { column: 0, name: 'id', source: 'ACC-9', changed: false, suppressed: false },
        { column: 1, name: 'name', source: 'Gone', changed: false, suppressed: false },
        { column: 2, name: 'amount', source: '1.00', changed: false, suppressed: false },
      ],
    };
    const [r] = presentRow(struct, row, opts());
    expect(r?.cells.map((c) => c.text)).toEqual(['ACC-9', 'Gone', '1.00']);
    expect(r?.rowNumber).toBe(7);
    expect(r?.marker).toBe('-');
  });

  it('carries the row number the emitter shows, which is the target’s where there is one', () => {
    const { struct, row } = modified();
    expect(presentRow(struct, row, opts())[0]?.rowNumber).toBe(14);
  });

  it('names the finding for CSS and for the aria label', () => {
    const { struct, row } = modified();
    struct.flags = new Uint8Array([0, CELL_CHANGED, CELL_TOO_LONG]);
    const [r] = presentRow(struct, row, opts());
    expect(r?.hasFinding).toBe(true);
    expect(r?.cells[2]?.finding).toBe('tooLong');
    expect(r?.cells[2]?.ariaLabel).toContain('longer than');
    expect(r?.cells[1]?.ariaLabel).toBe('name, changed from Acme to Acme Ltd');
  });

  it('renders a placeholder row with the right kind while the values are in flight', () => {
    const { struct } = modified();
    const [r] = presentRow(struct, null, opts());
    expect(r?.kind).toBe('modified');
    expect(r?.rowNumber).toBe(14);
    expect(r?.cells.every((c) => c.text === '' && c.placeholder)).toBe(true);
  });
});

describe('stacked layout', () => {
  it('puts the new values on top and only the changed old ones below', () => {
    const { struct, row } = modified();
    const rows = presentRow(struct, row, opts({ layout: 'stacked' }));
    expect(rows.map((r) => r.variant)).toEqual(['new', 'old']);
    expect(rows[0]?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme Ltd', '12.00']);
    expect(rows[1]?.cells.map((c) => c.text)).toEqual(['', 'Acme', '']);
    expect(rows[1]?.cells.map((c) => c.blank)).toEqual([true, false, true]);
  });

  it('keeps both physical rows on the same report index, so a click selects one row', () => {
    const { struct, row } = modified();
    expect(presentRow(struct, row, opts({ layout: 'stacked' })).map((r) => r.index)).toEqual([3, 3]);
  });
});

describe('unified layout', () => {
  it('renders a modification as a deletion followed by an addition', () => {
    const { struct, row } = modified();
    const rows = presentRow(struct, row, opts({ layout: 'unified' }));
    expect(rows.map((r) => r.kind)).toEqual(['deleted', 'added']);
    expect(rows[0]?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme', '12.00']);
    expect(rows[1]?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme Ltd', '12.00']);
    expect(rows[0]?.rowNumber).toBe(12);
    expect(rows[1]?.rowNumber).toBe(14);
  });

  it('pads an unchanged row to the same two slots, because the row height is fixed', () => {
    const { struct, row } = modified();
    struct.kind = 'unchanged';
    struct.flags = new Uint8Array(3);
    const rows = presentRow(struct, row, opts({ layout: 'unified' }));
    expect(rows.map((r) => r.variant)).toEqual(['single', 'gap']);
  });
});

describe('sideBySide layout', () => {
  it('puts the source in the left pane and the target in the right', () => {
    const { struct, row } = modified();
    const rows = presentRow(struct, row, opts({ layout: 'sideBySide' }));
    expect(rows.map((r) => r.pane)).toEqual(['source', 'target']);
    expect(rows[0]?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme', '12.00']);
    expect(rows[1]?.cells.map((c) => c.text)).toEqual(['ACC-1', 'Acme Ltd', '12.00']);
  });

  it('fills the pane that has no counterpart, so the two stay aligned', () => {
    const struct: RowStructure = {
      index: 1, kind: 'added', moved: false, moveDistance: 0,
      sourceRow: null, targetRow: 5, flags: new Uint8Array(3),
    };
    const row: DiffRow = {
      kind: 'added', moved: false, moveDistance: 0, sourceRow: null, targetRow: 5,
      key: null, changedCells: 0, suppressedCells: 0, findings: [],
      cells: COLUMNS.map((name, column) => ({ column, name, target: 'x', changed: false, suppressed: false })),
    };
    const rows = presentRow(struct, row, opts({ layout: 'sideBySide' }));
    expect(rows[0]?.variant).toBe('gap');
    expect(rows[0]?.rowNumber).toBeNull();
    expect(rows[1]?.variant).toBe('single');
    expect(rows[1]?.rowNumber).toBe(5);
  });
});

describe('cellPieces', () => {
  const segs = (...s: Array<[TextSegment['op'], number, number]>): TextSegment[] =>
    s.map(([op, start, len]) => ({ op, start, len }));

  it('splits both sides the way the emitter does', () => {
    // "Acme" -> "Acme Ltd": equal 4, insert 4.
    const p = cellPieces('Acme', 'Acme Ltd', segs(['equal', 0, 4], ['insert', 4, 4]));
    expect(p.old).toEqual([{ op: 'equal', text: 'Acme' }]);
    expect(p.new).toEqual([
      { op: 'equal', text: 'Acme' },
      { op: 'insert', text: ' Ltd' },
    ]);
  });

  it('advances the two offsets independently, which is what a replacement needs', () => {
    // "cat" -> "dog": delete 3 from the source, insert 3 into the target. Both
    // start at offset 0 of their own string.
    const p = cellPieces('cat', 'dog', segs(['delete', 0, 3], ['insert', 0, 3]));
    expect(p.old).toEqual([{ op: 'delete', text: 'cat' }]);
    expect(p.new).toEqual([{ op: 'insert', text: 'dog' }]);
  });

  /**
   * The offsets are byte offsets into the logical UTF-8 value, not UTF-16 code
   * unit offsets. "café" is four characters in five bytes, and an emoji is four
   * bytes in two code units. Slicing at a byte offset with `String.slice` mangles
   * both, which is why this goes through the engine's `sliceByBytes`.
   */
  it('treats the offsets as bytes, not as UTF-16 code units', () => {
    const p = cellPieces('café', 'café!', segs(['equal', 0, 5], ['insert', 5, 1]));
    expect(p.old).toEqual([{ op: 'equal', text: 'café' }]);
    expect(p.new).toEqual([
      { op: 'equal', text: 'café' },
      { op: 'insert', text: '!' },
    ]);

    const emoji = cellPieces('a🙂', 'a🙂b', segs(['equal', 0, 5], ['insert', 5, 1]));
    expect(emoji.new).toEqual([
      { op: 'equal', text: 'a🙂' },
      { op: 'insert', text: 'b' },
    ]);
  });

  it('feeds the highlighted runs through to the rendered cell', () => {
    const { struct, row } = modified();
    const [r] = presentRow(
      struct,
      row,
      opts({ segments: () => segs(['equal', 0, 4], ['insert', 4, 4]) }),
    );
    expect(r?.cells[1]?.pieces).toEqual([
      { op: 'equal', text: 'Acme' },
      { op: 'insert', text: ' Ltd' },
    ]);
    expect(r?.cells[1]?.oldPieces).toEqual([{ op: 'equal', text: 'Acme' }]);
  });
});

describe('structureFromCompact', () => {
  it('reads a row out of the parallel arrays without allocating one', async () => {
    const source = fakeRowSource(COLUMNS, [
      { kind: 'unchanged', sourceRow: 1, targetRow: 1, cells: [] },
      { kind: 'modified', moved: true, moveDistance: -3, sourceRow: 2, targetRow: 9, cells: [{ changed: true }] },
      { kind: 'added', targetRow: 10, cells: [] },
    ]);
    const page = await source.getRowsCompact(0, 50);

    expect(structureFromCompact(page, 1)).toMatchObject({
      index: 1,
      kind: 'modified',
      moved: true,
      moveDistance: -3,
      sourceRow: 2,
      targetRow: 9,
    });
    // 0 is the compact page's "no counterpart" sentinel, and a real record number
    // is 1 based, so it can never collide with one.
    expect(structureFromCompact(page, 2).sourceRow).toBeNull();
    expect(structureFromCompact(page, 1).flags.length).toBe(COLUMNS.length);
  });
});
