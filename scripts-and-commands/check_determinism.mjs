/**
 * Spec 3.2: the native build and the wasm builds must produce byte identical
 * output for the same inputs.
 *
 *     node scripts-and-commands/check_determinism.mjs [--quick]
 *
 * `wasm_smoke.mjs` proves the engine runs inside a wasm instance and that the
 * report row counts agree. That is not what 3.2 asks for. It asks that the bytes
 * agree, which is a much stronger claim and the one a consumer actually relies
 * on: a browser preview and a server side batch report of the same pair have to
 * be the same report, or the preview is not a preview.
 *
 * Four builds are compared on every case, and the last two are the ones that earn
 * their place:
 *
 *   native   the CLI, x86-64, GCC
 *   wasm     the scalar wasm32 module
 *   simd     the SIMD wasm32 module
 *   java     the JNI binding, through its own driver
 *
 * The scalar module is retained per spec 13.11 as the differential oracle for the
 * SIMD parser rather than as a build we expect to ship. Phase 7 replaces the
 * parser's inner loop with SIMD intrinsics, and this is the harness that will
 * catch it when the two disagree on a quoted field that straddles a 16 byte lane.
 * Wiring it up before there is anything to catch is the point: the check has to
 * already be passing for its first failure to mean something.
 *
 * Java is here because spec section 9's whole argument is that a server side
 * reconciliation and a browser preview of the same pair must be the same report.
 * The Java binding compiles the same C sources into a shared library and drives
 * them across a different boundary, with its own staging buffer, its own option
 * marshalling and its own two pass emit sizing. Any of those could produce a
 * report that is nearly right, and nearly right is the failure this exists to
 * refuse.
 *
 * The wasm and Java sides are driven through the real bindings rather than through
 * private drivers, so this also exercises the staging paths, the option structs
 * and the chunked feeds on every case.
 *
 * Java is included when java/target/classes is present and skipped with a notice
 * when it is not, so that a checkout with no Maven still runs the other three.
 * Pass --require-java to make its absence a failure, which is what the Phase 6
 * gate does.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { Engine } from '../js/packages/core/src/module.ts';
import { DiffEngine } from '../js/packages/core/src/engine.ts';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const F = path.join(root, 'core/fixtures/generated');
const CLI = path.join(root, 'core/build/ibha-csvdiff');
const WORK = path.join(root, 'core/build/determinism');

const quick = process.argv.includes('--quick');
const requireJava = process.argv.includes('--require-java');

/* ---------------------------------------------------- the input pairs -- */

mkdirSync(WORK, { recursive: true });

/**
 * Derives a target from a source by editing it, so that an edge case fixture
 * that ships as a single file still produces a diff with something in it.
 * Comparing a file against itself exercises the unchanged path and nothing else,
 * and the unchanged path is not where a build difference would hide.
 */
function derive(name, sourceFile, headerRows, edit) {
  const source = path.isAbsolute(sourceFile) ? sourceFile : path.join(F, sourceFile);
  const out = edit(splitRecords(readFileSync(source, 'latin1'))).join('\n');
  const target = path.join(WORK, name);
  writeFileSync(target, out, 'latin1');
  return { source, target, headerRows };
}

/**
 * Splits on record boundaries rather than on newlines.
 *
 * A newline inside a quoted field is not a record boundary, and treating it as
 * one produced a target with an unterminated quote and a fixture that could not
 * be parsed at all. Multiline fields are exactly the sort of input this check
 * exists to cover, so the splitter has to know about them.
 */
function splitRecords(text) {
  const out = [];
  let start = 0;
  let inQuotes = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"') {
      // A doubled quote inside a quoted field is an escaped quote, not the end.
      if (inQuotes && text[i + 1] === '"') i++;
      else inQuotes = !inQuotes;
    } else if (c === '\n' && !inQuotes) {
      out.push(text.slice(start, i));
      start = i + 1;
    }
  }
  out.push(text.slice(start));
  return out;
}

/** One changed cell, one appended row and one deleted row: enough to make every
 *  report row kind appear, which is what the emitters branch on. */
function stir(headerRows) {
  return (lines) => {
    const body = lines.slice(headerRows).filter((l) => l.length > 0);
    if (body.length === 0) return lines;
    const edited = body.slice();
    edited[0] = edited[0].replace(/,ACTIVE,/, ',CANCELLED,').replace(/,1\.00,/, ',1.50,');
    if (edited.length > 1) edited.splice(1, 1);
    const last = edited[edited.length - 1];
    edited.push(last.replace(/^ACC-\d+/, 'ACC-99999').replace(/^[^,]*/, (m) => `${m}Z`));
    return [...lines.slice(0, headerRows), ...edited, ''];
  };
}

const pairs = [
  {
    name: 'tiny',
    source: path.join(F, 'tiny_source.csv'),
    target: path.join(F, 'tiny_target.csv'),
    headerRows: 4,
  },
  derive('xss_target.csv', 'xss.csv', 1, stir(1)),
  derive('latin1_target.csv', 'latin1.csv', 1, stir(1)),
  derive('escaped_target.csv', 'escaped_quotes.csv', 4, stir(4)),
  derive('multiline_target.csv', 'multiline_quoted.csv', 4, stir(4)),
  derive('crlf_target.csv', 'crlf.csv', 4, stir(4)),
  bomPair(),
].map((p, i) => ({ name: p.name ?? path.basename(p.target, '.csv'), ...p, order: i }));

/**
 * bom.csv ships as four header rows and nothing else, so stirring it produces a
 * pair with an empty report and a jsonl comparison of zero bytes against zero
 * bytes, which passes without testing anything. Given a body it covers what it is
 * for: that the BOM is stripped identically on both builds and does not end up
 * inside the first column name.
 */
function bomPair() {
  const header = readFileSync(path.join(F, 'bom.csv'), 'latin1').replace(/\n*$/, '\n');
  const body = splitRecords(readFileSync(path.join(F, 'tiny_source.csv'), 'latin1'))
    .slice(4, 40)
    .filter((l) => l.length > 0);
  const source = path.join(WORK, 'bom_source.csv');
  writeFileSync(source, `${header}${body.join('\n')}\n`, 'latin1');
  return derive('bom_target.csv', source, 4, stir(4));
}

if (!quick) {
  pairs.push({
    name: 'p90',
    source: path.join(F, 'p90_source.csv'),
    target: path.join(F, 'p90_target.csv'),
    headerRows: 4,
  });
}

/* -------------------------------------------------- the emitter matrix -- */

/**
 * Each entry is one emitter configuration, expressed once and translated into
 * CLI flags and into binding options, so the two cannot drift into testing
 * different things.
 */
const configs = [
  { name: 'summary', format: 'summary', opts: {} },
  { name: 'jsonl', format: 'jsonl', opts: {} },
  { name: 'jsonl changes-only', format: 'jsonl', opts: { changesOnly: true } },
  { name: 'csv', format: 'csv', opts: {} },
  { name: 'csv changes-only', format: 'csv', opts: { changesOnly: true } },
  { name: 'html', format: 'html', opts: { changesOnly: true } },
  {
    name: 'html cell-diff word',
    format: 'html',
    opts: { changesOnly: true, cellDiff: 'word' },
  },
  {
    name: 'html cell-diff both, capped',
    format: 'html',
    opts: { changesOnly: true, cellDiff: 'word-then-character', maxRows: 25 },
  },
  { name: 'jsonl no-validate', format: 'jsonl', opts: {}, validate: false },
  { name: 'summary no-moves', format: 'summary', opts: {}, detectMoves: false },
];

function cliArgs(pair, cfg) {
  const args = ['diff', pair.source, pair.target, '--header', String(pair.headerRows)];
  args.push('--format', cfg.format);
  if (cfg.opts.changesOnly) args.push('--changes-only');
  if (cfg.opts.maxRows) args.push('--max-rows', String(cfg.opts.maxRows));
  if (cfg.opts.cellDiff === 'word') args.push('--cell-diff', 'word');
  if (cfg.opts.cellDiff === 'character') args.push('--cell-diff', 'char');
  if (cfg.opts.cellDiff === 'word-then-character') args.push('--cell-diff', 'both');
  if (cfg.validate === false) args.push('--no-validate');
  if (cfg.detectMoves === false) args.push('--no-moves');
  return args;
}

function jsOptions(pair, cfg) {
  const o = { header: { rows: pair.headerRows } };
  if (cfg.validate !== undefined) o.validate = cfg.validate;
  if (cfg.detectMoves !== undefined) o.matching = { detectMoves: cfg.detectMoves };
  return o;
}

/* --------------------------------------------------------- the drivers -- */

function runNative(pair, cfg) {
  // The CLI exits 1 when it finds differences, which is the diff(1) convention
  // and not a failure. Anything else is.
  try {
    return execFileSync(CLI, cliArgs(pair, cfg), { maxBuffer: 1 << 30 });
  } catch (err) {
    if (err.status === 1 && err.stdout) return err.stdout;
    throw new Error(
      `native run failed (${err.status}): ${cliArgs(pair, cfg).join(' ')}\n${err.stderr ?? ''}`,
    );
  }
}

/* --------------------------------------------------------- the java build -- */

/**
 * The JNI binding, driven through the same flags the CLI takes.
 *
 * One JVM per case rather than one for the whole matrix, because that is what
 * keeps this a comparison of the *binding* and not of a long lived process: a
 * fresh context, a fresh staging buffer and a fresh library load per case is the
 * shape a batch worker actually runs in.
 */
const JAVA_CLASSES = path.join(root, 'java/target/classes');
const JAVA_TEST_CLASSES = path.join(root, 'java/target/test-classes');
const JAVA_NATIVE = path.join(root, 'java/target/native');

function javaAvailable() {
  return existsSync(JAVA_CLASSES) && existsSync(JAVA_TEST_CLASSES) && existsSync(JAVA_NATIVE);
}

/** JDK 24 warns about loading a native library unless told it is expected. The
 *  flag does not exist on JDK 21, which the artifact targets, so it is added only
 *  where it is understood. */
const javaFlags = (() => {
  try {
    const out = execFileSync('java', ['-XshowSettings:properties', '-version'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    const line = (out ?? '').split('\n').find((l) => l.includes('java.specification.version'));
    const major = Number.parseInt((line ?? '').split('=')[1]?.trim() ?? '0', 10);
    return major >= 24 ? ['--enable-native-access=ALL-UNNAMED'] : [];
  } catch {
    return [];
  }
})();

function runJava(pair, cfg) {
  const args = [
    ...javaFlags,
    `-Dibha.csvdiff.nativeLibPath=${JAVA_NATIVE}`,
    '-cp',
    `${JAVA_CLASSES}${path.delimiter}${JAVA_TEST_CLASSES}`,
    'com.ibhatech.csvdiff.tools.EmitMain',
    ...cliArgs(pair, cfg).slice(1), // the same flags, without the `diff` subcommand
  ];
  try {
    return execFileSync('java', args, { maxBuffer: 1 << 30, stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (err) {
    // diff(1) exit codes, as the CLI uses: 1 means differences were found.
    if (err.status === 1 && err.stdout) return err.stdout;
    throw new Error(
      `java run failed (${err.status}): ${args.join(' ')}\n${err.stderr ?? ''}`,
    );
  }
}

async function runWasm(engine, pair, cfg) {
  const d = await DiffEngine.compare(
    engine,
    new Uint8Array(readFileSync(pair.source)),
    new Uint8Array(readFileSync(pair.target)),
    jsOptions(pair, cfg),
  );
  try {
    return d.emit(cfg.format, cfg.opts);
  } finally {
    d.dispose();
  }
}

/* ------------------------------------------------------------ comparing -- */

function firstDifference(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) {
    if (a[i] !== b[i]) return i;
  }
  return a.length === b.length ? -1 : n;
}

const dec = new TextDecoder('utf-8');

function context(bytes, at) {
  const from = Math.max(0, at - 60);
  return JSON.stringify(dec.decode(bytes.subarray(from, Math.min(bytes.length, at + 60))));
}

let checks = 0;
let failures = 0;

function compare(label, other, expected, actual) {
  checks++;
  const at = firstDifference(expected, actual);
  if (at < 0) return true;
  failures++;
  console.log(`FAIL ${label}`);
  console.log(`     lengths ${expected.length} vs ${actual.length}, first difference at byte ${at}`);
  console.log(`     native  ${context(expected, at)}`);
  console.log(`     ${other.padEnd(6)}  ${context(actual, at)}`);
  return false;
}

/* ------------------------------------------------------------------ run -- */

const withJava = javaAvailable();
if (!withJava) {
  const message =
    'the java binding is not built: expected java/target/{classes,test-classes,native}.\n' +
    '       Build it with:  cd java && mvn -q test-compile';
  if (requireJava) {
    console.log(`FAIL ${message}`);
    process.exit(1);
  }
  console.log(`note ${message}`);
  console.log('     comparing three builds instead of four.');
  console.log('');
}

const builds = withJava ? 'four' : 'three';

const scalar = await Engine.load({ simd: false });
const simd = await Engine.load({ simd: true });

for (const pair of pairs) {
  for (const cfg of configs) {
    const label = `${pair.name} / ${cfg.name}`;
    const native = new Uint8Array(runNative(pair, cfg));
    const wasm = await runWasm(scalar, pair, cfg);
    const wasmSimd = await runWasm(simd, pair, cfg);

    let ok = compare(`${label}  native vs wasm`, 'wasm', native, wasm);
    ok = compare(`${label}  native vs wasm+simd`, 'simd', native, wasmSimd) && ok;
    if (withJava) {
      const java = new Uint8Array(runJava(pair, cfg));
      ok = compare(`${label}  native vs java`, 'java', native, java) && ok;
    }
    if (ok) {
      console.log(`ok   ${label}  ${native.length} bytes identical across ${builds} builds`);
    }
  }
}

const names = withJava ? 'native, wasm, wasm+simd and java' : 'native, wasm and wasm+simd';
console.log('');
console.log(
  failures
    ? `FAILED ${failures} of ${checks} comparisons`
    : `all ${checks} comparisons byte identical across ${names}`,
);
process.exit(failures ? 1 : 0);
