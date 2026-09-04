/**
 * The random access consumer, and the byte-to-code-unit boundary.
 *
 * The property that matters most here is that seeking and streaming produce the
 * same rows. They are two different code paths over the same cursor: one decodes
 * as it walks, the other decodes from a retained index afterwards. If they can
 * disagree, a view and an export of the same diff disagree, which is the failure
 * a user reports as "the preview said something different from the file".
 */

import { compare, sliceByBytes } from './index.ts';
import { describe, expect, fixture, it, withHeader } from './testkit.ts';

describe('the retained index', () => {
  it('yields the same rows as the streaming walk, in the same order', async () => {
    const h = await compare(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      const streamed = [...h.rows({ changesOnly: true })];
      const idx = h.index({ changesOnly: true });
      expect(idx.rowCount).toBe(streamed.length);

      const seeked = idx.getRows(0, idx.rowCount);
      expect(JSON.stringify(seeked)).toBe(JSON.stringify(streamed));
    } finally {
      h.dispose();
    }
  });

  it('costs bytes proportional to the report rather than to the file', async () => {
    const h = await compare(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      const idx = h.index();
      const perRow = idx.bytesRetained / idx.rowCount;
      // 14 bytes of row fields plus one flag byte per compared column. Asserting
      // the shape of the cost, not a number: what this catches is somebody
      // retaining cell values here, which would multiply it by twenty.
      expect(perRow).toBeLessThanOrEqual(14 + idx.columns + 1);
      expect(perRow).toBeGreaterThanOrEqual(14);
    } finally {
      h.dispose();
    }
  });

  it('clamps a page that runs off the end, which a virtualized list routinely asks for', async () => {
    const h = await compare(withHeader('a,alpha,1,1.00'), withHeader('a,beta,1,1.00'));
    try {
      const idx = h.index();
      expect(idx.getRows(0, 500).length).toBe(idx.rowCount);
      expect(idx.getRows(idx.rowCount + 10, 5).length).toBe(0);
      expect(idx.getRows(-5, 2).length).toBe(Math.min(2, idx.rowCount));
      expect(() => idx.getRow(idx.rowCount)).toThrow(/outside/);
    } finally {
      h.dispose();
    }
  });

  it('gives the compact page the same facts as the row objects', async () => {
    const h = await compare(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      const idx = h.index({ changesOnly: true });
      const rows = idx.getRows(100, 8);
      const page = idx.getRowsCompact(100, 8);

      expect(page.count).toBe(rows.length);
      expect(page.columns).toBe(idx.columns);
      for (let i = 0; i < rows.length; i++) {
        expect(['unchanged', 'modified', 'added', 'deleted'][page.kinds[i]!]).toBe(rows[i]!.kind);
        expect(page.moved[i] === 1).toBe(rows[i]!.moved);
        // 0 is the "no counterpart" sentinel: row numbers are 1 based, so no real
        // row can be 0 and the arrays stay readable.
        expect(page.sourceRows[i] === 0 ? null : page.sourceRows[i]).toBe(rows[i]!.sourceRow);
        expect(page.targetRows[i] === 0 ? null : page.targetRows[i]).toBe(rows[i]!.targetRow);
      }
    } finally {
      h.dispose();
    }
  });

  it('copies the compact page, so a later one cannot change one the view still holds', async () => {
    const h = await compare(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      const idx = h.index({ changesOnly: true });
      const first = idx.getRowsCompact(0, 4);
      const before = first.kinds[0]!;
      first.kinds[0] = 99;
      expect(idx.getRowsCompact(0, 4).kinds[0]).toBe(before);
    } finally {
      h.dispose();
    }
  });
});

describe('the segment memo', () => {
  it('returns the identical array on a repeat ask, which is what a repaint does', async () => {
    const h = await compare(withHeader('a,one two three,1,1.00'), withHeader('a,one four three,1,1.00'));
    try {
      const idx = h.index();
      const first = idx.getCellSegments(0, 1, 'word');
      expect(idx.getCellSegments(0, 1, 'word')).toBe(first);
    } finally {
      h.dispose();
    }
  });

  it('drops the memo when the mode changes, because the answers are different answers', async () => {
    const h = await compare(withHeader('a,one two three,1,1.00'), withHeader('a,one four three,1,1.00'));
    try {
      const idx = h.index();
      const word = idx.getCellSegments(0, 1, 'word');
      const chars = idx.getCellSegments(0, 1, 'character');
      expect(chars === word).toBe(false);
      // And asking for the first mode again recomputes rather than returning the
      // other mode's answer, which is the bug a mode-blind key would have.
      const wordAgain = idx.getCellSegments(0, 1, 'word');
      expect(JSON.stringify(wordAgain)).toBe(JSON.stringify(word));
    } finally {
      h.dispose();
    }
  });

  it('returns nothing for a row with only one side, which has nothing to compare', async () => {
    const h = await compare(withHeader('a,alpha,1,1.00'), withHeader('b,beta,2,2.00'));
    try {
      const idx = h.index();
      for (let i = 0; i < idx.rowCount; i++) {
        expect(idx.getCellSegments(i, 1, 'word').length).toBe(0);
      }
    } finally {
      h.dispose();
    }
  });
});

describe('sliceByBytes', () => {
  it('converts byte offsets to code unit offsets', () => {
    // 'café' is 5 bytes and 4 code units; the engine counts the first.
    expect(sliceByBytes('café noir', 0, 5)).toBe('café');
    expect(sliceByBytes('café noir', 6, 4)).toBe('noir');
    expect(sliceByBytes('café noir', 5, 1)).toBe(' ');
  });

  it('handles a surrogate pair, which is four bytes and two code units', () => {
    const s = 'a🙂b';
    expect(sliceByBytes(s, 0, 1)).toBe('a');
    expect(sliceByBytes(s, 1, 4)).toBe('🙂');
    expect(sliceByBytes(s, 5, 1)).toBe('b');
  });

  it('returns the whole value when the range covers it', () => {
    expect(sliceByBytes('abc', 0, 3)).toBe('abc');
    expect(sliceByBytes('abc', 0, 99)).toBe('abc');
  });
});
