/**
 * The main thread's side of the worker.
 *
 * `compareInWorker` has the same shape as `compare` and returns a handle whose
 * methods are asynchronous, which is the only visible difference. Spec 4.3 puts
 * it as: the API is identical in all three modes, which is why `getRows` looks
 * synchronous but the handle is obtained from a promise, so a view model can
 * prefetch pages slightly ahead of the scroll position.
 *
 * **The fetch stays on the main thread.** The library never performs it, per spec
 * 13.7: authentication, headers, cookies, tokens, retries and CORS are
 * application concerns that differ per deployment. So the caller hands us a
 * `Response` here, where its credentials already live, and this pumps the body
 * across to the worker in chunks. Each chunk is awaited, which is how the
 * worker's backpressure reaches the reader: the engine parses the source side to
 * completion before pulling a byte of the target, so without it a 150 MB target
 * would sit buffered in the worker for the whole of the source parse.
 */

import type { ReplyOf, Request, RequestBody, Response, ResponseBody, Side, WireOptions } from './protocol.ts';
import { IbhaCsvError, toByteSource } from './source.ts';
import type { ByteSource, IbhaCsvSource } from './source.ts';
import type { CompactRowPage } from './handle.ts';
import type { LoadOptions } from './module.ts';
import type {
  CellDiffMode,
  CsvDiffOptions,
  DiffRow,
  DiffSummary,
  EmitFormat,
  EmitOptions,
  RowReadOptions,
  TextSegment,
} from './types.ts';

/** Enough of both Worker APIs to drive one, so nothing below cares which it has. */
interface WorkerLike {
  postMessage(msg: unknown, transfer?: Transferable[]): void;
  onMessage(fn: (msg: Response) => void): void;
  onError(fn: (err: unknown) => void): void;
  terminate(): void;
}

/**
 * Where the worker entry point lives.
 *
 * The published package ships `worker.js`; running from source under Node there
 * is only `worker.ts`, and this module's own URL says which of the two situations
 * we are in. Probing the filesystem instead would work in Node and mean nothing
 * in a browser.
 *
 * **Only the `.js` path is written as a literal, and that asymmetry is the
 * point.** `new URL('<literal>', import.meta.url)` is the form every bundler
 * recognizes, follows and rewrites, which is exactly what the published entry
 * needs. But a bundler analyses *both* arms of the branch statically, without
 * evaluating the condition, so a literal `'./worker.ts'` sent every consumer's
 * build looking for a TypeScript file that a published package does not contain.
 * Vite reported it as "doesn't exist at build time, it will remain unchanged to
 * be resolved at runtime" in a scratch consumer app, on a build that was
 * otherwise correct. Harmless, and a warning in someone else's build that they
 * cannot act on is a support question rather than a bug report.
 *
 * Building the development path from a template expression keeps it out of that
 * static analysis. It costs nothing: this arm never runs outside this repository.
 */
function workerEntry(): URL {
  if (import.meta.url.endsWith('.ts')) {
    return new URL(`./worker.${'ts'}`, import.meta.url);
  }
  return new URL('./worker.js', import.meta.url);
}

async function spawn(url: URL): Promise<WorkerLike> {
  if (typeof Worker !== 'undefined' && url.protocol !== 'file:') {
    const w = new Worker(url, { type: 'module' });
    return {
      postMessage: (m, t) => w.postMessage(m, t ?? []),
      onMessage: (fn) => w.addEventListener('message', (e) => fn((e as MessageEvent).data as Response)),
      onError: (fn) => w.addEventListener('error', fn),
      terminate: () => w.terminate(),
    };
  }
  const { Worker: NodeWorker } = await import('node:worker_threads');
  const w = new NodeWorker(url);
  // Nothing here should hold the process open on its own: dispose terminates the
  // worker, and a caller that forgets should still be able to exit.
  w.unref();
  return {
    postMessage: (m, t) => w.postMessage(m, t as never),
    onMessage: (fn) => w.on('message', fn),
    onError: (fn) => w.on('error', fn),
    terminate: () => void w.terminate(),
  };
}

/* ---------------------------------------------------------------- client -- */

interface Pending {
  resolve: (msg: ResponseBody) => void;
  reject: (err: unknown) => void;
}

class WorkerClient {
  private readonly worker: WorkerLike;
  private readonly pending = new Map<number, Pending>();
  private seq = 1;
  private failure: unknown = null;

  constructor(worker: WorkerLike) {
    this.worker = worker;
    worker.onMessage((msg) => {
      const waiting = this.pending.get(msg.seq);
      if (!waiting) return;
      this.pending.delete(msg.seq);
      if (msg.t === 'error') {
        waiting.reject(new IbhaCsvError(msg.code as never, msg.message, msg.status));
      } else {
        waiting.resolve(msg);
      }
    });
    worker.onError((err) => this.abort(err));
  }

  private abort(err: unknown): void {
    this.failure = err;
    for (const p of this.pending.values()) p.reject(err);
    this.pending.clear();
  }

  /**
   * One request, matched to its reply by sequence number.
   *
   * Not a queue: the worker starts each request as it arrives, so the pump's
   * acknowledgements and the comparison's completion come back interleaved, and
   * pairing them by arrival order would hand each the other's answer.
   */
  send<T extends ResponseBody['t']>(body: RequestBody, transfer?: Transferable[]): Promise<ReplyOf<T>> {
    if (this.failure) return Promise.reject(this.failure);
    const seq = this.seq++;
    return new Promise<ReplyOf<T>>((resolve, reject) => {
      this.pending.set(seq, { resolve: resolve as (m: ResponseBody) => void, reject });
      this.worker.postMessage({ ...body, seq } satisfies Request, transfer);
    });
  }

  terminate(): void {
    this.abort(new IbhaCsvError('INVALID_ARG', 'the worker has been terminated'));
    this.worker.terminate();
  }
}

/* ---------------------------------------------------------------- handle -- */

export class RemoteDiffHandle {
  private readonly client: WorkerClient;
  readonly columns: readonly string[];
  readonly bytesReserved: number;
  private disposed = false;

  /** @internal */
  constructor(client: WorkerClient, columns: string[], bytesReserved: number) {
    this.client = client;
    this.columns = columns;
    this.bytesReserved = bytesReserved;
  }

  private ask<T extends ResponseBody['t']>(body: RequestBody): Promise<ReplyOf<T>> {
    if (this.disposed) throw new IbhaCsvError('INVALID_ARG', 'this diff has been disposed');
    return this.client.send<T>(body);
  }

  async summary(): Promise<DiffSummary> {
    return (await this.ask<'summary'>({ t: 'summary' })).summary;
  }

  /**
   * Builds the retained index in the worker and reports what it cost.
   *
   * Every paging call needs it, and calling it explicitly is how a caller learns
   * the row count before asking for a page. It is separate from the comparison
   * because it is the one thing here whose memory grows with the diff, and a
   * summary or an export never needs it.
   */
  async index(changesOnly = false): Promise<{ rowCount: number; columns: number; bytesRetained: number }> {
    const r = await this.ask<'index'>({ t: 'index', changesOnly });
    return { rowCount: r.rowCount, columns: r.columns, bytesRetained: r.bytesRetained };
  }

  async getRows(offset: number, count: number, read: RowReadOptions = {}): Promise<DiffRow[]> {
    return (await this.ask<'rows'>({ t: 'rows', offset, count, read })).rows;
  }

  /** The page as parallel typed arrays, transferred rather than copied. This is
   *  the form a virtualized table binds to: no row object per visible row. */
  async getRowsCompact(offset: number, count: number): Promise<CompactRowPage> {
    return (await this.ask<'compact'>({ t: 'compact', offset, count })).page;
  }

  async getCellSegments(
    row: number,
    col: number,
    mode: CellDiffMode = 'word-then-character',
    maxBytes = 0,
  ): Promise<TextSegment[]> {
    return (await this.ask<'segments'>({ t: 'segments', row, col, mode, maxBytes })).segments;
  }

  async emit(format: EmitFormat, options: EmitOptions = {}): Promise<Uint8Array> {
    return new Uint8Array((await this.ask<'emit'>({ t: 'emit', format, options })).bytes);
  }

  /**
   * The report without an index, in batches.
   *
   * The streaming path across the thread boundary: the worker keeps the cursor
   * between requests and nothing is retained on either side but the batch in
   * flight. Use this for an export or a scan, and the index for a view that
   * seeks.
   */
  async *rows(read: RowReadOptions = {}, batch = 512): AsyncGenerator<DiffRow> {
    for (;;) {
      const r = await this.ask<'batch'>({ t: 'drain', read, batch });
      for (const row of r.rows) yield row;
      if (r.done) return;
    }
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    try {
      await this.client.send({ t: 'dispose' });
    } catch {
      // Disposing a worker that has already failed is not itself a failure.
    } finally {
      this.disposed = true;
      this.client.terminate();
    }
  }
}

/**
 * Compares two sources with the engine running in a worker.
 *
 * One worker per comparison, terminated by `dispose`. That mirrors one wasm
 * instance per diff: linear memory only grows, so a shared worker would carry the
 * high water mark of every comparison it had ever run.
 */
export async function compareInWorker(
  source: IbhaCsvSource,
  target: IbhaCsvSource,
  options: CsvDiffOptions = {},
  load: LoadOptions = {},
): Promise<RemoteDiffHandle> {
  const client = new WorkerClient(await spawn(workerEntry()));

  const limit = options.limits?.maxBytes !== undefined ? { maxBytes: options.limits.maxBytes } : {};
  const src = await toByteSource(source, limit);
  const tgt = await toByteSource(target, limit);

  // `signal` is not structured cloneable, and it would be the wrong thing to send
  // anyway: aborting is this pump stopping.
  const { signal, ...wire } = options;

  await client.send({
    t: 'open',
    options: wire as WireOptions,
    load,
    sizeHints: {
      ...(src.sizeHint !== undefined ? { source: src.sizeHint } : {}),
      ...(tgt.sizeHint !== undefined ? { target: tgt.sizeHint } : {}),
    },
  });

  const pump = async (side: Side, from: ByteSource): Promise<void> => {
    try {
      for (;;) {
        signal?.throwIfAborted();
        const chunk = await from.read();
        if (chunk === null) break;
        // A fresh buffer so it can be transferred. The chunk may be a view into a
        // larger buffer the reader still owns, and neutering that would break the
        // reader rather than the message.
        const buf = chunk.slice().buffer as ArrayBuffer;
        await client.send({ t: 'chunk', side, bytes: buf }, [buf as Transferable]);
      }
      await client.send({ t: 'end', side });
    } catch (err) {
      // Tell the worker, so its parse fails with the reason rather than hanging
      // on a stream that will never produce another byte.
      await client
        .send({ t: 'fail', side, message: err instanceof Error ? err.message : String(err) })
        .catch(() => undefined);
      throw err;
    }
  };

  try {
    // Both pumps and the comparison run at once. The engine parses the source to
    // completion before it pulls a byte of the target, and the target's push
    // source stops acknowledging once it holds its watermark, which stalls that
    // pump rather than buffering the whole file.
    const [ready] = await Promise.all([
      client.send<'ready'>({ t: 'ready' }),
      pump('source', src),
      pump('target', tgt),
    ]);
    return new RemoteDiffHandle(client, ready.columns, ready.bytesReserved);
  } catch (err) {
    client.terminate();
    throw err;
  }
}
