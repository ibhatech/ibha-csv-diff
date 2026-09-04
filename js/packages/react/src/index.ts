/**
 * @ibhatech/csvdiff-react
 *
 * React components over `@ibhatech/csvdiff-view`, and thin on purpose. Spec 8.2
 * makes React the primary binding; everything that is not React specific lives a
 * layer down, so that a Vue or Svelte binding later costs a rendering layer
 * rather than a reimplementation of scroll math, paging and presentation modes.
 *
 * Nothing here is styled. Per spec 8.3 a component with no stylesheet imported
 * renders unstyled semantic markup, which is the right default for a library:
 * import `@ibhatech/csvdiff-view/styles.css`, or write your own against the class
 * names, or set the CSS custom properties, or pass `theme`.
 *
 * ```tsx
 * const handle = await compareInWorker(() => fetch('/api/source.csv'), file);
 * const source = useMemo(() => remoteRowSource(handle, { changesOnly }), [handle, changesOnly]);
 * return <CsvDiffTable source={source} height={600} />;
 * ```
 */

export { CsvDiffTable, type CsvDiffClassNames, type CsvDiffTableProps } from './CsvDiffTable.tsx';
export { CsvDiffToolbar, type CsvDiffToolbarProps } from './CsvDiffToolbar.tsx';
export {
  useDiffView,
  SSR_VIEWPORT_HEIGHT,
  type DiffView,
  type UseDiffViewOptions,
} from './useDiffView.ts';

/**
 * Re-exported so a consumer of the components does not also have to depend on
 * the headless package for the things every call site needs: the two source
 * adapters, the class table, and the theme contract.
 */
export {
  classNames,
  localRowSource,
  remoteRowSource,
  themeToCssVars,
  DEFAULT_CLASS_PREFIX,
  ROW_LAYOUTS,
  type ClassNames,
  type DiffRowSource,
  type IbhaCsvDiffTheme,
  type OldValuePosition,
  type RowLayout,
  type ViewCell,
  type ViewRow,
  type ViewSnapshot,
} from '@ibhatech/csvdiff-view';
