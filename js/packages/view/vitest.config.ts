import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';

/**
 * The engine package is aliased to its sources rather than to its `dist`, so this
 * suite runs on a checkout that has never been built, which is the property the C
 * gate and the binding suite already have. The parity test needs the real engine,
 * so it needs the real sources.
 */
const src = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default defineConfig({
  resolve: {
    alias: { '@ibhatech/csvdiff-core': src('../core/src/index.ts') },
  },
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts'],
  },
});
