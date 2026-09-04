/**
 * Getting the engine module compiled and instantiated.
 *
 * Two modules are built, per spec 13.11: SIMD is the expected runtime path on
 * desktop browsers, which are the only browser target, and the scalar build is
 * retained as the differential oracle for testing the SIMD parser rather than as
 * a build we expect to ship. So the loader feature detects and prefers SIMD, and
 * lets a caller pin either one, which is what the determinism check needs.
 *
 * A compiled `WebAssembly.Module` is shared and a `WebAssembly.Instance` is per
 * diff. That split matters: linear memory only ever grows, so a long lived
 * instance accumulates the high water mark of every diff it has run. One instance
 * per diff, discarded with it, is what keeps a batch of comparisons flat.
 */

import { C, POINTER_SIZE } from './abi.ts';
import { WasmHeap } from './memory.ts';
import { IbhaCsvError } from './source.ts';

/**
 * Validates a module whose body needs `i8x16.splat`. It is a type error without
 * the SIMD proposal, so validation is a decisive answer rather than a guess, and
 * it costs microseconds.
 */
export function simdSupported(): boolean {
  return WebAssembly.validate(
    new Uint8Array([
      0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 9, 1, 7, 0, 65, 0, 253, 15,
      26, 11,
    ]),
  );
}

export interface LoadOptions {
  /** The module bytes, when the host already has them. Skips all resolution. */
  wasmBinary?: BufferSource;
  /** Where to fetch the module from. A bundler friendly default is used
   *  otherwise: `new URL('../wasm/<name>', import.meta.url)`. */
  wasmUrl?: string | URL;
  /** Pin the build instead of feature detecting. The determinism check pins both
   *  in turn, which is the only reason this is public. */
  simd?: boolean;
}

/** Every export the binding calls, so a truncated or mis-linked module fails on
 *  load with a list rather than on first use with `undefined is not a function`. */
const REQUIRED_EXPORTS = [
  'memory',
  'ibha_csvd_version',
  'ibha_csvd_status_name',
  'ibha_csvd_ctx_new',
  'ibha_csvd_ctx_free',
  'ibha_csvd_ctx_status',
  'ibha_csvd_ctx_error',
  'ibha_csvd_ctx_bytes_reserved',
  'ibha_csvd_limits_init',
  'ibha_csvd_dialect_init',
  'ibha_csvd_compare_opts_init',
  'ibha_csvd_parse_opts_init',
  'ibha_csvd_parse_begin',
  'ibha_csvd_parse_chunk',
  'ibha_csvd_parse_finish',
  'ibha_csvd_table_of',
  'ibha_csvd_schema_of',
  'ibha_csvd_parse_stats_of',
  'ibha_csvd_row_field',
  'ibha_csvd_row_field_count',
  'ibha_csvd_field_logical_len',
  'ibha_csvd_field_copy',
  'ibha_csvd_diff_opts_init',
  'ibha_csvd_diff_run',
  'ibha_csvd_diff_stats_of',
  'ibha_csvd_diff_table',
  'ibha_csvd_diff_schema',
  'ibha_csvd_diff_columns',
  'ibha_csvd_cursor_open',
  'ibha_csvd_cursor_next',
  'ibha_csvd_cursor_row',
  'ibha_csvd_cursor_reset',
  'ibha_csvd_cell_segments',
  'ibha_csvd_emit_opts_init',
  'ibha_csvd_emit',
  'ibha_csvd_buffer_sink_init',
  'ibha_csvd_buffer_sink_bind',
] as const;

/** The engine's exports, typed. Every parameter and every return is an i32 or an
 *  i64, because the ABI hands out opaque pointers and the binding reads the
 *  structs behind them out of linear memory itself. */
export interface EngineExports {
  memory: WebAssembly.Memory;
  ibha_csvd_version(major: number, minor: number, patch: number): void;
  ibha_csvd_status_name(status: number): number;
  ibha_csvd_ctx_new(limits: number): number;
  ibha_csvd_ctx_free(ctx: number): void;
  ibha_csvd_ctx_status(ctx: number): number;
  ibha_csvd_ctx_error(ctx: number): number;
  ibha_csvd_ctx_bytes_reserved(ctx: number): bigint;
  ibha_csvd_limits_init(out: number): void;
  ibha_csvd_dialect_init(out: number): void;
  ibha_csvd_compare_opts_init(out: number): void;
  ibha_csvd_parse_opts_init(out: number): void;
  ibha_csvd_parse_begin(ctx: number, opts: number): number;
  ibha_csvd_parse_chunk(parser: number, bytes: number, len: number): number;
  ibha_csvd_parse_finish(parser: number): number;
  ibha_csvd_table_of(parser: number): number;
  ibha_csvd_schema_of(parser: number): number;
  ibha_csvd_parse_stats_of(parser: number): number;
  ibha_csvd_row_field(table: number, row: number, col: number): number;
  ibha_csvd_row_field_count(table: number, row: number): number;
  ibha_csvd_field_logical_len(table: number, field: number): number;
  ibha_csvd_field_copy(table: number, field: number, dst: number, cap: number): number;
  ibha_csvd_diff_opts_init(out: number): void;
  ibha_csvd_diff_run(
    ctx: number,
    src: number,
    srcSchema: number,
    tgt: number,
    tgtSchema: number,
    opts: number,
  ): number;
  ibha_csvd_diff_stats_of(diff: number): number;
  /** side is ibha_csvd_side: 0 source, 1 target. */
  ibha_csvd_diff_table(diff: number, side: number): number;
  ibha_csvd_diff_schema(diff: number, side: number): number;
  ibha_csvd_diff_columns(diff: number): number;
  ibha_csvd_cursor_open(diff: number): number;
  ibha_csvd_cursor_next(cursor: number): number;
  ibha_csvd_cursor_row(cursor: number): number;
  ibha_csvd_cursor_reset(cursor: number): void;
  ibha_csvd_cell_segments(
    diff: number,
    row: number,
    col: number,
    mode: number,
    maxBytes: number,
    out: number,
    cap: number,
  ): number;
  ibha_csvd_emit_opts_init(out: number, format: number): void;
  ibha_csvd_emit(diff: number, opts: number, sink: number, rowsWritten: number): number;
  ibha_csvd_buffer_sink_init(sink: number, bytes: number, cap: number): void;
  ibha_csvd_buffer_sink_bind(sink: number, bufferSink: number): void;
}

/** One instantiation: the exports, and a heap over its linear memory. */
export interface EngineInstance {
  api: EngineExports;
  heap: WasmHeap;
}

async function readModuleBytes(url: URL): Promise<ArrayBuffer> {
  if (url.protocol === 'file:') {
    // A file: URL is the Node and the test path. fetch cannot read one, and the
    // import is dynamic so a bundler targeting the browser never pulls node:fs in.
    const { readFile } = await import('node:fs/promises');
    const buf = await readFile(url);
    // Sliced rather than handed over: Node returns a Buffer that for a small
    // read is a view into a shared pool, and `buf.buffer` would be the pool.
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer;
  }
  const res = await fetch(url);
  if (!res.ok) {
    throw new IbhaCsvError('IO_ERROR', `could not fetch the wasm module: HTTP ${res.status} ${url}`);
  }
  return res.arrayBuffer();
}

/**
 * A compiled engine module. Compile once, instantiate per diff.
 */
export class Engine {
  readonly module: WebAssembly.Module;
  readonly simd: boolean;

  private constructor(module: WebAssembly.Module, simd: boolean) {
    this.module = module;
    this.simd = simd;
  }

  static async load(options: LoadOptions = {}): Promise<Engine> {
    const simd = options.simd ?? simdSupported();
    const name = simd ? 'ibha_csvdiff.simd.wasm' : 'ibha_csvdiff.wasm';

    let bytes: BufferSource;
    if (options.wasmBinary) {
      bytes = options.wasmBinary;
    } else {
      const url = options.wasmUrl
        ? new URL(options.wasmUrl, typeof location === 'undefined' ? undefined : location.href)
        : new URL(`../wasm/${name}`, import.meta.url);
      bytes = await readModuleBytes(url);
    }

    const module = await WebAssembly.compile(bytes);

    const missing = REQUIRED_EXPORTS.filter(
      (n) => !WebAssembly.Module.exports(module).some((e) => e.name === n),
    );
    if (missing.length) {
      throw new IbhaCsvError(
        'INVALID_ARG',
        `the wasm module is missing ${missing.length} export(s) the binding needs: ${missing.join(', ')}. ` +
          'A public function that is not marked IBHA_CSVD_API is not a link error, it is a function ' +
          'missing from the module; run scripts-and-commands/mark_public_api.py.',
      );
    }
    // The module is built freestanding and imports nothing. If that ever stops
    // being true, instantiation below would fail with a LinkError naming an
    // import the host has never heard of, so say it plainly here instead.
    const imports = WebAssembly.Module.imports(module);
    if (imports.length) {
      throw new IbhaCsvError(
        'INVALID_ARG',
        `the wasm module imports ${imports.map((i) => `${i.module}.${i.name}`).join(', ')}; ` +
          'the engine is built freestanding and must import nothing.',
      );
    }

    return new Engine(module, simd);
  }

  /** A fresh instance with its own linear memory. */
  instantiate(): EngineInstance {
    const instance = new WebAssembly.Instance(this.module, {});
    const api = instance.exports as unknown as EngineExports;
    const heap = new WasmHeap(api.memory);

    if (POINTER_SIZE !== 4) {
      throw new IbhaCsvError(
        'INVALID_ARG',
        `src/abi.ts was generated for ${POINTER_SIZE} byte pointers; this binding reads wasm32.`,
      );
    }

    // The offsets in abi.ts were taken from one build of the header. A module
    // built from a different one loads and runs and returns wrong numbers, so
    // the versions are compared rather than assumed. It is three integers and it
    // turns a silent misread into a message.
    const out = heap.alloc(12, 4);
    api.ibha_csvd_version(out, out + 4, out + 8);
    const major = heap.i32At(out);
    const minor = heap.i32At(out + 4);
    const patch = heap.i32At(out + 8);
    if (major !== C.VERSION_MAJOR || minor !== C.VERSION_MINOR || patch !== C.VERSION_PATCH) {
      throw new IbhaCsvError(
        'INVALID_ARG',
        `wasm module is ${major}.${minor}.${patch} but src/abi.ts was generated for ` +
          `${C.VERSION_MAJOR}.${C.VERSION_MINOR}.${C.VERSION_PATCH}. ` +
          'Rebuild and regenerate: make -C core wasm && node scripts-and-commands/gen_abi.mjs',
      );
    }

    return { api, heap };
  }
}
