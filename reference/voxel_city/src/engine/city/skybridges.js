import { Rng } from "../core/hash.js";
import { floorZ } from "../buildings/archetypes.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";

/**
 * Skybridges: enclosed glass walkways between office buildings that face
 * each other across a street, at podium level (floor 3+, above lamps,
 * signals and street trees). Planned per cell once every envelope is known:
 * the bridge is recorded on the cell plan (geometry) and on both envelopes
 * (`env.skyDoors`), so the lazily planned interiors carve a door exactly
 * where the bridge meets the facade. Futuristic cities build many; modern
 * downtowns a few.
 */

const INNER = 20; // clear width 2.5 m
const H = 22; // clear height 2.75 m
const MIN_GAP = 64;
const MAX_GAP = 240;
const OPP = { N: "S", S: "N", E: "W", W: "E" };

function officeLike(env) {
  if (env.archetype === "office") return true;
  return env.archetype === "tower" && env.program.upper !== "apartments";
}

/** Floors where the front facade is the lot front (whole footprint). */
function bridgeFloors(env) {
  const top = env.archetype === "tower" ? env.podiumFloors - 1 : env.floors - 2;
  const out = [];
  for (let f = 3; f <= top; f += 1) out.push(f);
  return out;
}

export function planSkybridges(world, plan, buildings, corridors) {
  const seed = world.seed;
  // (between buildings square to the grid: a turned one, the angled world's, has no facade to meet)
  const cand = buildings.filter((b) => officeLike(b) && !b.turn);
  const bridges = [];
  const used = new Map();
  for (let a = 0; a < cand.length; a += 1) {
    for (let b = 0; b < cand.length; b += 1) {
      const A = cand[a];
      const B = cand[b];
      if (A === B || OPP[A.front] !== B.front) continue;
      // A faces south / east towards B across the street (each pair once)
      if (A.front !== "S" && A.front !== "E") continue;
      const alongX = A.front === "S";
      const gap = alongX ? B.R.y0 - A.R.y1 - 1 : B.R.x0 - A.R.x1 - 1;
      if (gap < MIN_GAP || gap > MAX_GAP) continue;
      const lo = alongX ? Math.max(A.R.x0, B.R.x0) : Math.max(A.R.y0, B.R.y0);
      const hi = alongX ? Math.min(A.R.x1, B.R.x1) : Math.min(A.R.y1, B.R.y1);
      // stay in the middle half of both facades (open office, not end rooms)
      const midA = alongX ? [A.R.x0 + (A.R.x1 - A.R.x0) / 4, A.R.x1 - (A.R.x1 - A.R.x0) / 4] : [A.R.y0 + (A.R.y1 - A.R.y0) / 4, A.R.y1 - (A.R.y1 - A.R.y0) / 4];
      const midB = alongX ? [B.R.x0 + (B.R.x1 - B.R.x0) / 4, B.R.x1 - (B.R.x1 - B.R.x0) / 4] : [B.R.y0 + (B.R.y1 - B.R.y0) / 4, B.R.y1 - (B.R.y1 - B.R.y0) / 4];
      const s0 = Math.ceil(Math.max(lo, midA[0], midB[0]));
      const s1 = Math.floor(Math.min(hi, midA[1], midB[1]));
      if (s1 - s0 + 1 < INNER + 6) continue;
      const futuristic = A.flavor === "futuristic" || B.flavor === "futuristic";
      const chance = futuristic ? 0.75 : A.district === "downtown" ? 0.15 : 0;
      const rng = Rng.from(seed, A.id, B.id, "skybridge");
      if (!rng.chance(chance)) continue;
      if ((used.get(A.id) ?? 0) >= 2 || (used.get(B.id) ?? 0) >= 2) continue;
      // matching floors (level within 2 voxels so the walk is a gentle slope)
      let fa = -1;
      let fb = -1;
      for (const f of bridgeFloors(A)) {
        const za = floorZ(A, f);
        const g = bridgeFloors(B).find((k) => Math.abs(floorZ(B, k) - za) <= 2);
        if (g !== undefined && !(A.skyDoors ?? []).some((d) => d.floor === f) && !(B.skyDoors ?? []).some((d) => d.floor === g)) {
          fa = f;
          fb = g;
          break;
        }
      }
      if (fa < 0) continue;
      const c = Math.round((s0 + s1) / 2);
      const w0 = c - INNER / 2 - 1;
      const w1 = c + INNER / 2;
      const rect = alongX ? { x0: w0, x1: w1, y0: A.R.y1 + 1, y1: B.R.y0 - 1 } : { x0: A.R.x1 + 1, x1: B.R.x0 - 1, y0: w0, y1: w1 };
      if (corridors.some((k) => k.hitsRect(rect))) continue;
      const za = floorZ(A, fa);
      const zb = floorZ(B, fb);
      const bridge = { id: `${plan.id}/sky${bridges.length}`, a: A.id, b: B.id, alongX, rect, za, zb, bb: { ...rect, z0: Math.min(za, zb) - 2, z1: Math.max(za, zb) + H + 6 } };
      bridges.push(bridge);
      // door spans: the clear width on each facade (world coordinates)
      const door = alongX ? { x0: c - 8, x1: c + 7 } : { y0: c - 8, y1: c + 7 };
      (A.skyDoors ??= []).push({ floor: fa, span: door, bridge: bridge.id });
      (B.skyDoors ??= []).push({ floor: fb, span: door, bridge: bridge.id });
      used.set(A.id, (used.get(A.id) ?? 0) + 1);
      used.set(B.id, (used.get(B.id) ?? 0) + 1);
    }
  }
  return bridges;
}

/** Feature source: slab, glass sides with mullions, roof with a light strip. */
export const skybridgeSource = {
  id: "skybridges",
  order: 9,
  maxLod: 4,
  zRange(world, rect) {
    let lo = Infinity;
    let hi = -Infinity;
    for (const br of bridgesNear(world, rect)) {
      lo = Math.min(lo, br.bb.z0);
      hi = Math.max(hi, br.bb.z1);
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk) {
    const box = chunk.worldBox;
    for (const br of bridgesNear(world, box)) {
      const r = br.rect;
      if (br.bb.z1 < box.z0 || br.bb.z0 > box.z1) continue;
      const [i0, i1] = chunk.rangeX(Math.max(r.x0, box.x0), Math.min(r.x1, box.x1));
      const [j0, j1] = chunk.rangeY(Math.max(r.y0, box.y0), Math.min(r.y1, box.y1));
      const span = br.alongX ? r.y1 - r.y0 : r.x1 - r.x0;
      for (let j = j0; j <= j1; j += 1) {
        const y = chunk.wy(j);
        for (let i = i0; i <= i1; i += 1) {
          const x = chunk.wx(i);
          const along = br.alongX ? y - r.y0 : x - r.x0;
          const across = br.alongX ? x - r.x0 : y - r.y0;
          const width = br.alongX ? r.x1 - r.x0 : r.y1 - r.y0;
          const t = along / Math.max(1, span);
          const zf = Math.round(br.za + (br.zb - br.za) * t);
          const side = across === 0 || across === width;
          const post = side && along % 16 === 0;
          const [k0, k1] = chunk.rangeZ(zf - 1, zf + H + 4);
          for (let k = k0; k <= k1; k += 1) {
            const zr = chunk.wz(k) - zf;
            let m = 0;
            if (zr <= 0) m = MAT.PANEL_GRAPHITE; // underside
            else if (zr === 1) m = side ? MAT.PANEL_GRAPHITE : MAT.FLOOR_TERRAZZO;
            else if (zr <= H + 1) {
              if (side) m = post || zr === 2 || zr === H + 1 ? MAT.MULLION : MAT.GLASS_TINT;
            } else m = zr === H + 2 && !side && across === Math.floor(width / 2) ? MAT.LIGHT_STRIP : MAT.PANEL_WHITE;
            chunk.data[i + j * P + k * P2] = m;
          }
        }
      }
    }
  },
};

function bridgesNear(world, rect) {
  const out = [];
  for (const { i, j } of world.cellsOverlapping(rect)) {
    for (const br of world.cellPlan(i, j).skybridges ?? []) {
      const r = br.rect;
      if (r.x1 < rect.x0 || r.x0 > rect.x1 || r.y1 < rect.y0 || r.y0 > rect.y1) continue;
      out.push(br);
    }
  }
  return out;
}
