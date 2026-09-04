/**
 * Test support for this package. Not exported from the public entry point.
 *
 * **Why `node:test` rather than vitest here.** Everything this package's tests
 * drive is wasm and bytes; none of it needs a DOM. Running them under bare node
 * gives the JS side the same property the C side already has, that the whole gate
 * runs on a checkout with nothing installed. `@ibhatech/csvdiff-view` and
 * `-react` keep vitest, because a component test does need a DOM.
 *
 * `expect` is a shim over `node:assert` covering exactly the matchers these tests
 * use. It exists so the assertions read the way the rest of the monorepo's do,
 * not to reimplement a matcher library, and it should stay that small.
 */

import assert from 'node:assert/strict';

export { describe, it } from 'node:test';

interface Matchers<T> {
  toBe(expected: T): void;
  toEqual(expected: unknown): void;
  toBeUndefined(): void;
  toBeNull(): void;
  toBeTruthy(): void;
  toBeGreaterThan(n: number): void;
  toBeGreaterThanOrEqual(n: number): void;
  toBeLessThanOrEqual(n: number): void;
  toContain(needle: string): void;
  toMatch(re: RegExp): void;
  toMatchObject(shape: Record<string, unknown>): void;
  toThrow(re?: RegExp): void;
}

interface AsyncMatchers {
  toThrow(re?: RegExp): Promise<void>;
  toMatchObject(shape: Record<string, unknown>): Promise<void>;
}

export function expect<T>(actual: T): Matchers<T> & { rejects: AsyncMatchers } {
  const matchObject = (value: unknown, shape: Record<string, unknown>) => {
    for (const [k, v] of Object.entries(shape)) {
      assert.deepStrictEqual((value as Record<string, unknown>)[k], v, `property ${k}`);
    }
  };

  const caught = async (): Promise<unknown> => {
    try {
      await (actual as unknown as Promise<unknown>);
    } catch (err) {
      return err;
    }
    assert.fail('expected a rejection, but it resolved');
  };

  return {
    toBe: (expected) => assert.strictEqual(actual, expected),
    toEqual: (expected) => assert.deepStrictEqual(actual, expected),
    toBeUndefined: () => assert.strictEqual(actual, undefined),
    toBeNull: () => assert.strictEqual(actual, null),
    toBeTruthy: () => assert.ok(actual),
    toBeGreaterThan: (n) => assert.ok((actual as number) > n, `${actual} > ${n}`),
    toBeGreaterThanOrEqual: (n) => assert.ok((actual as number) >= n, `${actual} >= ${n}`),
    toBeLessThanOrEqual: (n) => assert.ok((actual as number) <= n, `${actual} <= ${n}`),
    toContain: (needle) => assert.ok(String(actual).includes(needle), `${actual} contains ${needle}`),
    toMatch: (re) => assert.match(String(actual), re),
    toMatchObject: (shape) => matchObject(actual, shape),
    toThrow: (re) => {
      if (re) assert.throws(actual as () => unknown, re);
      else assert.throws(actual as () => unknown);
    },
    rejects: {
      toThrow: async (re) => {
        const err = await caught();
        if (re) assert.match(err instanceof Error ? err.message : String(err), re);
      },
      toMatchObject: async (shape) => matchObject(await caught(), shape),
    },
  };
}

/* ------------------------------------------------------------ html safety -- */

/**
 * Everything the HTML emitter is allowed to write. A `<` that opens anything else
 * is cell content that became markup.
 *
 * This is `core/tests/emitkit.h` restated in TypeScript, on purpose and not by
 * translation: the point of that checker is that it shares no code with the
 * emitter and is written from the rules of HTML rather than from the emitter's
 * structure. A second independent statement of the same invariant, on the other
 * side of the wasm boundary, is worth more than importing the first would be.
 *
 * The reason it is stated positively matters. A test that greps for `<script>`
 * passes on `<img src=x onerror=alert(1)>`; this cannot.
 */
const HTML_TAGS = [
  '<div ', '<div>', '<table ', '<table>', '<thead>', '<tbody>', '<tr ', '<tr>',
  '<th ', '<th>', '<td ', '<td>', '<span ', '<span>', '<del ', '<del>', '<ins ',
  '<ins>', '</div>', '</table>', '</thead>', '</tbody>', '</tr>', '</th>',
  '</td>', '</span>', '</del>', '</ins>',
];

const HTML_ENTITIES = ['&amp;', '&lt;', '&gt;', '&quot;', '&#39;'];

/**
 * Returns null when every `<` opens a known tag, every `&` opens one of the five
 * entities, and no attribute value carries a raw quote or a raw `<`. Returns a
 * message naming the offending offset otherwise.
 */
export function htmlSafetyViolation(html: string): string | null {
  for (let i = 0; i < html.length; i++) {
    const ch = html[i];

    if (ch === '&') {
      if (!HTML_ENTITIES.some((e) => html.startsWith(e, i))) {
        return `raw '&' at ${i}: ${JSON.stringify(html.slice(i, i + 40))}`;
      }
      continue;
    }
    if (ch !== '<') continue;

    const tag = HTML_TAGS.find((t) => html.startsWith(t, i));
    if (!tag) return `unknown tag at ${i}: ${JSON.stringify(html.slice(i, i + 40))}`;

    if (tag.endsWith('>')) {
      i += tag.length - 1;
      continue;
    }

    // Inside an open tag, attribute values are the only place text appears, and
    // each must be a quoted run containing neither '<' nor a stray quote.
    let j = i + tag.length;
    while (j < html.length && html[j] !== '>') {
      if (html[j] === '"') {
        j++;
        while (j < html.length && html[j] !== '"') {
          if (html[j] === '<' || html[j] === '&') {
            return `markup inside an attribute value at ${j}: ${JSON.stringify(html.slice(i, j + 20))}`;
          }
          j++;
        }
        if (j >= html.length) return `unterminated attribute value from ${i}`;
      } else if (html[j] === '<') {
        return `'<' inside a tag at ${j}`;
      }
      j++;
    }
    if (j >= html.length) return `unterminated tag from ${i}`;
    i = j;
  }
  return null;
}

/* ---------------------------------------------------------------- fixtures -- */

import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

/** The C engine's generated fixtures, which both sides of the project test
 *  against so that a disagreement is about behaviour and not about inputs. */
export const FIXTURES = path.resolve(here, '../../../../core/fixtures/generated');

export function fixture(name: string): Uint8Array {
  return new Uint8Array(readFileSync(path.join(FIXTURES, name)));
}

export function csv(text: string): Uint8Array {
  return new TextEncoder().encode(text);
}

/** A four header row file with the schema the fixtures use, so a test can state
 *  only the rows it cares about. */
export function withHeader(...rows: string[]): Uint8Array {
  return csv(
    [
      'KEY,,,',
      'REQUIRED,,,',
      'VARCHAR(10),VARCHAR(20),INTEGER,"DECIMAL(5,2)"',
      'id,name,qty,rate',
      ...rows,
      '',
    ].join('\n'),
  );
}
