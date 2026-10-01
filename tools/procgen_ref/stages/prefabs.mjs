// Stage "prefabs": the furniture prefab tables (buildings/interior/prefabs.js PREFABS,
// civicPrefabs.js CIVIC_PREFABS, civicRules.js RULE_PREFABS): every prefab's record, and its
// boxes built from sample streams and sample options (every option present or not, true or
// false); a parked car's boxes (carBoxes).
import { REF, f, line, samples } from "../lib/rec.mjs";

const P = await import(REF + "buildings/interior/prefabs.js");
const C = await import(REF + "buildings/interior/civicPrefabs.js");
const R = await import(REF + "buildings/interior/civicRules.js");
const { Rng } = await import(REF + "core/hash.js");

const box = (q) => [q.a0, q.a1, q.b0, q.b1, q.z0, q.z1, q.m];
const tri = (x) => (x < 1 / 3 ? undefined : x < 2 / 3);
const OPTS = ["w", "d", "monitor", "hood", "upper", "screen", "metal", "steel", "double", "band", "piano", "goods", "h", "top", "curtain", "seat", "frame"];
const fopt = (o) => OPTS.map((k) => f(o[k])).join(",");

// options from 28 draws
function sampleOpts(r, pf, key) {
  const v = [];
  for (let k = 0; k < 28; k += 1) v.push(r());
  const o = {};
  if (pf.w === 0 || v[0] < 0.5) o.w = 3 + Math.floor(v[1] * 45);
  if (pf.d === 0 || v[2] < 0.5) o.d = 3 + Math.floor(v[3] * 25);
  const flags = ["monitor", "hood", "upper", "screen", "metal", "steel", "double", "band", "piano"];
  flags.forEach((name, k) => {
    const t = tri(v[4 + k]);
    if (t !== undefined) o[name] = t;
  });
  if (v[13] < 0.5) o.goods = key === "goodsShelf" ? [1 + Math.floor(v[14] * 399), ...(v[15] < 0.5 ? [1 + Math.floor(v[16] * 399)] : [])] : 1 + Math.floor(v[14] * 399);
  if (v[17] < 0.5) o.h = 6 + Math.floor(v[18] * 50);
  if (v[19] < 0.4) o.top = 1 + Math.floor(v[20] * 399);
  if (v[21] < 0.4) o.curtain = 1 + Math.floor(v[22] * 399);
  if (v[23] < 0.4) o.seat = 1 + Math.floor(v[24] * 399);
  if (v[25] < 0.4) o.frame = 1 + Math.floor(v[26] * 399);
  return o;
}

export default function* prefabs() {
  const r = samples(9);
  for (const [t, table] of [["P", P.PREFABS], ["C", C.CIVIC_PREFABS], ["R", R.RULE_PREFABS]]) {
    for (const [key, pf] of Object.entries(table)) {
      yield line("pf", t, key, pf.w, pf.d, !!pf.tall, !!pf.free, !!pf.flat, pf.pad, pf.dExtra);
      for (let i = 0; i < 24; i += 1) {
        const o = sampleOpts(r, pf, key);
        const rng = new Rng(Math.floor(r() * 4294967296));
        const boxes = pf.build(rng, o);
        yield line("b", key, i, fopt(o), boxes.length, ...boxes.flatMap(box), rng.next());
      }
    }
  }
  for (let i = 0; i < 80; i += 1) {
    const rng = new Rng(Math.floor(r() * 4294967296));
    yield line("car", i, ...P.carBoxes(rng).flatMap(box), rng.next());
  }
}
