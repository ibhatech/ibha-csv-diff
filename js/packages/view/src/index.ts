/**
 * @ibhatech/csvdiff-view
 *
 * Headless view model. No DOM, no React, no framework. This layer exists so that
 * scroll math, paging, filtering and column sizing are unit testable without a
 * browser, and so a Vue or Svelte binding later costs a rendering layer rather
 * than a reimplementation. Spec section 8.1.
 *
 * What it holds to, and what a rendering layer over it must not undo:
 *
 *   - **It never materializes the diff.** It asks for the fifty rows it is about
 *     to paint and lets them be collected. `DiffViewModel` says what it retains
 *     and why, and `retainedPages` is there so a test can assert it.
 *   - **The class set is the HTML emitter's.** One stylesheet styles a saved
 *     report and the live view, because spec 13.3 makes them the same cursor.
 *     View-only state is a `data-` attribute, not a new class.
 *   - **Byte offsets are byte offsets.** Intra cell segments index the logical
 *     UTF-8 value, and `cellPieces` goes through the engine's `sliceByBytes`
 *     rather than a second conversion.
 */

export {
  assertClassPrefix,
  cellClass,
  classNames,
  classPrefixValid,
  CLASS_PREFIX_PATTERN,
  DEFAULT_CLASS_PREFIX,
  EMITTER_CLASS_SUFFIXES,
  VIEW_ONLY_CLASS_SUFFIXES,
  findingName,
  rowClass,
  ROW_KINDS,
  CELL_CHANGED,
  CELL_SUPPRESSED,
  CELL_REQUIRED_EMPTY,
  CELL_TOO_LONG,
  CELL_NOT_NUMERIC,
  CELL_PRECISION,
  CELL_FINDING,
  type ClassNames,
  type ClassSuffix,
  type FindingName,
  type RowKind,
} from './classes.ts';

export {
  themeToCssVars,
  type IbhaCsvDiffTheme,
} from './theme.ts';

export {
  defaultStylesheet,
  themeStylesheet,
  DARK_VARIABLES,
  HIGH_CONTRAST_VARIABLES,
  LIGHT_VARIABLES,
  type StylesheetOptions,
} from './stylesheet.ts';

export {
  computeWindow,
  pagesFor,
  scrollToShow,
  slotsPerRow,
  ROW_LAYOUTS,
  type RowLayout,
  type WindowInput,
  type WindowResult,
} from './virtual.ts';

export {
  cellPieces,
  presentRow,
  structureFromCompact,
  type CellPiece,
  type OldValuePosition,
  type PresentOptions,
  type RowStructure,
  type ViewCell,
  type ViewRow,
} from './rowModel.ts';

export {
  isPromise,
  localRowSource,
  remoteRowSource,
  type DiffIndexLike,
  type DiffRowSource,
  type DiffRowSourceInfo,
  type LocalDiffHandleLike,
  type MaybePromise,
  type RemoteDiffHandleLike,
} from './source.ts';

export {
  DiffViewModel,
  PAGE_SIZE,
  type ViewModelOptions,
  type ViewSnapshot,
} from './viewModel.ts';
