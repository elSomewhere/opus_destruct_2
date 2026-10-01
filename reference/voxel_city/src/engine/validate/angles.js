import { YAWS } from "../core/placement.js";
import { CHUNK } from "../core/units.js";
import { PART_REACH, partsIn } from "../world/parts.js";

/**
 * The angles audit (ANGLED_WORLD_PLAN.md §5.3): what the oriented parts of
 * an area would cost in structvox. Used by scripts/audit-angles.js and
 * test/angles.test.js.
 *
 *   perChunk        parts at home per chunk: the most in one chunk
 *   resident        parts at home within a disc of angles.residentRadius
 *                   (96 m, structvox's load radius), over discs every 32 m
 *                   in the area: mean, 95th percentile, most (the budget:
 *                   ~6-8 on average, never more than angles.maxResident)
 *   partChunks      chunks a part's box meets (junctions with the world
 *                   grid are computed there)
 *   interfaceChunks chunks where the boxes of two parts meet
 *   overlap         voxels inside the boxes of two parts, as a share of all
 *                   part volume (Rule A: ~0), and those not resolved by a
 *                   priority (equal priorities)
 *   reach           the farthest a part reaches from its home chunk (≤ 4)
 *   angles          yaws, pitches and rolls in use (table indices; a
 *                   product of two yaws is yaw + 132 yaw2)
 *
 * Rects are world voxels.
 */

/** structvox's default load radius (m): angles.residentRadius's default. */
export const LOAD_RADIUS = 96;

const key3 = (x, y, z) => `${x},${y},${z}`;

function extentVolume(e) {
  return (e.u1 - e.u0 + 1) * (e.v1 - e.v0 + 1) * ((e.w1 ?? 0) - (e.w0 ?? 0) + 1);
}

function inExtent(p, x, y, z) {
  const e = p.extent;
  if (p.placement.flat) {
    const [u, v] = p.placement.toLocalXY(x, y);
    const w = z - p.placement.origin.z;
    return u >= e.u0 && u <= e.u1 && v >= e.v0 && v <= e.v1 && w >= (e.w0 ?? 0) && w <= (e.w1 ?? 0);
  }
  const [u, v, w] = p.placement.toLocal(x, y, z);
  return u >= e.u0 && u <= e.u1 && v >= e.v0 && v <= e.v1 && w >= (e.w0 ?? 0) && w <= (e.w1 ?? 0);
}

/** Voxels in both parts' boxes (exact: every voxel's centre tested in both lattices). */
function overlapVoxels(a, b) {
  const x0 = Math.max(a.aabb.x0, b.aabb.x0);
  const x1 = Math.min(a.aabb.x1, b.aabb.x1);
  const y0 = Math.max(a.aabb.y0, b.aabb.y0);
  const y1 = Math.min(a.aabb.y1, b.aabb.y1);
  const z0 = Math.max(a.aabb.z0, b.aabb.z0);
  const z1 = Math.min(a.aabb.z1, b.aabb.z1);
  if (x0 > x1 || y0 > y1 || z0 > z1) return 0;
  let n = 0;
  if (a.placement.flat && b.placement.flat) {
    // flat parts: whole columns
    for (let y = y0; y <= y1; y += 1)
      for (let x = x0; x <= x1; x += 1) if (inExtent(a, x, y, z0) && inExtent(b, x, y, z0)) n += z1 - z0 + 1;
    return n;
  }
  for (let z = z0; z <= z1; z += 1) for (let y = y0; y <= y1; y += 1) for (let x = x0; x <= x1; x += 1) if (inExtent(a, x, y, z) && inExtent(b, x, y, z)) n += 1;
  return n;
}

function quantile(sorted, q) {
  if (!sorted.length) return 0;
  return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
}

export function auditAngles(world, rect, { discStep = 256 } = {}) {
  const R = (world.config.world.angles?.residentRadius ?? LOAD_RADIUS) * 8;
  // parts in the area and in reach of every disc in it
  const all = partsIn(world, { x0: rect.x0 - R, y0: rect.y0 - R, x1: rect.x1 + R, y1: rect.y1 + R });
  const inside = all.filter((p) => p.aabb.x1 >= rect.x0 && p.aabb.x0 <= rect.x1 && p.aabb.y1 >= rect.y0 && p.aabb.y0 <= rect.y1);

  // parts at home per chunk
  const homes = new Map();
  for (const p of inside) {
    const k = key3(p.home.cx, p.home.cy, p.home.cz);
    homes.set(k, (homes.get(k) ?? 0) + 1);
  }
  let perChunkMax = 0;
  for (const n of homes.values()) perChunkMax = Math.max(perChunkMax, n);

  // resident parts: home chunk centres within the load radius of a disc centre
  const counts = [];
  const centres = all.map((p) => [p.home.cx * CHUNK + CHUNK / 2, p.home.cy * CHUNK + CHUNK / 2]);
  for (let y = rect.y0; y <= rect.y1; y += discStep)
    for (let x = rect.x0; x <= rect.x1; x += discStep) {
      let n = 0;
      for (const [hx, hy] of centres) if ((hx - x) * (hx - x) + (hy - y) * (hy - y) <= R * R) n += 1;
      counts.push(n);
    }
  counts.sort((a, b) => a - b);
  const mean = counts.reduce((s, v) => s + v, 0) / Math.max(1, counts.length);

  // chunks the parts' boxes meet, and where two meet
  const touch = new Map();
  for (const p of inside) {
    const b = p.aabb;
    for (let cz = Math.floor(b.z0 / CHUNK); cz <= Math.floor(b.z1 / CHUNK); cz += 1)
      for (let cy = Math.floor(b.y0 / CHUNK); cy <= Math.floor(b.y1 / CHUNK); cy += 1)
        for (let cx = Math.floor(b.x0 / CHUNK); cx <= Math.floor(b.x1 / CHUNK); cx += 1) {
          const k = key3(cx, cy, cz);
          touch.set(k, (touch.get(k) ?? 0) + 1);
        }
  }
  let interfaceChunks = 0;
  for (const n of touch.values()) if (n > 1) interfaceChunks += 1;

  // overlaps between parts (Rule A)
  let volume = 0;
  let overlap = 0;
  let unowned = 0;
  for (const p of inside) volume += extentVolume(p.extent);
  for (let a = 0; a < inside.length; a += 1)
    for (let b = a + 1; b < inside.length; b += 1) {
      const n = overlapVoxels(inside[a], inside[b]);
      overlap += n;
      if (inside[a].priority === inside[b].priority) unowned += n;
    }

  // reach and angles
  let reach = 0;
  const far = [];
  const yaw = new Map();
  const pitch = new Map();
  const roll = new Map();
  const kinds = new Map();
  for (const p of inside) {
    reach = Math.max(reach, p.reach);
    if (p.reach > PART_REACH) far.push(p);
    const pl = p.placement;
    // (a product of two table yaws, a bay on a turned building: yaw + 132 yaw2)
    const y = pl.yaw + (pl.yaw2 ?? 0) * YAWS.length;
    yaw.set(y, (yaw.get(y) ?? 0) + 1);
    pitch.set(pl.pitch, (pitch.get(pl.pitch) ?? 0) + 1);
    roll.set(pl.roll, (roll.get(pl.roll) ?? 0) + 1);
    kinds.set(p.kind, (kinds.get(p.kind) ?? 0) + 1);
  }
  const areaM2 = ((rect.x1 - rect.x0 + 1) * (rect.y1 - rect.y0 + 1)) / 64;
  return {
    parts: inside.length,
    kinds,
    areaM2,
    perArea: (inside.length * (world.config.world.angles?.partArea ?? 3600)) / areaM2,
    perChunk: { max: perChunkMax, homes: homes.size },
    resident: { mean, p95: quantile(counts, 0.95), max: counts.length ? counts[counts.length - 1] : 0, discs: counts.length },
    partChunks: touch.size,
    interfaceChunks,
    overlap: { voxels: overlap, unowned, volume, fraction: volume ? overlap / volume : 0 },
    reach: { max: reach, over: far },
    angles: { yaw, pitch, roll },
  };
}
