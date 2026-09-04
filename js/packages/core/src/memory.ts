/**
 * Linear memory: reading it, writing it, and staging bytes into it.
 *
 * Two rules from the engine's side of the contract shape everything here, and
 * both are easy to violate without any symptom until a large input arrives.
 *
 * **The engine owns exactly the pages it grew itself, and nothing else.** Not
 * everything above `__heap_base`. So the binding stages its bytes in pages it
 * grows with `memory.grow`, and the two can never overlap, because `memory.grow`
 * hands each caller a fresh disjoint range whoever calls it. There is
 * deliberately no second allocator over the engine's break pointer here: that is
 * the reintroduction the engine's system layer warns about, and the corruption it
 * causes is silent and looks like a parser bug.
 *
 * **Every view into linear memory dies the moment memory grows.** `memory.grow`
 * replaces `memory.buffer` with a new `ArrayBuffer` and detaches the old one, so
 * a `Uint8Array` cached across *any* call into the engine may be pointing at a
 * detached buffer. The engine grows memory whenever it parses, so this is not an
 * edge case, it is the normal path. Everything below therefore re-derives its
 * view from `memory.buffer` on each use and never hands a long lived view out.
 */

const PAGE = 65536;

/** How much address space to take per grow, so a stream of small allocations is
 *  not a stream of `memory.grow` calls. Regions are never given back, matching
 *  the engine's own arena discipline: the context releases everything at once. */
const REGION_PAGES = 16;

export class WasmHeap {
  readonly memory: WebAssembly.Memory;

  private buffer: ArrayBuffer;
  private u8v: Uint8Array;
  private dvv: DataView;

  /** Bump pointer into the region this binding most recently grew. */
  private cur = 0;
  private end = 0;

  constructor(memory: WebAssembly.Memory) {
    this.memory = memory;
    this.buffer = memory.buffer as ArrayBuffer;
    this.u8v = new Uint8Array(this.buffer);
    this.dvv = new DataView(this.buffer);
  }

  /** Re-derives the cached views when linear memory has been replaced. One
   *  identity compare on the hot path, which is what makes it safe to call this
   *  before every single read. */
  private sync(): void {
    const buf = this.memory.buffer as ArrayBuffer;
    if (buf !== this.buffer) {
      this.buffer = buf;
      this.u8v = new Uint8Array(buf);
      this.dvv = new DataView(buf);
    }
  }

  get u8(): Uint8Array {
    this.sync();
    return this.u8v;
  }

  get view(): DataView {
    this.sync();
    return this.dvv;
  }

  /* ------------------------------------------------------------- staging -- */

  /**
   * Grows linear memory and returns the byte offset of the fresh region.
   *
   * This is the only place the binding calls `memory.grow`, and the returned
   * range is disjoint from anything the engine has or will allocate.
   */
  private growRegion(bytes: number): void {
    const pages = Math.max(REGION_PAGES, Math.ceil(bytes / PAGE));
    const prev = this.memory.grow(pages);
    if (prev === -1) throw new RangeError('WebAssembly.Memory.grow failed');
    this.cur = prev * PAGE;
    this.end = this.cur + pages * PAGE;
  }

  /**
   * Reserves `bytes` of host owned memory. Never freed individually: like the
   * engine's arena, the whole instance is discarded at once. Callers that need a
   * large buffer more than once should keep the pointer rather than re-alloc.
   */
  alloc(bytes: number, align = 8): number {
    const need = bytes || 1;
    let at = (this.cur + (align - 1)) & ~(align - 1);
    if (at + need > this.end) {
      this.growRegion(need + align);
      at = (this.cur + (align - 1)) & ~(align - 1);
    }
    this.cur = at + need;
    // The engine reads uninitialized option struct fields as whatever is there,
    // and a fresh page is zero but a reused region is not. Zeroing here means a
    // caller that sets six of a struct's nine fields still gets defined
    // behaviour for the other three.
    this.u8.fill(0, at, at + need);
    return at;
  }

  /** Copies bytes into host owned memory and returns where they landed. */
  allocBytes(bytes: Uint8Array): number {
    const at = this.alloc(bytes.length || 1, 1);
    if (bytes.length) this.u8.set(bytes, at);
    return at;
  }

  /** Copies a NUL terminated UTF-8 string in, for the few options that take one. */
  allocCString(s: string): number {
    const enc = new TextEncoder().encode(s);
    const at = this.alloc(enc.length + 1, 1);
    const mem = this.u8;
    mem.set(enc, at);
    mem[at + enc.length] = 0;
    return at;
  }

  /* -------------------------------------------------------------- scalars -- */

  u8At(ptr: number): number {
    return this.view.getUint8(ptr);
  }

  i32At(ptr: number): number {
    return this.view.getInt32(ptr, true);
  }

  u32At(ptr: number): number {
    return this.view.getUint32(ptr, true);
  }

  /**
   * A 64 bit engine counter as a JS number.
   *
   * Every one of these is a byte count or a cell count, and the largest input the
   * engine accepts is 150 MB, so none of them can reach 2^53. Returning a number
   * rather than a bigint keeps them usable in ordinary arithmetic and JSON, and
   * the assertion below means a future limit change surfaces as an error rather
   * than as a silently rounded count.
   */
  u64At(ptr: number): number {
    const v = this.view.getBigUint64(ptr, true);
    if (v > 9007199254740991n) {
      throw new RangeError(`engine counter ${v} exceeds Number.MAX_SAFE_INTEGER`);
    }
    return Number(v);
  }

  /** A pointer field. wasm32, so four bytes; the loader asserts that. */
  ptrAt(ptr: number): number {
    return this.view.getUint32(ptr, true);
  }

  setU8(ptr: number, v: number): void {
    this.view.setUint8(ptr, v);
  }

  setI32(ptr: number, v: number): void {
    this.view.setInt32(ptr, v, true);
  }

  setU32(ptr: number, v: number): void {
    this.view.setUint32(ptr, v >>> 0, true);
  }

  setU64(ptr: number, v: number): void {
    this.view.setBigUint64(ptr, BigInt(v), true);
  }

  setPtr(ptr: number, v: number): void {
    this.view.setUint32(ptr, v >>> 0, true);
  }

  /* --------------------------------------------------------------- bytes -- */

  /** A copy of a range. Copied rather than viewed on purpose: a view would be
   *  detached by the next parse and the failure would come much later. */
  bytesAt(ptr: number, len: number): Uint8Array {
    return this.u8.slice(ptr, ptr + len);
  }

  /** Reads a NUL terminated string, which is how every engine message arrives. */
  cstringAt(ptr: number): string {
    if (ptr === 0) return '';
    const mem = this.u8;
    let end = ptr;
    while (mem[end] !== 0) end++;
    return utf8.decode(mem.subarray(ptr, end));
  }
}

/** Shared decoders. Constructing a TextDecoder per cell would cost more than the
 *  decode; the fatal one is how invalid UTF-8 is detected exactly rather than by
 *  scanning the result for U+FFFD, which cannot tell a replacement from a
 *  replacement character the file genuinely contained. */
export const utf8 = new TextDecoder('utf-8');
export const utf8Strict = new TextDecoder('utf-8', { fatal: true });
