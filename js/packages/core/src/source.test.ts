import { describe, expect, it } from './testkit.ts';

import { IbhaCsvError, sniffNotCsv, toByteSource } from './source.ts';

const enc = (s: string) => new TextEncoder().encode(s);

async function drain(src: Awaited<ReturnType<typeof toByteSource>>): Promise<Uint8Array> {
  const chunks: Uint8Array[] = [];
  for (;;) {
    const c = await src.read();
    if (!c) break;
    chunks.push(c);
  }
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

describe('sniffNotCsv', () => {
  it('flags an HTML login page, which is the common expired-session failure', () => {
    expect(sniffNotCsv(enc('<!DOCTYPE html><html><body>Sign in'))).toContain('HTML');
    expect(sniffNotCsv(enc('<html lang="en">'))).toContain('HTML');
  });

  it('flags a JSON error body', () => {
    expect(sniffNotCsv(enc('{"message":"Unauthorized"}'))).toContain('JSON');
    expect(sniffNotCsv(enc('[]'))).toContain('JSON');
  });

  it('looks past a BOM and leading whitespace before judging', () => {
    expect(sniffNotCsv(enc('﻿<!doctype html>'))).toContain('HTML');
    expect(sniffNotCsv(enc('\n\n  <html>'))).toContain('HTML');
  });

  it('accepts anything that could plausibly be a CSV', () => {
    // The check must be narrow. A false positive here rejects a valid file.
    expect(sniffNotCsv(enc('id,name\n1,a\n'))).toBeNull();
    expect(sniffNotCsv(enc('"quoted",header\n'))).toBeNull();
    expect(sniffNotCsv(enc('KEY,KEY,,\n'))).toBeNull();
    expect(sniffNotCsv(enc(''))).toBeNull();
    // A cell that merely contains a tag is not a document starting with one.
    expect(sniffNotCsv(enc('id,note\n1,"see <b>here</b>"\n'))).toBeNull();
  });
});

describe('toByteSource', () => {
  it('accepts a Uint8Array and reports a size hint', async () => {
    const src = await toByteSource(enc('a,b\n1,2\n'));
    expect(src.sizeHint).toBe(8);
    expect(new TextDecoder().decode(await drain(src))).toBe('a,b\n1,2\n');
  });

  it('accepts an ArrayBuffer', async () => {
    const bytes = enc('x,y\n');
    const src = await toByteSource(bytes.buffer.slice(0) as ArrayBuffer);
    expect(new TextDecoder().decode(await drain(src))).toBe('x,y\n');
  });

  it('accepts a Blob and takes its size as the hint', async () => {
    const src = await toByteSource(new Blob([enc('p,q\n7,8\n')]));
    expect(src.sizeHint).toBe(8);
    expect(new TextDecoder().decode(await drain(src))).toBe('p,q\n7,8\n');
  });

  it('accepts a callback returning a Response, which is the recommended form', async () => {
    const src = await toByteSource(
      async () => new Response(enc('id\n1\n'), { headers: { 'content-type': 'text/csv' } }),
    );
    expect(new TextDecoder().decode(await drain(src))).toBe('id\n1\n');
  });

  it('does not care about Content-Type, including application/octet-stream', async () => {
    // S3 presigned URLs commonly serve octet-stream. Rejecting on Content-Type
    // would break a normal deployment, so it must be ignored entirely.
    for (const ct of ['application/octet-stream', 'text/plain', 'application/vnd.ms-excel', '']) {
      const src = await toByteSource(
        async () => new Response(enc('a\n1\n'), { headers: ct ? { 'content-type': ct } : {} }),
      );
      expect(new TextDecoder().decode(await drain(src))).toBe('a\n1\n');
    }
  });

  it('rejects a non-ok Response by naming the status', async () => {
    await expect(
      toByteSource(async () => new Response('nope', { status: 401, statusText: 'Unauthorized' })),
    ).rejects.toThrow(/HTTP 401/);
  });

  it('fails with BAD_CONTENT when the body is an HTML error page', async () => {
    const src = await toByteSource(async () => new Response(enc('<!DOCTYPE html><html>login')));
    await expect(drain(src)).rejects.toMatchObject({ code: 'BAD_CONTENT' });
  });

  it('allows the sniff to be disabled', async () => {
    const src = await toByteSource(async () => new Response(enc('<!DOCTYPE html>')), {
      sniffErrorResponses: false,
    });
    expect(new TextDecoder().decode(await drain(src))).toBe('<!DOCTYPE html>');
  });

  it('enforces maxBytes as bytes arrive, not after buffering', async () => {
    const big = new Uint8Array(4 * 1024 * 1024).fill(97);
    const src = await toByteSource(big, { maxBytes: 1024 * 1024 });
    await expect(drain(src)).rejects.toMatchObject({ code: 'TOO_LARGE' });
  });

  it('states both the limit and what was seen in the TOO_LARGE message', async () => {
    const big = new Uint8Array(3 * 1024 * 1024).fill(97);
    const src = await toByteSource(big, { maxBytes: 1024 * 1024 });
    await expect(drain(src)).rejects.toThrow(/Maximum allowed is 1 MB/);
  });

  it('accepts input exactly at the limit', async () => {
    const exact = new Uint8Array(1024).fill(97);
    const src = await toByteSource(exact, { maxBytes: 1024 });
    expect((await drain(src)).length).toBe(1024);
  });

  it('treats a zero length chunk as a no-op rather than end of stream', async () => {
    // A legal but awkward producer. Mistaking an empty chunk for EOF would
    // silently truncate the file, which is the worst failure mode here.
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        c.enqueue(enc('a,b\n'));
        c.enqueue(new Uint8Array(0));
        c.enqueue(enc('1,2\n'));
        c.close();
      },
    });
    const src = await toByteSource(stream);
    expect(new TextDecoder().decode(await drain(src))).toBe('a,b\n1,2\n');
  });

  it('ignores Content-Length when Content-Encoding is present', async () => {
    // Content-Length describes the compressed size there, so using it as a hint
    // would under-allocate the retain buffer.
    const src = await toByteSource(
      async () =>
        new Response(enc('a,b\n'), {
          headers: { 'content-length': '4', 'content-encoding': 'gzip' },
        }),
    );
    expect(src.sizeHint).toBeUndefined();
  });

  it('rejects an unsupported source with a message that lists what is accepted', async () => {
    await expect(toByteSource(42 as never)).rejects.toMatchObject({ code: 'INVALID_ARG' });
    await expect(toByteSource(42 as never)).rejects.toThrow(/Uint8Array/);
  });

  it('surfaces a source error rather than truncating', async () => {
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        c.enqueue(enc('a,b\n'));
        c.error(new Error('socket reset'));
      },
    });
    const src = await toByteSource(stream);
    await expect(drain(src)).rejects.toThrow(/socket reset/);
  });
});
