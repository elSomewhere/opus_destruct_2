// Stage "propprefabs": the street and industrial prop prefabs (city/propPrefabs.js PROPS, after
// its Object.assign(PROPS, INDUSTRY_PROPS) of city/industry.js): the table's keys, and every
// prop's boxes from sample streams and sample options (each option present or not).
import { REF, f, line, samples } from "../lib/rec.mjs";

const { PROPS } = await import(REF + "city/propPrefabs.js");
const { INDUSTRY_PROPS } = await import(REF + "city/industry.js");
const { Rng } = await import(REF + "core/hash.js");

const box = (q) => [q.a0, q.a1, q.b0, q.b1, q.z0, q.z1, q.m];
const PROP_OPTS = ["h", "reach", "pole", "r", "seed", "n", "hw", "len", "span", "plinth"];
const fextra = (o) => PROP_OPTS.map((k) => f(o[k])).join(",");

// options from 20 draws; an industrial prop always has the sizes it reads (dressing gives them:
// an undefined one would reach a box as it is)
function sampleOpts(r, sized) {
  const v = [];
  for (let k = 0; k < 20; k += 1) v.push(r());
  const o = {};
  if (sized || v[0] < 0.7) o.h = 8 + Math.floor(v[1] * 120);
  if (v[2] < 0.5) o.reach = 3 + Math.floor(v[3] * 30);
  if (v[4] < 0.3) o.pole = 1 + Math.floor(v[5] * 399);
  if (sized || v[6] < 0.7) o.r = 2 + Math.floor(v[7] * 24);
  if (v[8] < 0.7) o.seed = Math.floor(v[9] * 4294967296);
  if (sized || v[10] < 0.7) o.n = 1 + Math.floor(v[11] * 6);
  if (sized || v[12] < 0.7) o.hw = 8 + Math.floor(v[13] * 50);
  if (sized || v[14] < 0.7) o.len = 10 + Math.floor(v[15] * 300);
  if (sized || v[16] < 0.7) o.span = 40 + Math.floor(v[17] * 160);
  if (v[18] < 0.2) o.plinth = v[19] < 0.5;
  return o;
}

export default function* propprefabs() {
  yield line("keys", ...Object.keys(PROPS));
  yield line("industry", ...Object.keys(INDUSTRY_PROPS));
  const r = samples(37);
  for (const [key, build] of Object.entries(PROPS)) {
    const sized = key in INDUSTRY_PROPS;
    for (let i = 0; i < 16; i += 1) {
      const o = i === 0 && !sized ? {} : sampleOpts(r, sized);
      const rng = new Rng(Math.floor(r() * 4294967296));
      const boxes = build(rng, o);
      yield line("p", key, i, fextra(o), boxes.length, ...boxes.flatMap(box), rng.next());
    }
  }
}
