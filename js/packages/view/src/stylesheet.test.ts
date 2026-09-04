import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import { EMITTER_CLASS_SUFFIXES, VIEW_ONLY_CLASS_SUFFIXES } from './classes.ts';
import { defaultStylesheet, themeStylesheet } from './stylesheet.ts';

const styles = (name: string) =>
  readFileSync(fileURLToPath(new URL(`../styles/${name}`, import.meta.url)), 'utf8');

describe('the default stylesheet', () => {
  const css = defaultStylesheet();

  it('styles every class the emitter can write', () => {
    // A suffix the emitter writes and the sheet does not mention is a class that
    // renders unstyled in a saved report, which looks like a bug in the
    // consumer's CSS rather than a gap in ours.
    for (const suffix of EMITTER_CLASS_SUFFIXES) {
      expect(css, `no rule mentions .ibha-csvd-${suffix}`).toContain(`.ibha-csvd-${suffix}`);
    }
    for (const suffix of VIEW_ONLY_CLASS_SUFFIXES) {
      expect(css).toContain(`.ibha-csvd-${suffix}`);
    }
  });

  it('mentions no class outside the contract', () => {
    const used = new Set<string>();
    for (const m of css.matchAll(/\.ibha-csvd-([a-z-]+)/g)) used.add(m[1] as string);
    const known = new Set<string>([...EMITTER_CLASS_SUFFIXES, ...VIEW_ONLY_CLASS_SUFFIXES]);
    // `report-toolbar` is the toolbar's own element and is derived from `report`.
    known.add('report-toolbar');
    expect([...used].filter((u) => !known.has(u))).toEqual([]);
  });

  /**
   * The decision spec 8.3 exists to record. The emitter writes a cell's newline
   * through unchanged rather than inventing a `<br>`, so what a newline looks
   * like is the stylesheet's call, and without this a multiline CSV field
   * collapses onto one line and appears to differ when it does not.
   */
  it('sets white-space: pre-wrap on the cell class', () => {
    const cellRule = css.slice(css.indexOf('.ibha-csvd-cell {'));
    expect(cellRule.slice(0, cellRule.indexOf('}'))).toContain('white-space: pre-wrap');
  });

  it('gives change kinds a non colour signal for a saved report', () => {
    // Spec 8.5 calls colour-only signalling a correctness issue for colour blind
    // reviewers. The view renders a marker cell; an emitter document has no such
    // element, so it gets generated content instead, and the two are mutually
    // exclusive by selector.
    expect(css).toContain(':not([data-virtualized])');
    expect(css).toContain('content: " +"');
    expect(css).toContain('content: " -"');
  });

  it('leaves a saved report scrolling with the page and only boxes the live view', () => {
    const virtualized = css.slice(css.indexOf('.ibha-csvd-report[data-virtualized]'));
    expect(virtualized.slice(0, virtualized.indexOf('}'))).toContain('overflow: auto');
    const plain = css.slice(css.indexOf('.ibha-csvd-report {'));
    expect(plain.slice(0, plain.indexOf('}'))).not.toContain('overflow');
  });

  it('honours a custom prefix throughout, classes and custom properties alike', () => {
    const x = defaultStylesheet({ prefix: 'x-' });
    expect(x).toContain('.x-cell');
    expect(x).toContain('--x-added-bg');
    expect(x).not.toContain('ibha-csvd');
  });

  it('refuses a prefix the engine would refuse', () => {
    expect(() => defaultStylesheet({ prefix: '" onload="' })).toThrow(RangeError);
  });

  it('can be asked for structure alone, for a consumer with their own tokens', () => {
    const bare = defaultStylesheet({ variables: false });
    expect(bare).not.toContain('--ibha-csvd-added-bg: #');
    expect(bare).toContain('var(--ibha-csvd-added-bg)');
  });
});

describe('the theme files', () => {
  it('set variables and nothing else, so they compose with any structure', () => {
    for (const which of ['light', 'dark', 'high-contrast'] as const) {
      const css = themeStylesheet(which);
      expect(css).toContain(':root {');
      expect(css).not.toContain('.ibha-csvd-');
    }
  });

  it('drop the fills entirely in high contrast rather than deepening them', () => {
    expect(themeStylesheet('high-contrast')).toContain('--ibha-csvd-added-bg: transparent');
  });
});

describe('the checked in css', () => {
  /**
   * The same argument `gen_abi.mjs` makes about the struct offsets: a bundler
   * imports a file, not a function, so both exist, and only one of them is
   * checked by anything unless this is here.
   */
  it('is what the generator produces today', () => {
    expect(styles('ibha-csvdiff.css')).toBe(defaultStylesheet());
    expect(styles('theme-light.css')).toBe(themeStylesheet('light'));
    expect(styles('theme-dark.css')).toBe(themeStylesheet('dark'));
    expect(styles('theme-high-contrast.css')).toBe(themeStylesheet('high-contrast'));
  });
});
