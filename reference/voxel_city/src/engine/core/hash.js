/**
 * Deterministic hashing and seeded random streams.
 *
 * Every generated entity derives its own seed from the world seed plus a
 * stable structural key (cell coordinates, entity ids). Nothing ever shares a
 * single sequential RNG across unrelated entities, so generation order never
 * changes results — the property that makes lazy, streamed generation work.
 */

const M1 = 0x85ebca6b;
const M2 = 0xc2b2ae35;

/** Finalizer from murmur3: good avalanche for 32-bit ints. */
export function mix32(h) {
  h ^= h >>> 16;
  h = Math.imul(h, M1);
  h ^= h >>> 13;
  h = Math.imul(h, M2);
  h ^= h >>> 16;
  return h >>> 0;
}

/** Hash any number of integers into a uint32. */
export function hash32(a, b = 0, c = 0, d = 0) {
  let h = mix32((a | 0) ^ 0x9e3779b9);
  h = mix32(h ^ Math.imul(b | 0, 0x27d4eb2d));
  h = mix32(h ^ Math.imul(c | 0, 0x165667b1));
  h = mix32(h ^ Math.imul(d | 0, 0x61c88647));
  return h;
}

/** Hash to float in [0, 1). */
export function hashFloat(a, b = 0, c = 0, d = 0) {
  return hash32(a, b, c, d) / 4294967296;
}

/** FNV-1a over a string, then mixed. */
export function hashString(str) {
  let h = 2166136261;
  for (let i = 0; i < str.length; i += 1) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return mix32(h);
}

/**
 * Derive a seed from a root seed and any mix of string / number parts.
 * `deriveSeed(world, "building", id)` is the canonical pattern.
 */
export function deriveSeed(root, ...parts) {
  let h = mix32((root | 0) ^ 0x5bd1e995);
  for (const part of parts) {
    const v = typeof part === "number" ? mix32(part | 0) ^ mix32(Math.floor(part * 65536) | 0) : hashString(String(part));
    h = mix32(h ^ v ^ (h << 6));
  }
  return h;
}

/**
 * Small, fast seeded PRNG (sfc32). Methods are the vocabulary used across the
 * generators; keep them stable because every layout depends on call order
 * within one entity.
 */
export class Rng {
  constructor(seed) {
    this.a = seed >>> 0;
    this.b = mix32(seed ^ 0xdeadbeef);
    this.c = mix32(seed ^ 0x41c64e6d);
    this.d = 1;
    for (let i = 0; i < 12; i += 1) this.next();
  }

  static from(root, ...parts) {
    return new Rng(deriveSeed(root, ...parts));
  }

  /** float in [0,1) */
  next() {
    let { a, b, c, d } = this;
    const t = (((a + b) | 0) + d) | 0;
    d = (d + 1) | 0;
    a = b ^ (b >>> 9);
    b = (c + (c << 3)) | 0;
    c = (c << 21) | (c >>> 11);
    c = (c + t) | 0;
    this.a = a;
    this.b = b;
    this.c = c;
    this.d = d;
    return (t >>> 0) / 4294967296;
  }

  float(min = 0, max = 1) {
    return min + (max - min) * this.next();
  }

  /** integer in [min, max] inclusive */
  int(min, max) {
    return min + Math.floor(this.next() * (max - min + 1));
  }

  chance(p) {
    return this.next() < p;
  }

  sign() {
    return this.next() < 0.5 ? -1 : 1;
  }

  pick(items) {
    return items[Math.floor(this.next() * items.length)];
  }

  /** pairs: [[value, weight], ...] or objects with .weight */
  weighted(pairs) {
    let total = 0;
    for (const p of pairs) total += Array.isArray(p) ? p[1] : p.weight;
    let r = this.next() * total;
    for (const p of pairs) {
      const w = Array.isArray(p) ? p[1] : p.weight;
      if (r < w) return Array.isArray(p) ? p[0] : p;
      r -= w;
    }
    const last = pairs[pairs.length - 1];
    return Array.isArray(last) ? last[0] : last;
  }

  shuffle(items) {
    const out = items.slice();
    for (let i = out.length - 1; i > 0; i -= 1) {
      const j = Math.floor(this.next() * (i + 1));
      const tmp = out[i];
      out[i] = out[j];
      out[j] = tmp;
    }
    return out;
  }

  /** approx normal distribution via sum of uniforms */
  gauss(mean = 0, sd = 1) {
    const u = this.next() + this.next() + this.next() + this.next() - 2;
    return mean + u * sd * 0.8660254;
  }

  /** Spawn an independent child stream keyed by a label. */
  fork(label) {
    return new Rng(deriveSeed(this.a ^ this.c, label));
  }
}
