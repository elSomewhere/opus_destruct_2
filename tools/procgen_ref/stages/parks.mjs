// Stage "parks": park layouts (city/parks.js parkLayout) of synthetic park spaces - slivers, small
// parks with no entrance of their own, parks with one and two entrances a side, long ones, big
// ones with ponds - every field of each layout (the hub, the entrances, the pond, every path
// segment, the lookup grid's buckets); pathDist, groveAt, pondDist and parkSurface at points over
// and round each park (inside and out, on whole and fractional voxels, by the hub, the pond's shore
// and the paths), with canonical texture positions of their own now and then.
// tests/city/test_parks.cpp writes the same records.
import { REF, f, line, samples } from "../lib/rec.mjs";

const P = await import(REF + "city/parks.js");

/** The sizes (voxels) of the k-th park: [w0, w1, h0, h1] by k % 6. */
const SIZES = [
  [0, 100, 0, 100],
  [100, 300, 100, 300],
  [280, 720, 280, 720],
  [480, 1200, 480, 1200],
  [40, 300, 600, 1500],
  [700, 1600, 700, 1600],
];

const fseg = (s) => s.map(f).join(",");

/** Degenerate parks after the drawn ones: a point, flat and thin rects (zero-length paths). */
const DEGENERATE = [
  [0, 0],
  [50, 0],
  [0, 50],
  [0, 800],
  [1, 1],
  [900, 0],
];

export default function* parks() {
  const r = samples(29);
  for (let k = 0; k < 240 + DEGENERATE.length; k += 1) {
    const [w0, w1, h0, h1] = SIZES[k % SIZES.length];
    const i = Math.floor((r() - 0.5) * 100);
    const j = Math.floor((r() - 0.5) * 100);
    const x0 = Math.floor((r() - 0.5) * 600000);
    const y0 = Math.floor((r() - 0.5) * 600000);
    const w = k < 240 ? w0 + Math.floor(r() * (w1 - w0)) : DEGENERATE[k - 240][0];
    const h = k < 240 ? h0 + Math.floor(r() * (h1 - h0)) : DEGENERATE[k - 240][1];
    const space = { id: `C${i}_${j}/b${k}/o`, kind: "park", rect: { x0, y0, x1: x0 + w, y1: y0 + h } };
    const L = P.parkLayout(space);
    if (P.parkLayout(space) !== L) throw new Error("parks: a layout made twice");
    yield line("park", k, space.id, x0, y0, w, h, L.W, L.H, L.seed, L.hub.u, L.hub.v, L.hub.r, L.pathW, L.G, L.ents.length, L.segs.length);
    yield line("ents", L.ents.map((e) => `${f(e.u)},${f(e.v)}`).join(";"));
    const pd = L.pond;
    yield pd ? line("pond", pd.u, pd.v, pd.r, ...pd.a, ...pd.p, pd.stretch, pd.rot) : line("pond", null);
    for (let q = 0; q < L.segs.length; q += 20) yield line("segs", L.segs.slice(q, q + 20).map(fseg).join(";"));
    // the lookup grid: every bucket's key and its segments (indices), by key
    const index = new Map(L.segs.map((s, n) => [s, n]));
    const keys = [...L.grid.keys()].sort((a, b) => a - b);
    for (let q = 0; q < keys.length; q += 40) yield line("grid", keys.slice(q, q + 40).map((key) => `${f(key)}:${L.grid.get(key).map((s) => index.get(s)).join(".")}`).join(" "));
    // points: over and round the park, by the hub, the pond's shore and the paths
    const pts = [];
    for (let q = 0; q < 60; q += 1) {
      const u = -0.15 * L.W - 20 + r() * (1.3 * L.W + 40);
      const v = -0.15 * L.H - 20 + r() * (1.3 * L.H + 40);
      pts.push(q % 2 ? [u, v] : [Math.round(u), Math.round(v)]);
    }
    for (let q = 0; q < 20; q += 1) {
      const u = L.hub.u + (r() - 0.5) * 2 * (L.hub.r + 20);
      const v = L.hub.v + (r() - 0.5) * 2 * (L.hub.r + 20);
      pts.push(q % 2 ? [u, v] : [Math.round(u), Math.round(v)]);
    }
    if (pd)
      for (let q = 0; q < 24; q += 1) {
        const a = r() * 2 * Math.PI;
        const d = pd.r * (0.5 + r()) * pd.stretch;
        pts.push([Math.round(pd.u + Math.cos(a) * d), Math.round(pd.v + Math.sin(a) * d)]);
      }
    for (let q = 0; q < 20 && L.segs.length; q += 1) {
      const s = L.segs[Math.floor(r() * L.segs.length)];
      const t = r();
      const u = s[0] + (s[2] - s[0]) * t + (r() - 0.5) * 30;
      const v = s[1] + (s[3] - s[1]) * t + (r() - 0.5) * 30;
      pts.push(q % 2 ? [u, v] : [Math.round(u), Math.round(v)]);
    }
    const out = { mat: 0, dz: 0, water: false };
    const recs = [];
    for (const [u, v] of pts) {
      const lap = r() < 0.3 ? Math.floor((r() - 0.5) * 8) * 25600 : 0;
      out.mat = 0;
      out.dz = 0;
      out.water = false;
      P.parkSurface(space, u, v, out, u + lap, v - lap);
      recs.push(`${f(u)},${f(v)},${f(lap)}:${f(P.pathDist(L, u, v))},${f(P.groveAt(L, u, v))},${pd ? f(P.pondDist(pd, u, v)) : "-"}:${out.mat}/${f(out.dz)}/${f(out.water)}`);
    }
    for (let q = 0; q < recs.length; q += 12) yield line("pts", recs.slice(q, q + 12).join(" "));
  }
}
