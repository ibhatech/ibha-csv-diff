import { describe, expect, it } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import {
  EMITTER_CLASS_SUFFIXES,
  VIEW_ONLY_CLASS_SUFFIXES,
  localRowSource,
  type DiffRowSource,
} from '@ibhatech/csvdiff-view';
import {
  ENTITY_VALUES,
  fakeRowSource,
  htmlSafetyViolation,
  XSS_COLUMN_NAMES,
  XSS_VALUES,
  type FakeRow,
} from '@ibhatech/csvdiff-view/testkit';

import { CsvDiffTable } from './CsvDiffTable.tsx';

const COLUMNS = ['id', 'name', 'amount'];

function report(n: number): FakeRow[] {
  return Array.from({ length: n }, (_, i) => {
    const changed = i % 3 === 1;
    return {
      kind: changed ? 'modified' : 'unchanged',
      sourceRow: i + 1,
      targetRow: i + 1,
      cells: [
        { target: `ACC-${i}` },
        changed ? { source: `was ${i}`, target: `now ${i}`, changed: true } : { target: `n${i}` },
        { target: '12.00' },
      ],
    } satisfies FakeRow;
  });
}

const render = (source: DiffRowSource, props: Record<string, unknown> = {}) =>
  renderToStaticMarkup(<CsvDiffTable source={source} height={560} {...props} />);

/* ------------------------------------------------------------------ safety -- */

describe('escaping', () => {
  /**
   * The guarantee, proved the way the C engine and the binding prove theirs: an
   * independently written checker over real output, not a review and not a grep
   * for `<script>`. Cell content is untrusted, it arrives from a salesman's
   * spreadsheet, and spec 13.3 calls getting this wrong stored XSS against the
   * approver.
   */
  it('renders the XSS corpus as text in every layout', () => {
    const rows: FakeRow[] = XSS_VALUES.map((value, i) => ({
      kind: 'modified',
      sourceRow: i + 1,
      targetRow: i + 1,
      cells: [
        { target: value },
        { source: value, target: `${value}!`, changed: true, finding: 'tooLong' },
        { target: value },
      ],
    }));
    const source = fakeRowSource(XSS_COLUMN_NAMES.slice(0, 3), rows);

    for (const layout of ['inline', 'stacked', 'unified', 'sideBySide'] as const) {
      const html = render(source, { layout });
      expect(htmlSafetyViolation(html), `layout ${layout}`).toBeNull();
    }
  });

  it('escapes rather than drops, so the reviewer still sees what is in the file', () => {
    const source = fakeRowSource(COLUMNS, [
      { kind: 'added', targetRow: 1, cells: [{ target: '<script>alert(1)</script>' }] },
    ]);
    const html = render(source);
    expect(html).toContain('&lt;script&gt;alert(1)&lt;/script&gt;');
    expect(html).not.toContain('<script');
  });

  it('escapes a value that is already entity shaped, rather than passing it through', () => {
    // A cell holding the eight characters `&lt;foo&gt;` must display as those
    // eight characters. Writing them through would show `<foo>`, which is a
    // report that lies about its input even though nothing executes.
    const source = fakeRowSource(
      COLUMNS,
      ENTITY_VALUES.map((v, i) => ({ kind: 'added' as const, targetRow: i + 1, cells: [{ target: v }] })),
    );
    const html = render(source);
    expect(htmlSafetyViolation(html)).toBeNull();
    expect(html).toContain('&amp;lt;script&amp;gt;');
  });

  it('escapes an untrusted column name, which appears in every row of the report', () => {
    const source = fakeRowSource(XSS_COLUMN_NAMES, [
      { kind: 'added', targetRow: 1, cells: XSS_COLUMN_NAMES.map(() => ({ target: 'v' })) },
    ]);
    const html = render(source);
    expect(htmlSafetyViolation(html)).toBeNull();
    // Both as heading text and as the data-column attribute a stylesheet selects on.
    expect(html).toContain('data-column="&lt;script&gt;alert(1)&lt;/script&gt;"');
    expect(html).toContain('data-column="&quot; onmouseover=&quot;x"');
  });

  it('writes no attribute outside the contract, which is what refuses a breakout', () => {
    const source = fakeRowSource(COLUMNS, report(3));
    const html = render(source);
    for (const m of html.matchAll(/\s([a-zA-Z-]+)=/g)) {
      const attr = (m[1] as string).toLowerCase();
      expect(attr.startsWith('on'), `event handler attribute ${attr}`).toBe(false);
    }
  });
});

/* ------------------------------------------------------------------ classes -- */

describe('the class contract', () => {
  it('writes only classes the stylesheet knows, all behind the prefix', () => {
    const known = new Set<string>([...EMITTER_CLASS_SUFFIXES, ...VIEW_ONLY_CLASS_SUFFIXES]);
    const html = render(fakeRowSource(COLUMNS, report(6)));
    for (const m of html.matchAll(/class="([^"]*)"/g)) {
      for (const token of (m[1] as string).split(/\s+/).filter(Boolean)) {
        expect(token.startsWith('ibha-csvd-'), `unprefixed class ${token}`).toBe(true);
        expect(known.has(token.slice('ibha-csvd-'.length)), `unknown class ${token}`).toBe(true);
      }
    }
  });

  it('honours a custom prefix everywhere, including the theme variables', () => {
    const html = render(fakeRowSource(COLUMNS, report(2)), {
      classPrefix: 'x-',
      theme: { addedBg: '#0f0' },
    });
    expect(html).toContain('class="x-report"');
    expect(html).toContain('--x-added-bg:#0f0');
    expect(html).not.toContain('ibha-csvd-');
  });

  it('carries the schema version, as the emitter does on its container', () => {
    expect(render(fakeRowSource(COLUMNS, report(1)))).toContain('data-schema-version="1"');
  });

  it('marks a cell’s finding with the name the emitter writes', () => {
    const source = fakeRowSource(COLUMNS, [
      { kind: 'modified', sourceRow: 1, targetRow: 1, cells: [{ target: 'x', finding: 'notNumeric' }] },
    ]);
    expect(render(source)).toContain('data-finding="notNumeric"');
  });
});

/* ---------------------------------------------------------- virtualization -- */

describe('virtualization', () => {
  it('puts a bounded number of rows in the DOM for a 90,000 row report', () => {
    const html = render(fakeRowSource(COLUMNS, report(90_000)));
    const rows = [...html.matchAll(/<tr\b/g)].length;
    // A header, two spacers and the visible band. Not 90,000.
    expect(rows).toBeLessThan(60);
    expect(rows).toBeGreaterThan(10);
  });

  it('holds the scrollbar honest with spacer rows that carry the missing height', () => {
    const html = render(fakeRowSource(COLUMNS, report(90_000)), { rowHeight: 28 });
    const heights = [...html.matchAll(/<td class="ibha-csvd-spacer"[^>]*?height:(\d+)px/g)].map(
      (m) => Number(m[1]),
    );
    const total = heights.reduce((a, b) => a + b, 0);
    const rendered = [...html.matchAll(/<tr class="ibha-csvd-row/g)].length * 28;
    expect(total + rendered).toBe(90_000 * 28);
  });

  it('renders an empty report without a row and without throwing', () => {
    const html = render(fakeRowSource(COLUMNS, []));
    expect(htmlSafetyViolation(html)).toBeNull();
    expect(html).not.toContain('ibha-csvd-row');
  });
});

/* ------------------------------------------------------------------ layouts -- */

describe('row presentation modes', () => {
  const source = fakeRowSource(COLUMNS, report(9));

  it('inline puts the old value beside the new one in the same cell', () => {
    const html = render(source, { layout: 'inline' });
    expect(html).toContain('<span class="ibha-csvd-old">was 1</span>');
    expect(html).toContain('<span class="ibha-csvd-new">now 1</span>');
  });

  it('stacked gives a changed row two physical rows, new on top', () => {
    const html = render(source, { layout: 'stacked' });
    expect(html).toContain('data-variant="new"');
    expect(html).toContain('data-variant="old"');
    const newRows = [...html.matchAll(/data-variant="new"/g)].length;
    const oldRows = [...html.matchAll(/data-variant="old"/g)].length;
    expect(newRows).toBe(oldRows);
  });

  it('unified renders a modification as a deletion then an addition', () => {
    const html = render(source, { layout: 'unified' });
    const order = [...html.matchAll(/data-change="(\w+)"/g)].map((m) => m[1]);
    const at = order.indexOf('deleted');
    expect(at).toBeGreaterThanOrEqual(0);
    expect(order[at + 1]).toBe('added');
  });

  it('sideBySide renders two panes in one scroll container', () => {
    const html = render(source, { layout: 'sideBySide' });
    expect([...html.matchAll(/class="ibha-csvd-pane"/g)].length).toBe(2);
    expect(html).toContain('data-pane="source"');
    expect(html).toContain('data-pane="target"');
    // One scroll container, so the browser aligns the two rather than a scroll
    // handler doing it a frame late.
    expect([...html.matchAll(/data-virtualized/g)].length).toBe(1);
  });

  it('records where the old value sits, so the stylesheet can place it', () => {
    expect(render(source, { oldValuePosition: 'above' })).toContain(
      'data-old-value-position="above"',
    );
    // The DOM order is old then new whatever the setting, matching the emitter,
    // so a copy and a screen reader read the two documents the same way.
    const below = render(source, { oldValuePosition: 'below' });
    expect(below.indexOf('ibha-csvd-old')).toBeLessThan(below.indexOf('ibha-csvd-new'));
  });
});

/* ------------------------------------------------------------ accessibility -- */

describe('accessibility', () => {
  it('is a real table with grid semantics and a true row count', () => {
    const html = render(fakeRowSource(COLUMNS, report(90_000)));
    expect(html).toContain('role="grid"');
    expect(html).toContain('aria-rowcount="90001"');
    expect(html).toContain('aria-rowindex="2"');
  });

  it('says what a changed cell changed from and to, not only what colour it is', () => {
    const html = render(fakeRowSource(COLUMNS, report(3)));
    expect(html).toContain('aria-label="name, changed from was 1 to now 1"');
  });

  it('gives every change kind a text marker, because colour is never the only signal', () => {
    const source = fakeRowSource(COLUMNS, [
      { kind: 'added', targetRow: 1, cells: [{ target: 'a' }] },
      { kind: 'deleted', sourceRow: 2, cells: [{ source: 'b' }] },
      { kind: 'modified', sourceRow: 3, targetRow: 3, cells: [{ source: 'c', target: 'd', changed: true }] },
    ]);
    const html = render(source);
    const marks = [...html.matchAll(/<td class="ibha-csvd-mark" aria-hidden="true">([^<]*)<\/td>/g)];
    expect(marks.map((m) => m[1])).toEqual(['+', '-', '~']);
  });
});

/* ------------------------------------------------------------- the adapters -- */

describe('localRowSource', () => {
  it('builds the index lazily, so constructing a source in a render costs nothing', () => {
    let built = 0;
    const index = {
      rowCount: 0,
      columns: 3,
      bytesRetained: 0,
      getRows: () => [],
      getRowsCompact: () => ({
        offset: 0, count: 0, columns: 3,
        kinds: new Uint8Array(), moved: new Uint8Array(), moveDistance: new Int32Array(),
        sourceRows: new Uint32Array(), targetRows: new Uint32Array(), cellFlags: new Uint8Array(),
      }),
      getCellSegments: () => [],
    };
    const source = localRowSource({
      columns: COLUMNS,
      index: () => {
        built++;
        return index;
      },
    });
    expect(built).toBe(0);
    source.ready();
    source.ready();
    expect(built).toBe(1);
  });
});
