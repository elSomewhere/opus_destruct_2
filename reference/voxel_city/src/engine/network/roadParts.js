import { Placement, PITCHES, YAWS, nearestYaw } from "../core/placement.js";
import { segmentLevel } from "./roadLevel.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "./roadSurface.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { makePart } from "../world/parts.js";

/**
 * Pitched road pieces (ANGLED_WORLD_PLAN.md S4): where a street runs at one
 * grade of the pitch table (8.01%, 10.03%, 12.55%, 14.36%, 16.78%, 20.20%:
 * the grade limits a steep street's profile is held at, roadLevel.js) in a
 * direction of the yaw table (every town street; not a wandering country
 * road), its right-of-way becomes anchored oriented parts: a slab in a
 * lattice of its own, turned to the street and pitched to its grade, so its
 * surface is one plane, not a staircase of 12.5 cm steps (structvox: wheels
 * roll instead of judder; Rule C: an anchored slab on anchored ground costs
 * no bonds). A run shares one lattice (its pieces meet without a seam) and
 * is cut into pieces within PART_REACH chunks of their home chunks.
 *
 * Everything else about the street stays the world grid's: the level
 * across, junctions and their blends, curbs, markings (sampled on the
 * pitched plane from the same road surface), lamps and trees beside it.
 */

/** Grade samples along a segment (voxels), and how far one may stray from its table grade. */
const SAMPLE = 8;
const TOL = 0.0015;
/** Shortest run worth a lattice (voxels): 16 m. */
const MIN_RUN = 128;
/** Slab thickness below the road surface (cells) and room above it (curbs, markings). */
export const SLAB = 4;
const ABOVE = 3;

/** The signed pitch index whose grade is within TOL of `g` (0 when none). */
function pitchOf(g) {
  for (let k = 1; k < PITCHES.length; k += 1) {
    const q = PITCHES[k].s / PITCHES[k].c;
    if (Math.abs(Math.abs(g) - q) <= TOL) return g > 0 ? k : -k;
  }
  return 0;
}

/**
 * Runs of one table grade along a road segment of the owner's view (its
 * direction a table yaw): [{ a0, a1, pitch }] (arc along the segment,
 * voxels; pitch a signed PITCHES index, climbing along the segment when
 * positive). The level at every sample of a run lies on the run's line.
 */
export function pitchedRuns(world, seg) {
  const yaw = nearestYaw(seg.dx, seg.dy);
  const Y = YAWS[yaw];
  if (Math.abs(Y.c / Y.r - seg.dx) > 1e-9 || Math.abs(Y.s / Y.r - seg.dy) > 1e-9) return [];
  const out = [];
  const n = Math.floor(seg.len / SAMPLE);
  const z = [];
  for (let k = 0; k <= n; k += 1) z.push(segmentLevel(world, seg, k * SAMPLE));
  let k0 = -1;
  let p0 = 0;
  const close = (k1) => {
    if (k0 < 0 || !p0) return;
    const a0 = k0 * SAMPLE;
    const a1 = k1 * SAMPLE;
    if (a1 - a0 < MIN_RUN) return;
    // (on the line through its ends, a quarter voxel at most)
    const g = (z[k1] - z[k0]) / (a1 - a0);
    for (let k = k0; k <= k1; k += 1) if (Math.abs(z[k] - (z[k0] + g * (k - k0) * SAMPLE)) > 0.25) return;
    // (on dry land: a bridge or a causeway, its centre line over the water, keeps its deck)
    if (world.isWet) for (let k = k0; k <= k1; k += 1) if (world.isWet(seg.ax + seg.dx * k * SAMPLE, seg.ay + seg.dy * k * SAMPLE, 0)) return;
    out.push({ a0, a1, pitch: p0, yaw });
  };
  for (let k = 0; k < n; k += 1) {
    const p = pitchOf((z[k + 1] - z[k]) / SAMPLE);
    if (p && p === p0 && k0 >= 0) continue;
    close(k);
    k0 = p ? k : -1;
    p0 = p;
  }
  close(n);
  return out;
}

/**
 * The oriented parts of one road's pitched runs, each run one lattice cut
 * into pieces within reach (index 0 until the budget grants them one):
 * [{ ...part, road, s0, s1, grade, run }] (s0, s1 arc along the road).
 * `segs` are the road's segments in its owner cell's view; `cell` the owner
 * { i, j, ci, cj, rect }.
 */
export function roadRunParts(world, road, segs, cell) {
  const out = [];
  for (const seg of segs) {
    for (const run of pitchedRuns(world, seg)) {
      const P = PITCHES[Math.abs(run.pitch)];
      const cos = P.c / P.r;
      // the lattice: u along the street (up the slope), v across it, w up from the slab's foot
      const W = 2 * Math.ceil(seg.hr);
      const x0 = seg.ax + seg.dx * run.a0;
      const y0 = seg.ay + seg.dy * run.a0;
      // (the surface over the centre line at the run's start: the world grid's road top there)
      const s0z = segmentLevel(world, seg, run.a0) + 1;
      const probe = new Placement({ yaw: run.yaw, pitch: run.pitch });
      const m = probe.m;
      const d = probe.d;
      const at = (u, v, w) => [(m[0] * u + m[1] * v + m[2] * w) / d, (m[3] * u + m[4] * v + m[5] * w) / d, (m[6] * u + m[7] * v + m[8] * w) / d];
      const [ox, oy, oz] = at(0, W / 2, SLAB);
      const placement = new Placement({ origin: { x: Math.round(x0 - ox), y: Math.round(y0 - oy), z: Math.round(s0z - oz) }, yaw: run.yaw, pitch: run.pitch, anchored: true });
      // pieces: the fewest equal ones within reach (8 m at least)
      const L = (run.a1 - run.a0) / cos;
      const piece = (n, k) => {
        const u0 = Math.floor((L * k) / n);
        const u1 = Math.floor((L * (k + 1)) / n) - 1;
        // (the run's lattice, a placement of its own: its priority is the piece's)
        const pl = new Placement({ origin: placement.origin, yaw: run.yaw, pitch: run.pitch, anchored: true });
        const part = makePart({ cell, index: 0, key: `${road.id}/r${seg.idx}.${run.a0}.${k}`, kind: "road", placement: pl, extent: { u0, v0: 0, w0: 0, u1, v1: W - 1, w1: SLAB + ABOVE }, anchored: true });
        return { ...part, road: road.id, seg: seg.idx, s0: seg.s0 + run.a0 + u0 * cos, s1: seg.s0 + run.a0 + (u1 + 1) * cos, grade: (Math.sign(run.pitch) * P.s) / P.c, hr: seg.hr };
      };
      let best = null;
      for (let n = 1; n <= Math.max(1, Math.floor(L / 64)); n += 1) {
        const ps = [];
        for (let k = 0; k < n; k += 1) ps.push(piece(n, k));
        const r = Math.max(...ps.map((p) => p.reach));
        if (!best || r < best.r) best = { ps, r };
        if (r <= 4) break;
      }
      out.push(...best.ps);
    }
  }
  return out;
}

/**
 * A road piece's content in its own lattice (ANGLED_WORLD_PLAN.md §8:
 * generate_grid): `chunk` a ChunkBuffer addressed in the part's local cells
 * (its wx/wy/wz are u, v, w). Every column of the right-of-way takes the
 * road surface found at its centre on the pitched plane (the same
 * surface the world grid draws: carriageway, markings, curb, sidewalk),
 * over the slab.
 */
export function rasterizeRoadPart(world, part, chunk) {
  const e = part.extent;
  const pl = part.placement;
  const [i0, i1] = chunk.rangeX(e.u0, e.u1);
  const [j0, j1] = chunk.rangeY(e.v0, e.v1);
  // (every cell a voxel of the slab lies in, at any LOD)
  const k1 = chunk.rangeZ(e.w0, e.w1 + chunk.s - 1)[1];
  if (i0 > i1 || j0 > j1 || chunk.rangeZ(e.w0 - chunk.s + 1, e.w1)[0] > k1) return;
  const rs = makeRoadSample();
  const m = pl.m;
  const d = pl.d;
  for (let j = j0; j <= j1; j += 1) {
    const v = chunk.wy(j) + 0.5;
    for (let i = i0; i <= i1; i += 1) {
      const u = chunk.wx(i) + 0.5;
      // the column's centre on the surface, in the world
      const x = pl.origin.x + (m[0] * u + m[1] * v + m[2] * SLAB) / d;
      const y = pl.origin.y + (m[3] * u + m[4] * v + m[5] * SLAB) / d;
      const c = world.cellAt(x, y);
      sampleRoadSurface(world.roadView(c.i, c.j).near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x, y, rs, world.seed);
      if (rs.kind === KIND.NONE) continue;
      const top = SLAB - 1 + rs.dz;
      const sub = rs.kind === KIND.CARRIAGE ? MAT.GRAVEL : MAT.CONCRETE;
      // (a coarse cell holds the slab where any of its voxels does: a thin slab survives every LOD)
      const lo = chunk.half;
      for (let k = chunk.rangeZ(e.w0 - chunk.s + 1, e.w1)[0]; k <= k1; k += 1) {
        const w = chunk.wz(k) - lo;
        if (w > top) break;
        chunk.data[i + j * P + k * P2] = w + chunk.s > top ? rs.mat : sub;
      }
    }
  }
}

/**
 * World height (continuous) of a road piece's slab foot (its local w = 0
 * plane) over world point (x, y): where the ground under it stops in parts
 * mode (compose.js), so ground and slab meet without a gap.
 */
export function slabFoot(part, x, y) {
  const { m, d, origin } = part.placement;
  // (u, v) of the plane's point over (x, y): the 2 x 2 system of the placement's x, y rows at w = 0
  const a = m[0];
  const b = m[1];
  const c = m[3];
  const e = m[4];
  const det = a * e - b * c;
  const px = (x - origin.x) * d;
  const py = (y - origin.y) * d;
  const u = (e * px - b * py) / det;
  const v = (a * py - c * px) / det;
  return origin.z + (m[6] * u + m[7] * v) / d;
}
