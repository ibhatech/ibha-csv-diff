/**
 * The binding proper: option structs written into linear memory, the streaming
 * parse, the diff, and a lazy cursor over the report.
 *
 * Nothing here materializes a diff. `rows()` walks the engine's cursor one row at
 * a time and decodes exactly the row it is standing on, which is the whole reason
 * the engine exists: a 90,000 row diff as plain JS objects is a million
 * allocations, and that is the heap explosion the wasm core was chosen to
 * prevent. A caller that wants random access asks for it explicitly and pays for
 * an index, in `handle.ts`.
 */

import { C, OFF } from './abi.ts';
import { utf8, utf8Strict, WasmHeap } from './memory.ts';
import type { EngineExports, EngineInstance } from './module.ts';
import { Engine } from './module.ts';
import { IbhaCsvError, toByteSource } from './source.ts';
import type { ByteSource, IbhaCsvSource } from './source.ts';
import {
  CELL_DIFF_MODES,
  CHANGE_KINDS,
  EMIT_FORMATS,
  SEGMENT_OPS,
} from './types.ts';
import type {
  CellDiffMode,
  CsvDiffOptions,
  DiffCell,
  DiffRow,
  DiffSummary,
  EmitFormat,
  EmitOptions,
  FindingKind,
  RowFinding,
  RowReadOptions,
  TextSegment,
} from './types.ts';

/* ---------------------------------------------------------------- status -- */

const STATUS_OK = 0;
const STATUS_IO = -2;
const STATUS_TOO_LARGE = -3;
const STATUS_BAD_CONTENT = -10;
const STATUS_INVALID_ARG = -11;

function statusToCode(status: number) {
  switch (status) {
    case STATUS_TOO_LARGE:
      return 'TOO_LARGE' as const;
    case STATUS_BAD_CONTENT:
      return 'BAD_CONTENT' as const;
    case STATUS_IO:
      return 'IO_ERROR' as const;
    case STATUS_INVALID_ARG:
      return 'INVALID_ARG' as const;
    default:
      // Everything else is a structural error the engine names for itself:
      // unterminated quote, ragged row, duplicate key, column order, no header,
      // missing key column. The name travels in `status` so a caller can switch
      // on it, because collapsing all six into one code would lose the only
      // information that makes the message actionable.
      return 'ENGINE' as const;
  }
}

/** The staging chunk. Every chunk the byte source produces is copied through
 *  here, and one larger than this is split, which the parser handles because its
 *  state machine is resumable across arbitrary chunk boundaries: a chunk may
 *  split a multi byte UTF-8 sequence, a quoted field, a "" pair, a CRLF or the
 *  BOM. That property was built in Phase 1 for the streamed download; this is the
 *  second thing that depends on it. */
const STAGING_BYTES = 1 << 20;

/** Room for one materialized cell value on the escaped path. Grown on demand. */
const CELL_SCRATCH_BYTES = 1 << 16;

/**
 * One report row as the engine holds it, before any value is decoded.
 *
 * This is the shape both consumers agree on: the streaming walk builds one and
 * throws it away, the retained index keeps a copy of every one. Row numbers are
 * already 1 based here so that nothing downstream has to remember to add one.
 */
export interface RawRow {
  /** ibha_csvd_row_kind: 0 unchanged, 1 modified, 2 added, 3 deleted. */
  kind: number;
  moved: boolean;
  moveDistance: number;
  sourceRow: number | null;
  targetRow: number | null;
  changedCells: number;
  suppressedCells: number;
  findingCells: number;
  /** One IBHA_CSVD_CELL_* byte per compared column. */
  flags: Uint8Array;
}

/** The same rule the emitters apply. A finding on an otherwise unchanged row is
 *  the point of the run, so it is never what this drops. */
export function rowIsQuiet(r: RawRow): boolean {
  return r.kind === 0 && !r.moved && r.suppressedCells === 0 && r.findingCells === 0;
}

/* ------------------------------------------------------------ table view -- */

/**
 * A reader over one `ibha_csvd_table *`.
 *
 * The columnar arrays are read as typed array views straight over linear memory
 * rather than through an accessor call per cell, which is exactly what the
 * header's note about the JS and Java bindings asks for. The views are cached and
 * re-derived whenever linear memory has been replaced, because every one of them
 * is detached by the next `memory.grow` and the engine grows memory on any call
 * that allocates.
 */
export class CsvTable {
  private readonly heap: WasmHeap;
  private readonly api: EngineExports;
  readonly ptr: number;

  readonly nRows: number;
  readonly nColumns: number;
  readonly nFields: number;
  private readonly bytesPtr: number;
  private readonly bytesLen: number;
  private readonly offPtr: number;
  private readonly lenPtr: number;
  private readonly flagsPtr: number;

  private viewOf: ArrayBuffer | null = null;
  private bytesV!: Uint8Array;
  private offV!: Uint32Array;
  private lenV!: Uint32Array;
  private flagsV!: Uint8Array;

  constructor(inst: EngineInstance, ptr: number) {
    this.heap = inst.heap;
    this.api = inst.api;
    this.ptr = ptr;

    const h = inst.heap;
    this.bytesPtr = h.ptrAt(ptr + OFF.table.bytes);
    this.bytesLen = h.u32At(ptr + OFF.table.len);
    this.offPtr = h.ptrAt(ptr + OFF.table.field_off);
    this.lenPtr = h.ptrAt(ptr + OFF.table.field_len);
    this.flagsPtr = h.ptrAt(ptr + OFF.table.field_flags);
    this.nFields = h.u32At(ptr + OFF.table.n_fields);
    this.nRows = h.u32At(ptr + OFF.table.n_rows);
    this.nColumns = h.u32At(ptr + OFF.table.n_columns);
  }

  private sync(): void {
    const buf = this.heap.memory.buffer as ArrayBuffer;
    if (buf === this.viewOf) return;
    this.viewOf = buf;
    this.bytesV = new Uint8Array(buf, this.bytesPtr, this.bytesLen);
    this.offV = new Uint32Array(buf, this.offPtr, this.nFields);
    this.lenV = new Uint32Array(buf, this.lenPtr, this.nFields);
    this.flagsV = new Uint8Array(buf, this.flagsPtr, this.nFields);
  }

  /** Field index of a cell, or -1 when the row or the column is out of range. */
  fieldIndex(row: number, col: number): number {
    if (row === C.NO_ROW) return -1;
    const f = this.api.ibha_csvd_row_field(this.ptr, row, col);
    return f === 0xffffffff ? -1 : f;
  }

  /**
   * Decodes a cell's logical value.
   *
   * The fast path reads the byte range straight out of linear memory and never
   * calls into the engine: `field_off` points past the opening quote and
   * `field_len` excludes the closing one, so for a field with no "" pair that
   * range *is* the logical value. Only an escaped field needs
   * `ibha_csvd_field_copy` to collapse the pairs, and those are rare enough that
   * the engine counts them separately.
   *
   * `maxBytes` cuts at a UTF-8 boundary rather than mid sequence, matching what
   * the emitters do, so a truncated value is still decodable text.
   */
  decode(field: number, out: DecodedCell, maxBytes = 0): void {
    this.sync();
    out.truncated = false;
    out.invalidUtf8 = false;

    let bytes: Uint8Array;
    if ((this.flagsV[field]! & C.FIELD_HAS_ESCAPE) === 0) {
      const off = this.offV[field]!;
      const len = this.lenV[field]!;
      bytes = this.bytesV.subarray(off, off + len);
    } else {
      bytes = this.copyEscaped(field);
    }

    if (maxBytes > 0 && bytes.length > maxBytes) {
      let cut = maxBytes;
      // 0b10xxxxxx is a continuation byte; back off until the cut lands on the
      // start of a sequence.
      while (cut > 0 && (bytes[cut]! & 0xc0) === 0x80) cut--;
      bytes = bytes.subarray(0, cut);
      out.truncated = true;
    }

    // Decoding strictly and falling back is exact and costs nothing on the happy
    // path. Scanning the decoded string for U+FFFD instead cannot tell a
    // replacement from a replacement character the file genuinely contained.
    try {
      out.value = utf8Strict.decode(bytes);
    } catch {
      out.value = utf8.decode(bytes);
      out.invalidUtf8 = true;
    }
  }

  private scratch = 0;
  private scratchCap = 0;

  private copyEscaped(field: number): Uint8Array {
    const need = this.api.ibha_csvd_field_logical_len(this.ptr, field);
    if (need > this.scratchCap) {
      this.scratchCap = Math.max(need, CELL_SCRATCH_BYTES);
      this.scratch = this.heap.alloc(this.scratchCap, 1);
      this.viewOf = null; // the alloc may have grown memory
    }
    const n = this.api.ibha_csvd_field_copy(this.ptr, field, this.scratch, this.scratchCap);
    this.sync();
    return this.heap.u8.subarray(this.scratch, this.scratch + n);
  }
}

export interface DecodedCell {
  value: string;
  truncated: boolean;
  invalidUtf8: boolean;
}

/* ------------------------------------------------------- option structs -- */

function writeCompareOpts(inst: EngineInstance, ptr: number, o: CsvDiffOptions): void {
  const { api, heap } = inst;
  api.ibha_csvd_compare_opts_init(ptr);
  const c = o.comparison;
  if (!c) return;

  const b = (v: boolean | undefined, at: number) => {
    if (v !== undefined) heap.setI32(ptr + at, v ? 1 : 0);
  };
  b(c.trimWhitespace, OFF.compare_opts.trim_whitespace);
  b(c.charIgnorePad, OFF.compare_opts.char_ignore_pad);
  b(c.numeric, OFF.compare_opts.numeric);
  b(c.booleans, OFF.compare_opts.booleans);
  b(c.allowAddedColumns, OFF.compare_opts.allow_added_columns);
  b(c.allowRemovedColumns, OFF.compare_opts.allow_removed_columns);

  // The engine keeps the pointer and does not copy, so these have to outlive the
  // parse and the diff. They live in the heap's bump region, which is only
  // released when the whole instance is, so they do.
  if (c.boolTrue !== undefined) {
    heap.setPtr(ptr + OFF.compare_opts.bool_true, heap.allocCString(c.boolTrue));
  }
  if (c.boolFalse !== undefined) {
    heap.setPtr(ptr + OFF.compare_opts.bool_false, heap.allocCString(c.boolFalse));
  }
}

function oneByte(s: string | undefined, what: string): number | undefined {
  if (s === undefined) return undefined;
  const enc = new TextEncoder().encode(s);
  if (enc.length !== 1) {
    throw new IbhaCsvError('INVALID_ARG', `${what} must be a single byte, received ${JSON.stringify(s)}`);
  }
  return enc[0]!;
}

/* --------------------------------------------------------------- session -- */

export interface ParsedSide {
  parser: number;
  table: number;
  schema: number;
}

/**
 * One comparison, holding one wasm instance.
 *
 * The instance is per diff on purpose: linear memory only grows, and the engine's
 * allocator frees nothing individually because a context releases everything at
 * once. A shared instance would therefore carry the high water mark of every
 * comparison it had ever run.
 */
export class DiffEngine {
  private readonly inst: EngineInstance;
  private readonly api: EngineExports;
  private readonly heap: WasmHeap;
  private readonly opts: CsvDiffOptions;

  private ctx = 0;
  private diff = 0;
  private staging = 0;
  private disposed = false;

  /** The tables the diff is actually comparing, which are the projected ones
   *  when the column policy let the two files differ. */
  private srcTable!: CsvTable;
  private tgtTable!: CsvTable;
  private srcSchemaPtr = 0;
  private columnNames: string[] = [];
  private keyColumns: number[] = [];
  private colSizePtr = 0;
  private colScalePtr = 0;

  private constructor(inst: EngineInstance, options: CsvDiffOptions) {
    this.inst = inst;
    this.api = inst.api;
    this.heap = inst.heap;
    this.opts = options;
  }

  static async compare(
    engine: Engine,
    source: IbhaCsvSource,
    target: IbhaCsvSource,
    options: CsvDiffOptions = {},
  ): Promise<DiffEngine> {
    const self = new DiffEngine(engine.instantiate(), options);
    try {
      await self.run(source, target);
      return self;
    } catch (err) {
      self.dispose();
      throw err;
    }
  }

  private async run(source: IbhaCsvSource, target: IbhaCsvSource): Promise<void> {
    const { api, heap } = this;
    const maxBytes = this.opts.limits?.maxBytes ?? C.DEFAULT_MAX_BYTES;

    const limits = heap.alloc(OFF.limits.__size, OFF.limits.__align);
    api.ibha_csvd_limits_init(limits);
    heap.setU64(limits + OFF.limits.max_bytes, maxBytes);
    if (this.opts.limits?.maxRows !== undefined) {
      heap.setU32(limits + OFF.limits.max_rows, this.opts.limits.maxRows);
    }
    if (this.opts.limits?.maxColumns !== undefined) {
      heap.setU32(limits + OFF.limits.max_columns, this.opts.limits.maxColumns);
    }

    this.ctx = api.ibha_csvd_ctx_new(limits);
    if (!this.ctx) throw new IbhaCsvError('ENGINE', 'the engine could not allocate a context');

    this.staging = heap.alloc(STAGING_BYTES, 8);

    const src = await this.parseSide(source, null, maxBytes);
    const tgt = await this.parseSide(target, src, maxBytes);

    const dopts = heap.alloc(OFF.diff_opts.__size, OFF.diff_opts.__align);
    api.ibha_csvd_diff_opts_init(dopts);
    writeCompareOpts(this.inst, dopts + OFF.diff_opts.compare, this.opts);

    const m = this.opts.matching;
    if (m?.detectMoves !== undefined) {
      heap.setI32(dopts + OFF.diff_opts.detect_moves, m.detectMoves ? 1 : 0);
    }
    if (m?.sourceOrdered !== undefined) {
      heap.setI32(dopts + OFF.diff_opts.source_ordered, m.sourceOrdered ? 1 : 0);
    }
    if (m?.requireKey !== undefined) {
      heap.setI32(dopts + OFF.diff_opts.require_key, m.requireKey ? 1 : 0);
    }
    if (m?.deletedRowPlacement !== undefined) {
      heap.setU8(dopts + OFF.diff_opts.deleted_placement, m.deletedRowPlacement === 'end' ? 1 : 0);
    }
    if (m?.similarityPercent !== undefined) {
      heap.setU32(dopts + OFF.diff_opts.similarity_percent, m.similarityPercent);
    }
    if (m?.similarityCandidates !== undefined) {
      heap.setU32(dopts + OFF.diff_opts.similarity_k, m.similarityCandidates);
    }
    if (this.opts.countSuppressed !== undefined) {
      heap.setI32(dopts + OFF.diff_opts.count_suppressed, this.opts.countSuppressed ? 1 : 0);
    }
    if (this.opts.validate !== undefined) {
      heap.setI32(dopts + OFF.diff_opts.validate, this.opts.validate ? 1 : 0);
    }

    this.diff = api.ibha_csvd_diff_run(this.ctx, src.table, src.schema, tgt.table, tgt.schema, dopts);
    if (!this.diff) this.throwEngineError();

    // The projected tables, not the parsed ones: under the column policy of spec
    // 6.6 column c of a report row is column c of these, and reading the parsed
    // table instead would silently read the wrong column.
    this.srcTable = new CsvTable(this.inst, api.ibha_csvd_diff_table(this.diff, 0));
    this.tgtTable = new CsvTable(this.inst, api.ibha_csvd_diff_table(this.diff, 1));
    this.srcSchemaPtr = api.ibha_csvd_diff_schema(this.diff, 0);
    this.readSchema();
  }

  private async parseSide(
    input: IbhaCsvSource,
    expect: ParsedSide | null,
    maxBytes: number,
  ): Promise<ParsedSide> {
    const { api, heap } = this;
    const src: ByteSource = await toByteSource(input, { maxBytes });

    const popts = heap.alloc(OFF.parse_opts.__size, OFF.parse_opts.__align);
    api.ibha_csvd_parse_opts_init(popts);
    writeCompareOpts(this.inst, popts + OFF.parse_opts.compare, this.opts);

    const d = this.opts.dialect;
    const dialect = popts + OFF.parse_opts.dialect;
    const delim = oneByte(d?.delimiter, 'dialect.delimiter');
    const quote = oneByte(d?.quote, 'dialect.quote');
    if (delim !== undefined) heap.setU8(dialect + OFF.dialect.delimiter, delim);
    if (quote !== undefined) heap.setU8(dialect + OFF.dialect.quote, quote);
    if (d?.stripBom !== undefined) heap.setU8(dialect + OFF.dialect.strip_bom, d.stripBom ? 1 : 0);

    const header = popts + OFF.parse_opts.header;
    if (expect) {
      // Spec 13.8: the source file is authoritative for all schema metadata, and
      // the uploaded file's header count is detected against it rather than
      // guessed. A sales user who kept only the column name row is the case this
      // exists for.
      heap.setU32(header + OFF.header_opts.rows, C.HEADER_AUTO);
      heap.setPtr(popts + OFF.parse_opts.expect_table, expect.table);
      heap.setPtr(popts + OFF.parse_opts.expect_schema, expect.schema);
    } else {
      const h = this.opts.header ?? {};
      const rows = h.rows ?? 4;
      heap.setU32(header + OFF.header_opts.rows, rows);
      heap.setU32(header + OFF.header_opts.key_row, h.keyRow ?? (rows >= 1 ? 1 : 0));
      heap.setU32(header + OFF.header_opts.required_row, h.requiredRow ?? (rows >= 2 ? 2 : 0));
      heap.setU32(header + OFF.header_opts.type_row, h.typeRow ?? (rows >= 3 ? 3 : 0));
      heap.setU32(header + OFF.header_opts.name_row, h.nameRow ?? rows);
    }

    // Without the hint the index arrays double as they grow and the arena never
    // reclaims the abandoned copies, so retaining N bytes peaks at roughly 4x N
    // rather than 1x. That ratio is what sets how many concurrent diffs fit on a
    // batch worker, so it is always supplied when the source knows its size.
    if (src.sizeHint !== undefined) heap.setU64(popts + OFF.parse_opts.size_hint, src.sizeHint);

    const parser = api.ibha_csvd_parse_begin(this.ctx, popts);
    if (!parser) this.throwEngineError();

    for (;;) {
      this.opts.signal?.throwIfAborted();
      const chunk = await src.read();
      if (chunk === null) break;

      // A chunk larger than the staging buffer is split rather than staged whole,
      // which costs nothing because the parser resumes across any boundary.
      for (let off = 0; off < chunk.length; off += STAGING_BYTES) {
        const take = Math.min(STAGING_BYTES, chunk.length - off);
        this.heap.u8.set(chunk.subarray(off, off + take), this.staging);
        const st = api.ibha_csvd_parse_chunk(parser, this.staging, take);
        if (st !== STATUS_OK) this.throwEngineError(st);
      }
    }

    const st = api.ibha_csvd_parse_finish(parser);
    if (st !== STATUS_OK) this.throwEngineError(st);

    return {
      parser,
      table: api.ibha_csvd_table_of(parser),
      schema: api.ibha_csvd_schema_of(parser),
    };
  }

  /** Column names and key columns, read once. The names come from the report's
   *  own header row rather than from the file, because with a projected table
   *  they are not the same list. */
  private readSchema(): void {
    const { heap } = this;
    const s = this.srcSchemaPtr;
    const n = this.api.ibha_csvd_diff_columns(this.diff);
    const nameRow = heap.u32At(s + OFF.schema.name_row);
    const flagsPtr = heap.ptrAt(s + OFF.schema.col_flags);
    this.colSizePtr = heap.ptrAt(s + OFF.schema.col_size);
    this.colScalePtr = heap.ptrAt(s + OFF.schema.col_scale);

    const cell: DecodedCell = { value: '', truncated: false, invalidUtf8: false };
    this.columnNames = [];
    this.keyColumns = [];
    for (let c = 0; c < n; c++) {
      if (nameRow !== C.NO_ROW) {
        const f = this.srcTable.fieldIndex(nameRow, c);
        if (f >= 0) {
          this.srcTable.decode(f, cell);
          this.columnNames.push(cell.value);
        } else {
          this.columnNames.push('');
        }
      } else {
        this.columnNames.push('');
      }
      if (flagsPtr && (heap.u8At(flagsPtr + c) & C.COL_KEY) !== 0) this.keyColumns.push(c);
    }
  }

  get columns(): readonly string[] {
    return this.columnNames;
  }

  get nColumns(): number {
    return this.columnNames.length;
  }

  /** Bytes the engine has reserved from the host, so a batch driver can size its
   *  concurrency per spec 2.6.5. */
  get bytesReserved(): number {
    return Number(this.api.ibha_csvd_ctx_bytes_reserved(this.ctx));
  }

  /* ----------------------------------------------------------- the cursor -- */

  /**
   * Walks the report one row at a time.
   *
   * The row object is freshly built per iteration and the engine's own row buffer
   * is reused, so nothing accumulates on either side of the boundary. This is the
   * primary interface: everything else in this package is a consumer of it.
   */
  *rows(read: RowReadOptions = {}): Generator<DiffRow> {
    const includeValues = read.includeValues ?? true;
    const maxCellBytes = read.maxCellBytes ?? 0;
    for (const raw of this.rawRows({ changesOnly: read.changesOnly ?? false, copy: false })) {
      yield this.buildRow(raw, includeValues, maxCellBytes);
    }
  }

  /**
   * The cursor, undecoded.
   *
   * `copy: false` reuses one flags buffer across the whole walk, which is right
   * for a consumer that finishes with each row before asking for the next. A
   * consumer that keeps rows must ask for copies, because the engine's own flags
   * buffer is reused too and the borrowed view would silently change under it.
   */
  *rawRows(read: { changesOnly?: boolean; copy?: boolean } = {}): Generator<RawRow> {
    this.assertLive();
    const { api } = this;
    const changesOnly = read.changesOnly ?? false;
    const scratch = read.copy === false ? this.rawScratch : undefined;

    const cursor = api.ibha_csvd_cursor_open(this.diff);
    if (!cursor) this.throwEngineError();

    for (;;) {
      this.opts.signal?.throwIfAborted();
      const more = api.ibha_csvd_cursor_next(cursor);
      if (more === 0) break;
      if (more < 0) this.throwEngineError(more);

      const raw = this.readRawRow(api.ibha_csvd_cursor_row(cursor), scratch);
      if (changesOnly && rowIsQuiet(raw)) continue;
      yield raw;
    }
  }

  /**
   * Copies the engine's row into a plain record.
   *
   * Everything in the engine's row is valid only until the next
   * `cursor_next` on the same cursor, and `cell_flags` points at one buffer the
   * cursor reuses. So a consumer that keeps anything has to copy it, and the
   * retained index in `handle.ts` is exactly such a consumer. `into` lets the
   * streaming path reuse one flags buffer rather than allocating per row.
   */
  readRawRow(rowPtr: number, into?: Uint8Array): RawRow {
    const h = this.heap;
    const nColumns = h.u32At(rowPtr + OFF.row.n_columns);
    const flagsPtr = h.ptrAt(rowPtr + OFF.row.cell_flags);
    const flags = into && into.length >= nColumns ? into.subarray(0, nColumns) : new Uint8Array(nColumns);
    flags.set(h.u8.subarray(flagsPtr, flagsPtr + nColumns));

    const sourceRow = h.u32At(rowPtr + OFF.row.source_row);
    const targetRow = h.u32At(rowPtr + OFF.row.target_row);
    return {
      kind: h.u8At(rowPtr + OFF.row.kind) & 3,
      moved: h.u8At(rowPtr + OFF.row.moved) !== 0,
      moveDistance: h.i32At(rowPtr + OFF.row.move_distance),
      // Record numbers, 1 based, counting parsed records rather than physical
      // lines. Every row number in every emitter uses the same convention.
      sourceRow: sourceRow === C.NO_ROW ? null : sourceRow + 1,
      targetRow: targetRow === C.NO_ROW ? null : targetRow + 1,
      changedCells: h.u32At(rowPtr + OFF.row.n_changed_cells),
      suppressedCells: h.u32At(rowPtr + OFF.row.n_suppressed_cells),
      findingCells: h.u32At(rowPtr + OFF.row.n_findings),
      flags,
    };
  }

  private readonly rawScratch = new Uint8Array(4096);
  private readonly cellBuf: DecodedCell = { value: '', truncated: false, invalidUtf8: false };

  /** Turns a raw row into the public shape, decoding values on request. The raw
   *  row may have come from the cursor or from a retained index; this does not
   *  know which, which is what lets both paths produce identical rows. */
  buildRow(raw: RawRow, includeValues: boolean, maxCellBytes: number): DiffRow {
    const row: DiffRow = {
      kind: CHANGE_KINDS[raw.kind]!,
      moved: raw.moved,
      moveDistance: raw.moveDistance,
      sourceRow: raw.sourceRow,
      targetRow: raw.targetRow,
      key: null,
      changedCells: raw.changedCells,
      suppressedCells: raw.suppressedCells,
      cells: [],
      findings: [],
    };

    if (includeValues) {
      row.cells = this.decodeCells(raw, maxCellBytes);
      if (this.keyColumns.length) {
        row.key = this.keyColumns.map((c) => {
          const cell = row.cells[c];
          return cell?.target ?? cell?.source ?? '';
        });
      }
    }
    if (raw.findingCells > 0) row.findings = this.decodeFindings(raw.flags);
    return row;
  }

  private decodeCells(raw: RawRow, maxCellBytes: number): DiffCell[] {
    const cells: DiffCell[] = [];
    const buf = this.cellBuf;
    const sourceRow = raw.sourceRow === null ? C.NO_ROW : raw.sourceRow - 1;
    const targetRow = raw.targetRow === null ? C.NO_ROW : raw.targetRow - 1;

    for (let c = 0; c < raw.flags.length; c++) {
      const fs = this.srcTable.fieldIndex(sourceRow, c);
      const ft = this.tgtTable.fieldIndex(targetRow, c);
      const fl = raw.flags[c]!;

      const cell: DiffCell = {
        column: c,
        name: this.columnNames[c] ?? '',
        changed: (fl & C.CELL_CHANGED) !== 0,
        suppressed: (fl & C.CELL_SUPPRESSED) !== 0,
      };

      // The contract, and the one rule a consumer gets wrong: a matched row
      // carries `source` exactly when the cell differs in bytes from the target.
      // Its absence means the two sides are byte identical, not that the value
      // was empty.
      const wantSource =
        fs >= 0 && (ft < 0 || (fl & (C.CELL_CHANGED | C.CELL_SUPPRESSED)) !== 0);
      if (wantSource) {
        this.srcTable.decode(fs, buf, maxCellBytes);
        cell.source = buf.value;
        if (buf.truncated) cell.truncated = true;
        if (buf.invalidUtf8) cell.invalidUtf8 = true;
      }
      if (ft >= 0) {
        this.tgtTable.decode(ft, buf, maxCellBytes);
        cell.target = buf.value;
        if (buf.truncated) cell.truncated = true;
        if (buf.invalidUtf8) cell.invalidUtf8 = true;
      }
      cells.push(cell);
    }
    return cells;
  }

  private decodeFindings(flags: Uint8Array): RowFinding[] {
    const kinds: Array<[number, FindingKind]> = [
      [C.CELL_REQUIRED_EMPTY, 'requiredEmpty'],
      [C.CELL_TOO_LONG, 'tooLong'],
      [C.CELL_NOT_NUMERIC, 'notNumeric'],
      [C.CELL_PRECISION, 'precision'],
    ];
    const out: RowFinding[] = [];
    for (let c = 0; c < flags.length; c++) {
      const fl = flags[c]!;
      if ((fl & C.CELL_FINDING) === 0) continue;
      for (const [bit, kind] of kinds) {
        if ((fl & bit) === 0) continue;
        const f: RowFinding = { column: c, name: this.columnNames[c] ?? '', kind };
        if (kind === 'tooLong' && this.colSizePtr) {
          f.limit = this.heap.i32At(this.colSizePtr + c * 4);
        }
        if (kind === 'precision' && this.colSizePtr) {
          f.precision = this.heap.i32At(this.colSizePtr + c * 4);
          f.scale = this.colScalePtr ? this.heap.i32At(this.colScalePtr + c * 4) : -1;
        }
        out.push(f);
      }
    }
    return out;
  }

  /* ---------------------------------------------------------- the summary -- */

  private summaryCache: DiffSummary | null = null;

  /**
   * The counts, taken from the summary emitter rather than read out of the stats
   * struct.
   *
   * Two reasons, and the second is the one that matters. The cell level counters
   * accumulate as a cursor advances, so reading the struct gives a number that
   * depends on how many times the caller happened to have drained the diff; the
   * summary emitter zeroes them and drains its own cursor, so its numbers are
   * always those of exactly one pass. And parsing its JSON means the summary this
   * binding reports and the summary the emitter writes to a file cannot drift.
   */
  summary(): DiffSummary {
    if (this.summaryCache) return this.summaryCache;
    const bytes = this.emit('summary');
    this.summaryCache = JSON.parse(utf8.decode(bytes)) as DiffSummary;
    return this.summaryCache;
  }

  /* --------------------------------------------------------- cell segments -- */

  private segRowPtr = 0;
  private segOut = 0;
  private segCap = 0;

  /**
   * Intra cell segments for one column of one report row, per spec 7.
   *
   * `row` is identified by its source and target row indices rather than by a
   * live cursor position, because the engine only reads those two fields off the
   * row it is handed. That is what lets a random access consumer ask for segments
   * on a row it indexed earlier without walking the cursor back to it.
   *
   * The scratch is owned by the diff and reused, so two of these must not run
   * concurrently on one diff. The engine is thread agnostic per spec 2.6.3:
   * parallelism is across diffs, not inside one.
   */
  cellSegments(
    sourceRow: number | null,
    targetRow: number | null,
    col: number,
    mode: CellDiffMode,
    maxBytes = 0,
  ): TextSegment[] {
    this.assertLive();
    if (mode === 'none' || sourceRow === null || targetRow === null) return [];

    const { api, heap } = this;
    if (!this.segRowPtr) this.segRowPtr = heap.alloc(OFF.row.__size, OFF.row.__align);
    heap.setU32(this.segRowPtr + OFF.row.source_row, sourceRow - 1);
    heap.setU32(this.segRowPtr + OFF.row.target_row, targetRow - 1);

    const modeIndex = CELL_DIFF_MODES.indexOf(mode);
    if (modeIndex < 0) throw new IbhaCsvError('INVALID_ARG', `unknown cell diff mode ${mode}`);

    // The count does not depend on the cap, so one sizing call then one filling
    // call is exact. Most cells fit the buffer already in hand and only cost one.
    let n = api.ibha_csvd_cell_segments(this.diff, this.segRowPtr, col, modeIndex, maxBytes, this.segOut, this.segCap);
    if (n < 0) this.throwEngineError(n);
    if (n === 0) return [];
    if (n > this.segCap) {
      this.segCap = Math.max(n, 64);
      this.segOut = heap.alloc(this.segCap * OFF.segment.__size, OFF.segment.__align);
      n = api.ibha_csvd_cell_segments(this.diff, this.segRowPtr, col, modeIndex, maxBytes, this.segOut, this.segCap);
      if (n < 0) this.throwEngineError(n);
    }

    // Three uint32 and no padding, which is why this is a plain Uint32Array read
    // rather than a struct walk.
    const raw = new Uint32Array(heap.memory.buffer as ArrayBuffer, this.segOut, n * 3);
    const out: TextSegment[] = new Array(n);
    for (let i = 0; i < n; i++) {
      out[i] = { op: SEGMENT_OPS[raw[i * 3]!]!, start: raw[i * 3 + 1]!, len: raw[i * 3 + 2]! };
    }
    return out;
  }

  /* ---------------------------------------------------------------- emit -- */

  /**
   * Runs an emitter and returns its bytes.
   *
   * Sized in two passes: a first with no buffer at all, which allocates nothing
   * and tells us exactly how many bytes the report is, then one grow and a second
   * pass that fills it. The buffer sink counts past the end rather than
   * truncating, which is what makes the first pass a measurement.
   *
   * The cost is that a large report is produced twice. That is a real limitation
   * and it is stated rather than hidden: `ibha_csvd_emit` drains the whole diff in
   * one call, so there is no way for a host to pull a bounded piece of it. A
   * resumable emitter is the fix and it belongs in the engine, not here.
   */
  emit(format: EmitFormat, options: EmitOptions = {}): Uint8Array {
    this.assertLive();
    const { api, heap } = this;

    const formatIndex = EMIT_FORMATS.indexOf(format);
    if (formatIndex < 0) throw new IbhaCsvError('INVALID_ARG', `unknown emit format ${format}`);

    const eopts = heap.alloc(OFF.emit_opts.__size, OFF.emit_opts.__align);
    api.ibha_csvd_emit_opts_init(eopts, formatIndex);
    if (options.changesOnly !== undefined) {
      heap.setI32(eopts + OFF.emit_opts.changes_only, options.changesOnly ? 1 : 0);
    }
    if (options.includeValues !== undefined) {
      heap.setI32(eopts + OFF.emit_opts.include_values, options.includeValues ? 1 : 0);
    }
    if (options.cellDiff !== undefined) {
      const i = CELL_DIFF_MODES.indexOf(options.cellDiff);
      if (i < 0) throw new IbhaCsvError('INVALID_ARG', `unknown cell diff mode ${options.cellDiff}`);
      heap.setU8(eopts + OFF.emit_opts.cell_diff, i);
    }
    if (options.maxCellBytes !== undefined) {
      heap.setU32(eopts + OFF.emit_opts.max_cell_bytes, options.maxCellBytes);
    }
    if (options.maxRows !== undefined) heap.setU32(eopts + OFF.emit_opts.max_rows, options.maxRows);
    if (options.csvFormulaGuard !== undefined) {
      heap.setI32(eopts + OFF.emit_opts.csv_formula_guard, options.csvFormulaGuard ? 1 : 0);
    }
    const delim = oneByte(options.csvDelimiter, 'csvDelimiter');
    if (delim !== undefined) heap.setU8(eopts + OFF.emit_opts.csv_delimiter, delim);
    if (options.classPrefix !== undefined) {
      // Checked here rather than left to the engine, which would be the simpler
      // code and the worse behaviour. The context holds one error and the first
      // one wins, so an engine level refusal of a mistyped class prefix would
      // abort a comparison that was otherwise complete and correct, and every
      // later call on the handle with it. A caller's typo should cost the caller
      // the call, not the diff. The pattern is the engine's, restated.
      if (!/^[A-Za-z][A-Za-z0-9_-]{0,31}$/.test(options.classPrefix)) {
        throw new IbhaCsvError(
          'INVALID_ARG',
          `the HTML class prefix must match [A-Za-z][A-Za-z0-9_-]{0,31}, received ${JSON.stringify(options.classPrefix)}`,
        );
      }
      heap.setPtr(eopts + OFF.emit_opts.class_prefix, heap.allocCString(options.classPrefix));
    }

    const bufferSink = heap.alloc(OFF.buffer_sink.__size, OFF.buffer_sink.__align);
    const sink = heap.alloc(OFF.sink.__size, OFF.sink.__align);

    // A function pointer is an index into the module's indirect call table, which
    // is not exported and which JavaScript cannot portably add to. So the engine
    // hands the pair over rather than the binding assembling it.
    api.ibha_csvd_buffer_sink_bind(sink, bufferSink);

    api.ibha_csvd_buffer_sink_init(bufferSink, 0, 0);
    let st = api.ibha_csvd_emit(this.diff, eopts, sink, 0);
    if (st !== STATUS_OK) this.throwEngineError(st);
    const size = heap.u32At(bufferSink + OFF.buffer_sink.len);
    if (size === 0) return new Uint8Array(0);

    const out = heap.alloc(size, 8);
    api.ibha_csvd_buffer_sink_init(bufferSink, out, size);
    st = api.ibha_csvd_emit(this.diff, eopts, sink, 0);
    if (st !== STATUS_OK) this.throwEngineError(st);
    if (heap.u8At(bufferSink + OFF.buffer_sink.overflow) !== 0) {
      throw new IbhaCsvError('ENGINE', 'the emitter produced more bytes on the second pass than on the first');
    }
    return heap.bytesAt(out, size);
  }

  /* -------------------------------------------------------------- errors -- */

  private throwEngineError(status?: number): never {
    const st = status ?? this.api.ibha_csvd_ctx_status(this.ctx);
    const name = this.heap.cstringAt(this.api.ibha_csvd_status_name(st));
    const detail = this.heap.cstringAt(this.api.ibha_csvd_ctx_error(this.ctx));
    throw new IbhaCsvError(statusToCode(st), detail || name, name);
  }

  private assertLive(): void {
    if (this.disposed) throw new IbhaCsvError('INVALID_ARG', 'this diff has been disposed');
  }

  /** Frees the context and every allocation made from its arena. The instance and
   *  its linear memory go with it, which is what makes a batch of comparisons
   *  flat rather than monotonically growing. */
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    if (this.ctx) this.api.ibha_csvd_ctx_free(this.ctx);
    this.ctx = 0;
    this.diff = 0;
  }
}
