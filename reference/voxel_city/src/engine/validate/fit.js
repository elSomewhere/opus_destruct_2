import { groundTile } from "../voxel/compose.js";
import { P } from "../voxel/chunk.js";
import { frameOf } from "../buildings/frame.js";
import { tierRects } from "../buildings/archetypes.js";
import { segmentLevel } from "../network/roadLevel.js";
import { planDressing } from "../city/dressing.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";

/**
 * Fit audits: do terrain, roads, lots, buildings and plants fit together?
 * Used by tests (test/fit.test.js) and the dev report (scripts/audit-fit.js).
 *
 *   groundSteps      height steps between neighbouring ground columns,
 *                    by what meets what (road, lot, space, nature, field)
 *   roadJumps        the largest jump of a road's level between two points
 *                    2 voxels apart along it (junction blends included)
 *   buildingsOnRoads buildings whose footprint holds road surface
 *   plantFit         trees, plants and props not standing on the ground
 *
 * Rects are world voxels.
 */

export const KIND_NAMES = ["?", "road", "lot", "space", "nature", "site", "field"];

function cellsOf(world, rect) {
  const out = new Map();
  for (const c of world.cellsOverlapping(rect)) out.set(`${c.i},${c.j}`, c);
  return [...out.values()];
}

/** Ground column top (voxels) and kind at a world column, from the LOD 0 tile. */
export function groundAt(world, x, y) {
  const cx = Math.floor(x / 32);
  const cy = Math.floor(y / 32);
  const t = groundTile(world, 0, cx, cy);
  const k = x - cx * 32 + 1 + (y - cy * 32 + 1) * P;
  return { z: t.z[k], kind: t.kind[k], water: t.water[k] > t.z[k], deck: t.deck ? t.deck[k] : 0 };
}

/**
 * Steps of at least `thr` voxels between neighbouring columns of the LOD
 * `lod` ground tiles over `rect`, off building footprints, bridge decks and
 * water: { pairs: Map("a-b" -> {n, max}), worst: [{dz, pair, x, y}] }.
 */
export function groundSteps(world, rect, { lod = 1, thr = 4 } = {}) {
  const s = 1 << lod;
  const span = 32 * s;
  const fp = [];
  // (a turned building's footprint: its canonical rects, tested in its own axes)
  const turned = [];
  for (const c of cellsOf(world, rect))
    for (const b of world.cellPlan(c.i, c.j).buildings) {
      const f = frameOf(b);
      if (f.turned) {
        turned.push({ f, rects: [...tierRects(b, 0), ...(b.annexes ?? []).map((a) => a.canon)], bb: b.bounds });
        continue;
      }
      for (const r of tierRects(b, 0)) fp.push(f.rectToWorld(r));
      for (const a of b.annexes ?? []) fp.push(a.world);
    }
  const inTurned = (x, y) =>
    turned.some((t) => {
      if (x < t.bb.x0 || x > t.bb.x1 || y < t.bb.y0 || y > t.bb.y1) return false;
      const [u, v] = t.f.fromWorld(x, y);
      return t.rects.some((r) => u >= r.x0 - 2 && u <= r.x1 + 2 && v >= r.y0 - 2 && v <= r.y1 + 2);
    });
  const inFp = (x, y) => fp.some((r) => x >= r.x0 - 2 && x <= r.x1 + 2 && y >= r.y0 - 2 && y <= r.y1 + 2) || (turned.length > 0 && inTurned(x, y));
  const pairs = new Map();
  const worst = [];
  for (let cy = Math.floor(rect.y0 / span); cy <= Math.floor(rect.y1 / span); cy += 1)
    for (let cx = Math.floor(rect.x0 / span); cx <= Math.floor(rect.x1 / span); cx += 1) {
      const t = groundTile(world, lod, cx, cy);
      for (let j = 1; j < P - 2; j += 1)
        for (let i = 1; i < P - 2; i += 1) {
          const k = i + j * P;
          if ((t.deck && t.deck[k]) || t.water[k] > t.z[k]) continue;
          for (const k2 of [k + 1, k + P]) {
            if ((t.deck && t.deck[k2]) || t.water[k2] > t.z[k2]) continue;
            const dz = Math.abs(t.z[k] - t.z[k2]);
            if (dz < thr) continue;
            const x = (cx * 32 - 1 + i) * s;
            const y = (cy * 32 - 1 + j) * s;
            if (x < rect.x0 || x > rect.x1 || y < rect.y0 || y > rect.y1 || inFp(x, y) || inFp(x + s, y + s)) continue;
            const pair = [KIND_NAMES[t.kind[k]], KIND_NAMES[t.kind[k2]]].sort().join("-");
            const e = pairs.get(pair) ?? { n: 0, max: 0 };
            e.n += 1;
            e.max = Math.max(e.max, dz);
            pairs.set(pair, e);
            worst.push({ dz, pair, x, y });
          }
        }
    }
  worst.sort((a, b) => b.dz - a.dz);
  return { pairs, worst };
}

/** The largest jump (voxels) of any road's level between points 2 voxels apart along it, for the roads of a cell. */
export function roadJumps(world, i, j) {
  const view = world.roadView(i, j);
  let worst = { jump: 0, road: null, along: 0 };
  for (const s of view.segs) {
    if (s.road.cell !== `C${i}_${j}`) continue;
    let prev = segmentLevel(world, s, 0);
    for (let a = 2; a <= s.len; a += 2) {
      const z = segmentLevel(world, s, a);
      if (Math.abs(z - prev) > worst.jump) worst = { jump: Math.abs(z - prev), road: s.road.id, along: s.s0 + a };
      prev = z;
    }
  }
  return worst;
}

/** Buildings of the cells over `rect` whose footprint holds road surface (sampled every 4 voxels): [{ id, n }]. */
export function buildingsOnRoads(world, rect) {
  const rs = makeRoadSample();
  const out = [];
  for (const c of cellsOf(world, rect)) {
    const view = world.roadView(c.i, c.j);
    for (const env of world.cellPlan(c.i, c.j).buildings) {
      const f = frameOf(env);
      let n = 0;
      for (const r0 of tierRects(env, 0)) {
        const r = f.rectToWorld(r0);
        for (let y = r.y0 + 2; y <= r.y1 - 2; y += 4)
          for (let x = r.x0 + 2; x <= r.x1 - 2; x += 4) {
            // (a turned building: only the samples in its footprint, 2 cells in)
            if (f.turned) {
              const [u, v] = f.fromWorld(x, y);
              if (u < r0.x0 + 2 || u > r0.x1 - 2 || v < r0.y0 + 2 || v > r0.y1 - 2) continue;
            }
            sampleRoadSurface(view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x + 0.5, y + 0.5, rs, world.seed);
            if (rs.kind !== KIND.NONE) n += 1;
          }
      }
      if (n) out.push({ id: env.id, n });
    }
  }
  return out;
}

/**
 * Plants and props over `rect` whose base is not on the ground (more than
 * `tol` voxels off): { checked, off: [{ what, kind, dz, x, y }] }. Long
 * props on slopes (garage rows) may be dug in on purpose and are skipped.
 */
export function plantFit(world, rect, { tol = 1 } = {}) {
  const off = [];
  let checked = 0;
  const check = (what, t) => {
    if (t.x < rect.x0 || t.x > rect.x1 || t.y < rect.y0 || t.y > rect.y1) return;
    const g = groundAt(world, Math.round(t.x), Math.round(t.y));
    if (g.water || g.deck) return;
    checked += 1;
    const dz = t.z - 1 - g.z;
    if (Math.abs(dz) > tol) off.push({ what, kind: t.kind, dz, x: t.x, y: t.y });
  };
  for (const t of world.forest.treesIn(rect)) check("forest", t);
  for (const t of world.forest.understoryIn(rect)) check("understory", t);
  for (const c of cellsOf(world, rect)) {
    const d = planDressing(world, c.i, c.j);
    for (const t of d.trees) check("tree", t);
  }
  return { checked, off };
}
