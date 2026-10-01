// Stage "blockpoly": polygon blocks (city/blockPoly.js): rect blocks as polygons, split again and
// again by angled and axis-aligned lines; each piece's area, centroid, bounds and block record
// (rect, sides, cuts); insideCuts at sample points and trimToCuts of sample rects.
import { REF, f, line, samples } from "../lib/rec.mjs";

const B = await import(REF + "city/blockPoly.js");

const DIRS = [[1, 0], [0, 1], [3, 4], [-4, 3], [20, 21], [21, -20], [5, -12], [1, 1], [-7, 24], [0, -1]];
const CLS = ["street", "avenue", "alley"];

function side(r) {
  const k = Math.floor(r() * 4);
  const hr = 8 + Math.floor(r() * 30);
  const id = `r${Math.floor(r() * 1000)}`;
  return k === 0 ? { cls: null, hr: 0, id: null } : { cls: CLS[k - 1], hr, id: k === 3 ? null : id };
}
const fs = (s) => `${f(s.cls)}/${f(s.hr)}/${f(s.id)}`;
const fpoly = (p) => (p ? p.map((q) => `${f(q.x)},${f(q.y)},${fs(q.side)}`).join(";") : "-");
const fcut = (k) => [k.nx, k.ny, k.c, k.cls, k.hr, k.id].map(f).join(",");

export default function* blockpoly() {
  const r = samples(17);
  for (let i = 0; i < 160; i += 1) {
    const x0 = Math.floor((r() - 0.5) * 4000);
    const y0 = Math.floor((r() - 0.5) * 4000);
    const w = 30 + Math.floor(r() * 500);
    const h = 30 + Math.floor(r() * 500);
    const rect = { x0, y0, x1: x0 + w, y1: y0 + h };
    const sN = side(r);
    const sE = side(r);
    const sS = side(r);
    const sW = side(r);
    const some = r() < 0.8;
    const poly = B.rectPoly(rect, some ? { N: sN, E: sE, S: sS, W: sW } : { N: sN });
    yield line("rp", i, fpoly(poly), B.polyArea(poly));
    // split a queue of pieces by random lines
    const pieces = [];
    const queue = [poly];
    let splits = 0;
    while (queue.length) {
      const p = queue.shift();
      const go = r() < 0.75 && splits < 6;
      const ax = r();
      const ay = r();
      const dir = DIRS[Math.floor(r() * DIRS.length)];
      const jitter = r() < 0.3 ? r() - 0.5 : 0;
      const cut = side(r);
      if (!go) {
        pieces.push(p);
        continue;
      }
      const bb = B.polyBounds(p);
      const a = { x: bb.x0 + (bb.x1 - bb.x0) * ax, y: bb.y0 + (bb.y1 - bb.y0) * ay };
      const dx = dir[0] + jitter;
      const dy = dir[1];
      const crosses = B.lineCrosses(p, a, dx, dy);
      const [L, R] = B.splitPoly(p, a, dx, dy, cut);
      splits += 1;
      yield line("sp", i, splits, a.x, a.y, dx, dy, crosses, fpoly(L), fpoly(R));
      if (L) queue.push(L);
      if (R) queue.push(R);
      if (!L && !R) pieces.push(p);
    }
    for (const p of pieces) {
      const c = B.polyCentroid(p);
      const bb = B.polyBounds(p);
      const blk = B.polyBlock(p);
      yield line("pc", i, B.polyArea(p), c.x, c.y, bb.x0, bb.y0, bb.x1, bb.y1);
      yield line("blk", i, blk.r.x0, blk.r.y0, blk.r.x1, blk.r.y1, fs(blk.s.N), fs(blk.s.E), fs(blk.s.S), fs(blk.s.W), blk.cuts.map(fcut).join(";"), blk.poly === p);
      const ins = [];
      for (let k = 0; k < 24; k += 1) {
        const x = blk.r.x0 - 5 + r() * (blk.r.x1 - blk.r.x0 + 10);
        const y = blk.r.y0 - 5 + r() * (blk.r.y1 - blk.r.y0 + 10);
        ins.push(B.insideCuts(blk.cuts, x, y) ? 1 : 0);
      }
      yield line("in", i, ins.join(""));
      for (let k = 0; k < 8; k += 1) {
        const qx = blk.r.x0 + Math.floor(r() * (blk.r.x1 - blk.r.x0 + 1));
        const qy = blk.r.y0 + Math.floor(r() * (blk.r.y1 - blk.r.y0 + 1));
        const q = { x0: qx, y0: qy, x1: qx + Math.floor(r() * 120), y1: qy + Math.floor(r() * 120) };
        const t = B.trimToCuts(q, blk.cuts);
        yield line("tr", i, q.x0, q.y0, q.x1, q.y1, t ? [t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1] : "-", t ? t.trimmed.map((s) => `${s.side}/${f(s.cls)}/${f(s.id)}`).join(";") : "-");
      }
    }
  }
}
