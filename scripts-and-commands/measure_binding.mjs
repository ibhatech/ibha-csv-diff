/**
 * What the binding costs, on the p90 pair.
 *
 *     node scripts-and-commands/measure_binding.mjs
 *
 * Regenerates the table in js/packages/core/README.md. Re-run it rather than
 * editing the table, the same rule measure_emitters.py follows on the C side.
 *
 * The numbers worth watching are the two that separate the streaming path from
 * the random access one, because the whole API is built on that separation being
 * real: `rows(), values off` is the cost of the edit script with nothing
 * retained, and `index()` is what a view pays to be able to seek.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { compare, compareInWorker, loadEngine } from '../js/packages/core/src/index.ts';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const F = path.join(root, 'core/fixtures/generated');

const source = new Uint8Array(readFileSync(path.join(F, 'p90_source.csv')));
const target = new Uint8Array(readFileSync(path.join(F, 'p90_target.csv')));
const megabytes = (source.length + target.length) / (1024 * 1024);

const rows = [];
function record(what, seconds, note) {
  rows.push({ what, seconds, note });
}

async function time(fn) {
  const t0 = performance.now();
  const value = await fn();
  return [(performance.now() - t0) / 1000, value];
}

/* Warm the module compile and the JIT out of the first measurement. */
await loadEngine();
{
  const warm = await compare(source, target);
  warm.dispose();
}

const [tCompare, handle] = await time(() => compare(source, target));
record('compare, parse and match', tCompare, `${handle.bytesReserved / (1024 * 1024) | 0} MB reserved`);

const [tSummary] = await time(() => handle.summary());
record('summary()', tSummary, 'one drain, constant memory');

const [tDrainBare] = await time(() => {
  let n = 0;
  for (const _ of handle.rows({ includeValues: false })) n++;
  return n;
});
record('rows(), values off', tDrainBare, `${handle.summary().rows.report.toLocaleString()} rows`);

const [tDrainValues, decoded] = await time(() => {
  let cells = 0;
  for (const r of handle.rows()) cells += r.cells.length;
  return cells;
});
record('rows(), values decoded', tDrainValues, `${decoded.toLocaleString()} cells`);

const [tIndex, index] = await time(() => handle.index({ changesOnly: true }));
record('index({changesOnly})', tIndex, `${(index.bytesRetained / 1024) | 0} KB retained, ${index.rowCount.toLocaleString()} rows`);

const [tFullIndex, full] = await time(() => handle.index());
record('index(), whole report', tFullIndex, `${(full.bytesRetained / (1024 * 1024)).toFixed(1)} MB retained`);

const [tPage] = await time(() => {
  for (let i = 0; i < 100; i++) full.getRows(i * 50, 50);
});
record('100 pages of 50 rows', tPage, 'decoded from the index');

const [tEmit, jsonl] = await time(() => handle.emit('jsonl', { changesOnly: true }));
record('emit jsonl, changes only', tEmit, `${(jsonl.length / 1024) | 0} KB, two passes`);

const [tEmitFull, jsonlFull] = await time(() => handle.emit('jsonl'));
record('emit jsonl, whole report', tEmitFull, `${(jsonlFull.length / (1024 * 1024)) | 0} MB, two passes`);

handle.dispose();

const [tWorker, remote] = await time(() => compareInWorker(source, target));
record('compareInWorker, end to end', tWorker, 'includes the chunk pump');
await remote.dispose();

/* ------------------------------------------------------------------ print -- */

const w1 = Math.max(...rows.map((r) => r.what.length));
console.log(`p90 pair, ${megabytes.toFixed(1)} MB in both sides, node ${process.versions.node}\n`);
console.log(`${'what'.padEnd(w1)}   seconds   note`);
for (const r of rows) {
  console.log(`${r.what.padEnd(w1)}   ${r.seconds.toFixed(3).padStart(7)}   ${r.note}`);
}
