# @ibhatech/csvdiff-react

```
npm install @ibhatech/csvdiff-react
```

React components over `@ibhatech/csvdiff-view`, and thin on purpose. Everything
hard, the scroll math, the paging, the caches, the presentation modes and the
keyboard arithmetic, is a layer down and is tested without a DOM. What is here is
the three things React actually owns: subscribing to a store, keeping props in
sync with the model, and moving a real scroll container.

```tsx
import { useMemo, useState } from 'react';
import { compareInWorker } from '@ibhatech/csvdiff-core';
import { CsvDiffTable, CsvDiffToolbar, remoteRowSource } from '@ibhatech/csvdiff-react';
import '@ibhatech/csvdiff-view/styles.css';

function Review({ handle }) {
  const [changesOnly, setChangesOnly] = useState(true);
  // Memoized: a new source rebuilds the report index in the engine.
  const source = useMemo(() => remoteRowSource(handle, { changesOnly }), [handle, changesOnly]);

  return (
    <>
      <CsvDiffToolbar changesOnly={changesOnly} onChangesOnlyChange={setChangesOnly} />
      <CsvDiffTable source={source} height={600} layout="inline" />
    </>
  );
}
```

`compareInWorker` is the default for anything interactive: a 400 ms diff on the
main thread is a 400 ms frozen UI, and at the 150 MB ceiling it is seconds. Use
`localRowSource(handle)` for a handle in this thread, in Node or in a test.

## Props

| Prop | Default | What it does |
|---|---|---|
| `source` | required | `localRowSource(handle)` or `remoteRowSource(handle)`. Memoize it |
| `height` | none | The scroll container's height. The one dimension a virtualized list cannot infer |
| `layout` | `inline` | `inline`, `stacked`, `unified`, `sideBySide` |
| `oldValuePosition` | `below` | `above`, `below`, `tooltip`. Visual only; the DOM order is always old then new |
| `rowHeight` | 28 | One physical row. `stacked` and `unified` give a report row two |
| `cellDiff` | `none` | Intra cell highlighting. A call into the engine per changed cell, so it is opt in |
| `theme` | none | Becomes CSS custom properties on the container |
| `classPrefix` | `ibha-csvd-` | Validated as the engine validates it |
| `keyColumns` | none | Column indices to keep stuck to the left while scrolling wide files |
| `classNames` | none | `{ report, row, cell }`, each a string or a function of the row or cell |

## Three decisions the implementation must not drift from

**No shadow DOM.** Its entire purpose is to stop the host page's stylesheet
reaching inside, and the requirement is that the consumer supplies the
stylesheet. Those are in direct conflict, so this renders into light DOM with
stable class names and the consumer's CSS reaches everything.

**`theme` emits custom properties, never inline styles.** Inline styles beat the
consumer's own CSS on specificity, which makes a component harder to theme rather
than easier, and a JS object cannot express `:hover`, dark mode, print or
container queries. There is a test asserting no cell carries a `style` attribute.

**Nothing is styled by default.** A component with no stylesheet imported renders
unstyled semantic markup, which is the right default for a library. Import
`@ibhatech/csvdiff-view/styles.css` to get the shipped one, which also styles the
HTML emitter's saved reports.

## Escaping

Every value reaches the DOM as a JSX child or a JSX attribute value, both of
which React escapes. Nothing builds a markup string and nothing uses
`dangerouslySetInnerHTML`, which is the property that makes the guarantee
checkable rather than asserted: `CsvDiffTable.test.tsx` renders the XSS corpus
through the real component in all four layouts, with untrusted column names as
well as untrusted cells, and runs an independently written safety checker over
the result.

## Accessibility

A real `<table>` with `role="grid"`, `aria-rowcount` over the whole report rather
than the rendered band, and `aria-rowindex` on every row. A changed cell carries
`aria-label="premium amount, changed from 1200.00 to 1350.00"`, so what colour
conveys is also available non visually, and every change kind carries a text
marker, which spec 8.5 calls a correctness issue for colour blind reviewers
rather than a nicety.

Keyboard: arrow keys move the focused cell, page keys move a viewport at a time,
Home and End go to the ends, and Alt with an arrow jumps to the next or previous
changed row. That last one scans the compact form a page at a time and retains
nothing.

## Running the tests

```
pnpm test
```

28 assertions. Most render through `react-dom/server`, which needs no DOM and
produces the markup string the safety checker wants; `interaction.test.tsx` uses
jsdom for the two things that genuinely need one, a scroll event and a keydown.

## Licence

Apache-2.0. See `LICENSE` and `NOTICE` in this package.

The full guide is [docs/usage-javascript.md](https://github.com/ibhatech/ibha-csv-diff/blob/main/docs/usage-javascript.md).
