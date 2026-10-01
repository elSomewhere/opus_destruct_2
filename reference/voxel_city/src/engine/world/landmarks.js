import { Rng, hash32, hashFloat } from "../core/hash.js";
import { SpatialGrid } from "../core/geom2d.js";
import { vx } from "../core/units.js";
import { PROPS } from "../city/propPrefabs.js";
import { MAT } from "../voxel/materials.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";

/**
 * Landmarks of the open country on an island (world/island.js): a
 * lighthouse on the headland nearest the harbour, boathouses (naust) in
 * little rows along the shore near every place, fish-drying racks by the
 * fishing hamlets, cairns (varder) on the fell tops. The island plans them
 * once, as props (city/propPrefabs.js) standing on the natural ground; the
 * `landmarks` feature source rasterizes them and trees keep clear of them.
 */
export class Landmarks {
  constructor(world) {
    this.world = world;
    this.items = null;
    this.grid = null;
    this.rs = makeRoadSample();
  }

  /** All landmarks (planned lazily, once). */
  all() {
    if (!this.items) this.plan();
    return this.items;
  }

  near(rect) {
    this.all();
    return this.grid.query(rect);
  }

  /** Is (x, y) under a landmark's footprint (plus a margin, voxels)? */
  blocks(x, y, margin = 16) {
    this.all();
    for (const it of this.grid.query({ x0: x - margin, y0: y - margin, x1: x + margin, y1: y + margin })) {
      const b = it.foot;
      if (x >= b.x0 - margin && x <= b.x1 + margin && y >= b.y0 - margin && y <= b.y1 + margin) return true;
    }
    return false;
  }

  /** Open natural ground at (x, y): no road, lot, urban space or water. */
  free(x, y) {
    const w = this.world;
    const c = w.cellAt(x, y);
    const view = w.roadView(c.i, c.j);
    sampleRoadSurface(view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x + 0.5, y + 0.5, this.rs, w.seed);
    if (this.rs.kind !== KIND.NONE || this.rs.sdfR < vx(3)) return false;
    const plan = w.cellPlan(c.i, c.j);
    return !plan.lotAt(x, y) && !plan.spaceAt(x, y);
  }

  plan() {
    this.items = [];
    this.grid = new SpatialGrid(512);
    const w = this.world;
    const isl = w.fields.island;
    if (!isl) return;
    const seed = w.seed;
    const rng = Rng.from(seed, "landmarks");
    const coast = (x, y) => isl.coast(x / 8, y / 8);
    const taken = [];
    const clash = (x, y, r) => taken.some((t) => Math.hypot(t.x - x, t.y - y) < t.r + r);
    // unit vector towards the water from a shore point
    const seaward = (x, y) => {
      const c = coast(x, y);
      const gx = coast(x + 48, y) - c;
      const gy = coast(x, y + 48) - c;
      const g = Math.hypot(gx, gy) || 1;
      return [-gx / g, -gy / g];
    };
    const ground = (x, y) => Math.round(w.terrain.sample(x, y).h);
    // first point seaward of (x, y) along (nx, ny) where the ground meets the sea (1 m steps)
    const waterline = (x, y, nx, ny) => {
      for (let t = 0; t <= 45 * 8; t += 8) if (w.terrain.sample(x + nx * t, y + ny * t).h < 3) return { x: x + nx * t, y: y + ny * t };
      return null;
    };
    const add = (kind, x, y, z, ax, ay, bx, by, r, extra = {}) => {
      const it = makeProp(seed, kind, x, y, z, ax, ay, bx, by, extra);
      if (!it) return;
      this.items.push(it);
      this.grid.insert(it, it.bb);
      taken.push({ x, y, r });
    };

    // the lighthouse: the most exposed low headland 300-1400 m from the harbour
    const h = isl.harbour();
    if (h) {
      let best = null;
      for (let r = 300; r <= 1400; r += 50)
        for (let a = 0; a < 72; a += 1) {
          const t = (a / 72) * Math.PI * 2;
          const x = (h.x + Math.cos(t) * r) * 8;
          const y = (h.y + Math.sin(t) * r) * 8;
          const c = coast(x, y);
          if (c < 8 || c > 40) continue;
          let sea = 0;
          for (let k = 0; k < 16; k += 1) if (coast(x + Math.cos((k / 16) * Math.PI * 2) * 1000, y + Math.sin((k / 16) * Math.PI * 2) * 1000) < 0) sea += 1;
          const score = sea / 16 - Math.abs(r - 650) / 3000 + hashFloat(seed, a, r, 71) * 0.05;
          if (score > 0.4 && (!best || score > best.score) && this.free(x, y)) best = { x, y, score };
        }
      if (best) add("lighthouse", Math.round(best.x), Math.round(best.y), ground(best.x, best.y) + 1, 1, 0, 0, 1, vx(30), { plinth: true });
    }

    // boathouses in rows along the shore near each place; fish racks by the hamlets
    const { towns, villages } = w.fields.islandSettlements();
    for (const p of [...towns, ...villages]) {
      const want = p.hamlet ? 4 : p.village ? 5 : 7;
      const R = p.radius / 8 + 450;
      let placed = 0;
      for (let r = 60; r <= R && placed < want; r += 35)
        for (let a = 0; a < 90 && placed < want; a += 1) {
          const t = (a / 90) * Math.PI * 2 + hashFloat(seed, p.x | 0, a, 72);
          const x = Math.round(p.x + Math.cos(t) * r * 8);
          const y = Math.round(p.y + Math.sin(t) * r * 8);
          const c = coast(x, y);
          if (c < 2 || c > 24 || isl.cliff(x / 8, y / 8) > 0.6) continue;
          if (clash(x, y, vx(6)) || !this.free(x, y)) continue;
          const [nx, ny] = seaward(x, y);
          // the real waterline (the ground under a town is graded above the sea)
          const wl = waterline(x, y, nx, ny);
          if (wl === null) continue;
          // a row of two to four sheds side by side, their doors at the water
          const n = 2 + (hash32(seed, x, y, 73) % 3);
          for (let k = 0; k < n && placed < want; k += 1) {
            const off = (k - (n - 1) / 2) * vx(5.5);
            const qx = wl.x - ny * off;
            const qy = wl.y + nx * off;
            const w2 = waterline(qx - nx * vx(4), qy - ny * vx(4), nx, ny) ?? { x: qx, y: qy };
            const bx = Math.round(w2.x - nx * vx(7.5));
            const by = Math.round(w2.y - ny * vx(7.5));
            const z = ground(bx, by);
            // low shore only (no sheds up on a bank), dry land behind
            if (z > 3 * 8 || z < 2 || !this.free(bx, by) || clash(bx, by, vx(2.5))) continue;
            add("boathouse", bx, by, z + 1, -ny, nx, nx, ny, vx(3));
            placed += 1;
          }
        }
      if (!p.hamlet && !p.village) continue;
      // stockfish racks on open ground near the shore
      for (let k = 0, racks = 0; k < 40 && racks < 2; k += 1) {
        const t = rng.float(0, Math.PI * 2);
        const r = rng.float(40, 220);
        const x = Math.round(p.x + Math.cos(t) * r * 8);
        const y = Math.round(p.y + Math.sin(t) * r * 8);
        const c = coast(x, y);
        if (c < 12 || c > 160 || clash(x, y, vx(14)) || !this.free(x, y) || !this.free(x + vx(10), y)) continue;
        add("fishRack", x, y, ground(x, y) + 1, 1, 0, 0, 1, vx(14));
        racks += 1;
      }
    }

    // cairns: the highest point of each 1.2 km square of the fells (the
    // high ones, half a dozen at most, the highest first)
    const b = isl.bounds();
    const cell = 1200;
    const tops = [];
    for (let cy = b.y0; cy < b.y1; cy += cell)
      for (let cx = b.x0; cx < b.x1; cx += cell) {
        let top = null;
        for (let y = cy; y < cy + cell; y += 60)
          for (let x = cx; x < cx + cell; x += 60) {
            if (isl.coast(x, y) < 150) continue;
            const hz = w.terrain.sample(x * 8, y * 8).h;
            if (!top || hz > top.h) top = { x, y, h: hz };
          }
        if (top && top.h >= Math.max(90, 0.45 * (isl.cfg.peak ?? 400)) * 8) tops.push(top);
      }
    tops.sort((p, q) => q.h - p.h);
    for (const top of tops.slice(0, 6)) {
      // climb to the very top in 6 m steps
      {
        let { x, y } = top;
        for (let k = 0; k < 20; k += 1) {
          let nb = null;
          for (const [dx, dy] of [[6, 0], [-6, 0], [0, 6], [0, -6]]) {
            const hz = w.terrain.sample((x + dx) * 8, (y + dy) * 8).h;
            if (hz > (nb?.h ?? w.terrain.sample(x * 8, y * 8).h)) nb = { x: x + dx, y: y + dy, h: hz };
          }
          if (!nb) break;
          x = nb.x;
          y = nb.y;
        }
        const px = Math.round(x * 8);
        const py = Math.round(y * 8);
        if (this.free(px, py)) add("cairn", px, py, ground(px, py) + 1, 1, 0, 0, 1, vx(3));
      }
    }
  }
}

/** A prop (city/propPrefabs.js) placed in the world: boxes, bounds and footprint. */
function makeProp(seed, kind, x, y, z, ax, ay, bx, by, extra) {
  const rng = Rng.from(seed, "landmark", kind, x, y);
  const boxes = PROPS[kind](rng, extra);
  // (a granite plinth down into uneven ground)
  if (extra.plinth) boxes.push({ a0: -16, a1: 16, b0: -16, b1: 16, z0: -12, z1: -1, m: MAT.GRANITE });
  const out = [];
  for (const q of boxes) {
    const xa = x + q.a0 * ax + q.b0 * bx;
    const ya = y + q.a0 * ay + q.b0 * by;
    const xb = x + q.a1 * ax + q.b1 * bx;
    const yb = y + q.a1 * ay + q.b1 * by;
    out.push({ x0: Math.round(Math.min(xa, xb)), y0: Math.round(Math.min(ya, yb)), x1: Math.round(Math.max(xa, xb)), y1: Math.round(Math.max(ya, yb)), z0: z + q.z0, z1: z + q.z1, m: q.m });
  }
  let bb = null;
  for (const q of out) bb = bb ? { x0: Math.min(bb.x0, q.x0), y0: Math.min(bb.y0, q.y0), z0: Math.min(bb.z0, q.z0), x1: Math.max(bb.x1, q.x1), y1: Math.max(bb.y1, q.y1), z1: Math.max(bb.z1, q.z1) } : { ...q };
  if (!bb) return null;
  return { kind, boxes: out, bb, foot: { x0: bb.x0, y0: bb.y0, x1: bb.x1, y1: bb.y1 } };
}

/** Feature source: the island's landmarks (lighthouse, boathouses, fish racks, cairns). */
export const landmarkSource = {
  id: "landmarks",
  order: 8.5,
  maxLod: 6,
  zRange(world, rect) {
    if (!world.landmarks) return null;
    let lo = Infinity;
    let hi = -Infinity;
    for (const it of world.landmarks.near(rect)) {
      lo = Math.min(lo, it.bb.z0);
      hi = Math.max(hi, it.bb.z1);
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk) {
    if (!world.landmarks) return;
    const box = chunk.worldBox;
    for (const it of world.landmarks.near(box)) {
      // small things drop out at a distance, the lighthouse stays
      if (chunk.lod >= 3 && it.kind !== "lighthouse" && it.kind !== "boathouse") continue;
      if (it.bb.z1 < box.z0 || it.bb.z0 > box.z1) continue;
      for (const q of it.boxes) if (q.m) chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, 0);
    }
  },
};
