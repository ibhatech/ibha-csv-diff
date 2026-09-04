/**
 * Generates js/packages/core/src/abi.ts from the compiler's own answer about the
 * wasm32 layout of the public structs.
 *
 *     node scripts-and-commands/gen_abi.mjs           # write
 *     node scripts-and-commands/gen_abi.mjs --check   # fail if stale
 *
 * Why generated rather than written by hand: the binding reads
 * ibha_csvd_table and ibha_csvd_row straight out of linear memory, because the
 * header says an accessor call per cell would cost more than the diff. A field
 * that moves is then not a compile error, it is a binding that reads n_columns
 * out of the middle of a pointer and reports a plausible wrong number. The
 * --check mode is in the gate so that cannot happen quietly.
 *
 * core/tools/abi_offsets.c is the source of the values. It compiles to its own
 * tiny wasm32 module, which is instantiated here.
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const modulePath = path.join(root, 'core/build/abi_offsets.wasm');
const outPath = path.join(root, 'js/packages/core/src/abi.ts');

const check = process.argv.includes('--check');

/* ------------------------------------------------------- read the module -- */

let bytes;
try {
  bytes = readFileSync(modulePath);
} catch (err) {
  if (err.code !== 'ENOENT') throw err;
  console.error(
    [
      '',
      `error: ${path.relative(root, modulePath)} does not exist.`,
      '',
      'Build it first:',
      '  make -C core wasm',
      '',
    ].join('\n'),
  );
  process.exit(1);
}

const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
const api = instance.exports;
const mem = new DataView(api.memory.buffer);

const count = api.abi_count();
const namesPtr = api.abi_names();
const namesLen = api.abi_names_len();
const valuesPtr = api.abi_values();
const pointerSize = api.abi_pointer_size();

if (pointerSize !== 4) {
  console.error(`error: abi_offsets.wasm reports ${pointerSize} byte pointers, expected 4.`);
  process.exit(1);
}

const names = new TextDecoder()
  .decode(new Uint8Array(api.memory.buffer, namesPtr, namesLen))
  .split('\0')
  .filter((s) => s.length > 0);

if (names.length !== count) {
  console.error(`error: ${names.length} names against ${count} values; the X macro list is broken.`);
  process.exit(1);
}

/* Two levels: "table.n_columns" becomes groups.table.n_columns. The constants
 * arrive under the group name "const" and are emitted separately, because they
 * are values rather than offsets and the binding reads them for different
 * reasons. */
const groups = new Map();
for (let i = 0; i < count; i++) {
  const dot = names[i].indexOf('.');
  const group = names[i].slice(0, dot);
  const field = names[i].slice(dot + 1);
  if (!groups.has(group)) groups.set(group, new Map());
  groups.get(group).set(field, mem.getUint32(valuesPtr + i * 4, true));
}

const constants = groups.get('const');
groups.delete('const');

/* ------------------------------------------------------------- emit the TS -- */

function block(map, indent) {
  const pad = ' '.repeat(indent);
  return [...map].map(([k, v]) => `${pad}${k}: ${v},`).join('\n');
}

const structs = [...groups]
  .map(([name, fields]) => `  ${name}: {\n${block(fields, 4)}\n  },`)
  .join('\n');

const out = `/**
 * GENERATED FILE. Do not edit.
 *
 *   node scripts-and-commands/gen_abi.mjs
 *
 * The wasm32 byte layout of every public struct in core/include/ibha_csvdiff.h,
 * asked of the compiler that builds the module rather than written out by hand.
 * The list of fields lives in core/tools/abi_offsets.c; regenerating is checked
 * in the gate, so a struct cannot change without this file changing with it.
 *
 * Every offset here is a byte offset into wasm linear memory, and every pointer
 * field is 4 bytes wide because the target is wasm32. The binding asserts that
 * on load rather than assuming it.
 */

/** Width of a pointer on the target these offsets were taken for. */
export const POINTER_SIZE = ${pointerSize};

/** Compile time constants from the header, so the binding never restates one. */
export const C = {
${block(constants, 2)}
} as const;

/** Byte offsets, sizes and alignments, keyed by struct then by field. */
export const OFF = {
${structs}
} as const;

/** Number of entries the generator emitted, asserted by the ABI self check so
 *  that a truncated regeneration is loud rather than subtle. */
export const ABI_FIELD_COUNT = ${count};
`;

if (check) {
  let current = null;
  try {
    current = readFileSync(outPath, 'utf8');
  } catch (err) {
    if (err.code !== 'ENOENT') throw err;
  }
  if (current === out) {
    console.log(`${path.relative(root, outPath)}: up to date, ${count} entries`);
    process.exit(0);
  }
  console.error(
    `error: ${path.relative(root, outPath)} is stale. Run: node scripts-and-commands/gen_abi.mjs`,
  );
  process.exit(1);
}

writeFileSync(outPath, out);
console.log(`${path.relative(root, outPath)}: wrote ${count} entries`);
