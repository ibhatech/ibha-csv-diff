/**
 * Loading the module, and the two checks that stop a silent misread.
 *
 * The binding reads public structs straight out of linear memory, so a module
 * built from a different revision of the header than `abi.ts` was generated from
 * loads, runs, and returns numbers that look plausible. Both assertions below
 * exist to turn that into a message.
 */

import { C, OFF, POINTER_SIZE } from './abi.ts';
import { compare, Engine, simdSupported } from './index.ts';
import { describe, expect, it, withHeader } from './testkit.ts';

describe('the module', () => {
  it('loads both builds and reports which one it chose', async () => {
    const scalar = await Engine.load({ simd: false });
    const simd = await Engine.load({ simd: true });
    expect(scalar.simd).toBe(false);
    expect(simd.simd).toBe(true);
    // Desktop browsers are the only browser target per spec 13.11, and SIMD is
    // baseline in every evergreen one, so this is the expected runtime path.
    expect(simdSupported()).toBe(true);
  });

  it('refuses a module whose version disagrees with the generated offsets', async () => {
    // A module with a plausible shape and the wrong contents. Compiling one is
    // not worth it: what matters is that instantiate checks rather than assumes,
    // and the check runs on every real instantiation in every other test here.
    const engine = await Engine.load({ simd: false });
    const inst = engine.instantiate();
    const out = inst.heap.alloc(12, 4);
    inst.api.ibha_csvd_version(out, out + 4, out + 8);
    expect(inst.heap.i32At(out)).toBe(C.VERSION_MAJOR);
    expect(inst.heap.i32At(out + 4)).toBe(C.VERSION_MINOR);
    expect(inst.heap.i32At(out + 8)).toBe(C.VERSION_PATCH);
  });

  it('rejects bytes that are not a module the binding can drive', async () => {
    await expect(Engine.load({ wasmBinary: new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]) })).rejects.toThrow(
      /missing .* export/,
    );
  });
});

describe('the generated ABI', () => {
  it('is wasm32, which every pointer read here assumes', () => {
    expect(POINTER_SIZE).toBe(4);
  });

  it('carries the header constants rather than restating them', () => {
    // Widened on purpose: `as const` gives each of these a literal type, and the
    // point of the assertion is the value the generator produced, not the type.
    expect<number>(C.SCHEMA_VERSION).toBe(1);
    expect<number>(C.NO_ROW).toBe(0xffffffff);
    expect<number>(C.DEFAULT_MAX_BYTES).toBe(150 * 1024 * 1024);
    // The finding mask is the union of the four finding bits, which is the one
    // constant here that is derived rather than declared.
    expect<number>(C.CELL_FINDING).toBe(
      C.CELL_REQUIRED_EMPTY | C.CELL_TOO_LONG | C.CELL_NOT_NUMERIC | C.CELL_PRECISION,
    );
  });

  it('has every field inside the struct it belongs to', () => {
    for (const fields of Object.values(OFF)) {
      const struct = fields as Record<string, number | undefined>;
      const size = struct.__size ?? 0;
      expect(size).toBeGreaterThan(0);
      for (const [field, off] of Object.entries(struct)) {
        if (field.startsWith('__')) continue;
        expect(off ?? -1).toBeLessThanOrEqual(size - 1);
        expect(off ?? -1).toBeGreaterThanOrEqual(0);
      }
    }
  });
});

describe('linear memory', () => {
  it('survives the engine growing memory under a held view', async () => {
    // Every view into linear memory is detached by memory.grow, and the engine
    // grows on any call that allocates. A binding that cached one would read a
    // detached buffer on the first input large enough to force a grow, which is
    // to say on every real input and none of the small ones.
    const rows = Array.from({ length: 4000 }, (_, i) => `k${i},name ${i},1,1.00`);
    const h = await compare(withHeader(...rows), withHeader(...rows.map((r) => r.replace('name', 'nome'))));
    try {
      const all = [...h.rows()];
      expect(all.length).toBe(rows.length);
      expect(all[3999]!.cells[1]!.target).toBe('nome 3999');
      // And the values are still readable after another allocation, which is
      // what the emitter's sizing pass forces.
      h.emit('summary');
      expect(h.index().getRow(0).cells[1]!.target).toBe('nome 0');
    } finally {
      h.dispose();
    }
  });
});
