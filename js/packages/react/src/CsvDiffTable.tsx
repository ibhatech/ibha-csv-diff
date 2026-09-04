/**
 * The virtualized diff table.
 *
 * Two decisions from spec 8.2 that this must not drift from:
 *
 *   - **No shadow DOM.** Its entire purpose is to stop the host page's stylesheet
 *     reaching inside, and the requirement is that the consumer supplies the
 *     stylesheet. Those are in direct conflict, so this renders into light DOM
 *     with the stable class names of the styling contract.
 *   - **`theme` emits CSS custom properties, never inline styles.** Inline styles
 *     would beat the consumer's own CSS on specificity, and a JS object cannot
 *     express `:hover`, dark mode, print or container queries.
 *
 * And one from 8.5 that is a correctness issue rather than a preference: the
 * markup is a real `<table>` with grid semantics, a changed cell carries an
 * `aria-label` saying what it changed from and to, and every change kind carries
 * a text marker, because colour must never be the only signal.
 *
 * **Escaping.** Every value below reaches the DOM as a JSX child or as a JSX
 * attribute value, both of which React escapes. Nothing here builds a markup
 * string and nothing uses `dangerouslySetInnerHTML`, which is the property that
 * makes the guarantee checkable: `CsvDiffTable.test.tsx` renders the XSS corpus
 * through this component and runs an independently written checker over the
 * result, rather than asserting that a reviewer looked.
 */

import { Fragment } from 'react';
import type { CSSProperties, ReactNode } from 'react';
import { DIFF_ROW_SCHEMA_VERSION } from '@ibhatech/csvdiff-core';
import {
  cellClass,
  classNames,
  rowClass,
  themeToCssVars,
  type CellPiece,
  type ClassNames,
  type DiffRowSource,
  type IbhaCsvDiffTheme,
  type ViewCell,
  type ViewRow,
} from '@ibhatech/csvdiff-view';

import { useDiffView, type UseDiffViewOptions } from './useDiffView.ts';

export interface CsvDiffClassNames {
  /** Appended to the container's class list. */
  report?: string;
  row?: string | ((row: ViewRow) => string);
  cell?: string | ((cell: ViewCell) => string);
}

export interface CsvDiffTableProps extends UseDiffViewOptions {
  /**
   * Where rows come from. Build it with `localRowSource(handle)` for a handle in
   * this thread or `remoteRowSource(handle)` for one in a worker, and memoize it:
   * a new source rebuilds the report index.
   */
  source: DiffRowSource;
  theme?: IbhaCsvDiffTheme;
  /** Height of the scroll container. It is the one dimension the component
   *  cannot infer, because a virtualized list has no natural height. */
  height?: number | string;
  classNames?: CsvDiffClassNames;
  /** Rendered in place of the table while the worker is building the index. */
  loading?: ReactNode;
  onRowClick?: (row: ViewRow) => void;
}

/* ------------------------------------------------------------------- cells -- */

function pieces(runs: readonly CellPiece[], c: ClassNames): ReactNode[] {
  return runs.map((p, i) => {
    if (p.op === 'delete') return <del key={i} className={c.del}>{p.text}</del>;
    if (p.op === 'insert') return <ins key={i} className={c.ins}>{p.text}</ins>;
    return <Fragment key={i}>{p.text}</Fragment>;
  });
}

function cellBody(cell: ViewCell, c: ClassNames): ReactNode {
  if (cell.blank) return null;
  if (cell.old === undefined) {
    return cell.pieces ? pieces(cell.pieces, c) : cell.text;
  }
  // Old then new, which is the order the HTML emitter writes. Where the old value
  // sits visually is the stylesheet's decision, driven by data-old-value-position
  // on the container, so that the DOM order a screen reader and a copy see is the
  // same in a saved report and in the live view.
  return (
    <>
      <span className={c.old}>{cell.oldPieces ? pieces(cell.oldPieces, c) : cell.old}</span>
      <span className={c.new}>{cell.pieces ? pieces(cell.pieces, c) : cell.text}</span>
    </>
  );
}

function Cell({
  cell,
  c,
  extra,
}: {
  cell: ViewCell;
  c: ClassNames;
  extra: string | undefined;
}) {
  const className = extra ? `${cellClass(c, cell.flags)} ${extra}` : cellClass(c, cell.flags);
  return (
    <td
      className={className}
      role="gridcell"
      data-column={cell.name}
      {...(cell.key ? { 'data-key': 'true' } : {})}
      {...(cell.finding ? { 'data-finding': cell.finding } : {})}
      {...(cell.ariaLabel ? { 'aria-label': cell.ariaLabel } : {})}
    >
      {cellBody(cell, c)}
    </td>
  );
}

/* -------------------------------------------------------------------- rows -- */

function Row({
  row,
  c,
  classes,
  onClick,
}: {
  row: ViewRow;
  c: ClassNames;
  classes: CsvDiffClassNames | undefined;
  onClick: ((row: ViewRow) => void) | undefined;
}) {
  const extraRow = typeof classes?.row === 'function' ? classes.row(row) : classes?.row;
  const base = rowClass(c, row.kind, row.moved, row.hasFinding);
  const cellExtra = classes?.cell;

  return (
    <tr
      className={extraRow ? `${base} ${extraRow}` : base}
      role="row"
      // aria-rowindex is 1 based and counts the header, which is why a report row
      // at index 0 is row 2. Without it a screen reader announces "row 3 of 40"
      // for a table that is virtualizing 90,000.
      aria-rowindex={row.index + 2}
      data-change={row.kind}
      data-variant={row.variant}
      {...(row.moved ? { 'data-moved': 'true' } : {})}
      {...(row.pane ? { 'data-pane': row.pane } : {})}
      {...(onClick ? { onClick: () => onClick(row) } : {})}
    >
      <td className={c.num}>{row.rowNumber ?? ''}</td>
      <td className={c.mark} aria-hidden="true">
        {row.marker}
      </td>
      {row.cells.map((cell) => (
        <Cell
          key={cell.column}
          cell={cell}
          c={c}
          extra={typeof cellExtra === 'function' ? cellExtra(cell) : cellExtra}
        />
      ))}
    </tr>
  );
}

function Spacer({ height, span, c }: { height: number; span: number; c: ClassNames }) {
  if (height <= 0) return null;
  // The two spacers hold the scrollbar honest: they carry the height of every row
  // that is not in the DOM, so a 90,000 row report scrolls as if it all existed.
  // Height on the cell rather than the row, because a row's height is a minimum
  // and a cell's is not.
  return (
    <tr className={c.spacer} aria-hidden="true">
      <td className={c.spacer} colSpan={span} style={{ height, padding: 0 }} />
    </tr>
  );
}

/* ------------------------------------------------------------------- table -- */

function Grid({
  rows,
  columns,
  widths,
  isKey,
  c,
  classes,
  rowCount,
  topHeight,
  bottomHeight,
  onRowClick,
  label,
}: {
  rows: ViewRow[];
  columns: readonly string[];
  widths: (column: number) => number;
  isKey: (column: number) => boolean;
  c: ClassNames;
  classes: CsvDiffClassNames | undefined;
  rowCount: number;
  topHeight: number;
  bottomHeight: number;
  onRowClick: ((row: ViewRow) => void) | undefined;
  label: string | undefined;
}) {
  const span = columns.length + 2;
  return (
    <table
      className={c.table}
      role="grid"
      aria-rowcount={rowCount + 1}
      aria-colcount={span}
      {...(label ? { 'aria-label': label } : {})}
    >
      {/* Column widths live in the view model and are applied here rather than on
          every cell: a drag then re-lays out the table without React touching a
          single one of the 600 cells in the viewport. */}
      <colgroup>
        <col style={{ width: '6ch' }} />
        <col style={{ width: '2ch' }} />
        {columns.map((_, i) => (
          <col key={i} style={{ width: widths(i) }} />
        ))}
      </colgroup>
      <thead>
        <tr role="row">
          <th className={`${c.th} ${c.num}`} scope="col">
            row
          </th>
          {/* The marker column's heading is a label rather than text: the glyph
              below it is the non colour signal for the change kind, and a visible
              heading for a two character column is noise. */}
          <th className={`${c.th} ${c.mark}`} scope="col" aria-label="change kind" />
          {columns.map((name, i) => (
            <th
              key={i}
              className={c.th}
              scope="col"
              data-column={name}
              {...(isKey(i) ? { 'data-key': 'true' } : {})}
            >
              {name}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        <Spacer height={topHeight} span={span} c={c} />
        {rows.map((row) => (
          <Row
            key={`${row.index}:${row.variant}:${row.pane ?? ''}`}
            row={row}
            c={c}
            classes={classes}
            onClick={onRowClick}
          />
        ))}
        <Spacer height={bottomHeight} span={span} c={c} />
      </tbody>
    </table>
  );
}

/* ---------------------------------------------------------------- component -- */

export function CsvDiffTable(props: CsvDiffTableProps) {
  const { source, theme, height, classNames: classes, loading, onRowClick, ...options } = props;
  const view = useDiffView(source, options);
  const { snapshot: s, model } = view;

  const c = classNames(model.classPrefix);
  const style: CSSProperties = {
    ...themeToCssVars(theme, model.classPrefix),
    ...model.columnWidthVars(),
    ...(height !== undefined ? { height } : {}),
  };

  const rendered = (s.end - s.start) * s.slotHeight;
  const bottom = Math.max(0, s.totalHeight - s.offsetTop - rendered);
  const widths = (col: number) => model.columnWidth(col);
  const isKey = (col: number) => model.isKeyColumn(col);

  const body =
    s.status === 'loading' ? (
      // Not wrapped in another element carrying the report class: the container
      // below already has it, and nesting two would apply the sheet twice.
      (loading ?? null)
    ) : s.layout === 'sideBySide' ? (
      <>
        <div className={c.pane}>
          <Grid
            rows={s.rows.filter((r) => r.pane === 'source')}
            columns={s.columns}
            widths={widths}
            isKey={isKey}
            c={c}
            classes={classes}
            rowCount={s.rowCount}
            topHeight={s.offsetTop}
            bottomHeight={bottom}
            onRowClick={onRowClick}
            label="source"
          />
        </div>
        <div className={c.pane}>
          <Grid
            rows={s.rows.filter((r) => r.pane === 'target')}
            columns={s.columns}
            widths={widths}
            isKey={isKey}
            c={c}
            classes={classes}
            rowCount={s.rowCount}
            topHeight={s.offsetTop}
            bottomHeight={bottom}
            onRowClick={onRowClick}
            label="target"
          />
        </div>
      </>
    ) : (
      <Grid
        rows={s.rows}
        columns={s.columns}
        widths={widths}
        isKey={isKey}
        c={c}
        classes={classes}
        rowCount={s.rowCount}
        topHeight={s.offsetTop}
        bottomHeight={bottom}
        onRowClick={onRowClick}
        label={undefined}
      />
    );

  return (
    <div
      ref={view.ref}
      className={classes?.report ? `${c.report} ${classes.report}` : c.report}
      style={style}
      // The container is the single scroll container for both axes, which is what
      // keeps the sticky header from desynchronizing during a fast scroll.
      onScroll={view.onScroll}
      onKeyDown={view.onKeyDown}
      tabIndex={0}
      data-schema-version={DIFF_ROW_SCHEMA_VERSION}
      data-virtualized="true"
      data-layout={s.layout}
      data-old-value-position={s.oldValuePosition}
      {...(s.pending ? { 'data-pending': 'true' } : {})}
    >
      {body}
    </div>
  );
}
