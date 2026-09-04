/**
 * Source adapters: everything the browser or Node can hand us, normalized to the
 * pull based byte stream the engine consumes.
 *
 * Implements the contract decided in spec 13.7. The three rules, restated because
 * they are easy to erode later:
 *
 *   1. The library never performs the fetch. Authentication, headers, cookies,
 *      tokens, retries, CORS and base URLs are application concerns that differ
 *      per deployment. The caller assembles the request and hands us the Response.
 *
 *   2. Content-Type is ignored entirely. application/octet-stream is fully
 *      supported, because S3 presigned URLs commonly serve it and plenty of
 *      endpoints send text/plain for a CSV. We read bytes; the declared type
 *      carries no information we need.
 *
 *   3. The first bytes are sniffed for one specific failure only: a response that
 *      is actually an HTML login page or a JSON error body. That is the most
 *      common integration bug by a wide margin, and without the check it surfaces
 *      as a bizarre parse error hundreds of lines into the file.
 */

export type IbhaCsvSource =
  | Uint8Array
  | ArrayBuffer
  | Blob
  | ReadableStream<Uint8Array>
  /* An already normalized source, which is how the worker hands over bytes that
   * arrive as messages rather than as a stream. */
  | ByteSource
  | (() => Promise<Response | ReadableStream<Uint8Array> | Uint8Array | ArrayBuffer | Blob>);

export interface SourceOptions {
  /** Pipe the body through DecompressionStream. For a .csv.gz object served
   *  without Content-Encoding, which is common for S3 objects. Transparent
   *  Content-Encoding is handled by the browser and needs nothing here. */
  decompress?: 'gzip' | 'deflate' | 'none';
  /** Disable the HTML / JSON error page check. On by default; there is rarely a
   *  good reason to turn it off, but a CSV that legitimately starts with `<` is
   *  one of them. */
  sniffErrorResponses?: boolean;
  /** Hard byte ceiling. Defaults to the engine's 150 MB. Enforced as bytes
   *  arrive, so an oversized stream fails at the byte that crosses the limit
   *  rather than after being buffered. */
  maxBytes?: number;
}

export const DEFAULT_MAX_BYTES = 150 * 1024 * 1024;

export type IbhaCsvErrorCode =
  | 'TOO_LARGE'
  | 'BAD_CONTENT'
  | 'IO_ERROR'
  | 'INVALID_ARG'
  /* Everything the engine can record on a context that is not one of the above.
   * The C status name travels in `status` so a caller can switch on it without
   * parsing the message. */
  | 'ENGINE';

export class IbhaCsvError extends Error {
  readonly code: IbhaCsvErrorCode;
  /** The IBHA_CSVD_ERR_* name when this came from the engine, else undefined. */
  readonly status: string | undefined;

  // Written as ordinary fields rather than as constructor parameter properties
  // because those are the one piece of TypeScript that Node's type stripping
  // cannot erase, and this file has to run under bare node. See
  // rewriteRelativeImportExtensions in tsconfig.base.json.
  constructor(code: IbhaCsvErrorCode, message: string, status?: string) {
    super(message);
    this.name = 'IbhaCsvError';
    this.code = code;
    this.status = status;
  }
}

/** A normalized pull source. `read` resolves to null at end of stream. */
export interface ByteSource {
  read(): Promise<Uint8Array | null>;
  /** Total size when the source knows it, else undefined. Passed to the engine
   *  as a size hint, which takes the cost of retaining N bytes from about 4x N
   *  down to about 1x. Always supply it when it is knowable.
   *
   *  Declared as `number | undefined` rather than optional because an
   *  implementing class always has the property; under exactOptionalPropertyTypes
   *  those are not the same type. */
  readonly sizeHint: number | undefined;
  cancel(): Promise<void>;
}

function isByteSource(x: unknown): x is ByteSource {
  return (
    typeof x === 'object' &&
    x !== null &&
    typeof (x as ByteSource).read === 'function' &&
    typeof (x as ByteSource).cancel === 'function'
  );
}

function isThenable(x: unknown): x is Promise<unknown> {
  return typeof x === 'object' && x !== null && typeof (x as Promise<unknown>).then === 'function';
}

/**
 * Looks at the first bytes for a response that is not CSV at all.
 *
 * The case this catches: an expired session where the server returns a login page
 * with a 200 status, or an API gateway returning `{"message":"Unauthorized"}`.
 * Both parse as CSV without complaint for a surprisingly long way, then fail
 * somewhere confusing. Naming the real problem here saves hours.
 *
 * Deliberately narrow. This is not Content-Type enforcement and must not reject
 * anything that could plausibly be a CSV.
 */
export function sniffNotCsv(head: Uint8Array): string | null {
  // Skip a UTF-8 BOM and leading whitespace before judging.
  let i = 0;
  if (head.length >= 3 && head[0] === 0xef && head[1] === 0xbb && head[2] === 0xbf) i = 3;
  while (i < head.length && (head[i] === 0x20 || head[i] === 0x09 || head[i] === 0x0a || head[i] === 0x0d)) i++;

  const slice = head.subarray(i, Math.min(i + 64, head.length));
  const text = new TextDecoder('utf-8', { fatal: false }).decode(slice).toLowerCase();

  if (text.startsWith('<!doctype html') || text.startsWith('<html') || text.startsWith('<?xml')) {
    return 'expected CSV, received what appears to be an HTML or XML error page. Check whether the session expired and the server returned a login page with a 200 status.';
  }
  // A JSON object or array is never a CSV. Checking for an error-ish key would be
  // narrower, but any JSON body here is already wrong.
  if (text.startsWith('{') || text.startsWith('[')) {
    return 'expected CSV, received what appears to be a JSON response. Check whether the endpoint returned an error body instead of the file.';
  }
  return null;
}

class StreamSource implements ByteSource {
  private reader: ReadableStreamDefaultReader<Uint8Array>;
  private consumed = 0;
  private sniffed = false;
  private pending: Uint8Array | null = null;
  private readonly opts: Required<Pick<SourceOptions, 'sniffErrorResponses' | 'maxBytes'>>;
  readonly sizeHint: number | undefined;

  constructor(
    stream: ReadableStream<Uint8Array>,
    opts: Required<Pick<SourceOptions, 'sniffErrorResponses' | 'maxBytes'>>,
    sizeHint: number | undefined,
  ) {
    this.reader = stream.getReader();
    this.opts = opts;
    this.sizeHint = sizeHint;
  }

  async read(): Promise<Uint8Array | null> {
    if (this.pending) {
      const out = this.pending;
      this.pending = null;
      return out;
    }

    const { done, value } = await this.reader.read();
    if (done || !value) return null;

    // A zero length chunk is legal and means nothing; do not mistake it for EOF.
    if (value.length === 0) return this.read();

    if (!this.sniffed) {
      this.sniffed = true;
      if (this.opts.sniffErrorResponses) {
        const problem = sniffNotCsv(value);
        if (problem) {
          await this.cancel();
          throw new IbhaCsvError('BAD_CONTENT', problem);
        }
      }
    }

    this.consumed += value.length;
    if (this.consumed > this.opts.maxBytes) {
      await this.cancel();
      const limitMb = Math.floor(this.opts.maxBytes / (1024 * 1024));
      const seenMb = Math.ceil(this.consumed / (1024 * 1024));
      throw new IbhaCsvError(
        'TOO_LARGE',
        `File too large. Maximum allowed is ${limitMb} MB, received at least ${seenMb} MB.`,
      );
    }

    return value;
  }

  async cancel(): Promise<void> {
    try {
      await this.reader.cancel();
    } catch {
      // A cancel that fails after we have already decided to abort is not worth
      // surfacing; it would mask the real error.
    }
  }
}

function bytesToStream(bytes: Uint8Array): ReadableStream<Uint8Array> {
  // Chunked rather than one enqueue, so the streaming path is exercised
  // identically whether the input arrived as a buffer or as a network body.
  const CHUNK = 256 * 1024;
  let off = 0;
  return new ReadableStream<Uint8Array>({
    pull(controller) {
      if (off >= bytes.length) {
        controller.close();
        return;
      }
      const end = Math.min(off + CHUNK, bytes.length);
      controller.enqueue(bytes.subarray(off, end));
      off = end;
    },
  });
}

/** Normalizes any accepted source into a pull based ByteSource. */
export async function toByteSource(
  source: IbhaCsvSource,
  options: SourceOptions = {},
): Promise<ByteSource> {
  const opts = {
    sniffErrorResponses: options.sniffErrorResponses ?? true,
    maxBytes: options.maxBytes ?? DEFAULT_MAX_BYTES,
  };
  const decompress = options.decompress ?? 'none';

  // An already normalized source passes straight through: it has its own limit
  // and its own sniffing decision, and wrapping it again would apply both twice.
  if (isByteSource(source)) return source;

  let resolved: unknown = source;
  if (typeof source === 'function') {
    resolved = await source();
  } else if (isThenable(source)) {
    resolved = await source;
  }

  let stream: ReadableStream<Uint8Array> | null = null;
  let sizeHint: number | undefined;

  if (resolved instanceof Uint8Array) {
    sizeHint = resolved.length;
    stream = bytesToStream(resolved);
  } else if (resolved instanceof ArrayBuffer) {
    const view = new Uint8Array(resolved);
    sizeHint = view.length;
    stream = bytesToStream(view);
  } else if (typeof Blob !== 'undefined' && resolved instanceof Blob) {
    sizeHint = resolved.size;
    stream = resolved.stream() as ReadableStream<Uint8Array>;
  } else if (typeof Response !== 'undefined' && resolved instanceof Response) {
    if (!resolved.ok) {
      throw new IbhaCsvError(
        'IO_ERROR',
        `source request failed with HTTP ${resolved.status} ${resolved.statusText}`,
      );
    }
    if (!resolved.body) {
      // A Response with no body is legal but useless here.
      const buf = new Uint8Array(await resolved.arrayBuffer());
      sizeHint = buf.length;
      stream = bytesToStream(buf);
    } else {
      // Content-Length is a hint only. It is absent for chunked responses and,
      // when Content-Encoding is in play, it describes the compressed size rather
      // than what we will actually read. So it is used as a hint and never as a
      // limit; the limit is enforced on arrival.
      const len = resolved.headers.get('content-length');
      const encoded = resolved.headers.get('content-encoding');
      if (len && !encoded) {
        const n = Number(len);
        if (Number.isFinite(n) && n > 0) sizeHint = n;
      }
      stream = resolved.body;
    }
  } else if (resolved && typeof (resolved as ReadableStream<Uint8Array>).getReader === 'function') {
    stream = resolved as ReadableStream<Uint8Array>;
  } else {
    throw new IbhaCsvError(
      'INVALID_ARG',
      'unsupported source. Pass a Uint8Array, ArrayBuffer, Blob, File, ReadableStream, or a callback returning a Response.',
    );
  }

  if (decompress !== 'none') {
    if (typeof DecompressionStream === 'undefined') {
      throw new IbhaCsvError('INVALID_ARG', `decompress: '${decompress}' requires DecompressionStream, which this runtime does not provide`);
    }
    stream = stream.pipeThrough(
      // lib.dom types DecompressionStream's writable side as BufferSource, which
      // is wider than Uint8Array, so the pair does not typecheck directly. The
      // runtime contract is exactly what we need.
      new DecompressionStream(decompress) as unknown as ReadableWritablePair<Uint8Array, Uint8Array>,
    );
    // The decompressed size is unrelated to the compressed size, so the hint no
    // longer applies. A wrong hint costs performance, so drop it.
    sizeHint = undefined;
  }

  return new StreamSource(stream, opts, sizeHint);
}
