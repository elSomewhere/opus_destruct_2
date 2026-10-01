// Stage "chamfer": a building's chamfer (buildings/chamfer.js): chamferCut over every cell of
// sample buildings (as runs, row by row) and chamferHits on sample rects, inside the footprint
// and beyond it.
import { REF, line, samples } from "../lib/rec.mjs";

const { chamferCut, chamferHits } = await import(REF + "buildings/chamfer.js");

export default function* chamfer() {
  const r = samples(3);
  for (let i = 0; i < 80; i += 1) {
    const k = 1 + Math.floor(r() * 4);
    const swap = r() < 0.5;
    const side = r() < 0.5 ? "L" : "R";
    const c = { side, a: (swap ? 21 : 20) * k, b: (swap ? 20 : 21) * k };
    const U = c.a + 4 + Math.floor(r() * 70);
    const V = c.b + 4 + Math.floor(r() * 50);
    const runs = [];
    for (let v = 0; v < V; v += 1) {
      let u = 0;
      while (u < U) {
        const cut = chamferCut(c, U, u, v);
        let n = 0;
        while (u < U && chamferCut(c, U, u, v) === cut) {
          n += 1;
          u += 1;
        }
        runs.push(`${cut ? 1 : 0}:${n}`);
      }
    }
    yield line("ch", i, c.side, c.a, c.b, U, V, runs.join(","));
    const hits = [];
    for (let q = 0; q < 60; q += 1) {
      const x0 = Math.floor((r() - 0.3) * (U + 20));
      const y0 = Math.floor((r() - 0.3) * (V + 20));
      const rect = { x0, y0, x1: x0 + Math.floor(r() * 30), y1: y0 + Math.floor(r() * 30) };
      hits.push(`${rect.x0},${rect.y0},${rect.x1},${rect.y1}:${chamferHits(c, U, rect) ? 1 : 0}`);
    }
    yield line("hits", i, hits.join(" "));
  }
}
