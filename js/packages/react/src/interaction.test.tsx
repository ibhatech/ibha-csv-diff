/**
 * The two things that genuinely need a DOM: a scroll container and a keydown.
 *
 * Everything else about the view is tested in `@ibhatech/csvdiff-view` without
 * one, which is the reason that package exists. What is left here is the seam:
 * that a real scroll event reaches the model, that the model's reply reaches the
 * element, and that neither of those was wired to the wrong thing.
 */

import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { fakeRowSource, type FakeRow } from '@ibhatech/csvdiff-view/testkit';

import { CsvDiffTable } from './CsvDiffTable.tsx';
import { CsvDiffToolbar } from './CsvDiffToolbar.tsx';

declare global {
  // eslint-disable-next-line no-var
  var IS_REACT_ACT_ENVIRONMENT: boolean;
}
globalThis.IS_REACT_ACT_ENVIRONMENT = true;

const COLUMNS = ['id', 'name', 'amount'];
const VIEWPORT = 560;
const ROW_HEIGHT = 28;

function report(n: number): FakeRow[] {
  return Array.from({ length: n }, (_, i) => ({
    kind: i % 100 === 0 ? 'modified' : 'unchanged',
    sourceRow: i + 1,
    targetRow: i + 1,
    cells: [{ target: `ACC-${i}` }, { target: `n${i}` }, { target: '1.00' }],
  }));
}

let container: HTMLDivElement;
let root: Root;

/**
 * jsdom does no layout, so `clientHeight` is always 0 and `scrollTop` never
 * moves. Both are stubbed, and stubbed before the first render rather than after
 * it: a component that measures on mount would otherwise be measured against a
 * zero height viewport and render one row, which is a property of the stub and
 * not of the component.
 *
 * Only these two are stubbed, which is the honest amount: they are the only
 * geometry the component reads.
 */
const scrollTops = new WeakMap<Element, number>();

beforeEach(() => {
  Object.defineProperty(HTMLElement.prototype, 'clientHeight', {
    configurable: true,
    get: () => VIEWPORT,
  });
  Object.defineProperty(HTMLElement.prototype, 'scrollTop', {
    configurable: true,
    get(this: Element) {
      return scrollTops.get(this) ?? 0;
    },
    set(this: Element, value: number) {
      scrollTops.set(this, value);
    },
  });
  container = document.createElement('div');
  document.body.appendChild(container);
  root = createRoot(container);
});

afterEach(() => {
  act(() => root.unmount());
  container.remove();
  Reflect.deleteProperty(HTMLElement.prototype, 'clientHeight');
  Reflect.deleteProperty(HTMLElement.prototype, 'scrollTop');
});

function scroller(): HTMLDivElement {
  return container.querySelector('.ibha-csvd-report') as HTMLDivElement;
}

function scrollTo(el: HTMLElement, top: number): void {
  el.scrollTop = top;
  act(() => {
    el.dispatchEvent(new Event('scroll'));
  });
}

function renderedRows(): number[] {
  return [...container.querySelectorAll('tr[aria-rowindex]')].map(
    (tr) => Number(tr.getAttribute('aria-rowindex')) - 2,
  );
}

describe('scrolling', () => {
  it('renders a different band of the report without changing how much is in the DOM', () => {
    act(() => {
      root.render(
        <CsvDiffTable source={fakeRowSource(COLUMNS, report(90_000))} rowHeight={ROW_HEIGHT} />,
      );
    });
    const el = scroller();
    const atTop = renderedRows();
    expect(atTop[0]).toBe(0);

    scrollTo(el, ROW_HEIGHT * 50_000);
    const midway = renderedRows();
    expect(midway[0]).toBe(50_000 - 8);
    expect(midway.length).toBeLessThanOrEqual(atTop.length + 8);

    scrollTo(el, ROW_HEIGHT * 89_999);
    expect(renderedRows().at(-1)).toBe(89_999);
  });

  it('paints the values of the page it scrolled to, not the page it came from', () => {
    act(() => {
      root.render(
        <CsvDiffTable source={fakeRowSource(COLUMNS, report(5_000))} rowHeight={ROW_HEIGHT} />,
      );
    });
    scrollTo(scroller(), ROW_HEIGHT * 2_000);
    expect(container.textContent).toContain('ACC-2000');
    expect(container.textContent).not.toContain('ACC-0');
  });
});

describe('keyboard navigation', () => {
  function press(el: HTMLElement, key: string, alt = false): void {
    act(() => {
      el.dispatchEvent(new KeyboardEvent('keydown', { key, altKey: alt, bubbles: true }));
    });
  }

  it('moves the focused row and scrolls only when it has to', () => {
    act(() => {
      root.render(
        <CsvDiffTable source={fakeRowSource(COLUMNS, report(1_000))} rowHeight={ROW_HEIGHT} />,
      );
    });
    const el = scroller();
    for (let i = 0; i < 5; i++) press(el, 'ArrowDown');
    expect(el.scrollTop).toBe(0);

    for (let i = 0; i < 30; i++) press(el, 'ArrowDown');
    expect(el.scrollTop).toBeGreaterThan(0);
  });

  it('jumps to the end and back to the start', () => {
    act(() => {
      root.render(
        <CsvDiffTable source={fakeRowSource(COLUMNS, report(1_000))} rowHeight={ROW_HEIGHT} />,
      );
    });
    const el = scroller();
    press(el, 'End');
    expect(renderedRows().at(-1)).toBe(999);
    press(el, 'Home');
    expect(el.scrollTop).toBe(0);
    expect(renderedRows()[0]).toBe(0);
  });

  it('jumps to the next changed row, skipping the ninety nine unchanged ones', async () => {
    act(() => {
      root.render(
        <CsvDiffTable source={fakeRowSource(COLUMNS, report(1_000))} rowHeight={ROW_HEIGHT} />,
      );
    });
    const el = scroller();
    press(el, 'ArrowDown', true);
    // The scan is asynchronous even locally, because a source in a worker is.
    await act(async () => {
      await Promise.resolve();
    });
    expect(el.scrollTop).toBeGreaterThanOrEqual(ROW_HEIGHT * 100 - VIEWPORT);
  });
});

describe('the toolbar', () => {
  it('reports the filter change rather than owning it', () => {
    // Filtering rebuilds the report index in the engine, so the state has to live
    // where the source is built. A toolbar that owned it would have to reach into
    // the table to rebuild one.
    const seen: boolean[] = [];
    act(() => {
      root.render(
        <CsvDiffToolbar changesOnly onChangesOnlyChange={(v) => seen.push(v)} rowCount={12} />,
      );
    });
    const box = container.querySelector('input[type="checkbox"]') as HTMLInputElement;
    expect(box.checked).toBe(false);
    act(() => box.click());
    expect(seen).toEqual([false]);
    expect(container.textContent).toContain('12 rows');
  });
});

describe('the theme object', () => {
  it('becomes custom properties on the container, never inline styles on a cell', () => {
    act(() => {
      root.render(
        <CsvDiffTable
          source={fakeRowSource(COLUMNS, report(10))}
          theme={{ addedBg: '#e8f5e9', rowHeight: 32 }}
        />,
      );
    });
    const el = scroller();
    expect(el.style.getPropertyValue('--ibha-csvd-added-bg')).toBe('#e8f5e9');
    expect(el.style.getPropertyValue('--ibha-csvd-row-height')).toBe('32px');
    // Inline styles would beat the consumer's own stylesheet on specificity, so
    // no cell carries one.
    for (const td of container.querySelectorAll('td.ibha-csvd-cell')) {
      expect(td.getAttribute('style')).toBeNull();
    }
  });
});
