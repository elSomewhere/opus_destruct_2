import { frameOf } from "./frame.js";
import { chamferCut } from "./chamfer.js";
import { tierRects, floorZ } from "./archetypes.js";
import { buildingLook, facadeCell, facadeMaterial } from "./facade.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { hashFloat } from "../core/hash.js";
import { seasonOf } from "../world/season.js";
import { LAPSE } from "../nature/landcover.js";

/**
 * Coarse building voxelizer: solid massing with the shared facade rules,
 * flat roofs with parapets, pitched roofs, setbacks and annexes. Used for
 * every LOD above 0 (and as a fallback). Point-sampled through the chunk
 * writer, so it works at any resolution.
 */

/**
 * Snow on roofs (world/season.js): the season's snow cover (0..1) at the
 * building's local temperature (its town's climate cooled by the height of
 * its ground floor), or a climate's explicit `snowCover`, lies on the
 * outermost surface layer of pitched, flat and annex roofs, in the coarse
 * massing and in the LOD0 interiors alike. Eaves / verge rows and parapet
 * caps stay bare so roof outlines still read; below 1 a deterministic share
 * of the roof surface is bare, in patches of a few metres (smooth value
 * noise of world x, y, so every LOD agrees). Summer: no snow.
 */
export function roofSnowCover(world, env) {
  if (env._snow !== undefined) return env._snow;
  const season = seasonOf(world.config);
  let c = 0;
  if (season.any) {
    const t = world.fields.temperature((env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2) - Math.max(0, env.baseZ / 8) / LAPSE;
    c = Math.min(1, season.roofSnow(t));
  }
  env._snow = c;
  return c;
}

/** Is the roof surface at world column (x, y) under snow for this cover? */
export function snowAt(seed, cover, x, y) {
  if (cover >= 1) return true;
  // smooth 2.5 m value noise with a little 0.5 m grain: bare patches, not speckle
  const c = 20;
  const gx = Math.floor(x / c);
  const gy = Math.floor(y / c);
  let fx = x / c - gx;
  let fy = y / c - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const a = hashFloat(seed, gx, gy, 0x5e0);
  const b = hashFloat(seed, gx + 1, gy, 0x5e0);
  const d = hashFloat(seed, gx, gy + 1, 0x5e0);
  const e = hashFloat(seed, gx + 1, gy + 1, 0x5e0);
  const n = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
  return n * 0.8 + hashFloat(seed, x >> 2, y >> 2, 0x5e1) * 0.2 < cover;
}

/** Default eaves overhang (voxels) of pitched roofs. */
const OVERHANG = 3;

// (a building's frame, turned or not: buildings/frame.js)
export { frameOf };

function inside(rects, u, v) {
  for (let k = 0; k < rects.length; k += 1) {
    const r = rects[k];
    if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  }
  return false;
}

/** Facade side of a wall cell ('F','B','L','R') or null if interior. */
function wallSide(rects, u, v, t) {
  if (!inside(rects, u, v - t)) return "F";
  if (!inside(rects, u, v + t)) return "B";
  if (!inside(rects, u - t, v)) return "L";
  if (!inside(rects, u + t, v)) return "R";
  return null;
}

function containingRect(rects, u, v) {
  for (const r of rects) if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return r;
  return rects[0];
}

export function voxelizeMassing(world, env, chunk) {
  const s = chunk.s;
  const box = chunk.worldBox;
  const b = env.bounds;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1) return;
  if (env.topZ < box.z0 || env.bottomZ > box.z1) return;
  const frame = frameOf(env);
  const look = buildingLook(env, world.seed);
  const st = look.style;
  const [i0, i1] = chunk.rangeX(Math.max(b.x0, box.x0), Math.min(b.x1, box.x1));
  const [j0, j1] = chunk.rangeY(Math.max(b.y0, box.y0), Math.min(b.y1, box.y1));
  const thick = Math.max(2, s);
  const nF = env.floors;
  const zTop = floorZ(env, nF);
  const snow = roofSnowCover(world, env);

  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const [u, v] = frame.fromWorld(x, y);

      // annexes (garages): simple single-story boxes
      for (const a of env.annexes) {
        const r = a.world;
        if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
        // (a turned building's annex is its canonical rect; the world box only culls)
        const c = a.canon;
        if (c && (u < c.x0 || u > c.x1 || v < c.y0 || v > c.y1)) continue;
        if (a.kind === "canopy" || a.kind === "pylon") {
          openAnnex(chunk, i, j, a, env, x, y, snow && snowAt(world.seed, snow, x, y));
          continue;
        }
        const z0 = env.baseZ;
        const edge = c ? u - c.x0 < thick || c.x1 - u < thick || v - c.y0 < thick || c.y1 - v < thick : x - r.x0 < thick || r.x1 - x < thick || y - r.y0 < thick || r.y1 - y < thick;
        const [k0, k1] = chunk.rangeZ(z0, z0 + a.height + 1);
        for (let k = k0; k <= k1; k += 1) {
          const zr = chunk.wz(k) - z0;
          let m = edge ? st.wall : MAT.PAINT_DARK;
          if (zr >= a.height) m = snow && zr > a.height + 1 - s && snowAt(world.seed, snow, x, y) ? MAT.SNOW : MAT.ROOF_MEMBRANE;
          else if (zr < 2) m = MAT.CONCRETE;
          chunk.data[i + j * P + k * P2] = m;
        }
      }

      // basements: solid foundation block under the ground tier
      const ground = env.tiers[0].rects;
      const inGround = inside(ground, u, v);
      if (inGround && env.basements > 0) {
        const [k0, k1] = chunk.rangeZ(env.bottomZ, env.baseZ - 1);
        for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = MAT.CONCRETE_DARK;
      }
      // raised ground floors on a plinth (cabins): stone up to the floor slab
      if (inGround && env.plinth) {
        const [k0, k1] = chunk.rangeZ(env.groundZ + 1, env.baseZ - 1);
        for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = st.base;
      }

      // (a chamfered corner: nothing from the ground floor up, its slab part holds the facade)
      if (env.chamfer && chamferCut(env.chamfer, env.U, u, v)) continue;
      for (let f = 0; f < nF; f += 1) {
        const z0 = floorZ(env, f);
        const H = env.storyH[f];
        if (z0 > box.z1) break;
        const rects = tierRects(env, f);
        const inF = inside(rects, u, v);
        const upper = f + 1 < nF ? tierRects(env, f + 1) : null;
        // roof over this tier where the next tier doesn't continue
        if (inF && (f === nF - 1 || !inside(upper, u, v))) {
          if (!(f === nF - 1 && env.roof.type !== "flat")) {
            flatRoof(chunk, i, j, z0 + H, rects, u, v, thick, st, snow && snowAt(world.seed, snow, x, y));
          }
        }
        if (z0 + H - 1 < box.z0) continue;
        if (!inF) continue;
        const side = wallSide(rects, u, v, thick);
        const [k0, k1] = chunk.rangeZ(z0, z0 + H - 1);
        let len = 0;
        let t = 0;
        if (side) {
          const r = containingRect(rects, u, v);
          if (side === "F" || side === "B") {
            len = r.x1 - r.x0 + 1;
            t = side === "F" ? u - r.x0 : r.x1 - u;
          } else {
            len = r.y1 - r.y0 + 1;
            t = side === "L" ? r.y1 - v : v - r.y0;
          }
        }
        for (let k = k0; k <= k1; k += 1) {
          const zr = chunk.wz(k) - z0;
          let m;
          if (side) {
            const cls = facadeCell(look, len, t, zr, H, f);
            m = facadeMaterial(look, cls, f, zr);
          } else {
            m = zr < 2 ? MAT.CONCRETE : MAT.PAINT_DARK;
          }
          chunk.data[i + j * P + k * P2] = m;
        }
      }

      if (env.roof.type !== "flat" && !(env.steeple && steeple(chunk, i, j, env, zTop, u, v, st))) {
        pitchedRoof(chunk, i, j, env, zTop, u, v, st, snow && snowAt(world.seed, snow, x, y));
      }
      if (env.domes) naveDomes(chunk, i, j, env, zTop, u, v, st);
    }
  }
}

/** Pitched roofs + annexes only (LOD0 interiors draw everything else). */
export function pitchedRoofOnly(world, env, chunk) {
  const box = chunk.worldBox;
  const b = env.bounds;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1) return;
  const frame = frameOf(env);
  const st = buildingLook(env, world.seed).style;
  const [i0, i1] = chunk.rangeX(Math.max(b.x0, box.x0), Math.min(b.x1, box.x1));
  const [j0, j1] = chunk.rangeY(Math.max(b.y0, box.y0), Math.min(b.y1, box.y1));
  const zTop = floorZ(env, env.floors);
  const thick = 2;
  const snow = roofSnowCover(world, env);
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const [u, v] = frame.fromWorld(x, y);
      for (const a of env.annexes) {
        const r = a.world;
        if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
        // (a turned building's annex is its canonical rect; the world box only culls)
        const c = a.canon;
        if (c && (u < c.x0 || u > c.x1 || v < c.y0 || v > c.y1)) continue;
        if (a.kind === "canopy" || a.kind === "pylon") {
          openAnnex(chunk, i, j, a, env, x, y, snow && snowAt(world.seed, snow, x, y));
          continue;
        }
        const z0 = env.baseZ;
        const edge = c ? u - c.x0 < thick || c.x1 - u < thick || v - c.y0 < thick || c.y1 - v < thick : x - r.x0 < thick || r.x1 - x < thick || y - r.y0 < thick || r.y1 - y < thick;
        const [k0, k1] = chunk.rangeZ(z0, z0 + a.height + 1);
        for (let k = k0; k <= k1; k += 1) {
          const zr = chunk.wz(k) - z0;
          let m = edge ? (zr > 1 && zr < 18 && a.kind === "garage" && isGarageDoor(env, frame, a, u, v) ? MAT.ROLLUP_DOOR : st.wall) : zr < 2 ? MAT.FLOOR_CONCRETE : 0;
          if (zr >= a.height) m = snow && zr > a.height && snowAt(world.seed, snow, x, y) ? MAT.SNOW : MAT.ROOF_MEMBRANE;
          chunk.data[i + j * P + k * P2] = m;
        }
      }
      if (env.roof.type !== "flat" && !(env.steeple && steeple(chunk, i, j, env, zTop, u, v, st))) {
        pitchedRoof(chunk, i, j, env, zTop, u, v, st, snow && snowAt(world.seed, snow, x, y));
      }
      if (env.domes) naveDomes(chunk, i, j, env, zTop, u, v, st);
    }
  }
}

/** Profile of an onion dome: radius share at height share t (a bulge a third of the way up, a drawn-out point). */
function onionProfile(t) {
  if (t < 0.36) return 0.68 + 0.32 * Math.sin((t / 0.36) * (Math.PI / 2));
  return Math.pow(Math.cos(((t - 0.36) / 0.64) * (Math.PI / 2)), 1.4);
}

/**
 * One column of an onion dome centred (du, dv) away: a drum (radius rd,
 * dh voxels tall from z0, a cornice on top), the bulb (radius R, H tall)
 * above it and a gilded three-bar cross on the tip.
 */
function onion(chunk, i, j, du, dv, z0, rd, dh, R, H, m, st) {
  const col = (a, b, mat) => {
    const [k0, k1] = chunk.rangeZ(a, b);
    for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = mat;
  };
  const d = Math.hypot(du, dv);
  if (dh > 0 && d <= rd + 0.5) col(z0, z0 + dh - 2, st.wall);
  if (dh > 0 && d <= rd + 1.5) col(z0 + dh - 1, z0 + dh - 1, st.trim);
  const zb = z0 + dh;
  let lo = -1;
  let hi = -1;
  for (let h = 0; h < H; h += 1) {
    if (R * onionProfile((h + 0.5) / H) + 0.35 >= d) {
      if (lo < 0) lo = h;
      hi = h;
    }
  }
  if (lo >= 0) col(zb + lo, zb + hi, m);
  // the cross: a rod, a short top bar, the main bar and a slanted foot bar
  // (one column wide at every LOD: columns are 1 << lod voxels apart)
  const step = 1 << chunk.lod;
  if (dv < 0 || dv >= step) return;
  const top = zb + H;
  const a = Math.abs(du);
  if (du >= 0 && du < step) col(top - 1, top + 15, MAT.GOLD);
  else if (a <= 4) {
    col(top + 10, top + 10, MAT.GOLD);
    if (a <= 2) col(top + 13, top + 13, MAT.GOLD);
    if (a <= 3) col(top + 5 + Math.sign(du), top + 5 + Math.sign(du), MAT.GOLD);
  }
}

/** Onion domes on drums along a church's nave ridge (env.domes; the drums rise from the eaves through the roof). */
function naveDomes(chunk, i, j, env, zTop, u, v, st) {
  for (const dm of env.domes) {
    const r = env.tiers[0].rects[dm.rect];
    const cu = Math.floor((r.x0 + r.x1) / 2);
    const cv = Math.round(r.y0 + (r.y1 - r.y0) * dm.fv);
    const du = u - cu;
    const dv = v - cv;
    if (Math.abs(du) > dm.r + 5 || Math.abs(dv) > dm.r + 5) continue;
    onion(chunk, i, j, du, dv, zTop, Math.round(dm.r * 0.62), dm.drum, dm.r, dm.h, dm.m, st);
  }
}

/**
 * Open annexes of civic lots: a petrol canopy (a flat roof with a coloured
 * fascia on steel columns at its corners and along its sides) and a sign
 * pylon (a post with a lit panel). Drawn at every LOD.
 */
function openAnnex(chunk, i, j, a, env, x, y, snowy) {
  const r = a.world;
  const z0 = env.groundZ + 1;
  const fascia = env.signColor ?? MAT.SIGNAGE_RED;
  if (a.kind === "pylon") {
    const post = x - r.x0 < 2 && y - r.y0 < 2;
    const [k0, k1] = chunk.rangeZ(z0, z0 + a.height);
    for (let k = k0; k <= k1; k += 1) {
      const zr = chunk.wz(k) - z0;
      if (zr >= a.height - 20) chunk.data[i + j * P + k * P2] = zr >= a.height - 2 ? MAT.METAL_PANEL_DARK : fascia;
      else if (post) chunk.data[i + j * P + k * P2] = MAT.METAL_PANEL_DARK;
    }
    return;
  }
  const H = a.height;
  const edge = x - r.x0 < 2 || r.x1 - x < 2 || y - r.y0 < 2 || r.y1 - y < 2;
  const col = (Math.abs(x - r.x0 - 12) < 2 || Math.abs(r.x1 - x - 12) < 2 || Math.abs(x - (r.x0 + r.x1) / 2) < 2) && Math.abs(y - (r.y0 + r.y1) / 2) < 2;
  const [k0, k1] = chunk.rangeZ(z0, z0 + H + 3);
  for (let k = k0; k <= k1; k += 1) {
    const zr = chunk.wz(k) - z0;
    let m = 0;
    if (zr >= H) m = zr === H + 3 ? (snowy && !edge ? MAT.SNOW : edge ? fascia : MAT.PLASTIC_WHITE) : edge ? fascia : MAT.PLASTIC_WHITE;
    else if (col) m = MAT.METAL_PANEL;
    if (zr === H && !edge && ((x >> 4) + (y >> 4)) % 3 === 0) m = MAT.LIGHT_STRIP;
    if (m) chunk.data[i + j * P + k * P2] = m;
  }
}

function isGarageDoor(env, frame, a, u, v) {
  const g = a.canon ?? frame.rectFromWorld(a.world);
  return v <= g.y0 + 1 && u >= g.x0 + 3 && u <= g.x1 - 3;
}

function flatRoof(chunk, i, j, zr0, rects, u, v, thick, st, snowy) {
  const edge =
    !inside(rects, u - thick, v) || !inside(rects, u + thick, v) || !inside(rects, u, v - thick) || !inside(rects, u, v + thick);
  const parapet = edge ? 5 : 0;
  const [k0, k1] = chunk.rangeZ(zr0, zr0 + 1 + parapet);
  for (let k = k0; k <= k1; k += 1) {
    const zz = chunk.wz(k) - zr0;
    let m;
    if (zz < 2) m = edge ? st.wall : snowy && zz > 1 - chunk.s ? MAT.SNOW : st.roof;
    else m = zz === 1 + parapet ? MAT.PARAPET_CAP : st.wall;
    chunk.data[i + j * P + k * P2] = m;
  }
}

/** Eaves overhang of a pitched roof per canonical side (`roof.overhang`: voxels or {F,B,L,R}). */
function overhangOf(roof) {
  const o = roof.overhang ?? OVERHANG;
  return typeof o === "number" ? { F: o, B: o, L: o, R: o } : { F: o.F ?? OVERHANG, B: o.B ?? OVERHANG, L: o.L ?? OVERHANG, R: o.R ?? OVERHANG };
}

/**
 * Pitched roof over the top tier's main rect. `roof.slope` is the rise per
 * voxel (default 0.7, ~35°), `roof.ridge` the ridge direction: "u" (default,
 * parallel to the street, eaves at the front and back) or "v" (gable to the
 * street). The top `skin` voxels are roofing, the attic below is solid.
 */
function pitchedRoof(chunk, i, j, env, zTop, u, v, st, snowy) {
  const roof = env.roof;
  const rects = env.tiers[env.tiers.length - 1].rects;
  const main = rects[0];
  const oh = overhangOf(roof);
  const r = { x0: main.x0 - oh.L, y0: main.y0 - oh.F, x1: main.x1 + oh.R, y1: main.y1 + oh.B };
  if (u < r.x0 || u > r.x1 || v < r.y0 || v > r.y1) return;
  const dv = Math.min(v - r.y0, r.y1 - v);
  const du = Math.min(u - r.x0, r.x1 - u);
  const alongV = roof.ridge === "v";
  let h;
  if (roof.type === "hip") h = Math.min(du, dv);
  else if (roof.type === "sawtooth") h = ((v - r.y0) % 48) / 3;
  else h = alongV ? du : dv;
  const slope = roof.slope ?? 0.7;
  h = Math.floor(h * slope) + 1;
  // roofing deep enough to cover the steps between sampled columns at
  // coarse LODs (2 voxels up close), so no attic shows through
  const skin = Math.ceil(slope * chunk.s) + chunk.s;
  const inMain = u >= main.x0 && u <= main.x1 && v >= main.y0 && v <= main.y1;
  const base = inMain ? zTop : zTop - 2;
  const [k0, k1] = chunk.rangeZ(base, zTop + h);
  const gableWall = roof.type === "gable" && inMain && (alongV ? v - main.y0 < 2 || main.y1 - v < 2 : u - main.x0 < 2 || main.x1 - u < 2);
  // snow on the top voxel(s) (as deep as the steps of a steep roof), the eaves and verge rows stay bare
  const snowTop = snowy && u > r.x0 && u < r.x1 && v > r.y0 && v < r.y1 ? h - chunk.s * Math.max(1, Math.ceil(slope)) : Infinity;
  const gable = gableWall ? gableMaterial(st, main, u, v, alongV) : 0;
  for (let k = k0; k <= k1; k += 1) {
    const zz = chunk.wz(k) - zTop;
    let m = zz >= h - (skin - 1) ? (zz > snowTop ? MAT.SNOW : st.pitched) : gableWall ? (st.seamDir === "h" && zz % 3 === 0 ? st.seam : gable) : MAT.WOOD_MED;
    if (!inMain && zz < h - skin) continue;
    chunk.data[i + j * P + k * P2] = m;
  }
}

/** Gable triangle wall: the facade wall, with the nordic board seams / corner boards continued. */
function gableMaterial(st, main, u, v, alongV) {
  if (!st.seam && !st.corners) return st.wall;
  // position along the gable's facade side, measured like the facade below
  const len = alongV ? main.x1 - main.x0 + 1 : main.y1 - main.y0 + 1;
  const t = alongV ? (v - main.y0 < 2 ? u - main.x0 : main.x1 - u) : u - main.x0 < 2 ? main.y1 - v : v - main.y0;
  if (st.corners && (t < 2 || t >= len - 2)) return st.trim;
  if (st.seam && st.seamDir === "v" && t % 3 === 0) return st.seam;
  return st.wall;
}

/**
 * Church steeple over ground-tier rect `env.steeple.rect`: the tower shaft
 * rises `shaft` voxels above the eaves, boarded like the facade with corner
 * boards and louvred belfry openings near the top; a pyramid spire of
 * `spire` voxels flares out over it and carries a finial cross. Draws the
 * column and returns true inside the tower rect (no roof there); outside it
 * only adds the spire's flared eaves, high above the nave roof.
 */
function steeple(chunk, i, j, env, zTop, u, v, st) {
  const sp = env.steeple;
  const t = env.tiers[0].rects[sp.rect];
  const oh = 2;
  if (u < t.x0 - oh || u > t.x1 + oh || v < t.y0 - oh || v > t.y1 + oh) return false;
  const inT = u >= t.x0 && u <= t.x1 && v >= t.y0 && v <= t.y1;
  const zS = zTop + sp.shaft;
  if (inT) {
    // shaft: boarded shell, solid inside
    const W = t.x1 - t.x0 + 1;
    const D = t.y1 - t.y0 + 1;
    let len = 0;
    let tt = -1;
    if (v - t.y0 < 2) [len, tt] = [W, u - t.x0];
    else if (t.y1 - v < 2) [len, tt] = [W, t.x1 - u];
    else if (u - t.x0 < 2) [len, tt] = [D, t.y1 - v];
    else if (t.x1 - u < 2) [len, tt] = [D, v - t.y0];
    const [k0, k1] = chunk.rangeZ(zTop, zS - 1);
    for (let k = k0; k <= k1; k += 1) {
      const zr = chunk.wz(k) - zTop;
      let m = st.wall;
      if (tt >= 0) {
        const belfry = zr >= sp.shaft - 22 && zr < sp.shaft - 5 && Math.abs(tt - (len - 1) / 2) <= len * 0.22;
        if (tt < 2 || tt >= len - 2) m = st.trim;
        else if (belfry) m = zr % 2 === 0 ? MAT.WOOD_DARK : MAT.FRAME_DARK;
        else if (zr === sp.shaft - 23 || zr === sp.shaft - 4) m = st.trim;
        else m = st.seam && st.seamDir === "v" && tt % 3 === 0 ? st.seam : st.wall;
      }
      chunk.data[i + j * P + k * P2] = m;
    }
  }
  const cu0 = Math.floor((t.x0 + t.x1) / 2);
  const cv0 = Math.floor((t.y0 + t.y1) / 2);
  if (sp.dome && !sp.tent) {
    // an onion dome on a drum over a cornice round the tower top
    if (!inT) {
      const [k0, k1] = chunk.rangeZ(zS - 1, zS - 1);
      for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = st.trim;
      return false;
    }
    const [k0, k1] = chunk.rangeZ(zS, zS);
    for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = st.trim;
    const R = Math.floor((Math.min(t.x1 - t.x0, t.y1 - t.y0) + 1) * 0.46);
    onion(chunk, i, j, u - cu0, v - cv0, zS + 1, Math.round(R * 0.6), 8, R, Math.min(sp.spire, Math.round(R * 2.5)), sp.dome, st);
    return true;
  }
  // spire: a pyramid over the tower + overhang, flared eaves at its foot
  const half = (Math.min(t.x1 - t.x0, t.y1 - t.y0) + 1) / 2 + oh;
  const dist = Math.min(u - t.x0 + oh, t.x1 + oh - u, v - t.y0 + oh, t.y1 + oh - v);
  const slope = sp.spire / half;
  const hs = Math.floor(dist * slope);
  const [k0, k1] = chunk.rangeZ(inT ? zS : zS - 1, zS + hs);
  for (let k = k0; k <= k1; k += 1) chunk.data[i + j * P + k * P2] = chunk.wz(k) < zS ? st.trim : sp.dome ?? st.pitched;
  if (sp.dome) {
    // a tented spire (shatyor) with a small onion on its apex
    onion(chunk, i, j, u - cu0, v - cv0, zS + Math.floor(half * slope) - 3, 0, 0, 5, 14, sp.dome, st);
    return inT;
  }
  // finial: a rod with a cross arm over the apex
  const cu = (t.x0 + t.x1) / 2;
  const cv = (t.y0 + t.y1) / 2;
  const top = zS + Math.floor(half * slope);
  if (Math.abs(u - cu) <= 0.5 && Math.abs(v - cv) <= 0.5) {
    const [a0, a1] = chunk.rangeZ(zS + hs + 1, top + 16);
    for (let k = a0; k <= a1; k += 1) chunk.data[i + j * P + k * P2] = MAT.METAL_BLACK;
  } else if (Math.abs(u - cu) <= 3.5 && Math.abs(v - cv) <= 0.5) {
    const [a0, a1] = chunk.rangeZ(top + 11, top + 11);
    for (let k = a0; k <= a1; k += 1) chunk.data[i + j * P + k * P2] = MAT.METAL_BLACK;
  }
  return inT;
}
