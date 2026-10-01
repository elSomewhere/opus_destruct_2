// Stage "wings": wings, corner bays, canted bays and chamfers (buildings/wings.js) - planWings on
// scripted sites (lib/shells.mjs wingSite: lots of the winged archetypes' sizes, and some others',
// plain and turned, on scripted streets of every class owned by the cell or not, cut back at a
// corner by slanted streets on and off the yaw table, whole-block lots, no lot) in a World of
// World.js whose cells serve the sites' streets (the road levels and surfaces planWings asks), in
// summer and winter: the wings and their placements; then, granted as the cell plan does, their
// chunks in the world grid (rasterizeWing) at LOD 0 to 3 round their box, and in their own lattices
// (rasterizeWingPart) at LOD 0 to 3. tests/city/test_wings.cpp plans and draws the same.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { SHELL_WORLDS, wingSite, lotLine, chunkDigest, chunksAt } from "../lib/shells.mjs";
import { pureTerrain } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const W = await import(REF + "buildings/wings.js");
const { roadSpecs } = await import(REF + "network/roadClasses.js");
const { Rng } = await import(REF + "core/hash.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");

const WORLDS = [0, 1, 6];
const SITES = 242;
const LODS = [0, 1, 2, 3];
// (the fields of planWings' record: the port's Wing has each)
const WING_KEYS = new Set(["kind", "chamfer", "turn", "U", "V", "f0", "f1", "z0", "z1", "bounds", "canon"]);

const bstr = (b) => [b.x0, b.y0, b.z0, b.x1, b.y1, b.z1].join(",");
const rstr = (q) => [q.x0, q.y0, q.x1, q.y1].join(",");

/** A wing's record, and its lattice's placement (wingPlacement). */
function wingLine(w) {
  for (const k of Object.keys(w)) if (!WING_KEYS.has(k)) throw new Error(`wings: a field the port's record lacks: ${k}`);
  const t = w.turn;
  const p = W.wingPlacement(w);
  return line("wing", w.kind, [t.yaw, t.yaw2, t.origin.x, t.origin.y, t.ou, t.ov].map(f).join("/"), w.U, w.V, w.f0, w.f1, w.z0, w.z1, bstr(w.bounds), rstr(w.canon),
    w.chamfer ? `${w.chamfer.side}${w.chamfer.a}/${w.chamfer.b}` : "-", [p.origin.x, p.origin.y, p.origin.z, p.yaw, p.yaw2, p.q, ...p.m, p.d].join(","));
}

export default function* wings() {
  const r = samples(67);
  const DS = districtList();
  const styles = STYLES.all().map((s) => s.id);
  for (const wk of WORLDS) {
    const w = pureTerrain(new World(JSON.parse(SHELL_WORLDS[wk])));
    const cellRoads = new Map();
    w.cellNet = (i, j) => ({ roads: cellRoads.get(`${i},${j}`) ?? [] });
    const specs = roadSpecs(w.config);
    for (let k = 0; k < SITES; k += 1) {
      const i = 3 * (k % 11) - 15;
      const j = 3 * Math.floor(k / 11) - 33;
      const { lot, block, env, cellId, view } = wingSite(r, w, cellRoads, i, j, k, DS, styles, specs);
      yield lotLine("site", lot, block);
      yield envLine(env);
      if (!env) continue;
      const nolot = r() < 0.04;
      const rng = Rng.from(w.seed, env.id, "wings");
      const ws = W.planWings(w, env, nolot ? null : lot, block, view, cellId, rng);
      yield line("wings", nolot, ws.length, rng.next());
      for (const q of ws) yield wingLine(q);
      if (!ws.length) continue;
      // (granted, as the cell plan does: the envelope keeps them, a chamfer cuts its corner)
      env.wings = ws;
      for (const q of ws) if (q.chamfer) env.chamfer = q.chamfer;
      for (const q of ws) {
        const b = q.bounds;
        const zr = A.floorZ(env, q.f1 + 1);
        const pts = [
          [(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2],
          [b.x0, b.y0, q.z0 + 2],
          [b.x1, b.y1, q.z1 - 1],
          [(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, zr + 1],
          [b.x1 + 100, b.y1 + 100, q.z0],
        ];
        for (const [lod, cx, cy, cz] of chunksAt(LODS, pts)) {
          const c = new ChunkBuffer(lod, cx, cy, cz);
          W.rasterizeWing(w, env, q, c);
          yield line("g", lod, cx, cy, cz, ...chunkDigest(c));
        }
        const lp = [
          [0, 0, q.z0],
          [q.U - 1, q.V - 1, q.z1],
          [q.U / 2, q.V / 2, zr + 1],
          [q.U + 200, q.V / 2, zr],
        ];
        for (const [lod, cx, cy, cz] of chunksAt(LODS, lp)) {
          const c = new ChunkBuffer(lod, cx, cy, cz);
          W.rasterizeWingPart(w, env, q, c);
          yield line("q", lod, cx, cy, cz, ...chunkDigest(c));
        }
      }
    }
  }
}
