import { Placement, PITCHES, nearestYaw } from "../core/placement.js";
import { localPointToWorld } from "../core/obb.js";
import { frameOf } from "./frame.js";
import { floorZ } from "./archetypes.js";
import { makePart } from "../world/parts.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";

/**
 * A parking garage's ramps as pitched parts (the angled world, S4 for the
 * decks of a garage): a garage the cell marks `pitchedRamps` plans each
 * ramp at the gentlest table grade that fits (interior/garage.js: its run
 * H c / s), and a ramp the budget grants is one slab in a lattice climbing
 * along it, a smooth plane from one deck to the next where the world grid
 * steps (voxelize.js rampColumn draws the rest, and every ramp in grid
 * mode). Structure, not ground: not anchored, cast between the decks the
 * world grid holds, which own the ends it shares with them (it yields to
 * the world grid).
 *
 * Its lattice: u up the ramp (the garage's canonical +v), v across it
 * (canonical -u, from the ramp's right edge), w up from its surface; the
 * corner of local cell (0, 0, 0) at canonical (x1 + 1, y0), at the height
 * of the lower deck's floor surface.
 */

/** Slab under a ramp's surface and the kerbs along its edges over it (cells). */
const SLAB = 3;
const KERB = 7;

/** The part of ramp `r` of garage `env` (a plan's ramp with its table pitch), index 0 until granted. */
export function rampPart(cell, env, r) {
  const F = frameOf(env);
  const q = r.rect;
  const [ox, oy] = localPointToWorld(F.placement, q.x1 + 1, q.y0);
  const [dx, dy] = F.dirToWorld(0, 1);
  const placement = new Placement({ origin: { x: ox, y: oy, z: floorZ(env, r.f) + 2 }, yaw: nearestYaw(dx, dy), pitch: r.pitch });
  const T = PITCHES[r.pitch];
  // (as long as its run covers the plan's ramp: a hair into the upper deck, which owns it)
  const Lu = Math.ceil(((q.y1 - q.y0 + 1) * T.r) / T.c);
  const W = q.x1 - q.x0 + 1;
  const part = makePart({ cell, index: 0, key: `${env.id}/ramp${r.f}`, kind: "ramp", placement, extent: { u0: 0, v0: 0, w0: -SLAB, u1: Lu - 1, v1: W - 1, w1: KERB - 1 } });
  part.env = env.id;
  part.ramp = { f: r.f, W, Lu };
  return part;
}

/** Material of a ramp's cell (u, v, w) of a W-cell-wide ramp: its surface, the slab under it, its kerbs. */
function rampMaterial(W, u, v, w) {
  if (v === 0 || v === W - 1) return w === KERB - 1 ? MAT.HAZARD_YELLOW : MAT.CONCRETE_LIGHT;
  if (w >= 0) return 0;
  if (w < -1) return MAT.CONCRETE;
  return (v === 3 || v === W - 4) && ((u >> 3) & 1) === 0 ? MAT.LINE_YELLOW : MAT.FLOOR_CONCRETE;
}

/** A ramp part's content chunk of its lattice (world/partRaster.js): at a coarse LOD a cell is the slab's where its range meets it. */
export function rasterizeRampPart(world, part, chunk) {
  const { W, Lu } = part.ramp;
  const s = chunk.s;
  const lo = (c) => c - chunk.half;
  const [i0, i1] = chunk.rangeX(0, Lu - 1);
  const [j0, j1] = chunk.rangeY(0, W - 1);
  const [k0, k1] = chunk.rangeZ(-SLAB, KERB - 1);
  for (let k = k0; k <= k1; k += 1)
    for (let j = j0; j <= j1; j += 1)
      for (let i = i0; i <= i1; i += 1) {
        let m;
        if (s === 1) m = rampMaterial(W, chunk.wx(i), chunk.wy(j), chunk.wz(k));
        else {
          // (the cell's lowest w within the slab or the kerbs, at its u and its edge v if it holds one)
          const v0 = Math.max(0, lo(chunk.wy(j)));
          const v1 = Math.min(W - 1, lo(chunk.wy(j)) + s - 1);
          const w0 = Math.max(-SLAB, lo(chunk.wz(k)));
          const w1 = Math.min(KERB - 1, lo(chunk.wz(k)) + s - 1);
          if (v0 > v1 || w0 > w1) continue;
          const v = v0 === 0 || v1 === W - 1 ? (v0 === 0 ? 0 : W - 1) : v0;
          m = rampMaterial(W, Math.max(0, lo(chunk.wx(i))), v, w0 < 0 ? Math.max(w0, -1) : w0);
        }
        if (m) chunk.data[i + j * P + k * P2] = m;
      }
}
