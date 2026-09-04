/**
 * Copies the wasm modules built by ibha-csvdiff-core into the core package, where
 * the loader expects them.
 *
 * Run via `pnpm wasm`, which builds them first. Kept as a script rather than a
 * build step because the wasm toolchain is a separate prerequisite and most
 * contributors working on the view layer never need it.
 */
import { copyFile, mkdir, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const coreBuild = resolve(here, '..', '..', 'core', 'build');
const dest = resolve(here, '..', 'packages', 'core', 'wasm');

const MODULES = ['ibha_csvdiff.wasm', 'ibha_csvdiff.simd.wasm'];

async function main() {
  await mkdir(dest, { recursive: true });

  let copied = 0;
  for (const name of MODULES) {
    const from = join(coreBuild, name);
    try {
      const info = await stat(from);
      await copyFile(from, join(dest, name));
      console.log(`  ${name}  ${(info.size / 1024).toFixed(1)} KB`);
      copied++;
    } catch (err) {
      if (err.code === 'ENOENT') {
        console.warn(`  ${name}  missing`);
      } else {
        throw err;
      }
    }
  }

  if (copied === 0) {
    console.error(
      [
        '',
        'error: no wasm modules found in ' + coreBuild,
        '',
        'Build them first. Apple clang has no WebAssembly backend, so point the',
        'core Makefile at a toolchain that does:',
        '',
        '  brew install llvm',
        '  make -C ../core wasm WASM_CC=/opt/homebrew/opt/llvm/bin/clang \\',
        '                       WASM_LD=/opt/homebrew/opt/llvm/bin/wasm-ld',
        '',
      ].join('\n'),
    );
    process.exit(1);
  }
}

await main();
