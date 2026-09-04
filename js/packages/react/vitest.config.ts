import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';

/**
 * jsdom, because two of these tests are about a scroll container and a keydown
 * and there is no honest way to test those without a DOM. The rest render through
 * `react-dom/server`, which needs none, and that is deliberate: the safety
 * checker wants a markup string, and a string is what a server render produces.
 *
 * Both workspace packages are aliased to their sources so the suite runs on an
 * unbuilt checkout.
 */
const src = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default defineConfig({
  resolve: {
    alias: {
      '@ibhatech/csvdiff-core': src('../core/src/index.ts'),
      '@ibhatech/csvdiff-view/testkit': src('../view/src/testkit.ts'),
      '@ibhatech/csvdiff-view': src('../view/src/index.ts'),
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
  },
});
