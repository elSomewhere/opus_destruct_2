// Stage "stairs": U-shaped stairs (buildings/interior/stairs.js) - dims, local frames, the near
// landing, slab openings and the stair's boxes over its flights - and the planners' pure helpers
// (buildings/interior/common.js: splitLength, freeIntervals, rectsOverlapAny).
import { REF, line, samples } from "../lib/rec.mjs";

const S = await import(REF + "buildings/interior/stairs.js");
const C = await import(REF + "buildings/interior/common.js");
const { Rng } = await import(REF + "core/hash.js");

const box = (b) => [b.x0, b.y0, b.z0, b.x1, b.y1, b.z1, b.m];

export default function* stairs() {
  const r = samples(5);
  for (let H = 6; H <= 70; H += 1) {
    const d = S.stairDims(H);
    const d2 = S.stairDims(H, 5 + (H % 4), 6 + (H % 5));
    yield line("dims", H, d.W, d.L, d.lane, d.landing, d2.W, d2.L, d2.lane, d2.landing);
  }
  for (let i = 0; i < 300; i += 1) {
    const axis = r() < 0.5 ? "u" : "v";
    const dir = r() < 0.5 ? 1 : -1;
    const laneLow = r() < 0.5;
    const lr = r();
    const lv = r();
    const lane = lr < 0.7 ? 8 : 5 + Math.floor(lv * 6);
    const landing = lr < 0.7 ? 9 : 6 + Math.floor(lv * 5);
    const storyH = 14 + Math.floor(r() * 30);
    const dims = S.stairDims(storyH, lane, landing);
    const x0 = Math.floor((r() - 0.5) * 200);
    const y0 = Math.floor((r() - 0.5) * 200);
    const rect = axis === "v" ? { x0, y0, x1: x0 + dims.W - 1, y1: y0 + dims.L - 1 } : { x0, y0, x1: x0 + dims.L - 1, y1: y0 + dims.W - 1 };
    const f0 = Math.floor(r() * 3) - 1;
    const f1 = f0 + 1 + Math.floor(r() * 4);
    const open = r() < 0.3;
    const st = i % 7 === 0 ? S.makeStair({ rect, axis, dir, f0, f1 }) : S.makeStair({ rect, axis, dir, laneLow, lane, landing, f0, f1, open });
    yield line("st", i, st.rect.x0, st.rect.y0, st.rect.x1, st.rect.y1, st.axis, st.dir, st.laneLow, st.lane, st.landing, st.f0, st.f1, st.L, st.W, st.open);
    const nl = S.nearLanding(st);
    yield line("nl", i, nl.x0, nl.y0, nl.x1, nl.y1);
    const loc = [];
    const slab = [];
    for (let k = 0; k < 30; k += 1) {
      const u = rect.x0 - 2 + Math.floor(r() * (rect.x1 - rect.x0 + 5));
      const v = rect.y0 - 2 + Math.floor(r() * (rect.y1 - rect.y0 + 5));
      const p = S.stairLocal(st, u, v);
      loc.push(p ? `${p.s},${p.t}` : "-");
      for (let f = f0 - 1; f <= f1 + 1; f += 1) slab.push(S.stairSlabOpen(st, f, u, v) ? 1 : 0);
    }
    yield line("loc", i, loc.join(" "), slab.join(""));
    // flights as PlanBuilder.addStair makes them (a story height of each floor)
    st.flights = [];
    for (let f = st.f0; f < st.f1; f += 1) st.flights.push({ f, z0: f * (storyH + 2) + 3, H: storyH + (f & 1 ? 2 : 0) });
    const mats = { tread: 11, landing: 12, divider: 13, rail: 14 };
    yield line("sb", i, ...S.stairBoxes(st, mats).flatMap(box));
  }
  // common.js
  for (let i = 0; i < 400; i += 1) {
    const a0 = Math.floor((r() - 0.5) * 300);
    const a1 = a0 + Math.floor(r() * 400);
    const target = 4 + Math.floor(r() * 80);
    const minW = 2 + Math.floor(r() * 30);
    const jr = r();
    const rng = new Rng(Math.floor(r() * 4294967296));
    const pieces = jr < 0.5 ? C.splitLength(a0, a1, target, minW, rng) : C.splitLength(a0, a1, target, minW, rng, jr);
    yield line("split", a0, a1, target, minW, jr, pieces.map((p) => `${p[0]}:${p[1]}`).join(","), rng.next());
  }
  for (let i = 0; i < 300; i += 1) {
    const a0 = Math.floor((r() - 0.5) * 100);
    const a1 = a0 + Math.floor(r() * 200);
    const n = Math.floor(r() * 6);
    const blocked = [];
    for (let k = 0; k < n; k += 1) {
      const b0 = a0 - 20 + Math.floor(r() * 240);
      blocked.push([b0, b0 + Math.floor(r() * 30)]);
    }
    const free = C.freeIntervals(a0, a1, blocked);
    yield line("free", a0, a1, blocked.map((b) => `${b[0]}:${b[1]}`).join(","), free.map((b) => `${b[0]}:${b[1]}`).join(","));
  }
  for (let i = 0; i < 300; i += 1) {
    const rect = { x0: Math.floor(r() * 100), y0: Math.floor(r() * 100), x1: 0, y1: 0 };
    rect.x1 = rect.x0 + Math.floor(r() * 20);
    rect.y1 = rect.y0 + Math.floor(r() * 20);
    const list = [];
    const n = Math.floor(r() * 5);
    for (let k = 0; k < n; k += 1) {
      const x0 = Math.floor(r() * 120);
      const y0 = Math.floor(r() * 120);
      list.push({ x0, y0, x1: x0 + Math.floor(r() * 15), y1: y0 + Math.floor(r() * 15) });
    }
    yield line("ovl", rect.x0, rect.y0, rect.x1, rect.y1, n, C.rectsOverlapAny(rect, list));
  }
}
