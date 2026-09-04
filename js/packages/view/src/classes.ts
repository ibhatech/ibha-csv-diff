/**
 * The class name contract, and the one caller supplied string that reaches markup.
 *
 * **This is the emitter's class set, not a parallel one.** `core/src/emit.c`
 * writes a fixed, compiled in list of suffixes behind a validated prefix, and a
 * consumer stylesheet has to work against the emitter's output untouched. So the
 * names here are that list, read off the emitter, rather than the BEM-ish list
 * sketched in spec 8.3 before the emitter existed. Spec section 13 overrides
 * sections 1 to 12 where they conflict, and 13.0 fixes the prefix at
 * `ibha-csvd-`.
 *
 * The set is closed on purpose. Everything the view needs to express that the
 * emitter does not, which is the layout mode, which physical row of a stacked
 * pair this is, which pane a side by side row belongs to and where the old value
 * sits, goes into a `data-` attribute instead of into a new class. A class the
 * emitter never writes is a rule in the consumer's stylesheet that silently does
 * nothing on half their output.
 *
 * Three exceptions are marked `viewOnly` below, because they are elements the
 * emitter has no equivalent of at all rather than states of an element it does
 * write. They are additive: an emitter document is unaffected by their existence,
 * and a stylesheet written against the emitter still styles the view correctly
 * without them.
 */

/** Spec 13.0. Configurable at the component level, for a host page collision. */
export const DEFAULT_CLASS_PREFIX = 'ibha-csvd-';

/**
 * Exactly what `core/src/emit.c` can write, in the order it is documented in.
 *
 * `report` and `table` are the two containers, `th` and `num` the header and the
 * row number gutter, `row` and `cell` the two repeated elements, the four kinds
 * plus `moved` the row state, `changed`, `suppressed` and `finding` the cell
 * state, `old` and `new` the two sides of a changed cell, and `del` and `ins` the
 * intra cell segments.
 */
export const EMITTER_CLASS_SUFFIXES = [
  'report', 'table', 'th', 'num', 'row', 'cell',
  'unchanged', 'modified', 'added', 'deleted', 'moved',
  'changed', 'suppressed', 'finding', 'old', 'new', 'del', 'ins',
] as const;

/** Elements the emitter has no equivalent of. See the note at the top. */
export const VIEW_ONLY_CLASS_SUFFIXES = ['mark', 'spacer', 'pane'] as const;

export type ClassSuffix =
  | (typeof EMITTER_CLASS_SUFFIXES)[number]
  | (typeof VIEW_ONLY_CLASS_SUFFIXES)[number];

/**
 * The engine's own pattern, restated. `core/src/emit.c` refuses a prefix that
 * does not match it and the binding refuses it before the engine sees it, per
 * handoff 4.5: a caller's bad argument must not poison a comparison that was
 * otherwise complete. The view is a third caller and holds the same line.
 *
 * It is checked rather than escaped because it is the one caller supplied string
 * that is interpolated into markup rather than into text.
 */
export const CLASS_PREFIX_PATTERN = /^[A-Za-z][A-Za-z0-9_-]{0,31}$/;

export function classPrefixValid(prefix: string): boolean {
  return CLASS_PREFIX_PATTERN.test(prefix);
}

/** Throws on a prefix the engine would refuse, naming the pattern. */
export function assertClassPrefix(prefix: string): string {
  if (!classPrefixValid(prefix)) {
    throw new RangeError(
      `the HTML class prefix must match ${CLASS_PREFIX_PATTERN.source}, received ${JSON.stringify(prefix)}`,
    );
  }
  return prefix;
}

export type ClassNames = Record<ClassSuffix, string>;

/**
 * The full class table for one prefix.
 *
 * Built once per prefix by a component rather than per row: at 50 visible rows
 * times 12 columns this is otherwise 600 string concatenations a frame, for a
 * result that never changes.
 */
export function classNames(prefix: string = DEFAULT_CLASS_PREFIX): ClassNames {
  assertClassPrefix(prefix);
  const out = {} as ClassNames;
  for (const s of EMITTER_CLASS_SUFFIXES) out[s] = prefix + s;
  for (const s of VIEW_ONLY_CLASS_SUFFIXES) out[s] = prefix + s;
  return out;
}

/* ------------------------------------------------------------- cell flags -- */

/**
 * The cell flag byte, restated from `ibha_csvdiff.h`.
 *
 * The compact page hands these out one byte per cell and they are what a view
 * styles from. They are duplicated here rather than imported so that the headless
 * layer has no runtime dependency on the engine package for something this small;
 * `flagsAgreeWithEngine` in the test suite asserts the two do not drift.
 */
export const CELL_CHANGED = 1 << 0;
export const CELL_SUPPRESSED = 1 << 1;
export const CELL_REQUIRED_EMPTY = 1 << 2;
export const CELL_TOO_LONG = 1 << 3;
export const CELL_NOT_NUMERIC = 1 << 4;
export const CELL_PRECISION = 1 << 5;
export const CELL_FINDING =
  CELL_REQUIRED_EMPTY | CELL_TOO_LONG | CELL_NOT_NUMERIC | CELL_PRECISION;

export type FindingName = 'requiredEmpty' | 'tooLong' | 'notNumeric' | 'precision';

/**
 * What `data-finding` carries, in the emitter's priority order.
 *
 * A cell can fail more than one rule, an over-long value that is also not
 * numeric, and the attribute holds one name. `finding_class` in the emitter picks
 * the first of these four; picking a different one here would make the same cell
 * style differently in a saved report and in the view.
 */
export function findingName(flags: number): FindingName | null {
  if (flags & CELL_REQUIRED_EMPTY) return 'requiredEmpty';
  if (flags & CELL_TOO_LONG) return 'tooLong';
  if (flags & CELL_NOT_NUMERIC) return 'notNumeric';
  if (flags & CELL_PRECISION) return 'precision';
  return null;
}

/* --------------------------------------------------------- composed names -- */

/** 0 unchanged, 1 modified, 2 added, 3 deleted. Matches `ibha_csvd_row_kind`. */
export type RowKind = 'unchanged' | 'modified' | 'added' | 'deleted';

export const ROW_KINDS: readonly RowKind[] = ['unchanged', 'modified', 'added', 'deleted'];

/**
 * The `<tr>` class list, in the emitter's order: `row`, the kind, then `moved`
 * and `finding` when they apply.
 *
 * Order is not cosmetic. The two documents are compared byte for byte by the
 * parity test, and a stylesheet using the child combinator on a class list is
 * unaffected but a snapshot is not.
 */
export function rowClass(c: ClassNames, kind: RowKind, moved: boolean, finding: boolean): string {
  let s = `${c.row} ${c[kind]}`;
  if (moved) s += ` ${c.moved}`;
  if (finding) s += ` ${c.finding}`;
  return s;
}

/** The `<td>` class list for one cell, from its flag byte. */
export function cellClass(c: ClassNames, flags: number): string {
  let s = c.cell;
  if (flags & CELL_CHANGED) s += ` ${c.changed}`;
  if (flags & CELL_SUPPRESSED) s += ` ${c.suppressed}`;
  if (flags & CELL_FINDING) s += ` ${c.finding}`;
  return s;
}
