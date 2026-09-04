/**
 * The CSS custom property contract of spec 8.3, and the `theme` object over it.
 *
 * This lives in the headless package rather than in the React one because the
 * property names are part of the styling contract that the shipped stylesheet,
 * the React component and any future element or Vue wrapper all have to agree
 * on. `@ibhatech/csvdiff-react` re-exports it, so the Phase 0 import path still
 * works.
 *
 * The design note from spec 8.3, restated because it is the part most likely to
 * be undone by someone trying to be helpful: **the theme object emits custom
 * properties, never inline styles.** Inline styles beat the consumer's own
 * stylesheet on specificity, which makes the component harder to theme rather
 * than easier, and a JS object fundamentally cannot express `:hover`,
 * `prefers-color-scheme`, print rules or container queries. Custom property
 * declarations are the one thing that is correct to put in a `style` attribute,
 * because they do not participate in that contest at all.
 */

import { assertClassPrefix, DEFAULT_CLASS_PREFIX } from './classes.ts';

/**
 * Every property admits `undefined` explicitly rather than being merely
 * optional. Under `exactOptionalPropertyTypes` those differ, and the realistic
 * consumer pattern is `theme={{ addedBg: maybeColor }}` where the value is
 * `string | undefined`. Making them build the object conditionally would be a
 * poor trade for no safety gained: undefined values are skipped at runtime.
 */
export interface IbhaCsvDiffTheme {
  fontFamily?: string | undefined;
  fontSize?: number | string | undefined;
  fontMono?: string | undefined;
  rowHeight?: number | string | undefined;
  cellPaddingX?: number | string | undefined;
  borderColor?: string | undefined;
  addedBg?: string | undefined;
  addedFg?: string | undefined;
  deletedBg?: string | undefined;
  deletedFg?: string | undefined;
  modifiedBg?: string | undefined;
  modifiedFg?: string | undefined;
  movedBg?: string | undefined;
  findingBg?: string | undefined;
  findingFg?: string | undefined;
  suppressedFg?: string | undefined;
  oldValueFg?: string | undefined;
  oldValueDecoration?: string | undefined;
  segAddedBg?: string | undefined;
  segRemovedBg?: string | undefined;
  focusOutline?: string | undefined;
}

const CSS_VAR_NAMES: Record<keyof IbhaCsvDiffTheme, string> = {
  fontFamily: 'font-family',
  fontSize: 'font-size',
  fontMono: 'font-mono',
  rowHeight: 'row-height',
  cellPaddingX: 'cell-padding-x',
  borderColor: 'border-color',
  addedBg: 'added-bg',
  addedFg: 'added-fg',
  deletedBg: 'deleted-bg',
  deletedFg: 'deleted-fg',
  modifiedBg: 'modified-bg',
  modifiedFg: 'modified-fg',
  movedBg: 'moved-bg',
  findingBg: 'finding-bg',
  findingFg: 'finding-fg',
  suppressedFg: 'suppressed-fg',
  oldValueFg: 'old-value-fg',
  oldValueDecoration: 'old-value-decoration',
  segAddedBg: 'seg-added-bg',
  segRemovedBg: 'seg-removed-bg',
  focusOutline: 'focus-outline',
};

/** Numeric values for these keys are pixel lengths; a bare number is invalid CSS
 *  for them, so it is suffixed. Unitless numbers elsewhere are left alone. */
const PX_KEYS = new Set<keyof IbhaCsvDiffTheme>(['fontSize', 'rowHeight', 'cellPaddingX']);

/**
 * Converts a theme object into custom properties for the container element.
 *
 * Returned as a plain record so a caller can spread it into `style`.
 */
export function themeToCssVars(
  theme: IbhaCsvDiffTheme | undefined,
  prefix: string = DEFAULT_CLASS_PREFIX,
): Record<string, string> {
  if (!theme) return {};
  assertClassPrefix(prefix);
  const out: Record<string, string> = {};
  const varPrefix = `--${prefix.replace(/-$/, '')}`;

  for (const [key, value] of Object.entries(theme)) {
    if (value === undefined || value === null) continue;
    const name = CSS_VAR_NAMES[key as keyof IbhaCsvDiffTheme];
    if (!name) continue;
    const needsPx = typeof value === 'number' && PX_KEYS.has(key as keyof IbhaCsvDiffTheme);
    out[`${varPrefix}-${name}`] = needsPx ? `${value}px` : String(value);
  }
  return out;
}
