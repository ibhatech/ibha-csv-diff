import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { ENGINE_CONSTANTS } from '@ibhatech/csvdiff-core';

import {
  assertClassPrefix,
  cellClass,
  classNames,
  classPrefixValid,
  CELL_CHANGED,
  CELL_FINDING,
  CELL_NOT_NUMERIC,
  CELL_PRECISION,
  CELL_REQUIRED_EMPTY,
  CELL_SUPPRESSED,
  CELL_TOO_LONG,
  DEFAULT_CLASS_PREFIX,
  EMITTER_CLASS_SUFFIXES,
  findingName,
  rowClass,
} from './classes.ts';

const emitC = fileURLToPath(new URL('../../../../core/src/emit.c', import.meta.url));

describe('the class contract against the emitter', () => {
  /**
   * The one test in this file that could actually fail one day.
   *
   * The class names are a public contract shared by a C file and a TypeScript
   * file that cannot see each other, and the failure mode is silent: a suffix
   * added to the emitter and not here is a class the shipped stylesheet never
   * styles, which looks like a CSS bug in a consumer's application. So this reads
   * the emitter's own source and compares the sets.
   */
  it('knows exactly the suffixes core/src/emit.c writes', () => {
    const source = readFileSync(emitC, 'utf8');
    const written = new Set<string>();
    for (const m of source.matchAll(/wr_cls\(e,\s*"([a-z]+)"/g)) written.add(m[1] as string);
    // The four kind names reach wr_cls through k_kind[] rather than as literals.
    for (const m of source.matchAll(/k_kind\[4\]\s*=\s*\{([^}]*)\}/g)) {
      for (const q of (m[1] as string).matchAll(/"([a-z]+)"/g)) written.add(q[1] as string);
    }

    expect([...written].sort()).toEqual([...EMITTER_CLASS_SUFFIXES].sort());
  });

  it('validates the prefix against the same pattern the engine does', () => {
    const source = readFileSync(emitC, 'utf8');
    expect(source).toContain('[A-Za-z][A-Za-z0-9_-]{0,31}');
  });

  it('states the finding names the emitter writes into data-finding', () => {
    const source = readFileSync(emitC, 'utf8');
    for (const name of ['requiredEmpty', 'tooLong', 'notNumeric', 'precision']) {
      expect(source).toContain(`"${name}"`);
    }
  });
});

describe('classNames', () => {
  it('uses the ibha-csvd- prefix by default, per spec 13.0', () => {
    const c = classNames();
    expect(DEFAULT_CLASS_PREFIX).toBe('ibha-csvd-');
    expect(c.table).toBe('ibha-csvd-table');
    expect(c.cell).toBe('ibha-csvd-cell');
    expect(c.modified).toBe('ibha-csvd-modified');
  });

  it('honours a custom prefix for a host page collision', () => {
    expect(classNames('x-').row).toBe('x-row');
  });

  it('refuses a prefix the engine would refuse, rather than passing it through', () => {
    // Handoff 4.5: data errors belong to the engine, caller mistakes belong to
    // the binding. A prefix carrying a quote is a caller mistake, and it is the
    // one caller supplied string that reaches markup.
    expect(() => classNames('x" onload="')).toThrow(RangeError);
    expect(() => classNames('9lives')).toThrow(RangeError);
    expect(() => classNames('')).toThrow(RangeError);
    expect(() => classNames('a'.repeat(33))).toThrow(RangeError);
    expect(classPrefixValid('a'.repeat(32))).toBe(true);
    expect(assertClassPrefix('csvd-')).toBe('csvd-');
  });
});

describe('composed class lists', () => {
  const c = classNames();

  it('writes the row list in the emitter’s order', () => {
    expect(rowClass(c, 'unchanged', false, false)).toBe('ibha-csvd-row ibha-csvd-unchanged');
    expect(rowClass(c, 'modified', true, true)).toBe(
      'ibha-csvd-row ibha-csvd-modified ibha-csvd-moved ibha-csvd-finding',
    );
  });

  it('writes the cell list from the flag byte', () => {
    expect(cellClass(c, 0)).toBe('ibha-csvd-cell');
    expect(cellClass(c, CELL_CHANGED)).toBe('ibha-csvd-cell ibha-csvd-changed');
    expect(cellClass(c, CELL_SUPPRESSED | CELL_TOO_LONG)).toBe(
      'ibha-csvd-cell ibha-csvd-suppressed ibha-csvd-finding',
    );
  });
});

describe('findingName', () => {
  it('picks the same one the emitter picks when a cell fails two rules', () => {
    expect(findingName(CELL_TOO_LONG | CELL_NOT_NUMERIC)).toBe('tooLong');
    expect(findingName(CELL_REQUIRED_EMPTY | CELL_PRECISION)).toBe('requiredEmpty');
    expect(findingName(CELL_PRECISION)).toBe('precision');
  });

  it('is null on a cell with no finding', () => {
    expect(findingName(CELL_CHANGED | CELL_SUPPRESSED)).toBeNull();
    expect(CELL_FINDING).toBe(
      CELL_REQUIRED_EMPTY | CELL_TOO_LONG | CELL_NOT_NUMERIC | CELL_PRECISION,
    );
  });

  /**
   * The flag bits are restated in this package rather than imported, so that the
   * headless layer carries no runtime dependency on the engine for six integers.
   * Restating without checking is how two copies drift, and the drifted version
   * of this one styles the wrong cells rather than failing.
   */
  it('agrees with the engine’s own constants', () => {
    expect({
      changed: CELL_CHANGED,
      suppressed: CELL_SUPPRESSED,
      requiredEmpty: CELL_REQUIRED_EMPTY,
      tooLong: CELL_TOO_LONG,
      notNumeric: CELL_NOT_NUMERIC,
      precision: CELL_PRECISION,
      finding: CELL_FINDING,
    }).toEqual({
      changed: ENGINE_CONSTANTS.CELL_CHANGED,
      suppressed: ENGINE_CONSTANTS.CELL_SUPPRESSED,
      requiredEmpty: ENGINE_CONSTANTS.CELL_REQUIRED_EMPTY,
      tooLong: ENGINE_CONSTANTS.CELL_TOO_LONG,
      notNumeric: ENGINE_CONSTANTS.CELL_NOT_NUMERIC,
      precision: ENGINE_CONSTANTS.CELL_PRECISION,
      finding: ENGINE_CONSTANTS.CELL_FINDING,
    });
  });
});
