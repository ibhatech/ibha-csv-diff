/**
 * Generates the shipped CSS files from `stylesheet.ts`, and checks them.
 *
 *   node scripts-and-commands/gen_stylesheet.mjs
 *   node scripts-and-commands/gen_stylesheet.mjs --check
 *
 * The stylesheet is a function of the class prefix, because the prefix is
 * configurable per component under spec 13.0 and a consumer who changes it would
 * otherwise have to hand write the whole sheet. But a bundler imports a file, not
 * a function, so the default prefix's output is checked in. Generating it rather
 * than maintaining both is the same argument `gen_abi.mjs` makes about the struct
 * offsets: two copies of a thing that must agree, where only one of them is a
 * compile error when it drifts, is a way to ship a stylesheet that no longer
 * matches the classes the emitter writes.
 *
 * `--check` is in the Phase 5 gate and fails when the files on disk are stale.
 *
 * Runs the TypeScript source directly under node, which is why the import names
 * the `.ts` file. See `js/tsconfig.base.json`.
 */
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const viewPkg = path.resolve(here, '../js/packages/view');
const outDir = path.join(viewPkg, 'styles');

const { defaultStylesheet, themeStylesheet } = await import(
  path.join(viewPkg, 'src/stylesheet.ts')
);

const files = {
  'ibha-csvdiff.css': defaultStylesheet(),
  'theme-light.css': themeStylesheet('light'),
  'theme-dark.css': themeStylesheet('dark'),
  'theme-high-contrast.css': themeStylesheet('high-contrast'),
};

const check = process.argv.includes('--check');
let stale = 0;

mkdirSync(outDir, { recursive: true });
for (const [name, text] of Object.entries(files)) {
  const file = path.join(outDir, name);
  let current = null;
  try {
    current = readFileSync(file, 'utf8');
  } catch {
    /* not written yet */
  }
  if (current === text) continue;
  if (check) {
    console.error(`stale: styles/${name} differs from stylesheet.ts`);
    stale++;
    continue;
  }
  writeFileSync(file, text);
  console.log(`wrote styles/${name}, ${text.length} bytes`);
}

if (check) {
  if (stale) {
    console.error('run: node scripts-and-commands/gen_stylesheet.mjs');
    process.exit(1);
  }
  console.log(`${Object.keys(files).length} stylesheets are current`);
}
