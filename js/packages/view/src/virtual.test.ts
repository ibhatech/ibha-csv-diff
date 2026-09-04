import { describe, expect, it } from 'vitest';

import { computeWindow, pagesFor, scrollToShow, slotsPerRow } from './virtual.ts';

describe('computeWindow', () => {
  const base = { rowHeight: 28, viewportHeight: 560, rowCount: 90_000, overscan: 0 };

  it('renders only the visible band, not the whole report', () => {
    const w = computeWindow({ ...base, scrollTop: 0 });
    expect(w.start).toBe(0);
    // 560 / 28 = 20 visible, plus one partial row.
    expect(w.end).toBe(21);
    expect(w.totalHeight).toBe(90_000 * 28);
  });

  it('tracks the scroll position', () => {
    const w = computeWindow({ ...base, scrollTop: 28 * 1000 });
    expect(w.start).toBe(1000);
    expect(w.offsetTop).toBe(28 * 1000);
  });

  it('applies overscan without going out of bounds at the top', () => {
    expect(computeWindow({ ...base, scrollTop: 0, overscan: 8 }).start).toBe(0);
  });

  it('clamps negative scrollTop from rubber band overscroll', () => {
    const w = computeWindow({ ...base, scrollTop: -400 });
    expect(w.start).toBe(0);
    expect(w.offsetTop).toBe(0);
  });

  it('clamps a scrollTop past the end, which happens when a filter shrinks the report', () => {
    // Scrolled to the bottom of 90,000 rows, then "changes only" is ticked and the
    // count drops to 40. Without the clamp the window would be empty and the table
    // would render blank.
    const w = computeWindow({ ...base, rowCount: 40, scrollTop: 28 * 89_000 });
    expect(w.end).toBe(40);
    expect(w.start).toBeLessThan(40);
    expect(w.end).toBeGreaterThan(w.start);
  });

  it('never returns an end past the row count', () => {
    expect(computeWindow({ ...base, rowCount: 5, scrollTop: 0, overscan: 50 }).end).toBe(5);
  });

  it('handles an empty report', () => {
    expect(computeWindow({ ...base, rowCount: 0, scrollTop: 0 })).toEqual({
      start: 0,
      end: 0,
      offsetTop: 0,
      totalHeight: 0,
    });
  });

  it('rejects a non-positive row height rather than dividing by zero', () => {
    expect(() => computeWindow({ ...base, rowHeight: 0, scrollTop: 0 })).toThrow(RangeError);
  });

  it('keeps the rendered band a constant size however far down the report it is', () => {
    const top = computeWindow({ ...base, scrollTop: 0, overscan: 8 });
    const bottom = computeWindow({ ...base, scrollTop: 28 * 89_000, overscan: 8 });
    expect(bottom.end - bottom.start).toBeLessThanOrEqual(top.end - top.start + 8);
    expect(bottom.end - bottom.start).toBeLessThan(50);
  });
});

describe('slotsPerRow', () => {
  it('gives two physical rows to the layouts that stack a pair', () => {
    expect(slotsPerRow('inline')).toBe(1);
    expect(slotsPerRow('sideBySide')).toBe(1);
    expect(slotsPerRow('stacked')).toBe(2);
    expect(slotsPerRow('unified')).toBe(2);
  });
});

describe('pagesFor', () => {
  it('covers the window and nothing else', () => {
    expect(pagesFor(0, 21, 50)).toEqual([0]);
    expect(pagesFor(40, 60, 50)).toEqual([0, 1]);
    expect(pagesFor(100, 150, 50)).toEqual([2]);
    expect(pagesFor(0, 0, 50)).toEqual([]);
  });

  it('asks for one more page only when the window actually crosses a boundary', () => {
    expect(pagesFor(49, 50, 50)).toEqual([0]);
    expect(pagesFor(50, 51, 50)).toEqual([1]);
  });
});

describe('scrollToShow', () => {
  it('does not move when the row is already visible', () => {
    expect(scrollToShow(5, 0, 560, 28)).toBe(0);
  });

  it('moves the minimum needed, rather than recentring on every arrow key', () => {
    // Row 20 is the first one fully below a 560 px viewport at scrollTop 0.
    expect(scrollToShow(20, 0, 560, 28)).toBe(21 * 28 - 560);
    expect(scrollToShow(3, 28 * 10, 560, 28)).toBe(3 * 28);
  });
});
