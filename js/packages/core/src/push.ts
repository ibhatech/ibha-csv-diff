/**
 * A ByteSource fed by pushes rather than by pulls.
 *
 * The engine's ingest is a pull callback and streaming is the only path, which is
 * right for a file or a fetch body. A worker is the other way round: the bytes
 * arrive as messages whenever the main thread sends them, and the worker's parse
 * has to wait for them. This is the adapter between the two, and it is the
 * `DiffSession` push interface of spec 4.2 in the shape the rest of this package
 * already speaks.
 *
 * **Backpressure is the whole reason this is more than a queue.** The engine
 * parses the source side to completion before it pulls a single byte of the
 * target, so a main thread that pumps both sides as fast as the messages will go
 * would buffer the entire target in the worker before the target parse starts.
 * On the p90 pair that is 15 MB held for no reason, and at the 150 MB ceiling it
 * is fatal. So `write` returns a promise that does not resolve until the buffered
 * amount drops below the watermark, and a pump loop that awaits it is bounded
 * without having to know anything about the engine's parse order.
 */

import { IbhaCsvError } from './source.ts';
import type { ByteSource } from './source.ts';

const HIGH_WATER_BYTES = 4 * 1024 * 1024;

export class PushSource implements ByteSource {
  readonly sizeHint: number | undefined;

  private queue: Uint8Array[] = [];
  private buffered = 0;
  private ended = false;
  private failure: unknown = null;

  private wakeReader: (() => void) | null = null;
  private wakeWriter: (() => void) | null = null;

  private readonly highWater: number;

  constructor(sizeHint?: number, highWater = HIGH_WATER_BYTES) {
    this.sizeHint = sizeHint;
    this.highWater = highWater;
  }

  /** Resolves once there is room for more. Await it in the pump loop. */
  async write(bytes: Uint8Array): Promise<void> {
    if (this.ended) throw new IbhaCsvError('INVALID_ARG', 'write after end');
    if (bytes.length === 0) return;

    this.queue.push(bytes);
    this.buffered += bytes.length;
    this.wakeReader?.();
    this.wakeReader = null;

    while (this.buffered >= this.highWater && !this.failure) {
      await new Promise<void>((resolve) => {
        this.wakeWriter = resolve;
      });
    }
    if (this.failure) throw this.failure;
  }

  end(): void {
    this.ended = true;
    this.wakeReader?.();
    this.wakeReader = null;
  }

  /** Aborts both ends. The reader sees the error on its next pull, so a failed
   *  download surfaces as that failure rather than as a truncated parse. */
  fail(err: unknown): void {
    this.failure = err;
    this.ended = true;
    this.wakeReader?.();
    this.wakeReader = null;
    this.wakeWriter?.();
    this.wakeWriter = null;
  }

  async read(): Promise<Uint8Array | null> {
    for (;;) {
      if (this.failure) throw this.failure;
      const next = this.queue.shift();
      if (next !== undefined) {
        this.buffered -= next.length;
        if (this.buffered < this.highWater) {
          this.wakeWriter?.();
          this.wakeWriter = null;
        }
        return next;
      }
      if (this.ended) return null;
      await new Promise<void>((resolve) => {
        this.wakeReader = resolve;
      });
    }
  }

  async cancel(): Promise<void> {
    this.queue = [];
    this.buffered = 0;
    this.end();
    this.wakeWriter?.();
    this.wakeWriter = null;
  }
}
