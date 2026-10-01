import { hash32 } from "../core/hash.js";

/** A 52-bit id of structural keys (strings or numbers): the same everywhere, safe as a JS number and a u64. */
export function idOf(...keys) {
  let a = 0x9e3779b1;
  let b = 0x85ebca77;
  for (const k of keys) {
    const s = String(k);
    for (let i = 0; i < s.length; i += 1) {
      a = hash32(a, s.charCodeAt(i), 17);
      b = hash32(b, s.charCodeAt(i), 29);
    }
    a = hash32(a, 0x2f, 3);
    b = hash32(b, 0x2f, 5);
  }
  return (a & 0xfffff) * 4294967296 + (b >>> 0);
}
