/**
 * Scroll math. No DOM, so it is unit testable without a browser, which is the
 * whole reason the headless layer of spec 8.1 exists.
 *
 * Virtualization is version 0.1 in spec 8.5 rather than an enhancement, because a
 * 90,000 row diff at 28 px a row is a 2.5 million pixel tall table and putting
 * that many rows in the DOM is what makes diff viewers unusable at the size they
 * are most needed.
 */

export interface WindowInput {
  /** Pixels scrolled from the top of the scroll container. */
  scrollTop: number;
  /** Visible height of the scroll container, in pixels. */
  viewportHeight: number;
  /** Height of one report row's slot. In a layout that renders two physical rows
   *  per report row this is twice the physical row height; see `slotsPerRow`. */
  rowHeight: number;
  /** Total rows in the report after filtering. */
  rowCount: number;
  /** Rows rendered above and below the visible band, to avoid blank gaps during
   *  fast scrolling. */
  overscan?: number;
}

export interface WindowResult {
  /** First row index to render, inclusive. */
  start: number;
  /** One past the last row index to render. */
  end: number;
  /** Pixel offset to translate the rendered band by. */
  offsetTop: number;
  /** Full scrollable height, so the scrollbar reflects the whole report. */
  totalHeight: number;
}

/**
 * Computes which rows a virtualized table should render.
 */
export function computeWindow(input: WindowInput): WindowResult {
  const { scrollTop, viewportHeight, rowHeight, rowCount } = input;
  const overscan = input.overscan ?? 8;

  if (rowHeight <= 0) {
    throw new RangeError('rowHeight must be positive');
  }
  if (rowCount <= 0) {
    return { start: 0, end: 0, offsetTop: 0, totalHeight: 0 };
  }

  const totalHeight = rowCount * rowHeight;

  // Clamp the scroll position. A negative scrollTop happens during rubber band
  // overscroll on macOS, and an over-large one happens when the row count shrinks
  // because a filter was applied while scrolled to the bottom. Both would
  // otherwise produce a negative start index or an empty window.
  const clampedTop = Math.min(Math.max(scrollTop, 0), Math.max(totalHeight - viewportHeight, 0));

  const firstVisible = Math.floor(clampedTop / rowHeight);
  const visibleCount = Math.ceil(viewportHeight / rowHeight) + 1;

  const start = Math.max(0, firstVisible - overscan);
  const end = Math.min(rowCount, firstVisible + visibleCount + overscan);

  return { start, end, offsetTop: start * rowHeight, totalHeight };
}

/**
 * How many physical `<tr>` elements one report row occupies in each layout.
 *
 * It is deliberately uniform, so that an unchanged row in `stacked` occupies two
 * slots with the lower one blank rather than one. Fixed row height is what spec
 * 8.5 puts in 0.1 and the measured offset table it needs to be otherwise is 0.3,
 * the entry the spec itself calls the hard one. Making only changed rows expand
 * would quietly require that table now: the scrollbar would have to know how many
 * of the rows above the viewport were changed, which is a prefix sum over the
 * whole report, which is exactly the structure 0.3 is for.
 *
 * In practice the cost is nothing, because `stacked` and `unified` are read with
 * `changesOnly` on, where every row is changed and the layout is uniform anyway.
 */
export function slotsPerRow(layout: RowLayout): 1 | 2 {
  return layout === 'stacked' || layout === 'unified' ? 2 : 1;
}

/** How a changed row is laid out. All four come from the same row model, per
 *  spec 8.4. */
export type RowLayout = 'inline' | 'stacked' | 'sideBySide' | 'unified';

export const ROW_LAYOUTS: readonly RowLayout[] = ['inline', 'stacked', 'sideBySide', 'unified'];

/**
 * The page indices a window spans, given a page size.
 *
 * The view fetches whole pages rather than exactly the visible band so that a one
 * row scroll does not re-ask the engine for 49 rows it already has.
 */
export function pagesFor(start: number, end: number, pageSize: number): number[] {
  if (end <= start) return [];
  const first = Math.floor(start / pageSize);
  const last = Math.floor((end - 1) / pageSize);
  const out: number[] = [];
  for (let p = first; p <= last; p++) out.push(p);
  return out;
}

/** The scrollTop that brings a report row fully into view, moving as little as
 *  possible. Used by keyboard navigation, which must not recentre the viewport on
 *  every arrow key. */
export function scrollToShow(
  index: number,
  scrollTop: number,
  viewportHeight: number,
  rowHeight: number,
): number {
  const top = index * rowHeight;
  const bottom = top + rowHeight;
  if (top < scrollTop) return top;
  if (bottom > scrollTop + viewportHeight) return bottom - viewportHeight;
  return scrollTop;
}
