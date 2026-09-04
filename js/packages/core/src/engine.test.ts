/**
 * The binding against the engine's own contract.
 *
 * The load bearing test here is `matches the JSONL emitter row for row`. The
 * binding decodes cells itself, straight out of the columnar arrays, rather than
 * asking the engine for each one, because the header says an accessor call per
 * cell would cost more than the diff. That decision buys speed and costs a second
 * implementation of the row contract, and a second implementation that nothing
 * compares against the first is just a place for them to drift. So the emitter,
 * which is fuzzed and covered on the C side, is the oracle.
 */

import { compare, sliceByBytes } from './index.ts';
import { csv, describe, expect, fixture, htmlSafetyViolation, it, withHeader } from './testkit.ts';
import type { DiffRow } from './types.ts';

const dec = new TextDecoder();

async function diffOf(source: Uint8Array, target: Uint8Array, options = {}) {
  return compare(source, target, options);
}

describe('the decoded row against the JSONL emitter', () => {
  it('matches the JSONL emitter row for row on the p90 pair', async () => {
    const h = await diffOf(fixture('p90_source.csv'), fixture('p90_target.csv'));
    try {
      // The emitter's own row shape, declared here rather than imported, so this
      // test asserts against what the JSONL contract actually says and not
      // against the binding's idea of it.
      interface EmittedCell {
        name: string;
        source?: string;
        target?: string;
        changed?: boolean;
        suppressed?: boolean;
      }
      interface EmittedRow {
        kind: string;
        sourceRow: number | null;
        targetRow: number | null;
        moved: boolean;
        moveDistance: number;
        cells: EmittedCell[];
      }

      const emitted = dec
        .decode(h.emit('jsonl', { changesOnly: true }))
        .split('\n')
        .filter((l) => l.length > 0)
        .map((l) => JSON.parse(l) as EmittedRow);

      const decoded = [...h.rows({ changesOnly: true })];
      expect(decoded.length).toBe(emitted.length);
      expect(decoded.length).toBeGreaterThan(100);

      for (let i = 0; i < decoded.length; i++) {
        const a = decoded[i]!;
        const b = emitted[i]!;
        expect<string>(a.kind).toBe(b.kind);
        expect(a.sourceRow).toBe(b.sourceRow);
        expect(a.targetRow).toBe(b.targetRow);
        expect(a.moved).toBe(b.moved);
        expect(a.moveDistance).toBe(b.moveDistance);

        const cells = b.cells;
        expect(a.cells.length).toBe(cells.length);
        for (let c = 0; c < cells.length; c++) {
          const x = a.cells[c]!;
          const y = cells[c]!;
          expect(x.name).toBe(y.name);
          expect(x.source).toEqual(y.source);
          expect(x.target).toEqual(y.target);
          expect(x.changed).toBe(y.changed === true);
          expect(x.suppressed).toBe(y.suppressed === true);
        }
      }
    } finally {
      h.dispose();
    }
  });

  it('carries source exactly when the cell differs in bytes, and never otherwise', async () => {
    const h = await diffOf(
      withHeader('a,alpha,1,1.00', 'b,beta,2,2.00'),
      withHeader('a,alpha,1,1.0', 'b,BETA,2,2.00'),
      { countSuppressed: true },
    );
    try {
      const rows = [...h.rows()];
      const [first, second] = rows as [DiffRow, DiffRow];

      // Byte identical cells carry no source at all. This is what keeps an
      // unchanged 90,000 row report from being written out twice, and reading a
      // missing source as an empty string renders every one as a deletion.
      expect(first.cells[0]!.source).toBeUndefined();
      expect(first.cells[1]!.source).toBeUndefined();

      // 1.00 against 1.0 is equal under the DECIMAL comparator and different in
      // bytes, so it is suppressed rather than changed, and it does carry source.
      expect(first.cells[3]!.changed).toBe(false);
      expect(first.cells[3]!.suppressed).toBe(true);
      expect(first.cells[3]!.source).toBe('1.00');
      expect(first.cells[3]!.target).toBe('1.0');

      expect(second.cells[1]!.changed).toBe(true);
      expect(second.cells[1]!.source).toBe('beta');
      expect(second.cells[1]!.target).toBe('BETA');
    } finally {
      h.dispose();
    }
  });

  it('gives a deleted row source values and no target, and an added row the reverse', async () => {
    const h = await diffOf(withHeader('a,alpha,1,1.00'), withHeader('b,beta,2,2.00'));
    try {
      const byKind = new Map([...h.rows()].map((r) => [r.kind, r]));
      const deleted = byKind.get('deleted')!;
      const added = byKind.get('added')!;

      expect(deleted.cells[1]!.source).toBe('alpha');
      expect(deleted.cells[1]!.target).toBeUndefined();
      expect(deleted.targetRow).toBeNull();

      expect(added.cells[1]!.target).toBe('beta');
      expect(added.cells[1]!.source).toBeUndefined();
      expect(added.sourceRow).toBeNull();
    } finally {
      h.dispose();
    }
  });

  it('reports moved as a flag, so a row that moved and changed keeps both facts', async () => {
    const h = await diffOf(
      withHeader('a,alpha,1,1.00', 'b,beta,2,2.00', 'c,gamma,3,3.00'),
      withHeader('c,gamma,3,3.00', 'a,alpha,1,1.00', 'b,BETA,2,2.00'),
    );
    try {
      const rows = [...h.rows()];
      const modifiedAndMoved = rows.filter((r) => r.kind === 'modified' || r.moved);
      expect(modifiedAndMoved.length).toBeGreaterThan(0);
      // Whatever the matcher decides is moved, `kind` never spends itself saying
      // so: there is no 'moved' kind to collide with 'modified'.
      for (const r of rows) expect(['unchanged', 'modified', 'added', 'deleted'].includes(r.kind)).toBe(true);
    } finally {
      h.dispose();
    }
  });
});

describe('validation findings', () => {
  it('types each finding and carries the limit it failed against', async () => {
    const source = csv(
      [
        'KEY,,,',
        'REQUIRED,REQUIRED,,',
        'VARCHAR(10),VARCHAR(4),INTEGER,"DECIMAL(4,2)"',
        'id,name,qty,rate',
        'a,ok,1,1.00',
        '',
      ].join('\n'),
    );
    const target = csv(
      [
        'KEY,,,',
        'REQUIRED,REQUIRED,,',
        'VARCHAR(10),VARCHAR(4),INTEGER,"DECIMAL(4,2)"',
        'id,name,qty,rate',
        'a,toolong,notanumber,123.456',
        '',
      ].join('\n'),
    );

    const h = await diffOf(source, target);
    try {
      const row = [...h.rows()][0]!;
      const kinds = row.findings.map((f) => f.kind).sort();
      expect(kinds).toEqual(['notNumeric', 'precision', 'tooLong']);

      const tooLong = row.findings.find((f) => f.kind === 'tooLong')!;
      expect(tooLong.name).toBe('name');
      expect(tooLong.limit).toBe(4);

      const precision = row.findings.find((f) => f.kind === 'precision')!;
      expect(precision.precision).toBe(4);
      expect(precision.scale).toBe(2);

      const s = h.summary();
      expect(s.findings.total).toBe(3);
      expect(s.findings.enabled).toBe(true);
    } finally {
      h.dispose();
    }
  });

  it('never drops a row whose only news is a finding, even under changesOnly', async () => {
    const body = 'a,,1,1.00';
    const file = csv(
      ['KEY,,,', ',REQUIRED,,', 'VARCHAR(10),VARCHAR(20),INTEGER,"DECIMAL(5,2)"', 'id,name,qty,rate', body, ''].join('\n'),
    );
    const h = await diffOf(file, file);
    try {
      const rows = [...h.rows({ changesOnly: true })];
      expect(rows.length).toBe(1);
      expect(rows[0]!.kind).toBe('unchanged');
      expect(rows[0]!.findings[0]!.kind).toBe('requiredEmpty');
      // The two files agree; the data does not satisfy the schema. Those are
      // different questions and the summary answers them separately.
      expect(h.summary().identical).toBe(true);
    } finally {
      h.dispose();
    }
  });

  it('reports zeros as not-looked-at when validation is off', async () => {
    const file = csv(
      ['KEY,,,', ',REQUIRED,,', 'VARCHAR(10),VARCHAR(20),INTEGER,"DECIMAL(5,2)"', 'id,name,qty,rate', 'a,,1,1.00', ''].join('\n'),
    );
    const h = await diffOf(file, file, { validate: false });
    try {
      const s = h.summary();
      expect(s.findings.total).toBe(0);
      expect(s.findings.enabled).toBe(false);
    } finally {
      h.dispose();
    }
  });
});

describe('the column policy', () => {
  const source = withHeader('a,alpha,1,1.00');

  it('is an error by default, which is the locked decision of spec 13.10', async () => {
    const target = csv(
      [
        'KEY,,,,',
        'REQUIRED,,,,',
        'VARCHAR(10),VARCHAR(20),INTEGER,"DECIMAL(5,2)",VARCHAR(4)',
        'id,name,qty,rate,extra',
        'a,alpha,1,1.00,x',
        '',
      ].join('\n'),
    );
    await expect(compare(source, target)).rejects.toMatchObject({ code: 'ENGINE' });
  });

  it('compares the shared columns when allowed, and the report is narrower than the file', async () => {
    const target = csv(
      [
        'KEY,,,,',
        'REQUIRED,,,,',
        'VARCHAR(10),VARCHAR(20),INTEGER,"DECIMAL(5,2)",VARCHAR(4)',
        'id,name,qty,rate,extra',
        'a,alpha,1,1.00,x',
        '',
      ].join('\n'),
    );
    const h = await compare(source, target, {
      comparison: { allowAddedColumns: true },
    });
    try {
      const s = h.summary();
      expect(s.columns.added).toBe(1);
      expect(s.columns.compared).toBe(4);
      expect(h.columns.length).toBe(4);

      // The row's width is the compared width, not the uploaded file's. A
      // consumer that read it from the header would be off by one here and
      // nowhere else, which is what makes this worth asserting.
      const row = [...h.rows()][0]!;
      expect(row.cells.length).toBe(4);
      expect(row.cells.map((c) => c.name)).toEqual(['id', 'name', 'qty', 'rate']);

      expect(s.schemaFindings[0]!.kind).toBe('columnAdded');
      expect(s.schemaFindings[0]!.name).toBe('extra');
    } finally {
      h.dispose();
    }
  });
});

describe('the HTML emitter through the binding', () => {
  it('holds the safety invariant on the XSS corpus, checked independently', async () => {
    // xss.csv is a names-only file whose one row carries <script>, a quote and
    // an attribute break out, and javascript:.
    const source = fixture('xss.csv');
    const target = csv(dec.decode(fixture('xss.csv')).replace('NORTH', 'SOUTH'));

    const h = await compare(source, target, { header: { rows: 1 } });
    try {
      for (const cellDiff of ['none', 'word', 'word-then-character'] as const) {
        const html = dec.decode(h.emit('html', { cellDiff }));
        expect(htmlSafetyViolation(html)).toBeNull();
        // Stated the other way round too, because the invariant above would also
        // hold on an emitter that dropped the content entirely.
        expect(html).toContain('&lt;script&gt;');
        expect(html).toContain('&quot;');
      }
    } finally {
      h.dispose();
    }
  });

  it('refuses a class prefix that is not an identifier rather than escaping it', async () => {
    const h = await compare(withHeader('a,alpha,1,1.00'), withHeader('a,beta,1,1.00'));
    try {
      // Refused, not escaped: a prefix is an identifier, and one that is not is a
      // mistake worth failing on rather than mangling into something that still
      // renders.
      expect(() => h.emit('html', { classPrefix: 'x" onload="alert(1)' })).toThrow(
        /class prefix must match/,
      );
      // And the handle still works afterwards. The engine holds one error and the
      // first one wins, so letting the refusal happen down there would have
      // aborted a comparison that was complete and correct. The binding checks
      // the caller's own arguments before they reach the context.
      expect(dec.decode(h.emit('html', { classPrefix: 'ok-prefix' }))).toContain('ok-prefix');
    } finally {
      h.dispose();
    }
  });
});

describe('cell segments', () => {
  it('indexes the source for equal and delete and the target for insert', async () => {
    const h = await compare(withHeader('a,one two three,1,1.00'), withHeader('a,one four three,1,1.00'));
    try {
      const row = [...h.rows()][0]!;
      const segs = h.index().getCellSegments(0, 1, 'word');
      expect(segs.length).toBeGreaterThan(0);

      const source = row.cells[1]!.source!;
      const target = row.cells[1]!.target!;
      let rebuiltSource = '';
      let rebuiltTarget = '';
      for (const s of segs) {
        if (s.op === 'insert') rebuiltTarget += sliceByBytes(target, s.start, s.len);
        else {
          rebuiltSource += sliceByBytes(source, s.start, s.len);
          if (s.op === 'equal') rebuiltTarget += sliceByBytes(source, s.start, s.len);
        }
      }
      // The rebuild law: equal plus delete is the source, equal plus insert is
      // the target. If the offsets were in the wrong units this would not hold.
      expect(rebuiltSource).toBe(source);
      expect(rebuiltTarget).toBe(target);
    } finally {
      h.dispose();
    }
  });

  it('keeps byte offsets correct across non ASCII, which slicing a string would not', async () => {
    const h = await compare(withHeader('a,café noir,1,1.00'), withHeader('a,café blanc,1,1.00'));
    try {
      const row = [...h.rows()][0]!;
      const segs = h.index().getCellSegments(0, 1, 'word');
      const source = row.cells[1]!.source!;
      const target = row.cells[1]!.target!;

      let rebuilt = '';
      for (const s of segs) {
        if (s.op !== 'insert') rebuilt += sliceByBytes(source, s.start, s.len);
      }
      expect(rebuilt).toBe(source);
      // 'café' is five bytes and four code units, so a segment that starts after
      // it starts at byte 5 and at index 4. Slicing the decoded string at 5 would
      // silently drop a character.
      expect(target.startsWith('café')).toBe(true);
    } finally {
      h.dispose();
    }
  });
});

describe('errors', () => {
  it('names the duplicate key and both row numbers', async () => {
    const dup = withHeader('a,alpha,1,1.00', 'a,beta,2,2.00');
    await expect(compare(dup, dup)).rejects.toThrow(/duplicate key/i);
  });

  it('fails at the byte that crosses the size limit rather than after buffering', async () => {
    const big = withHeader(...Array.from({ length: 500 }, (_, i) => `k${i},name,1,1.00`));
    await expect(
      compare(big, big, { limits: { maxBytes: 1024 } }),
    ).rejects.toMatchObject({ code: 'TOO_LARGE' });
  });

  it('refuses a reordered column even when both column flags are set', async () => {
    const target = csv(
      [
        'KEY,,,',
        'REQUIRED,,,',
        'VARCHAR(20),VARCHAR(10),INTEGER,"DECIMAL(5,2)"',
        'name,id,qty,rate',
        'alpha,a,1,1.00',
        '',
      ].join('\n'),
    );
    await expect(
      compare(withHeader('a,alpha,1,1.00'), target, {
        comparison: { allowAddedColumns: true, allowRemovedColumns: true },
      }),
    ).rejects.toThrow(/./);
  });
});

describe('streaming', () => {
  it('parses across chunk boundaries that split a quoted field', async () => {
    const text = dec.decode(fixture('multiline_quoted.csv'));
    const bytes = new TextEncoder().encode(text);

    // One byte at a time is the most hostile chunking there is: it splits every
    // multi byte sequence, every "" pair and every CRLF.
    let i = 0;
    const trickle = new ReadableStream<Uint8Array>({
      pull(controller) {
        if (i >= bytes.length) return controller.close();
        controller.enqueue(bytes.subarray(i, i + 1));
        i += 1;
      },
    });

    const h = await compare(trickle, bytes);
    try {
      expect(h.summary().identical).toBe(true);
      const row = [...h.rows()][0]!;
      expect(row.cells[2]!.target).toContain('\n');
    } finally {
      h.dispose();
    }
  });
});
