/**
 * The view and the HTML emitter must agree, and this is where that is checked.
 *
 * Spec 13.3 splits the two by size, not by meaning: the emitter for bounded
 * output, which in practice is changes-only or a page at a time, and the
 * virtualized view for interactive browsing of a full diff. They agree by
 * construction because they consume the same cursor. "By construction" is a claim
 * about a design, though, and the two decode paths are separate code: the emitter
 * writes bytes out of engine memory in C, and the view decodes cells through the
 * binding and lays them out in TypeScript. A disagreement between them means a
 * user's saved report and their screen show different diffs of the same pair,
 * which is the failure this file exists to make impossible to ship.
 *
 * It drives the real engine over the real fixtures. No fakes.
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { compare } from '@ibhatech/csvdiff-core';
import type { CompactRowPage, DiffRow } from '@ibhatech/csvdiff-core';

import { cellClass, classNames, rowClass } from './classes.ts';
import { presentRow, structureFromCompact } from './rowModel.ts';
import { localRowSource } from './source.ts';
import { htmlSafetyViolation } from './testkit.ts';

const FIXTURES = new URL('../../../../core/fixtures/generated/', import.meta.url);
const fixture = (name: string) => new Uint8Array(readFileSync(fileURLToPath(new URL(name, FIXTURES))));

/** The five the emitter writes, and nothing else: it never emits a named entity
 *  the checker does not know about, which is itself asserted below. */
function unescape(html: string): string {
  return html
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&');
}

interface EmittedCell {
  className: string;
  finding: string | null;
  text: string;
  old: string | null;
}

interface EmittedRow {
  className: string;
  number: string;
  cells: EmittedCell[];
}

/**
 * A reader for the emitter's output, written against what the emitter documents
 * rather than against how it is built. It is regex based because the output is
 * machine generated and its shape is fixed; anything it cannot read it refuses
 * rather than skips, so a change in the emitter's markup fails here loudly.
 */
function parseEmittedRows(html: string): EmittedRow[] {
  const body = html.slice(html.indexOf('<tbody>') + 7, html.lastIndexOf('</tbody>'));
  const rows: EmittedRow[] = [];

  for (const rowMatch of body.matchAll(/<tr class="([^"]*)">(.*?)<\/tr>/gs)) {
    const inner = rowMatch[2] as string;
    const cells: EmittedCell[] = [];
    let number = '';
    let first = true;

    for (const cellMatch of inner.matchAll(/<td class="([^"]*)"([^>]*)>(.*?)<\/td>/gs)) {
      const className = cellMatch[1] as string;
      const attrs = cellMatch[2] as string;
      const content = cellMatch[3] as string;
      if (first) {
        number = content;
        first = false;
        continue;
      }
      const finding = /data-finding="([^"]*)"/.exec(attrs);
      const both = /^<span class="[^"]*old">(.*?)<\/span><span class="[^"]*new">(.*?)<\/span>$/s.exec(
        content,
      );
      cells.push({
        className,
        finding: finding ? (finding[1] as string) : null,
        text: unescape(both ? (both[2] as string) : content),
        old: both ? unescape(both[1] as string) : null,
      });
    }
    rows.push({ className: rowMatch[1] as string, number, cells });
  }
  return rows;
}

/** Compares every row of one emitted report against the view's own layout of the
 *  same rows, and returns how many it checked. */
function assertAgreement(
  handle: Awaited<ReturnType<typeof compare>>,
  changesOnly: boolean,
): number {
  const html = new TextDecoder().decode(handle.emit('html', { changesOnly }));
  expect(htmlSafetyViolation(html)).toBeNull();

  const emitted = parseEmittedRows(html);
  const source = localRowSource(handle, { changesOnly });
  const info = source.ready() as { rowCount: number };
  expect(info.rowCount).toBe(emitted.length);

  const c = classNames();
  // A local source answers synchronously, which is the whole reason the model has
  // a synchronous path; the casts say so rather than awaiting a value that was
  // never a promise.
  const page = source.getRowsCompact(0, info.rowCount) as CompactRowPage;
  const rows = source.getRows(0, info.rowCount, { includeValues: true }) as DiffRow[];

  for (let i = 0; i < info.rowCount; i++) {
    const struct = structureFromCompact(page, i);
    const [view] = presentRow(struct, rows[i] as DiffRow, {
      columns: source.columns,
      layout: 'inline',
      oldValuePosition: 'above',
    });
    const want = emitted[i] as EmittedRow;

    expect(rowClass(c, view!.kind, view!.moved, view!.hasFinding), `row ${i} class`).toBe(
      want.className,
    );
    expect(String(view!.rowNumber ?? ''), `row ${i} number`).toBe(want.number);
    expect(view!.cells.length, `row ${i} width`).toBe(want.cells.length);

    for (let col = 0; col < want.cells.length; col++) {
      const got = view!.cells[col]!;
      const expected = want.cells[col]!;
      expect(cellClass(c, got.flags), `row ${i} cell ${col} class`).toBe(expected.className);
      expect(got.finding, `row ${i} cell ${col} finding`).toBe(expected.finding);
      expect(got.text, `row ${i} cell ${col} value`).toBe(expected.text);
      expect(got.old ?? null, `row ${i} cell ${col} old value`).toBe(expected.old);
    }
  }
  return info.rowCount;
}

describe('the view against the HTML emitter, over the same cursor', () => {
  it('renders the same rows, classes, row numbers and values', async () => {
    const handle = await compare(fixture('tiny_source.csv'), fixture('tiny_target.csv'));
    try {
      expect(assertAgreement(handle, true)).toBeGreaterThan(0);
    } finally {
      handle.dispose();
    }
  });

  it('agrees on the unfiltered report too, where most rows are unchanged', async () => {
    // A different code path in both: the emitter reads an unchanged row's cells
    // only when validation is on, and the view's `source` absence rule applies to
    // every cell of every one of them.
    const handle = await compare(fixture('tiny_source.csv'), fixture('tiny_target.csv'));
    try {
      const filtered = assertAgreement(handle, true);
      const all = assertAgreement(handle, false);
      expect(all).toBeGreaterThan(filtered);
    } finally {
      handle.dispose();
    }
  });

  it('agrees at the p90, which is where a disagreement would actually hide', async () => {
    // 2,198 changed rows over twelve columns. The tiny fixture has a handful of
    // rows and would not catch a rule that is wrong only for, say, a moved row
    // that is also modified.
    const handle = await compare(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      expect(assertAgreement(handle, true)).toBeGreaterThan(1_000);
    } finally {
      handle.dispose();
    }
  }, 30_000);

  /**
   * A cell whose value contains markup must be text in both documents. The
   * emitter's side of this is checked in the C suite and in the binding's; what
   * is checked here is that the view decodes the same characters, so that the
   * component escaping them is escaping the right thing.
   */
  it('decodes the same characters the emitter escaped', async () => {
    const handle = await compare(fixture('xss.csv'), fixture('xss.csv'), {
      header: { rows: 1 },
      matching: { requireKey: false },
    });
    try {
      const html = new TextDecoder().decode(handle.emit('html', { changesOnly: false }));
      expect(htmlSafetyViolation(html)).toBeNull();
      expect(html).toContain('&lt;script&gt;alert(1)&lt;/script&gt;');

      const source = localRowSource(handle);
      source.ready();
      const rows = source.getRows(0, 10, { includeValues: true }) as DiffRow[];
      expect(rows[0]?.cells[0]?.target).toBe('<script>alert(1)</script>');
      expect(rows[0]?.cells[2]?.target).toBe('" onload="x');
    } finally {
      handle.dispose();
    }
  });
});
