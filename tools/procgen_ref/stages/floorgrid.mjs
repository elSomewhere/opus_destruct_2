// Stage "floorgrid": FloorGrid (buildings/interior/grid.js) and the planners' grid helpers
// (buildings/interior/common.js: addStairRoom, stairDoor, splitLength, facadeSidesOf) on sample
// floors: footprints of one or two rects (some reaching beyond the grid, some with a chamfered
// corner, some turned: wider doors), a stairwell, a corridor, rooms in one or two rows, doors of
// every kind and placement; then every query: the label grid (as runs), rooms, doors, wall runs,
// door leaves, the door graph, rooms at points, free rects, facade sides, snapshot and restore.
// tests/city/test_floorgrid.cpp plans the same floors the same way.
import { REF, f, line, samples } from "../lib/rec.mjs";

const G = await import(REF + "buildings/interior/grid.js");
const S = await import(REF + "buildings/interior/stairs.js");
const C = await import(REF + "buildings/interior/common.js");
const { chamferCut } = await import(REF + "buildings/chamfer.js");
const { Rng } = await import(REF + "core/hash.js");

function rle(a) {
  const out = [];
  let i = 0;
  while (i < a.length) {
    let j = i;
    while (j < a.length && a[j] === a[i]) j += 1;
    out.push(`${a[i]}:${j - i}`);
    i = j;
  }
  return out.join(",");
}
const frect = (q) => `${q.x0},${q.y0},${q.x1},${q.y1}`;
const fdoor = (d) => [d.id, d.u0, d.u1, d.v0, d.v1, d.orient, d.a, d.b, d.kind, d.sideA, d.width, d.leaf, d.height].map(f).join(",");
const frun = (w) => [w.orient, w.fixed, w.t0, w.t1, w.thick, w.sideA].map(f).join(",");
const froom = (m) => [m.id, m.type, m.rects.map(frect).join("|"), m.paint, m.floorMat, m.stair].map(f).join(",");

const KINDS = [undefined, "interior", "opening", "closet", "elevator", "entry"];
const LEAVES = [undefined, "wood", "none", "glass", "metal", "rollup"];
const PLACES = [undefined, "auto", "center", "start", "end", "near"];

// door options from 6 draws (each option present or not)
function doorOpts(r, b0, b1) {
  const v = [r(), r(), r(), r(), r(), r(), r()];
  const o = {};
  if (v[0] < 0.6) o.width = 6 + Math.floor(v[1] * 6);
  if (v[1] < 0.3) o.margin = Math.floor(v[2] * 3);
  const place = PLACES[Math.floor(v[2] * PLACES.length)];
  if (place) o.place = place;
  if (place === "near" || v[3] < 0.2) o.near = { u: b0 + Math.floor(v[3] * (b1 - b0 + 1)), v: Math.floor(v[4] * 80) - 20 };
  const kind = KINDS[Math.floor(v[4] * KINDS.length)];
  if (kind) o.kind = kind;
  const leaf = LEAVES[Math.floor(v[5] * LEAVES.length)];
  if (leaf) o.leaf = leaf;
  if (v[6] < 0.3) o.height = Math.floor(v[6] * 70) - 1;
  return o;
}

export default function* floorgrid() {
  const r = samples(23);
  for (let i = 0; i < 90; i += 1) {
    const U = 40 + Math.floor(r() * 100);
    const V = 34 + Math.floor(r() * 80);
    const shape = Math.floor(r() * 4);
    const s1 = r();
    const s2 = r();
    let fp;
    if (shape === 0) fp = [{ x0: 0, y0: 0, x1: U - 1, y1: V - 1 }];
    else if (shape === 1) {
      const v1 = 16 + Math.floor(s1 * (V - 30));
      const u1 = 16 + Math.floor(s2 * (U - 30));
      fp = [
        { x0: 0, y0: 0, x1: U - 1, y1: v1 },
        { x0: 0, y0: v1 + 1, x1: u1, y1: V - 1 },
      ];
    } else if (shape === 2) {
      fp = [
        { x0: 0, y0: 0, x1: U - 1, y1: Math.floor(V / 2) },
        { x0: Math.floor(U / 4), y0: 0, x1: Math.floor((3 * U) / 4), y1: V - 1 },
      ];
    } else fp = [{ x0: -3, y0: 2, x1: U + 4, y1: V - 3 }];
    const cr = r();
    const cs = r();
    const ck = r();
    const ch = cr < 0.3 ? { side: cs < 0.5 ? "L" : "R", a: 20 * (1 + Math.floor(ck * 2)), b: 21 * (1 + Math.floor(ck * 2)) } : null;
    const g = new G.FloorGrid(U, V, fp, ch ? (u, v) => chamferCut(ch, U, u, v) : null);
    const dr = r();
    const dy = r();
    g.doorExtra = dr < 0.25 ? G.doorExtraOf({ turn: { yaw: Math.floor(dy * 132) } }) : 0;
    yield line("grid", i, U, V, shape, fp.map(frect).join("|"), ch ? `${ch.side}${ch.a}/${ch.b}` : "-", g.doorExtra, rle(g.cells));
    yield line("inner", i, g.innerRects().map(frect).join("|"));

    const inner = g.innerRects()[0];
    const bx = { x0: Math.max(inner.x0, 2), y0: Math.max(inner.y0, 2), x1: Math.min(inner.x1, U - 3), y1: Math.min(inner.y1, V - 3) };
    const storyH = 20 + Math.floor(r() * 16);
    const laneLow = r() < 0.5;
    const dims = S.stairDims(storyH);
    const wantStair = r() < 0.8;
    let st = null;
    let sRoom = null;
    if (wantStair && bx.x1 - bx.x0 + 1 >= dims.W + 20 && bx.y1 - bx.y0 + 1 >= dims.L + 6) {
      st = S.makeStair({ rect: { x0: bx.x0, y0: bx.y0, x1: bx.x0 + dims.W - 1, y1: bx.y0 + dims.L - 1 }, axis: "v", dir: 1, laneLow, f0: 0, f1: 2 });
      st.id = 0;
      sRoom = C.addStairRoom(g, st);
    }
    const cw = 6 + Math.floor(r() * 6);
    const cx0 = st ? st.rect.x1 + 2 : bx.x0;
    const corr = g.addRoom("corridor", [{ x0: cx0, y0: bx.y0, x1: bx.x1, y1: bx.y0 + cw - 1 }], { paint: "PAINT_CREAM", floorMat: "FLOOR_TERRAZZO" });
    // rooms below the corridor, one or two rows
    const band = { x0: cx0, y0: bx.y0 + cw + 1, x1: bx.x1, y1: bx.y1 };
    const twoRows = band.y1 - band.y0 + 1 >= 30 && r() < 0.6;
    const mid = twoRows ? band.y0 + Math.floor((band.y1 - band.y0) * (0.35 + 0.3 * r())) : band.y1;
    const target = 10 + Math.floor(r() * 30);
    const rng = new Rng(Math.floor(r() * 4294967296));
    const front = [];
    const back = [];
    const TYPES = ["office", "bedroom", "living", "kitchen", "bath", "storage"];
    if (band.x1 - band.x0 + 1 >= 8 && mid - band.y0 + 1 >= 5) {
      for (const [a0, a1] of C.splitLength(band.x0, band.x1, target, 7, rng)) {
        front.push(g.addRoom(TYPES[Math.floor(r() * TYPES.length)], [{ x0: a0, y0: band.y0, x1: a1, y1: mid }]));
      }
      if (twoRows && band.y1 - (mid + 2) + 1 >= 5) {
        for (const [a0, a1] of C.splitLength(band.x0, band.x1, target + 6, 8, rng)) {
          back.push(g.addRoom(TYPES[Math.floor(r() * TYPES.length)], [{ x0: a0, y0: mid + 2, x1: a1, y1: band.y1 }, { x0: a0, y0: band.y1 + 1, x1: a0 - 1, y1: band.y1 }]));
        }
      }
    }
    // doors: the stair, the corridor's entrance, rooms to the corridor, back rooms to a front room, some outside
    if (st) {
      const so = {};
      const sw = r();
      const sk = r();
      if (sw < 0.5) so.width = 6 + Math.floor(sw * 8);
      if (sk < 0.3) so.kind = "opening";
      if (sk > 0.8) so.leaf = "glass";
      const d = C.stairDoor(g, sRoom, st, corr, so);
      yield line("sd", i, d ? d.id : "-");
    }
    yield line("ed", i, f(g.addDoor(corr, null, doorOpts(r, bx.x0, bx.x1))?.id));
    for (const m of front) {
      const d = g.addDoor(m, corr, doorOpts(r, m.rects[0].x0, m.rects[0].x1));
      const d2 = d ? null : g.addDoor(m, corr, { width: 6, margin: 1 });
      yield line("fd", i, m.id, d ? d.id : "-", d2 ? d2.id : "-");
    }
    for (const m of back) {
      let best = null;
      for (const q of front) {
        const ov = Math.min(q.rects[0].x1, m.rects[0].x1) - Math.max(q.rects[0].x0, m.rects[0].x0);
        if (!best || ov > best.ov) best = { q, ov };
      }
      const o = doorOpts(r, m.rects[0].x0, m.rects[0].x1);
      const d = best ? g.addDoor(m, best.q, o) : null;
      const out = r() < 0.4 ? g.addDoor(m, null, doorOpts(r, m.rects[0].x0, m.rects[0].x1)) : null;
      yield line("bd", i, m.id, d ? d.id : "-", out ? out.id : "-");
    }
    // snapshot, a probe room and door, restore
    const snap = g.snapshot();
    const before = rle(g.cells);
    const px = bx.x0 + Math.floor(r() * 10);
    const py = bx.y1 - Math.floor(r() * 10);
    const probe = g.addRoom("probe", [{ x0: px, y0: py - 6, x1: px + 6, y1: py }]);
    const pd = g.addDoor(probe, front[0] ?? corr, { width: 6, margin: 1 });
    yield line("probe", i, g.rooms.length, g.doors.length, pd ? pd.id : "-", rle(g.cells) === before);
    g.restore(snap);
    yield line("restored", i, g.rooms.length, g.doors.length, rle(g.cells) === before);
    // extend a room into free cells below the stair
    if (st && front.length) {
      const ext = { x0: st.rect.x0, y0: st.rect.y1 + 2, x1: st.rect.x1, y1: st.rect.y1 + 2 + Math.floor(r() * 8) };
      yield line("ext", i, frect(ext), g.isFree(ext), g.extendRoom(front[0], ext), front[0].rects.length);
    }
    yield line("cells", i, rle(g.cells));
    for (const m of g.rooms) yield line("room", i, froom(m), g.area(m), [...C.facadeSidesOf(g, m.rects[0] ?? { x0: 0, y0: 0, x1: 0, y1: 0 })].join(""));
    for (const d of g.doors) {
      const leaf = g.doorLeafRect(d);
      yield line("door", i, fdoor(d), leaf ? frect(leaf) : "-");
    }
    for (const m of g.rooms) {
      yield line("runs", i, m.id, "c", g.wallRuns(m, corr).map(frun).join(";"), "x", g.wallRuns(m, null).map(frun).join(";"));
    }
    for (const m of back) for (const q of front) yield line("runs2", i, m.id, q.id, g.wallRuns(m, q).map(frun).join(";"));
    const adj = g.doorGraph();
    yield line("graph", i, [...adj].map(([k, s]) => `${k}>${[...s].join("/")}`).join(" "));
    const at = [];
    const free = [];
    for (let k = 0; k < 40; k += 1) {
      const u = Math.floor(r() * (U + 4)) - 2;
      const v = Math.floor(r() * (V + 4)) - 2;
      const m = g.roomAt(u, v);
      at.push(`${g.get(u, v)}/${m ? m.id : "-"}`);
      const q = { x0: u, y0: v, x1: u + Math.floor(r() * 8) - 1, y1: v + Math.floor(r() * 8) - 1 };
      free.push(g.isFree(q) ? 1 : 0);
    }
    yield line("at", i, at.join(" "), free.join(""));
    // set / fill / paintInside out of range and in
    g.set(-1, 3, 7);
    g.set(U, 3, 7);
    g.set(1, 1, 65536 + 5);
    g.fill({ x0: U - 3, y0: V - 3, x1: U + 5, y1: V + 5 }, 70000);
    g.paintInside({ x0: -2, y0: -2, x1: 4, y1: 4 }, 99);
    yield line("edit", i, g.get(1, 1), g.get(U - 1, V - 1), rle(g.cells));
  }
}
