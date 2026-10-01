// Record helpers for the conformance stages: one line per record, fields separated by spaces,
// numbers as String(x), strings as they are (the generator's ids hold no spaces), arrays in [].
export const REF = new URL("../../../reference/voxel_city/src/engine/", import.meta.url).href;

/** A field: numbers as JavaScript prints them, booleans as 1 / 0, null / undefined as "-". */
export function f(v) {
  if (v === null || v === undefined) return "-";
  if (typeof v === "boolean") return v ? "1" : "0";
  if (typeof v === "number") return String(v);
  if (typeof v === "string") return v === "" ? '""' : v;
  if (ArrayBuffer.isView(v) || Array.isArray(v)) return `[${Array.from(v, f).join(",")}]`;
  if (typeof v === "object") return `{${Object.keys(v).map((k) => `${k}:${f(v[k])}`).join(",")}}`;
  return String(v);
}

/** A line: its fields, space-separated. */
export const line = (...fields) => fields.map(f).join(" ");

/** A deterministic stream of sample numbers (an LCG, the same in tests/city/records.hpp). */
export function samples(seed) {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 4294967296;
  };
}
