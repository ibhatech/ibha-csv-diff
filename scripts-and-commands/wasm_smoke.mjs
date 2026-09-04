/**
 * Runs the engine inside a WebAssembly instance and checks it against the native
 * build's answer for the same inputs.
 *
 *     node scripts-and-commands/wasm_smoke.mjs [core/build/ibha_csvdiff.wasm]
 *
 * This is not the Phase 4 binding. It is the smallest thing that proves the wasm
 * artifact is real: a module that links and exports nothing looks exactly like a
 * module that works until something tries to drive it, which is how a 292 byte
 * module got built and not noticed.
 *
 * It deliberately touches no struct layouts. Every call here takes and returns
 * scalars or opaque pointers, so nothing in it has to be revised when the Phase 4
 * binding works out the wasm32 field offsets.
 *
 * The host memory rule this relies on, from src/sys_wasm.c: the engine owns
 * exactly the pages it grew itself. So staging bytes are put in pages this script
 * grows, and the two can never overlap.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const modulePath = process.argv[2] ?? path.join(root, 'core/build/ibha_csvdiff.wasm');

const PAGE = 65536;
let failures = 0;

function check(condition, name, detail) {
  if (condition) {
    console.log(`ok   ${name}`);
  } else {
    failures++;
    console.log(`FAIL ${name}${detail === undefined ? '' : `  (${detail})`}`);
  }
}

const bytes = readFileSync(modulePath);
const module = new WebAssembly.Module(bytes);

check(WebAssembly.Module.imports(module).length === 0,
      'the module imports nothing, so it needs no runtime from the host',
      JSON.stringify(WebAssembly.Module.imports(module)));

const instance = new WebAssembly.Instance(module, {});
const api = instance.exports;
const memory = api.memory;

/** Grows linear memory and returns the byte offset of the fresh region. The
 *  engine never touches pages it did not grow itself, so this is safe. */
function hostAlloc(nbytes) {
  const pages = Math.ceil(nbytes / PAGE);
  const prev = memory.grow(pages);
  if (prev === -1) throw new Error('memory.grow failed');
  return prev * PAGE;
}

function writeBytes(buf) {
  const at = hostAlloc(buf.length);
  new Uint8Array(memory.buffer, at, buf.length).set(buf);
  return at;
}

function readCString(ptr) {
  const view = new Uint8Array(memory.buffer, ptr);
  let end = 0;
  while (view[end] !== 0) end++;
  return new TextDecoder().decode(view.subarray(0, end));
}

/* --------------------------------------------------------- it runs at all -- */

const scratch = hostAlloc(64);
api.ibha_csvd_version(scratch, scratch + 4, scratch + 8);
const version = new Int32Array(memory.buffer, scratch, 3);
check(version[0] === 0 && version[1] === 1 && version[2] === 0,
      'ibha_csvd_version writes through host supplied pointers', Array.from(version).join('.'));

check(readCString(api.ibha_csvd_status_name(-7)) === 'COLUMN_ORDER',
      'a status name comes back out of the static data segment');

const ctx = api.ibha_csvd_ctx_new(0);
check(ctx !== 0, 'a context allocates, so memory.grow works from inside the module');
check(api.ibha_csvd_ctx_status(ctx) === 0, 'and it starts with no error');
check(api.ibha_csvd_ctx_bytes_reserved(ctx) > 0n, 'and its arena reserved real memory');

/* ------------------------------------------------- it parses and it diffs -- */

const header =
  'KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n';
const sourceText = header + 'A,alice,1\nB,bob,2\nC,carol,3\n';
const targetText = header + 'A,alicia,1\nC,carol,3\nD,dave,4\n';

function parseSide(text) {
  const buf = new TextEncoder().encode(text);
  const at = writeBytes(buf);
  const parser = api.ibha_csvd_parse_begin(ctx, 0); /* NULL opts: the four row default */
  if (parser === 0) throw new Error('parse_begin returned null');
  let st = api.ibha_csvd_parse_chunk(parser, at, buf.length);
  if (st !== 0) throw new Error(`parse_chunk: ${readCString(api.ibha_csvd_ctx_error(ctx))}`);
  st = api.ibha_csvd_parse_finish(parser);
  if (st !== 0) throw new Error(`parse_finish: ${readCString(api.ibha_csvd_ctx_error(ctx))}`);
  return parser;
}

const sp = parseSide(sourceText);
const tp = parseSide(targetText);
check(true, 'both sides parse inside the module');

/* A cell read back through the public accessors, which proves the index points
 * at the right bytes rather than merely existing. */
const srcTable = api.ibha_csvd_table_of(sp);
const nameField = api.ibha_csvd_row_field(srcTable, 4, 1); /* row 5, column 2 */
const cell = hostAlloc(64);
const cellLen = api.ibha_csvd_field_copy(srcTable, nameField, cell, 64);
const cellText = new TextDecoder().decode(new Uint8Array(memory.buffer, cell, cellLen));
check(cellText === 'alice', 'a cell reads back as the bytes that went in', cellText);

const diff = api.ibha_csvd_diff_run(ctx, srcTable, api.ibha_csvd_schema_of(sp),
                                    api.ibha_csvd_table_of(tp), api.ibha_csvd_schema_of(tp), 0);
check(diff !== 0, 'the diff runs', readCString(api.ibha_csvd_ctx_error(ctx)));

const cursor = api.ibha_csvd_cursor_open(diff);
let reportRows = 0;
while (api.ibha_csvd_cursor_next(cursor) === 1) reportRows++;

/* Source A B C against target A C D: A modified, C unchanged, D added, B
 * deleted. Four report rows, and the same four the native build reports. */
check(reportRows === 4, 'the cursor yields the report rows the native build yields', reportRows);
check(api.ibha_csvd_ctx_status(ctx) === 0, 'and nothing errored along the way');

api.ibha_csvd_ctx_free(ctx);
check(true, 'the context frees without trapping');

console.log(`\n${failures ? `FAILED ${failures}` : 'all checks passed'}  ${path.relative(root, modulePath)}`);
process.exit(failures ? 1 : 0);
