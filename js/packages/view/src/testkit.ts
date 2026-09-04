/**
 * The escaping guarantee, proved the way the rest of this project proves it.
 *
 * **Why this file exists rather than a code review.** Cell content is untrusted:
 * it arrives from a salesman's spreadsheet and it is rendered into a page an
 * approver is looking at. Spec 13.3 calls getting this wrong stored XSS against
 * the approver, and that is exactly what it is. The C engine proves its emitter
 * safe with `core/tests/emitkit.h`, and the binding proves the same property
 * again from the JavaScript side with `htmlSafetyViolation` in
 * `js/packages/core/src/testkit.ts`. This is the third statement of the same
 * invariant, for the markup the view renders itself.
 *
 * It is restated rather than imported, on purpose and for the same reason the
 * binding restated it rather than importing the C one: a checker that shares code
 * with the thing it checks proves that the two agree, not that either is right.
 * The rules below are written from the rules of HTML.
 *
 * **The invariant is stated positively**, which is the whole trick: every `<` in
 * the output opens one of a known list of tags, and every `&` opens a character
 * reference. A test that greps for `<script>` passes on
 * `<img src=x onerror=alert(1)>`; this cannot.
 *
 * One difference from the engine's checker, and it is a real one. The emitter
 * never puts file data in an attribute, so its checker can reject a `&` inside an
 * attribute value outright. The view does: `data-column` carries a column name
 * and `aria-label` carries cell values, both so that what colour conveys is also
 * available to CSS and to a screen reader. So the rule here is the correct HTML
 * one, that a `&` inside an attribute value must open a character reference, and
 * a `<` must not appear there at all. That still refuses every escape from the
 * attribute; it just does not refuse React's own correct escaping of `&`.
 */

import type { CompactRowPage, DiffCell, DiffRow, TextSegment } from '@ibhatech/csvdiff-core';

import {
  CELL_CHANGED,
  CELL_NOT_NUMERIC,
  CELL_PRECISION,
  CELL_REQUIRED_EMPTY,
  CELL_SUPPRESSED,
  CELL_TOO_LONG,
  ROW_KINDS,
  type FindingName,
  type RowKind,
} from './classes.ts';
import type { DiffRowSource, MaybePromise } from './source.ts';

/** Everything the view and the emitter between them are allowed to open. */
const ALLOWED_TAGS = new Set([
  'div', 'table', 'thead', 'tbody', 'tr', 'th', 'td', 'span', 'del', 'ins',
  'colgroup', 'col', 'button', 'label', 'input', 'select', 'option', 'template',
]);

/**
 * Every attribute the view and the emitter write, plus the two open families.
 *
 * This list is the part that catches the attack a tag whitelist misses. A value
 * of `" onload="x` carries no angle bracket at all: it closes the attribute it is
 * inside and opens a new one, and the result is well formed markup that a scanner
 * checking only tags and quoting accepts. Checking the attribute *names* refuses
 * it, because `onload` is not on this list and no amount of escaping failure can
 * put it there legitimately.
 *
 * `data-*` and `aria-*` are open because the view writes `data-column` and
 * `aria-label` from file data. Both families are inert: the worst an escape into
 * one can do is add an attribute nothing reads. `href`, `src` and every `on*` are
 * absent on purpose, so a `javascript:` URL cannot appear either.
 */
const ALLOWED_ATTRS = new Set([
  'class', 'style', 'id', 'role', 'scope', 'colspan', 'rowspan', 'title', 'hidden',
  'tabindex', 'type', 'checked', 'value', 'for', 'name', 'selected', 'disabled',
  'width', 'height', 'lang', 'dir',
]);

const NAMED_ENTITIES = ['&amp;', '&lt;', '&gt;', '&quot;', '&apos;'];
/** `&#39;` and React's `&#x27;` are the same character by a different spelling. */
const NUMERIC_ENTITY = /^&#(?:[0-9]{1,7}|[xX][0-9a-fA-F]{1,6});/;

function entityAt(html: string, i: number): number {
  for (const e of NAMED_ENTITIES) if (html.startsWith(e, i)) return e.length;
  const m = NUMERIC_ENTITY.exec(html.slice(i, i + 12));
  return m ? m[0].length : 0;
}

function tagNameAt(html: string, i: number): string | null {
  // `<` then an optional `/` then a name, ended by whitespace, `/` or `>`.
  let j = i + 1;
  if (html[j] === '/') j++;
  const start = j;
  while (j < html.length && /[A-Za-z0-9]/.test(html[j] as string)) j++;
  if (j === start) return null;
  const next = html[j];
  if (next !== undefined && next !== '>' && next !== '/' && !/\s/.test(next)) return null;
  return html.slice(start, j).toLowerCase();
}

/**
 * Returns null when the markup is safe, and a message naming the offset when it
 * is not.
 *
 * Safe means, precisely, all four of:
 *
 *   1. every `<` opens a tag whose name is in the allowed set;
 *   2. every attribute inside an open tag has a name from the allowed set or a
 *      `data-` or `aria-` prefix, so nothing executable can appear;
 *   3. every attribute value is quoted, and contains no `<` and no `&` that does
 *      not open a character reference;
 *   4. in text content, every `&` opens a character reference.
 *
 * What it therefore guarantees: no cell value can close its element, open a tag,
 * or become an executable attribute. What it does not claim: that an escape into
 * `data-`, which is inert, is impossible.
 */
export function htmlSafetyViolation(html: string): string | null {
  const near = (i: number) => JSON.stringify(html.slice(Math.max(0, i - 20), i + 40));

  for (let i = 0; i < html.length; i++) {
    const ch = html[i];

    if (ch === '&') {
      const n = entityAt(html, i);
      if (n === 0) return `raw '&' in text at ${i}: ${near(i)}`;
      i += n - 1;
      continue;
    }
    if (ch === '>') continue;
    if (ch !== '<') continue;

    const name = tagNameAt(html, i);
    if (name === null) return `'<' that opens no tag at ${i}: ${near(i)}`;
    if (!ALLOWED_TAGS.has(name)) return `unexpected tag <${name}> at ${i}: ${near(i)}`;

    // Walk the open tag as a sequence of name="value" pairs, rather than as text
    // with quotes in it. Reading it as a sequence is what makes an attribute
    // breakout visible: the injected `onload` is simply the next name.
    let j = i + 1 + name.length;
    if (html[i + 1] === '/') j++;
    for (;;) {
      while (j < html.length && /\s/.test(html[j] as string)) j++;
      if (j >= html.length) return `unterminated tag from ${i}`;
      if (html[j] === '/') j++;
      if (html[j] === '>') break;
      if (html[j] === '<') return `'<' inside a tag at ${j}: ${near(j)}`;

      const nameStart = j;
      while (j < html.length && /[A-Za-z0-9:_.-]/.test(html[j] as string)) j++;
      if (j === nameStart) return `unparseable attribute at ${j}: ${near(j)}`;
      const attr = html.slice(nameStart, j).toLowerCase();
      if (!ALLOWED_ATTRS.has(attr) && !attr.startsWith('data-') && !attr.startsWith('aria-')) {
        return `disallowed attribute ${attr} at ${nameStart}: ${near(nameStart)}`;
      }

      while (j < html.length && /\s/.test(html[j] as string)) j++;
      if (html[j] !== '=') continue; // a bare boolean attribute
      j++;
      while (j < html.length && /\s/.test(html[j] as string)) j++;
      const quote = html[j];
      if (quote !== '"' && quote !== "'") {
        // An unquoted value ends at the first space, so a value containing one
        // becomes further attributes. A renderer must never emit them.
        return `unquoted attribute value for ${attr} at ${j}: ${near(j)}`;
      }
      j++;
      while (j < html.length && html[j] !== quote) {
        if (html[j] === '<') return `'<' inside an attribute value at ${j}: ${near(j)}`;
        if (html[j] === '&' && entityAt(html, j) === 0) {
          return `raw '&' inside an attribute value at ${j}: ${near(j)}`;
        }
        j++;
      }
      if (j >= html.length) return `unterminated attribute value from ${nameStart}`;
      j++;
    }
    i = j;
  }
  return null;
}

/* ------------------------------------------------------------- xss corpus -- */

/**
 * Values that must never become live markup.
 *
 * The first two are the C fixture's, so all three checkers in this project are
 * pointed at the same thing at least once. The rest are the vectors that defeat
 * the naive defences: an attribute breakout with no angle bracket at all, a tag
 * that is not `<script>`, and a closing tag that escapes the cell rather than the
 * document.
 *
 * Every one of them is rejected by `htmlSafetyViolation` when inserted raw, and
 * that is asserted, because a corpus nothing can fail is decoration.
 */
export const XSS_VALUES: readonly string[] = [
  '<script>alert(1)</script>',
  '" onload="x',
  '<img src=x onerror=alert(1)>',
  "'><svg/onload=alert(1)>",
  '</td></tr><script>alert(1)</script>',
  '"><a href="javascript:alert(1)">click</a>',
  '<!-- comment --><b>bold</b>',
  '<![CDATA[<script>alert(1)</script>]]>',
  ' <script>',
  'café <b>é</b>',
  '](){}<>&"\'\\',
];

/**
 * Values that are already entity shaped, which is a different failure.
 *
 * A cell containing the eight characters `&lt;foo&gt;` is not an attack: written
 * straight through it renders as the text `<foo>`, which is wrong but harmless.
 * The renderer must escape the ampersand so the reviewer sees what is actually in
 * their file. These are kept out of the corpus above because the safety checker
 * correctly accepts them raw, and folding the two together would force either a
 * weaker checker or a test that asserts something false.
 */
export const ENTITY_VALUES: readonly string[] = ['&lt;script&gt;', '&amp;', 'a & b', '&#39;'];

/** Column names are file data too. A header row is the one place an attacker can
 *  put a value that appears in every row of the report. */
export const XSS_COLUMN_NAMES: readonly string[] = [
  'id',
  '<script>alert(1)</script>',
  '" onmouseover="x',
  'amount & tax',
];

/* -------------------------------------------------------------- fake source -- */

/**
 * A `DiffRowSource` over rows written by hand, with no engine behind it.
 *
 * It exists so the view's own tests, and a consumer's tests of their own
 * renderer, can pin a specific shape of row without instantiating wasm: an added
 * row next to a deleted one, a modified row whose second cell is suppressed, a
 * cell carrying a finding. Every one of those is a handful of bytes to state here
 * and a fixture pair to produce for real.
 *
 * It reproduces the two parts of the row contract that are easy to get wrong,
 * because a fake that got them wrong would let a renderer that gets them wrong
 * pass: **`source` is emitted only where it differs from `target`**, and the
 * compact page's row numbers use 0 as the "no counterpart" sentinel.
 */
export interface FakeCell {
  source?: string;
  target?: string;
  changed?: boolean;
  suppressed?: boolean;
  finding?: FindingName;
}

export interface FakeRow {
  kind: RowKind;
  moved?: boolean;
  moveDistance?: number;
  sourceRow?: number | null;
  targetRow?: number | null;
  cells: FakeCell[];
}

const FINDING_BIT: Record<FindingName, number> = {
  requiredEmpty: CELL_REQUIRED_EMPTY,
  tooLong: CELL_TOO_LONG,
  notNumeric: CELL_NOT_NUMERIC,
  precision: CELL_PRECISION,
};

function flagByte(c: FakeCell): number {
  let f = 0;
  if (c.changed) f |= CELL_CHANGED;
  if (c.suppressed) f |= CELL_SUPPRESSED;
  if (c.finding) f |= FINDING_BIT[c.finding];
  return f;
}

export interface FakeSourceOptions {
  /** Answer through a resolved promise, as a source in a worker does. Off gives
   *  the synchronous path a local handle takes. */
  async?: boolean;
  /** Called with every page request, so a test can assert that scrolling asks for
   *  fifty rows rather than for the report. */
  onRequest?: (what: 'rows' | 'compact' | 'segments', offset: number, count: number) => void;
  segments?: (row: number, column: number) => TextSegment[];
}

export function fakeRowSource(
  columns: readonly string[],
  rows: readonly FakeRow[],
  options: FakeSourceOptions = {},
): DiffRowSource {
  const width = columns.length;
  const wrap = <T>(v: T): MaybePromise<T> => (options.async ? Promise.resolve(v) : v);
  const clamp = (offset: number, count: number): [number, number] => {
    const from = Math.max(0, Math.min(offset, rows.length));
    return [from, Math.max(from, Math.min(from + count, rows.length))];
  };

  return {
    columns,
    ready: () =>
      wrap({ rowCount: rows.length, columns: width, bytesRetained: rows.length * (14 + width) }),

    getRows: (offset, count, read) => {
      options.onRequest?.('rows', offset, count);
      const [from, to] = clamp(offset, count);
      const out: DiffRow[] = [];
      for (let i = from; i < to; i++) {
        const r = rows[i] as FakeRow;
        const cells: DiffCell[] = [];
        for (let c = 0; c < width; c++) {
          const f = r.cells[c] ?? {};
          const cell: DiffCell = {
            column: c,
            name: columns[c] ?? '',
            changed: f.changed === true,
            suppressed: f.suppressed === true,
          };
          // The contract: `source` is present exactly when the cell differs in
          // bytes from the target.
          if (read?.includeValues !== false) {
            if (f.source !== undefined && f.source !== f.target) cell.source = f.source;
            if (f.target !== undefined) cell.target = f.target;
          }
          cells.push(cell);
        }
        out.push({
          kind: r.kind,
          moved: r.moved === true,
          moveDistance: r.moveDistance ?? 0,
          sourceRow: r.sourceRow ?? null,
          targetRow: r.targetRow ?? null,
          key: null,
          changedCells: cells.filter((c) => c.changed).length,
          suppressedCells: cells.filter((c) => c.suppressed).length,
          cells: read?.includeValues === false ? [] : cells,
          findings: [],
        });
      }
      return wrap(out);
    },

    getRowsCompact: (offset, count) => {
      options.onRequest?.('compact', offset, count);
      const [from, to] = clamp(offset, count);
      const n = to - from;
      const page: CompactRowPage = {
        offset: from,
        count: n,
        columns: width,
        kinds: new Uint8Array(n),
        moved: new Uint8Array(n),
        moveDistance: new Int32Array(n),
        sourceRows: new Uint32Array(n),
        targetRows: new Uint32Array(n),
        cellFlags: new Uint8Array(n * width),
      };
      for (let i = 0; i < n; i++) {
        const r = rows[from + i] as FakeRow;
        page.kinds[i] = ROW_KINDS.indexOf(r.kind);
        page.moved[i] = r.moved ? 1 : 0;
        page.moveDistance[i] = r.moveDistance ?? 0;
        page.sourceRows[i] = r.sourceRow ?? 0;
        page.targetRows[i] = r.targetRow ?? 0;
        for (let c = 0; c < width; c++) {
          page.cellFlags[i * width + c] = flagByte(r.cells[c] ?? {});
        }
      }
      return wrap(page);
    },

    getCellSegments: (row, col) => {
      options.onRequest?.('segments', row, col);
      return wrap(options.segments?.(row, col) ?? []);
    },
  };
}
