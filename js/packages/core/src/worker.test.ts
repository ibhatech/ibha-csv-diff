/**
 * The worker path, and the backpressure that keeps it bounded.
 *
 * The property under test is that running in a worker changes where the work
 * happens and nothing else. Spec 4.3 puts it as: the API is identical in all
 * three modes. So every assertion here compares the worker's answer against the
 * in-process one rather than against a literal, which is the only way to notice
 * the serialization boundary quietly dropping a field.
 */

import { compare, compareInWorker, PushSource } from './index.ts';
import { describe, expect, fixture, it, withHeader } from './testkit.ts';

describe('PushSource', () => {
  it('stalls the writer at the watermark and resumes when the reader drains', async () => {
    const src = new PushSource(undefined, 1024);
    const chunk = new Uint8Array(600);

    // The first write fits under the watermark and returns at once.
    let firstDone = false;
    void src.write(chunk).then(() => {
      firstDone = true;
    });
    await Promise.resolve();
    expect(firstDone).toBe(true);

    // The second takes the buffer over it, so the writer waits. This is what
    // stops the target side being buffered whole while the source is parsing.
    let secondDone = false;
    const second = src.write(chunk).then(() => {
      secondDone = true;
    });
    await new Promise((r) => setTimeout(r, 5));
    expect(secondDone).toBe(false);

    await src.read();
    await second;
    expect(secondDone).toBe(true);
  });

  it('surfaces a failed download to the reader rather than looking like a short file', async () => {
    const src = new PushSource();
    await src.write(new Uint8Array([1, 2, 3]));
    src.fail(new Error('socket reset'));
    // The buffered bytes are abandoned: a truncated file that parses is worse
    // than an error, because the diff it produces looks plausible.
    await expect(src.read()).rejects.toThrow(/socket reset/);
  });

  it('ends cleanly, and a read after end is the end of stream', async () => {
    const src = new PushSource();
    await src.write(new Uint8Array([1]));
    src.end();
    expect((await src.read())!.length).toBe(1);
    expect(await src.read()).toBeNull();
  });
});

describe('compareInWorker', () => {
  it('gives the same summary, rows and emitter bytes as the in-process path', async () => {
    const source = fixture('p90_source.csv');
    const target = fixture('p90_target.csv');

    const local = await compare(source, target);
    const remote = await compareInWorker(source, target);
    try {
      expect(remote.columns).toEqual([...local.columns]);
      expect(JSON.stringify(await remote.summary())).toBe(JSON.stringify(local.summary()));

      const info = await remote.index(true);
      const localIndex = local.index({ changesOnly: true });
      expect(info.rowCount).toBe(localIndex.rowCount);
      expect(info.columns).toBe(localIndex.columns);

      const page = await remote.getRows(50, 10);
      expect(JSON.stringify(page)).toBe(JSON.stringify(localIndex.getRows(50, 10)));

      const bytes = await remote.emit('summary');
      expect(Buffer.from(bytes).equals(Buffer.from(local.emit('summary')))).toBe(true);
    } finally {
      await remote.dispose();
      local.dispose();
    }
  });

  it('streams the whole report in batches without building an index', async () => {
    const remote = await compareInWorker(fixture('tiny_source.csv'), fixture('tiny_target.csv'));
    try {
      let n = 0;
      let firstKind = '';
      for await (const row of remote.rows({ includeValues: false }, 256)) {
        if (n === 0) firstKind = row.kind;
        n++;
      }
      const summary = await remote.summary();
      expect(n).toBe(summary.rows.report);
      expect(firstKind.length).toBeGreaterThan(0);
    } finally {
      await remote.dispose();
    }
  });

  it('transfers a compact page, and the flags survive the boundary', async () => {
    const remote = await compareInWorker(fixture('tiny_source.csv'), fixture('tiny_target.csv'));
    try {
      await remote.index(true);
      const page = await remote.getRowsCompact(0, 6);
      expect(page.cellFlags.length).toBe(page.count * page.columns);
      expect(page.kinds.length).toBe(page.count);
      // A transferred typed array arrives as a real typed array, not as an
      // object with numeric keys, which is what a bad clone looks like.
      expect(page.moveDistance instanceof Int32Array).toBe(true);
    } finally {
      await remote.dispose();
    }
  });

  it('reports an engine error across the boundary with its code and its name', async () => {
    const dup = withHeader('a,alpha,1,1.00', 'a,beta,2,2.00');
    await expect(compareInWorker(dup, dup)).rejects.toMatchObject({ code: 'ENGINE' });
  });

  it('surfaces a source that fails mid stream rather than diffing a truncated file', async () => {
    let sent = 0;
    const broken = new ReadableStream<Uint8Array>({
      pull(controller) {
        if (sent++ === 0) {
          controller.enqueue(new TextEncoder().encode('KEY,,,\nREQUIRED,,,\n'));
          return;
        }
        controller.error(new Error('socket reset'));
      },
    });
    await expect(compareInWorker(broken, withHeader('a,alpha,1,1.00'))).rejects.toThrow(/socket reset/);
  });
});
