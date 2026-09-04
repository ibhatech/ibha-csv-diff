/**
 * The row shape the binding decodes to, reconciled against the C contract.
 *
 * These types were originally written in Phase 0, before the engine existed, and
 * they described a shape the engine never implemented while exporting the same
 * version number. That is the worst of both worlds: a consumer that trusts
 * `DIFF_ROW_SCHEMA_VERSION` and reads `oldValue` gets `undefined` on every cell
 * and renders a report full of empty old values without a single error.
 *
 * So the C side wins, because it is the side that is implemented, tested and
 * fuzzed. Four things changed, and each is a decision rather than a rename:
 *
 *   1. `moved` is a flag on the row, not a kind. A row can move and be modified
 *      in one edit, and a single enum loses one of the two facts.
 *   2. A cell carries `source` and `target`, not `oldValue` and `newValue`, and
 *      **`source` is present exactly when the cell differs in bytes from the
 *      target**. Its absence means the two sides are byte identical. This is
 *      what keeps an unchanged 90,000 row report from being written out twice,
 *      and a consumer that reads a missing `source` as an empty string will
 *      render every unchanged cell as a deletion.
 *   3. `violations?: string[]` becomes typed findings carrying the declared
 *      limit or precision they failed against.
 *   4. The summary carries findings counts, column counts and `identical`.
 *
 * Everything here mirrors the JSONL emitter of core/src/emit.c field for field,
 * so a consumer can move between the streamed rows and a saved JSONL report
 * without a translation layer.
 */

import { C } from './abi.ts';

/**
 * Stable, versioned row shape, taken from the engine rather than restated.
 *
 * The emitter and the consumer are separate components that must agree on this,
 * per spec 13.3, so it is a public contract and changing a shape is a breaking
 * change that bumps it.
 */
export const DIFF_ROW_SCHEMA_VERSION: number = C.SCHEMA_VERSION;

/** Moved is deliberately absent: it is a flag on the row, not a kind. */
export type ChangeKind = 'unchanged' | 'modified' | 'added' | 'deleted';

/** Index order matches ibha_csvd_row_kind, so it doubles as the decode table. */
export const CHANGE_KINDS: readonly ChangeKind[] = ['unchanged', 'modified', 'added', 'deleted'];

/**
 * The validation findings of spec 13.5. These are output, not errors: aborting
 * on the first empty REQUIRED cell would hide the other four hundred and make
 * the feature useless.
 */
export type FindingKind = 'requiredEmpty' | 'tooLong' | 'notNumeric' | 'precision';

export interface RowFinding {
  /** Column index within the compared columns, not within the source file. */
  column: number;
  name: string;
  kind: FindingKind;
  /** The declared VARCHAR(n) or CHAR(n) length, on `tooLong`. */
  limit?: number;
  /** The declared DECIMAL(p,s), on `precision`. -1 where the type declared none. */
  precision?: number;
  scale?: number;
}

export interface DiffCell {
  /** Index within the compared columns. */
  column: number;
  name: string;
  /**
   * The source side's value, present **only when it differs in bytes** from the
   * target. Absent means the two sides are byte identical; it never means empty.
   */
  source?: string;
  /** The target side's value. Absent on a deleted row, which has no target. */
  target?: string;
  /** Differs under the comparator that the declared type selects. */
  changed: boolean;
  /** Differs in bytes but is equal once normalized, per spec 5.3. */
  suppressed: boolean;
  /** Cut at `maxCellBytes`, backing off to a UTF-8 boundary. */
  truncated?: boolean;
  /** The value was not well formed UTF-8 and carries U+FFFD in place of the
   *  offending bytes, exactly as the HTML and JSON emitters write it. */
  invalidUtf8?: boolean;
}

export interface DiffRow {
  kind: ChangeKind;
  /** Outside the longest increasing subsequence, per spec 6.2. */
  moved: boolean;
  moveDistance: number;
  /** 1 based record number, counting parsed records rather than physical lines.
   *  null when the row has no counterpart on that side. */
  sourceRow: number | null;
  targetRow: number | null;
  /** The KEY column values of the row the report carries. null when the schema
   *  declares no key column, which is the all-keys path of spec 6.4. */
  key: string[] | null;
  changedCells: number;
  suppressedCells: number;
  /** Empty when the read asked for the edit script without values. */
  cells: DiffCell[];
  /** Empty when the row satisfies the schema, which is the common case. */
  findings: RowFinding[];
}

export interface DiffSummary {
  schemaVersion: number;
  /** No modified, added, deleted or moved row. Unchanged rows carrying a finding
   *  do not make a pair non-identical: the files agree, the data has a problem. */
  identical: boolean;
  rows: {
    unchanged: number;
    modified: number;
    added: number;
    deleted: number;
    moved: number;
    /** How many rows the cursor yields in total, before changes-only filtering. */
    report: number;
  };
  /** Cell counts accumulate as a cursor advances, so these are the numbers of
   *  exactly one pass and are final only once the diff has been drained. The
   *  summary emitter recounts from zero for the same reason. */
  cells: { changed: number; suppressed: number };
  findings: {
    total: number;
    rows: number;
    requiredEmpty: number;
    tooLong: number;
    notNumeric: number;
    precision: number;
    /** False when the caller turned validation off, which makes the zeros above
     *  mean "not looked at" rather than "nothing found". */
    enabled: boolean;
  };
  matching: {
    allKeys: boolean;
    pairedBySimilarity: number;
    pairingTruncated: boolean;
    movesForcedOff: boolean;
  };
  /**
   * `compared` is what the report rows actually carry: the columns the two files
   * have in common, in the source's order. It equals the source's column count
   * unless the caller allowed an added or a removed column.
   */
  columns: { compared: number; added: number; removed: number };
  /** What auto-detection made of the uploaded file's header, per spec 13.8.
   *  `namesOnly` means it kept the column name row and inherited the rest. */
  targetHeader: { rows: number; namesOnly: boolean };
  /**
   * Column level findings: a column the two files do not share, and metadata
   * rows that disagree with the source, which is authoritative. The list is
   * capped and `schemaFindingCount` is not, so a file that disagrees in every
   * column reports the true number without emitting it a thousand times.
   */
  schemaFindings: SchemaFinding[];
  schemaFindingCount: number;
}

export interface SchemaFinding {
  kind: 'columnAdded' | 'columnRemoved' | 'metadataDisagreement';
  column: number;
  name: string;
  /** Which header row disagreed, on `metadataDisagreement`. */
  row?: 'key' | 'required' | 'type';
  source?: string;
  target?: string;
}

/* ------------------------------------------------------- cell level diff -- */

export type CellDiffMode = 'none' | 'word' | 'character' | 'word-then-character';

/** Index order matches ibha_csvd_cell_diff_mode. */
export const CELL_DIFF_MODES: readonly CellDiffMode[] = [
  'none',
  'word',
  'character',
  'word-then-character',
];

export type SegmentOp = 'equal' | 'delete' | 'insert';

/** Index order matches ibha_csvd_seg_op. */
export const SEGMENT_OPS: readonly SegmentOp[] = ['equal', 'delete', 'insert'];

/**
 * One intra cell segment.
 *
 * `start` and `len` are **byte offsets into the logical value**: `equal` and
 * `delete` index the source value, `insert` indexes the target. They are not
 * UTF-16 code unit offsets, so they cannot be handed to `String.prototype.slice`
 * on a decoded value without conversion. `segmentsToString` in this package does
 * that conversion; getting it wrong is the classic source of mangled non ASCII
 * text, which is why the unit is stated here rather than implied.
 */
export interface TextSegment {
  op: SegmentOp;
  start: number;
  len: number;
}

/* ------------------------------------------------------------- emitters -- */

export type EmitFormat = 'jsonl' | 'csv' | 'html' | 'summary';

/** Index order matches ibha_csvd_emit_format. */
export const EMIT_FORMATS: readonly EmitFormat[] = ['jsonl', 'csv', 'html', 'summary'];

export interface EmitOptions {
  /** Skip rows that are unchanged, unmoved and carry neither a suppressed cell
   *  nor a finding. A finding on an otherwise unchanged row is the point of the
   *  run, so it is never what this drops. */
  changesOnly?: boolean;
  /** Off gives the edit script alone, which is what a pass/fail check wants. */
  includeValues?: boolean;
  /** HTML only. The other formats carry values rather than markup. */
  cellDiff?: CellDiffMode;
  maxCellBytes?: number;
  /** Stop after this many report rows. This is how HTML output is kept bounded:
   *  a 90,000 row diff as one HTML string is tens of megabytes of DOM. */
  maxRows?: number;
  /** CSV only. A value opening with `=`, `+`, `@`, a tab or a CR is a formula to
   *  Excel, so a report of untrusted data is a script delivery mechanism unless
   *  something intervenes. On by default. */
  csvFormulaGuard?: boolean;
  csvDelimiter?: string;
  /** HTML only. Validated against `[A-Za-z][A-Za-z0-9_-]{0,31}` and refused
   *  otherwise: it is the one caller supplied string that reaches the markup, so
   *  it is checked rather than escaped. */
  classPrefix?: string;
}

/* ------------------------------------------------------- comparison input -- */

export interface HeaderOptions {
  /** 4 (the default), 1 for a names-only file, or 0 for no header at all. */
  rows?: number;
  keyRow?: number;
  requiredRow?: number;
  typeRow?: number;
  nameRow?: number;
}

export interface ComparisonOptions {
  trimWhitespace?: boolean;
  /** CHAR(n) ignores trailing pad even when trimWhitespace is off. */
  charIgnorePad?: boolean;
  /** DECIMAL and INTEGER compare by value, so 1.50 equals 1.5 and 007 equals 7. */
  numeric?: boolean;
  booleans?: boolean;
  /** Comma separated, matched case insensitively. */
  boolTrue?: string;
  boolFalse?: string;
  /**
   * Spec 6.6 and 13.10. Both default to false, which is the locked decision:
   * the uploaded file carries the same columns as the source, in the same order.
   *
   * They are separate because the two cases are not equally serious. A salesman
   * appending a column is being helpful; a column that has gone missing is data
   * loss. Neither relaxes column order, neither excuses a missing KEY column,
   * and neither works on a file with no column name row.
   */
  allowAddedColumns?: boolean;
  allowRemovedColumns?: boolean;
}

export interface MatchingOptions {
  detectMoves?: boolean;
  /** The caller's assertion that the source side has a meaningful row order. A
   *  CSV file always does, so this defaults to true and is a safety valve for a
   *  future caller feeding an unordered SELECT. */
  sourceOrdered?: boolean;
  deletedRowPlacement?: 'anchored' | 'end';
  /** An integer percentage rather than a fraction, so the result cannot depend
   *  on floating point: spec 3.2 requires the wasm and native builds to agree. */
  similarityPercent?: number;
  similarityCandidates?: number;
  /** Fail when the schema declares no key column instead of taking the all-keys
   *  path of spec 6.4. */
  requireKey?: boolean;
}

export interface CsvDiffOptions {
  header?: HeaderOptions;
  dialect?: { delimiter?: string; quote?: string; stripBom?: boolean };
  comparison?: ComparisonOptions;
  matching?: MatchingOptions;
  /** Count the cells that are equal only because normalization suppressed a
   *  difference. Costs a cell walk over rows whose raw digests differ. */
  countSuppressed?: boolean;
  /** The validation findings of spec 13.5. On by default, and the only thing
   *  that makes the cursor read an unchanged row's cells. */
  validate?: boolean;
  limits?: { maxBytes?: number; maxRows?: number; maxColumns?: number };
  signal?: AbortSignal;
}

export interface RowReadOptions {
  /** Decode cell values. Off yields the edit script alone, which is what a
   *  pass/fail check or a row count wants and costs nothing. */
  includeValues?: boolean;
  /** Skip rows that are unchanged, unmoved and carry no finding. */
  changesOnly?: boolean;
  /** Truncate a decoded value at this many logical bytes, backing off to a UTF-8
   *  boundary, and mark the cell truncated. */
  maxCellBytes?: number;
}
