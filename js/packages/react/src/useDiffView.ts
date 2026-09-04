/**
 * The React binding to the headless model. One hook, and it is deliberately thin.
 *
 * Everything hard, the scroll math, the paging, the caches, the presentation
 * modes and the keyboard arithmetic, is in `@ibhatech/csvdiff-view` and is tested
 * without a DOM. What is left here is the three things React actually owns:
 * subscribing to a store, keeping props in sync with the model, and moving a real
 * scroll container when the model says the focus went off screen.
 */

import { useCallback, useEffect, useRef, useSyncExternalStore } from 'react';
import type { KeyboardEvent, RefObject, UIEvent } from 'react';
import { DiffViewModel } from '@ibhatech/csvdiff-view';
import type {
  DiffRowSource,
  ViewModelOptions,
  ViewSnapshot,
} from '@ibhatech/csvdiff-view';

/**
 * A viewport height to lay out with before a real element has been measured.
 *
 * It matters for exactly one case, and that case is not a mistake to design for:
 * server rendering, where there is no element and never will be. Zero would
 * render one row into the document and then jump to twenty on hydration.
 */
export const SSR_VIEWPORT_HEIGHT = 560;

export interface UseDiffViewOptions extends ViewModelOptions {
  /** Height to assume until the scroll container has been measured. */
  initialViewportHeight?: number;
}

export interface DiffView {
  model: DiffViewModel;
  snapshot: ViewSnapshot;
  /** Attach to the scroll container. */
  ref: RefObject<HTMLDivElement | null>;
  onScroll: (event: UIEvent<HTMLDivElement>) => void;
  onKeyDown: (event: KeyboardEvent<HTMLDivElement>) => void;
}

export function useDiffView(source: DiffRowSource, options: UseDiffViewOptions = {}): DiffView {
  const ref = useRef<HTMLDivElement | null>(null);
  const modelRef = useRef<DiffViewModel | null>(null);

  if (modelRef.current === null) {
    const model = new DiffViewModel(source, options);
    // Before the first snapshot, so that a local handle, which answers in the
    // same tick, renders complete rows on the very first pass rather than a
    // frame of placeholders.
    model.setViewport(0, options.initialViewportHeight ?? SSR_VIEWPORT_HEIGHT);
    model.start();
    modelRef.current = model;
  }
  const model = modelRef.current;

  // The filter is applied by rebuilding the report index in the engine, per spec
  // 8.4, which means a new source rather than a new predicate. A consumer changes
  // `changesOnly`, memoizes a new source, and this notices.
  const lastSource = useRef(source);
  useEffect(() => {
    if (lastSource.current !== source) {
      lastSource.current = source;
      model.setSource(source);
    }
  }, [model, source]);

  // The props the model can change after it exists. `classPrefix`, `overscan`,
  // `maxCellBytes` and the initial column widths are read once at construction:
  // they are configuration rather than state, and a component that changed its
  // class prefix mid-life would invalidate the consumer's stylesheet anyway.
  const { layout, oldValuePosition, rowHeight, cellDiff, keyColumns } = options;
  useEffect(() => {
    if (layout) model.setLayout(layout);
    if (oldValuePosition) model.setOldValuePosition(oldValuePosition);
    if (rowHeight) model.setRowHeight(rowHeight);
    if (cellDiff) model.setCellDiff(cellDiff);
    if (keyColumns) model.setKeyColumns(keyColumns);
  }, [model, layout, oldValuePosition, rowHeight, cellDiff, keyColumns]);

  const snapshot = useSyncExternalStore(model.subscribe, model.snapshot, model.snapshot);

  // Measure once mounted, and again whenever the element resizes. A table sized
  // by its container is the normal case and a window resize is not the only way
  // that changes, so this is a ResizeObserver rather than a window listener.
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const measure = () => model.setViewport(el.scrollTop, el.clientHeight);
    measure();
    if (typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [model]);

  // Keyboard navigation moves the model's idea of the scroll position; this is
  // the one place that is pushed back into the element. Comparing first matters:
  // assigning scrollTop unconditionally would fight the user's own scrolling.
  useEffect(() => {
    const el = ref.current;
    if (el && Math.abs(el.scrollTop - snapshot.scrollTop) > 1) {
      el.scrollTop = snapshot.scrollTop;
    }
  }, [snapshot.scrollTop]);

  const onScroll = useCallback(
    (event: UIEvent<HTMLDivElement>) => {
      const el = event.currentTarget;
      model.setViewport(el.scrollTop, el.clientHeight);
    },
    [model],
  );

  const onKeyDown = useCallback(
    (event: KeyboardEvent<HTMLDivElement>) => {
      const page = model.pageRows();
      const focus = model.snapshot().focus;
      const at = focus?.row ?? model.snapshot().start;

      switch (event.key) {
        case 'ArrowDown':
          // Alt is the jump to the next change of spec 8.5's 0.2 row. It is a
          // scan rather than a lookup, so it is asynchronous even locally.
          if (event.altKey) {
            void model.findNextChange(at, 1).then((n) => {
              if (n !== null) model.setFocus(n, focus?.column ?? 0);
            });
          } else {
            model.moveFocus(1, 0);
          }
          break;
        case 'ArrowUp':
          if (event.altKey) {
            void model.findNextChange(at, -1).then((n) => {
              if (n !== null) model.setFocus(n, focus?.column ?? 0);
            });
          } else {
            model.moveFocus(-1, 0);
          }
          break;
        case 'ArrowLeft':
          model.moveFocus(0, -1);
          break;
        case 'ArrowRight':
          model.moveFocus(0, 1);
          break;
        case 'PageDown':
          model.moveFocus(page, 0);
          break;
        case 'PageUp':
          model.moveFocus(-page, 0);
          break;
        case 'Home':
          model.setFocus(0, 0);
          break;
        case 'End':
          model.setFocus(model.snapshot().rowCount - 1, 0);
          break;
        default:
          return;
      }
      event.preventDefault();
    },
    [model],
  );

  return { model, snapshot, ref, onScroll, onKeyDown };
}
