// Refuse to pack @ibhatech/csvdiff-core without its engine.
//
// Phase N1 of docs/PUBLISHING-PLAN.md, and the single most likely way to ship a
// broken package. js/.gitignore excludes packages/*/wasm/*.wasm because they are
// build output of `pnpm wasm`, which runs `make -C ../core wasm`. So a publish
// from a fresh clone that skipped that step produces a tarball which installs
// cleanly, type checks cleanly, and throws at the consumer's first compare(),
// because there is no engine inside it. npm unpublish only works within 72 hours
// and only when nothing depends on the package, so this has to fail at pack time
// rather than be caught by a runbook step somebody skips.
//
// Wired as `prepack` in packages/core/package.json, which pnpm runs for both
// `pnpm pack` and `pnpm publish`.

import { readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const wasmDir = join(here, "..", "packages", "core", "wasm");
const coreSrc = join(here, "..", "..", "core", "src");

const REQUIRED = ["ibha_csvdiff.wasm", "ibha_csvdiff.simd.wasm"];

function fail(message) {
  console.error("\ncheck-wasm: " + message);
  console.error("\nBuild them first:\n    cd js && pnpm wasm\n");
  process.exit(1);
}

let present;
try {
  present = readdirSync(wasmDir);
} catch {
  fail(`no wasm directory at ${wasmDir}`);
}

const missing = REQUIRED.filter((f) => !present.includes(f));
if (missing.length > 0) {
  fail(
    `the engine is missing: ${missing.join(", ")}.\n` +
      "A package published without it installs and type checks, then throws at the\n" +
      "first compare(). That is not recoverable: npm unpublish is 72 hours only.",
  );
}

for (const f of REQUIRED) {
  const size = statSync(join(wasmDir, f)).size;
  if (size === 0) fail(`${f} is zero bytes`);
}

// A stale module is not fatal, because a legitimate release can be built from a
// tree whose C sources were touched without changing them. It is worth saying out
// loud, though: shipping an engine older than the sources it claims to be is how
// a fixed bug reappears in a published package.
let newestSrc = 0;
try {
  for (const f of readdirSync(coreSrc)) {
    if (!f.endsWith(".c") && !f.endsWith(".h")) continue;
    newestSrc = Math.max(newestSrc, statSync(join(coreSrc, f)).mtimeMs);
  }
} catch {
  newestSrc = 0; // core/ is not there, which is the case in a consumer's tree
}

if (newestSrc > 0) {
  for (const f of REQUIRED) {
    const built = statSync(join(wasmDir, f)).mtimeMs;
    if (built < newestSrc) {
      console.warn(
        `check-wasm: warning: ${f} is older than the newest file in core/src.\n` +
          "            Run `pnpm wasm` unless you know why.",
      );
    }
  }
}

const sizes = REQUIRED.map(
  (f) => `${f} ${statSync(join(wasmDir, f)).size.toLocaleString()} bytes`,
).join(", ");
console.log(`check-wasm: ok, ${sizes}`);
