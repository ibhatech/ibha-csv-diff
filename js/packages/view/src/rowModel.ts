/**
 * The row presentation modes of spec 8.4, all four out of the one row model.
 *
 * This is where the contract from `js/packages/core/README.md` is actually
 * implemented, so it is worth restating: **a matched row's cell carries `source`
 * exactly when that cell differs in bytes from the target.** Its absence means
 * the two sides are byte identical, never that the value was empty. Everything
 * below that reads the source side reads it as `cell.source ?? cell.target`, and
 * a version of this file that read it as `cell.source ?? ''` would render every
 * unchanged cell of every modified row as a deletion.
 *
 * The other rule this file holds to: **the row's width comes from the row, and
 * the names come from the handle.** Under the column policy of spec 6.6 a report
 * row carries the columns the two files share, which is not the source file's
 * column count when the caller allowed an added or a removed column.
 */

import { sliceByBytes } from '@ibhatech/csvdiff-core';
import type { DiffCell, DiffRow, TextSegment } from '@ibhatech/csvdiff-core';

import {
  CELL_CHANGED,
  CELL_FINDING,
  CELL_SUPPRESSED,
  findingName,
  ROW_KINDS,
  type FindingName,
  type RowKind,
} from './classes.ts';
import type { RowLayout } from './virtual.ts';

/** Where the previous value sits in `inline` layout. */
export type OldValuePosition = 'above' | 'below' | 'tooltip';

/**
 * Everything about a report row that is not a string.
 *
 * This is what the compact page carries, and it is the form the view binds to:
 * one row of it is 14 bytes plus one flag byte per column and no allocation per
 * cell. Values arrive separately and only for the rows actually being painted.
 */
export interface RowStructure {
  /** Position in the filtered report, which is what the index is keyed by. */
  index: number;
  kind: RowKind;
  moved: boolean;
  moveDistance: number;
  sourceRow: number | null;
  targetRow: number | null;
  /** One byte per compared column. Its length is the row's width. */
  flags: Uint8Array;
}

/** One run of a cell's text under intra cell diffing. */
export interface CellPiece {
  op: 'equal' | 'delete' | 'insert';
  text: string;
}

export interface ViewCell {
  column: number;
  name: string;
  /** The raw flag byte, so a consumer's `classNames` function can style from the
   *  same thing the built in classes are derived from. */
  flags: number;
  changed: boolean;
  suppressed: boolean;
  finding: FindingName | null;
  key: boolean;
  /** The value this cell shows. Empty on a blank or placeholder cell. */
  text: string;
  /** The source side, present only where the layout shows both in one cell. */
  old?: string;
  /** `text` split by the intra cell diff, when it is on and the segments have
   *  arrived. Absent means render `text` as one run. */
  pieces?: CellPiece[];
  oldPieces?: CellPiece[];
  /** Renders empty on purpose: the lower row of a stacked pair where nothing
   *  changed, or a gap filler opposite an added or deleted row. */
  blank: boolean;
  /** True while the values for this page have not arrived. The structure is
   *  known, so the row paints in the right colour rather than as a blank gap. */
  placeholder: boolean;
  ariaLabel: string;
}

export interface ViewRow {
  /** Position in the filtered report. Two physical rows of one report row share
   *  it, which is what lets a click on either select the same row. */
  index: number;
  /** Distinguishes the physical rows of one report row. `single` is the only one
   *  in `inline`; `new` and `old` are the stacked and unified pair; `gap` is a
   *  filler with no counterpart on this side. */
  variant: 'single' | 'new' | 'old' | 'gap';
  /** Which pane, in `sideBySide` only. */
  pane?: 'source' | 'target';
  /** The class the row carries, which in `unified` is not the report row's kind:
   *  a modified row is rendered as a deletion followed by an addition. */
  kind: RowKind;
  moved: boolean;
  moveDistance: number;
  /** What the row number gutter shows. Null on a gap filler. */
  rowNumber: number | null;
  /** A non colour signal for the change kind, per spec 8.5. */
  marker: string;
  hasFinding: boolean;
  cells: ViewCell[];
}

export interface PresentOptions {
  columns: readonly string[];
  layout: RowLayout;
  oldValuePosition: OldValuePosition;
  /** Column indices the schema declares as KEY, for the sticky columns of 8.5. */
  keyColumns?: ReadonlySet<number>;
  /** Intra cell segments for one cell of this row, when they are available.
   *  Returning null means "not on, or not arrived yet", and the cell renders as
   *  one run. */
  segments?: ((column: number) => TextSegment[] | null) | undefined;
}

const MARKERS: Record<RowKind, string> = {
  unchanged: '',
  modified: '~',
  added: '+',
  deleted: '-',
};

/** Reads one row's structure out of a compact page. No allocation but the flag
 *  view, which is a subarray of the page's own buffer. */
export function structureFromCompact(
  page: {
    offset: number;
    count: number;
    columns: number;
    kinds: Uint8Array;
    moved: Uint8Array;
    moveDistance: Int32Array;
    sourceRows: Uint32Array;
    targetRows: Uint32Array;
    cellFlags: Uint8Array;
  },
  i: number,
): RowStructure {
  const w = page.columns;
  return {
    index: page.offset + i,
    kind: ROW_KINDS[page.kinds[i] ?? 0] ?? 'unchanged',
    moved: page.moved[i] === 1,
    moveDistance: page.moveDistance[i] ?? 0,
    // 0 is the compact page's "no counterpart" sentinel; real record numbers are
    // 1 based, so 0 is not a value any row can have.
    sourceRow: page.sourceRows[i] ? (page.sourceRows[i] as number) : null,
    targetRow: page.targetRows[i] ? (page.targetRows[i] as number) : null,
    flags: page.cellFlags.subarray(i * w, (i + 1) * w),
  };
}

/**
 * Splits a cell's two sides into runs, from the engine's segments.
 *
 * Mirrors `html_cell_side` in `core/src/emit.c`, including the part that is easy
 * to get wrong: an `equal` segment advances both offsets, a `delete` advances
 * only the source's and an `insert` only the target's, because the offsets index
 * two different strings.
 *
 * The offsets are **byte offsets into the logical value**, not UTF-16 code unit
 * offsets, so they go through `sliceByBytes` from the engine package rather than
 * to `String.prototype.slice`. There is exactly one implementation of that
 * conversion in this project and this is not it.
 */
export function cellPieces(
  source: string,
  target: string,
  segments: readonly TextSegment[],
): { old: CellPiece[]; new: CellPiece[] } {
  const oldRuns: CellPiece[] = [];
  const newRuns: CellPiece[] = [];
  let soff = 0;
  let toff = 0;

  for (const seg of segments) {
    if (seg.op === 'equal') {
      const text = sliceByBytes(source, soff, seg.len);
      if (text) {
        oldRuns.push({ op: 'equal', text });
        newRuns.push({ op: 'equal', text: sliceByBytes(target, toff, seg.len) });
      }
      soff += seg.len;
      toff += seg.len;
    } else if (seg.op === 'delete') {
      const text = sliceByBytes(source, soff, seg.len);
      if (text) oldRuns.push({ op: 'delete', text });
      soff += seg.len;
    } else {
      const text = sliceByBytes(target, toff, seg.len);
      if (text) newRuns.push({ op: 'insert', text });
      toff += seg.len;
    }
  }
  return { old: oldRuns, new: newRuns };
}

/* ---------------------------------------------------------------- helpers -- */

function findingText(kind: FindingName): string {
  switch (kind) {
    case 'requiredEmpty':
      return 'required value is empty';
    case 'tooLong':
      return 'value is longer than the declared length';
    case 'notNumeric':
      return 'value is not numeric';
    case 'precision':
      return 'value does not fit the declared precision';
  }
}

function label(name: string, text: string, old: string | undefined, finding: FindingName | null): string {
  let s = old === undefined ? `${name}, ${text}` : `${name}, changed from ${old} to ${text}`;
  if (finding) s += `, ${findingText(finding)}`;
  return s;
}

interface CellInput {
  column: number;
  name: string;
  flags: number;
  key: boolean;
  cell: DiffCell | undefined;
}

function inputs(struct: RowStructure, row: DiffRow | null, opts: PresentOptions): CellInput[] {
  const keys = opts.keyColumns;
  const out: CellInput[] = [];
  // The width comes from the row, not from the column list: under the column
  // policy of spec 6.6 they are the same only when the caller allowed neither an
  // added nor a removed column.
  for (let c = 0; c < struct.flags.length; c++) {
    out.push({
      column: c,
      name: opts.columns[c] ?? '',
      flags: struct.flags[c] ?? 0,
      key: keys ? keys.has(c) : false,
      cell: row?.cells[c],
    });
  }
  return out;
}

/** The source side of a cell. Absent `source` means byte identical, so it falls
 *  through to the target rather than to an empty string. */
function sourceText(cell: DiffCell | undefined): string {
  return cell?.source ?? cell?.target ?? '';
}

function targetText(cell: DiffCell | undefined): string {
  return cell?.target ?? '';
}

function blankCell(i: CellInput): ViewCell {
  return {
    column: i.column,
    name: i.name,
    flags: 0,
    changed: false,
    suppressed: false,
    finding: null,
    key: i.key,
    text: '',
    blank: true,
    placeholder: false,
    ariaLabel: '',
  };
}

function makeCell(
  i: CellInput,
  text: string,
  old: string | undefined,
  placeholder: boolean,
  pieces?: { old: CellPiece[]; new: CellPiece[] },
): ViewCell {
  const finding = i.flags & CELL_FINDING ? findingName(i.flags) : null;
  const cell: ViewCell = {
    column: i.column,
    name: i.name,
    flags: i.flags,
    changed: (i.flags & CELL_CHANGED) !== 0,
    suppressed: (i.flags & CELL_SUPPRESSED) !== 0,
    finding,
    key: i.key,
    text,
    blank: false,
    placeholder,
    ariaLabel: placeholder ? '' : label(i.name, text, old, finding),
  };
  if (old !== undefined) cell.old = old;
  if (pieces) {
    cell.pieces = pieces.new;
    if (old !== undefined) cell.oldPieces = pieces.old;
  }
  return cell;
}

function segmentsFor(
  opts: PresentOptions,
  column: number,
  source: string,
  target: string,
): { old: CellPiece[]; new: CellPiece[] } | undefined {
  const segs = opts.segments?.(column);
  if (!segs || segs.length === 0) return undefined;
  return cellPieces(source, target, segs);
}

function baseRow(struct: RowStructure, kind: RowKind, rowNumber: number | null): Omit<ViewRow, 'variant' | 'cells'> {
  return {
    index: struct.index,
    kind,
    moved: struct.moved,
    moveDistance: struct.moveDistance,
    rowNumber,
    marker: (struct.moved ? '⇅' : '') + MARKERS[kind],
    hasFinding: struct.flags.some((f) => (f & CELL_FINDING) !== 0),
  };
}

/* ------------------------------------------------------------ the four modes -- */

/**
 * Turns one report row into the physical rows that render it.
 *
 * `row` is null while the values for the page have not arrived, which happens
 * only with a source in a worker. The structure is known either way, so the row
 * paints with its correct kind, row number and marker and its cells render empty,
 * rather than the viewport showing a blank gap that then reflows.
 */
export function presentRow(
  struct: RowStructure,
  row: DiffRow | null,
  opts: PresentOptions,
): ViewRow[] {
  const cells = inputs(struct, row, opts);
  const missing = row === null;
  const num = struct.targetRow ?? struct.sourceRow;

  switch (opts.layout) {
    case 'inline': {
      const out = cells.map((i) => {
        const changed = (i.flags & CELL_CHANGED) !== 0;
        const hasBoth = i.cell?.source !== undefined && i.cell?.target !== undefined;
        if (changed && hasBoth) {
          const src = sourceText(i.cell);
          const tgt = targetText(i.cell);
          // 'tooltip' still puts the old value in the DOM, hidden by the
          // stylesheet, because a value only a mouse can reach is a value a
          // screen reader and a copy cannot.
          return makeCell(i, tgt, src, missing, segmentsFor(opts, i.column, src, tgt));
        }
        const text = struct.kind === 'deleted' ? sourceText(i.cell) : targetText(i.cell);
        return makeCell(i, missing ? '' : text, undefined, missing);
      });
      return [{ ...baseRow(struct, struct.kind, num), variant: 'single', cells: out }];
    }

    case 'stacked': {
      // New on top, old below, with unchanged cells in the lower row blank so the
      // eye goes straight to the differences. Spec 8.4's fourth bullet.
      const upper = cells.map((i) => {
        const text = struct.kind === 'deleted' ? sourceText(i.cell) : targetText(i.cell);
        return makeCell(i, missing ? '' : text, undefined, missing);
      });
      const lower = cells.map((i) => {
        const changed = (i.flags & CELL_CHANGED) !== 0;
        const hasBoth = i.cell?.source !== undefined && i.cell?.target !== undefined;
        if (!changed || !hasBoth || missing) return blankCell(i);
        return makeCell(i, sourceText(i.cell), undefined, false);
      });
      const base = baseRow(struct, struct.kind, num);
      return [
        { ...base, variant: 'new', cells: upper },
        { ...base, variant: 'old', rowNumber: null, cells: lower },
      ];
    }

    case 'unified': {
      // Git style: the deletion then the addition, which is the readable form for
      // the all keys case of spec 6.4 where a modified row is really a pair.
      const base = baseRow(struct, struct.kind, num);
      const src = cells.map((i) => makeCell(i, missing ? '' : sourceText(i.cell), undefined, missing));
      const tgt = cells.map((i) => makeCell(i, missing ? '' : targetText(i.cell), undefined, missing));
      const gap = cells.map(blankCell);

      if (struct.kind === 'modified') {
        return [
          { ...base, kind: 'deleted', variant: 'old', rowNumber: struct.sourceRow, marker: MARKERS.deleted, cells: src },
          { ...base, kind: 'added', variant: 'new', rowNumber: struct.targetRow, cells: tgt },
        ];
      }
      if (struct.kind === 'deleted') {
        return [
          { ...base, variant: 'single', rowNumber: struct.sourceRow, cells: src },
          { ...base, variant: 'gap', rowNumber: null, marker: '', cells: gap },
        ];
      }
      return [
        { ...base, variant: 'single', cells: tgt },
        { ...base, variant: 'gap', rowNumber: null, marker: '', cells: gap },
      ];
    }

    case 'sideBySide': {
      // Two panes, source left and target right, aligned by the browser because
      // they share one scroll container. Gap filler rows where one side has no
      // counterpart, so row N of the left pane is always beside row N of the
      // right.
      const base = baseRow(struct, struct.kind, num);
      const left: ViewRow =
        struct.kind === 'added'
          ? { ...base, variant: 'gap', pane: 'source', rowNumber: null, marker: '', cells: cells.map(blankCell) }
          : {
              ...base,
              variant: 'single',
              pane: 'source',
              rowNumber: struct.sourceRow,
              cells: cells.map((i) => makeCell(i, missing ? '' : sourceText(i.cell), undefined, missing)),
            };
      const right: ViewRow =
        struct.kind === 'deleted'
          ? { ...base, variant: 'gap', pane: 'target', rowNumber: null, marker: '', cells: cells.map(blankCell) }
          : {
              ...base,
              variant: 'single',
              pane: 'target',
              rowNumber: struct.targetRow,
              cells: cells.map((i) => {
                const changed = (i.flags & CELL_CHANGED) !== 0;
                const src = sourceText(i.cell);
                const tgt = targetText(i.cell);
                const pieces =
                  changed && i.cell?.source !== undefined ? segmentsFor(opts, i.column, src, tgt) : undefined;
                return makeCell(i, missing ? '' : tgt, undefined, missing, pieces);
              }),
            };
      return [left, right];
    }
  }
}
