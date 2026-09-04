/**
 * The worker body. Runs the engine off the main thread, per spec 4.3.
 *
 * "Doing a 400 ms diff on the main thread is a 400 ms frozen UI, so the worker is
 * not optional." At the 150 MB ceiling it is not 400 ms, it is seconds.
 *
 * This file is loaded both as a Web Worker and as a `node:worker_threads` Worker,
 * and the two have different names for the same two operations. The adapter at
 * the top is the whole of that difference; nothing below it knows which host it
 * is running in.
 *
 * Requests are started as they arrive rather than one at a time, which is
 * required rather than an optimization: the comparison cannot finish until the
 * pump has delivered its bytes, so a worker that finished each request before
 * reading the next would deadlock on the first one.
 */

import { DiffEngine } from './engine.ts';
import { DiffIndex } from './handle.ts';
import { Engine } from './module.ts';
import { pageTransfers } from './protocol.ts';
import type { Request, Response, ResponseBody } from './protocol.ts';
import { PushSource } from './push.ts';
import { IbhaCsvError } from './source.ts';
import type { DiffRow } from './types.ts';

/* ------------------------------------------------------------- host glue -- */

type Post = (msg: Response, transfer?: Transferable[]) => void;

let post: Post;
let listen: (fn: (msg: Request) => void) => void;

const host = globalThis as unknown as {
  postMessage?: (msg: unknown, transfer?: Transferable[]) => void;
  addEventListener?: (t: string, fn: (e: { data: Request }) => void) => void;
};

if (typeof host.postMessage === 'function' && typeof host.addEventListener === 'function') {
  post = (msg, transfer) => host.postMessage!(msg, transfer ?? []);
  listen = (fn) => host.addEventListener!('message', (e) => fn(e.data));
} else {
  const { parentPort } = await import('node:worker_threads');
  if (!parentPort) throw new Error('worker.ts was loaded outside a worker');
  // The two hosts name the same set of transferable things differently, and
  // neither list is assignable to the other. The values are the ones this file
  // produces, so the cast is over a naming difference rather than a real one.
  post = (msg, transfer) => parentPort.postMessage(msg, transfer as never);
  listen = (fn) => parentPort.on('message', fn);
}

/* ------------------------------------------------------------- the state -- */

const sources = {
  source: new PushSource(),
  target: new PushSource(),
};

let comparison: Promise<DiffEngine> | null = null;
let index: DiffIndex | null = null;
let indexChangesOnly = false;
let drain: Generator<DiffRow> | null = null;

function engine(): Promise<DiffEngine> {
  if (!comparison) throw new IbhaCsvError('INVALID_ARG', 'no comparison has been opened');
  return comparison;
}

async function indexOf(changesOnly: boolean): Promise<DiffIndex> {
  const e = await engine();
  if (!index || indexChangesOnly !== changesOnly) {
    index = DiffIndex.build(e, { changesOnly });
    indexChangesOnly = changesOnly;
  }
  return index;
}

/* --------------------------------------------------------------- handler -- */

listen((msg) => {
  handle(msg).then(
    ([body, transfer]) => post({ ...body, seq: msg.seq }, transfer),
    (err: unknown) => {
      const body: ResponseBody =
        err instanceof IbhaCsvError
          ? {
              t: 'error',
              code: err.code,
              message: err.message,
              ...(err.status ? { status: err.status } : {}),
            }
          : { t: 'error', code: 'ENGINE', message: err instanceof Error ? err.message : String(err) };
      post({ ...body, seq: msg.seq });
    },
  );
});

async function handle(msg: Request): Promise<[ResponseBody, Transferable[]?]> {
  switch (msg.t) {
    case 'open': {
      sources.source = new PushSource(msg.sizeHints.source);
      sources.target = new PushSource(msg.sizeHints.target);
      // The comparison starts now and pulls as the chunks land, so the parse
      // overlaps the transfer rather than following it. That is the whole reason
      // ReadableStream was promoted to the primary input path.
      comparison = Engine.load(msg.load).then((e) =>
        DiffEngine.compare(e, sources.source, sources.target, msg.options),
      );
      // Nothing awaits it until `ready`, so an early rejection would otherwise be
      // an unhandled rejection that takes the worker down before it can be
      // reported.
      comparison.catch(() => undefined);
      return [{ t: 'ack' }];
    }

    case 'chunk':
      // Awaited, which is what applies backpressure: the acknowledgement the pump
      // is waiting on does not go out until the push source has room again.
      await sources[msg.side].write(new Uint8Array(msg.bytes));
      return [{ t: 'ack' }];

    case 'end':
      sources[msg.side].end();
      return [{ t: 'ack' }];

    case 'fail':
      sources[msg.side].fail(new IbhaCsvError('IO_ERROR', msg.message));
      return [{ t: 'ack' }];

    case 'ready': {
      const e = await engine();
      return [{ t: 'ready', columns: [...e.columns], bytesReserved: e.bytesReserved }];
    }

    case 'summary':
      return [{ t: 'summary', summary: (await engine()).summary() }];

    case 'index': {
      const idx = await indexOf(msg.changesOnly);
      return [
        { t: 'index', rowCount: idx.rowCount, columns: idx.columns, bytesRetained: idx.bytesRetained },
      ];
    }

    case 'rows': {
      const idx = await indexOf(indexChangesOnly);
      return [{ t: 'rows', rows: idx.getRows(msg.offset, msg.count, msg.read) }];
    }

    case 'compact': {
      const idx = await indexOf(indexChangesOnly);
      const page = idx.getRowsCompact(msg.offset, msg.count);
      return [{ t: 'compact', page }, pageTransfers(page)];
    }

    case 'segments': {
      const idx = await indexOf(indexChangesOnly);
      return [{ t: 'segments', segments: idx.getCellSegments(msg.row, msg.col, msg.mode, msg.maxBytes) }];
    }

    case 'emit': {
      const bytes = (await engine()).emit(msg.format, msg.options);
      // A fresh ArrayBuffer so it can be transferred rather than copied: the
      // report is by far the largest thing that crosses this boundary.
      const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      return [{ t: 'emit', bytes: buf as ArrayBuffer }, [buf as Transferable]];
    }

    case 'drain': {
      // Streaming without an index: the generator is kept between requests, so
      // nothing is retained on either side but the batch in flight.
      const e = await engine();
      drain ??= e.rows(msg.read);
      const rows: DiffRow[] = [];
      let done = false;
      for (let i = 0; i < msg.batch; i++) {
        const next = drain.next();
        if (next.done) {
          done = true;
          drain = null;
          break;
        }
        rows.push(next.value);
      }
      return [{ t: 'batch', rows, done }];
    }

    case 'dispose': {
      await sources.source.cancel();
      await sources.target.cancel();
      const pending = comparison;
      comparison = null;
      index = null;
      drain = null;
      try {
        (await pending)?.dispose();
      } catch {
        // A comparison that never completed has nothing to dispose, and whatever
        // it failed with has already been reported to the caller.
      }
      return [{ t: 'ack' }];
    }
  }
}
