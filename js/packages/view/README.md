# @ibhatech/csvdiff-view

```
npm install @ibhatech/csvdiff-view
```

The headless view model, and the styling contract both it and the HTML emitter
answer to. No DOM, no React, no framework: scroll math, paging, presentation
modes, filtering, column widths and keyboard arithmetic, all unit testable
without a browser.

This is the layer people usually skip and then regret. It is what lets a Vue or
Svelte binding later cost a rendering layer rather than a reimplementation, and
it is what makes 83 assertions about a virtualized table run in three seconds.

```ts
import { compareInWorker } from '@ibhatech/csvdiff-core';
import { DiffViewModel, remoteRowSource } from '@ibhatech/csvdiff-view';

const handle = await compareInWorker(() => fetch('/api/source.csv'), file);
const model = new DiffViewModel(remoteRowSource(handle, { changesOnly: true }), {
  layout: 'inline',
  rowHeight: 28,
});
model.start();
model.setViewport(scrollTop, 600);
for (const row of model.snapshot().rows) paint(row);
```

## What it retains, which is the whole design

The view never materializes the diff. It asks for the fifty rows it is about to
paint and lets them be collected. Two caches, sized very differently:

| Cache | Size | Why |
|---|---|---|
| Decoded value pages | 4 pages, 200 rows | A row object and a cell object per cell. Bounded, so scrolling to the bottom of a 90,000 row report costs the same as scrolling to row 200 |
| Compact structure pages | 64 pages | One byte per cell and no strings, so 38 KB at twelve columns. A page whose values were evicted still knows its kinds and row numbers and repaints in the right colours the instant it scrolls back |

That second row is why `getRowsCompact` is the form to bind to rather than a
nicety about typed arrays. `model.retainedPages` reports both, and there is a
test that scrolls a 90,000 row report end to end and asserts they do not grow.

## The styling contract

**The class set is the HTML emitter's**, not a parallel one, because spec 13.3
makes a saved report and the live view the same cursor and one stylesheet has to
work on both. All eighteen carry the prefix, which defaults to `ibha-csvd-` and
is validated against `[A-Za-z][A-Za-z0-9_-]{0,31}` exactly as the engine
validates it:

```
report table th num row cell
unchanged modified added deleted moved
changed suppressed finding old new del ins
```

Plus `data-schema-version` on the container and `data-finding` on a cell carrying
one. Three more, `mark`, `spacer` and `pane`, are elements the emitter has no
equivalent of; everything else the view needs to express is a `data-` attribute,
so a rule written against the emitter's output is never silently dead on half of
it.

Three ways to style, in the order people actually use them:

```ts
import '@ibhatech/csvdiff-view/styles.css';          // the default sheet
import '@ibhatech/csvdiff-view/styles/theme-dark.css'; // variables only
```

```ts
defaultStylesheet({ prefix: 'x-' });   // the same sheet for a custom prefix
themeToCssVars({ addedBg: '#e8f5e9' }); // the theme object, as custom properties
```

The shipped `styles/*.css` are generated from `src/stylesheet.ts` by
`scripts-and-commands/gen_stylesheet.mjs`, and the gate fails when they are
stale. Two copies of a thing that must agree, where neither is a compile error
when it drifts, is how you ship a stylesheet that no longer matches the classes.

Two decisions the sheet records rather than leaves implicit:

- **`white-space: pre-wrap` on the cell class.** The emitter writes a cell's
  newline through unchanged and does not invent a `<br>`, so what a newline looks
  like is the stylesheet's call. Without this a multiline CSV field collapses to
  one line and appears to differ when it does not.
- **Colour is never the only signal.** Spec 8.5 calls this a correctness issue
  for colour blind reviewers. The view renders a marker cell; an emitter document
  has no such element, so it gets the same glyph from generated content, and the
  two are mutually exclusive by selector.

## Row presentation modes

All four of spec 8.4 come out of the same row model, and `presentRow` returns the
physical rows that render one report row:

| Layout | Physical rows | Notes |
|---|---|---|
| `inline` | 1 | New value with the old beside it. `oldValuePosition` places it |
| `stacked` | 2 | New on top, old below, unchanged cells in the lower row blank |
| `unified` | 2 | Git style: the deletion then the addition |
| `sideBySide` | 2 | Source pane and target pane, with gap fillers, in one scroll container |

The row height is fixed, which is version 0.1 in spec 8.5, so the two-row layouts
give every report row two slots rather than only the changed ones. Expanding only
changed rows needs the measured offset table of 0.3: the scrollbar would have to
know how many rows above the viewport were changed, which is a prefix sum over
the whole report. In practice it costs nothing, because those layouts are read
with `changesOnly` on, where every row is changed anyway.

## The rules a renderer over this must not undo

- **A matched row's cell carries `source` exactly when it differs in bytes from
  the target.** Absence means byte identical, never empty. `sourceText` reads it
  as `cell.source ?? cell.target` for that reason.
- **`row.cells.length` is not the source file's column count.** Under the column
  policy of spec 6.6 a report row carries the columns the two files share.
- **Segment offsets are byte offsets**, not UTF-16 code units. `cellPieces` goes
  through `sliceByBytes` from the engine package; there is one implementation of
  that conversion in this project and this is not it.

## Testing your own renderer

```ts
import { fakeRowSource, htmlSafetyViolation, XSS_VALUES } from '@ibhatech/csvdiff-view/testkit';
```

`fakeRowSource` builds a source over rows written by hand, with no engine behind
it, and reproduces the parts of the row contract that are easy to get wrong.
`htmlSafetyViolation` is the escaping guarantee stated positively: every `<` in
the output opens a known tag, every attribute name is structural or `data-` or
`aria-`, and every `&` opens a character reference. A test that greps for
`<script>` passes on `<img src=x onerror=alert(1)>`; this one cannot.

## Running the tests

```
pnpm test
```

Seven suites, 83 assertions, including `parity.test.ts`, which drives the real
engine over the real fixtures and compares the HTML emitter's saved report
against the view's own layout of the same rows, cell for cell, at the p90.

## Licence

Apache-2.0. See `LICENSE` and `NOTICE` in this package.

The full guide is [docs/usage-javascript.md](https://github.com/ibhatech/ibha-csv-diff/blob/main/docs/usage-javascript.md).
