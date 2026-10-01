// Stage "complex": underground complexes (sites/complex.js) - the COMPLEX_THEMES registry, segRect,
// shapeRects over every shape, stair shafts (mkShaft) and their boxes (emitShaft), and complexes
// planned (planComplex) from specs in the shapes the site kinds give it (lib/complexes.mjs: a
// base's sector, a campus's four with a tram, a stronghold's two) and random ones: every sector,
// level, room, corridor, shaft, ladder and tram station; the box lists emitComplex writes from
// the same stream; the structure finishStructure (sites/kit.js) makes of them, rasterized as
// siteSource does into ground-filled chunks at LODs 0, 2, 3 (the deep rooms culled) and 5.
import { REF, line, samples } from "../lib/rec.mjs";
import { boxLines, complexLines, complexPoints, digest, drawSpec, groundChunk, rasterizeStructure } from "../lib/complexes.mjs";

const C = await import(REF + "sites/complex.js");
const { finishStructure } = await import(REF + "sites/kit.js");
const { Rng } = await import(REF + "core/hash.js");

const SHAPES = [undefined, "rect", "octagon", "round", "cross"];

export default function* complex() {
  const r = samples(41);
  for (const t of C.COMPLEX_THEMES.all()) {
    const big = [];
    for (let k = 0; k < 4; k += 1) big.push(t.big(k, false), t.big(k, true));
    yield line("theme", t.id, t.weights.map(([id, w]) => `${id}:${w}`).join(","), big.join(","), t.frame, t.accent, t.shapes);
  }
  yield line("gap", C.LEVEL_GAP);
  for (let i = 0; i < 300; i += 1) {
    const x0 = Math.floor((r() - 0.5) * 2000);
    const y0 = Math.floor((r() - 0.5) * 2000);
    const x1 = r() < 0.2 ? x0 : Math.floor((r() - 0.5) * 2000);
    const y1 = r() < 0.2 ? y0 : Math.floor((r() - 0.5) * 2000);
    const w = r() < 0.5 ? 24 : 1 + Math.floor(r() * 60);
    const q = C.segRect(x0, y0, x1, y1, w);
    yield line("seg", x0, y0, x1, y1, w, q.x0, q.y0, q.x1, q.y1);
  }
  for (let i = 0; i < 400; i += 1) {
    const x0 = Math.floor((r() - 0.5) * 4000);
    const y0 = Math.floor((r() - 0.5) * 4000);
    const big = r() < 0.7;
    const q = { x0, y0, x1: x0 + Math.floor(r() * (big ? 300 : 12)), y1: y0 + Math.floor(r() * (big ? 300 : 12)), shape: SHAPES[i % 5] };
    yield line("shape", q.x0, q.y0, q.x1, q.y1, q.shape, C.shapeRects(q).map((s) => `${s.x0},${s.y0},${s.x1},${s.y1}`).join(";") || "none");
  }
  for (let i = 0; i < 120; i += 1) {
    const n = 1 + Math.floor(r() * 5);
    const zs = [];
    for (let k = 0; k < n; k += 1) zs.push(Math.floor((r() - 0.5) * 600));
    const cx = Math.floor((r() - 0.5) * 4000);
    const cy = Math.floor((r() - 0.5) * 4000);
    const rect = { x0: cx, y0: cy, x1: cx + 16, y1: cy + 45 };
    const dir = r() < 0.5 ? -1 : 1;
    const t = r();
    const sh = t < 0.3 ? C.mkShaft(rect, zs) : C.mkShaft(rect, zs, dir, t < 0.65);
    yield line("mk", zs.join(","), sh.dir, sh.openTop, sh.zLow, sh.zHigh, sh.levels.join(","), sh.st.f0, sh.st.f1, sh.st.flights.map((fl) => `${fl.f}/${fl.z0}/${fl.H}`).join(",") || "none");
    const det = [];
    C.emitShaft(det, sh);
    yield* boxLines("D", det);
  }
  const kinds = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2];
  for (let k = 0; k < 24; k += 1) kinds.push(3);
  kinds.push(4, 4, 4, 4, 5, 5, 5, 5, 5, 5);
  for (let n = 0; n < kinds.length; n += 1) {
    const { spec, top } = drawSpec(r, kinds[n]);
    const seed = Math.floor(r() * 4294967296);
    const rng = new Rng(seed);
    const cx = C.planComplex(rng, spec);
    yield line("complex", n, kinds[n], seed, top);
    yield* complexLines(cx);
    const lists = { shells: [], carves: [], details: [] };
    C.emitComplex(lists, cx, rng);
    yield line("rng", rng.next());
    // (every box of one complex of each kind, digests of 32 boxes elsewhere)
    const full = n === 0 || n === 10 || n === 18 || n === 28 || n === 52;
    yield* boxLines("S", lists.shells, full);
    yield* boxLines("C", lists.carves, full);
    yield* boxLines("D", lists.details, full);
    const st = finishStructure(lists);
    const bb = st.bb;
    yield line("bb", st.boxes.length, bb ? [bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1] : "-");
    // chunks round points of the complex (and one at the top of its first shaft, at LOD 3:
    // what reaches up to the ground stays, the deep rooms are culled)
    const pts = complexPoints(cx);
    const lods = [0, 0, 0, 0, 0, 0, 2, 2, 3, 5];
    if (cx.shafts.length) lods.push(-3);
    for (const l of lods) {
      const lod = Math.abs(l);
      let [x, y, z] = l < 0 ? [cx.shafts[0].rect.x0 + 8, cx.shafts[0].rect.y0 + 20, cx.shafts[0].zHigh] : pts[Math.floor(r() * pts.length)];
      const e = 32 << lod;
      const ccx = Math.floor((x + Math.floor((r() - 0.5) * e * 0.5)) / e);
      const ccy = Math.floor((y + Math.floor((r() - 0.5) * e * 0.5)) / e);
      const ccz = Math.floor((z + Math.floor((r() - 0.5) * e * 0.5)) / e);
      const ch = groundChunk(lod, ccx, ccy, ccz, top - 1);
      rasterizeStructure(st, ch, lod >= 3 ? top - 1 - 16 : -Infinity);
      yield line("ch", lod, ccx, ccy, ccz, ch.countNonAir(), digest(ch.data));
    }
  }
}
