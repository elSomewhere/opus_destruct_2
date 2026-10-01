import { MAT } from "../voxel/materials.js";
import { hash32 } from "../core/hash.js";
import { P, P2 } from "../voxel/chunk.js";
import { seasonOf, patchNoise } from "../world/season.js";
import { smoothstep } from "../core/math.js";
import { LAPSE } from "./landcover.js";
import { groundTile } from "../voxel/compose.js";

/**
 * Ground cover up close (LOD 0 only): the natural columns of the ground
 * tile grow small plants a few voxels tall, so meadows and verges are not
 * painted carpets:
 *
 *   tall grass  tufts of one to four voxels in drifts (a ~12 m noise), in
 *               the colour of the season (fresh, summer, straw, dead)
 *   flowers     (world/season.js flower) a stem with a head: lupins stand
 *               tall, buttercups, daisies and campion low, heather in bloom
 *   reeds       along the edge of ponds, lakes and creeks
 *   forest floor blueberry and lingonberry carpets (berries in summer,
 *               red leaves in autumn) in the northern woods, moss cushions,
 *               herbs and woodland grasses under broadleaves, heather in
 *               clumps; stones, twigs and cones, a mushroom now and then
 *   forest floor now and then a tuft of grass in the moss
 *
 * A pure function of the tile and position, the chunk's padding columns
 * too (a neighbour draws them alike: its temperature, its water); lawns,
 * parks, lots and roads (other tile kinds) stay clear.
 */

const MEADOW = new Uint8Array(1024);
for (const m of ["GRASS", "GRASS_DRY", "GRASS_DARK", "GRASS_SPRING", "GRASS_AUTUMN", "GRASS_STRAW", "GRASS_DEAD", "MARSH_GRASS", "MARSH_AUTUMN", "SAVANNA_GRASS", "TUNDRA", "TUNDRA_AUTUMN"]) MEADOW[MAT[m]] = 1;
const FLOOR = new Uint8Array(1024);
for (const m of ["MOSS", "FOREST_FLOOR", "LEAF_LITTER", "NEEDLE_LITTER", "HEATHER"]) FLOOR[MAT[m]] = 1;
const WETEDGE = new Uint8Array(1024);
for (const m of ["MUD", "MARSH_GRASS", "MARSH_AUTUMN", "SAND", "GRAVEL"]) WETEDGE[MAT[m]] = 1;

/** Grass blades in the colour of their ground (and the season). */
function bladeOf(top, season) {
  if (top === MAT.GRASS_STRAW || top === MAT.GRASS_DEAD || top === MAT.SAVANNA_GRASS) return MAT.GRASS_STRAW;
  if (top === MAT.GRASS_AUTUMN || top === MAT.MARSH_AUTUMN) return MAT.GRASS_AUTUMN;
  if (top === MAT.GRASS_SPRING) return MAT.GRASS_SPRING;
  if (top === MAT.GRASS_DRY || top === MAT.TUNDRA) return season === "summer" ? MAT.GRASS_DRY : MAT.GRASS_STRAW;
  return MAT.GRASS_TALL;
}

export const groundcoverSource = {
  id: "groundcover",
  order: 6.8,
  maxLod: 0,
  zRange(world, rect, lod, tile) {
    if (lod > 0 || !tile) return null;
    return [tile.zMin, tile.zMax + 6];
  },
  rasterize(world, chunk, tile) {
    if (chunk.lod > 0 || !tile) return;
    const box = chunk.worldBox;
    if (box.z1 < tile.zMin + 1 || box.z0 > tile.zMax + 6) return;
    const season = seasonOf(world.config);
    const seed = world.seed;
    const wrap = tile.wrap;
    const d = chunk.data;
    // one local temperature per (4 m) tile: the one of the chunk the column is in (the
    // padding's columns are the neighbours', so both draw them alike)
    const temps = new Map();
    const tempAt = (x, y) => {
      const ox = Math.floor(x / 32);
      const oy = Math.floor(y / 32);
      const key = `${ox},${oy}`;
      let t = temps.get(key);
      if (t === undefined) {
        t = world.landCover.climate(ox * 32 + 15.5, oy * 32 + 15.5, 0).t;
        temps.set(key, t);
      }
      return t;
    };
    // (water over the ground at a column: this tile's, or a neighbour tile's past the padding)
    const wetAt = (x, y) => {
      const ox = Math.floor(x / 32);
      const oy = Math.floor(y / 32);
      const t = ox === chunk.cx && oy === chunk.cy ? tile : groundTile(world, 0, ox, oy);
      const k = x - ox * 32 + 1 + (y - oy * 32 + 1) * P;
      return t.water[k] > t.z[k];
    };
    for (let j = 0; j < P; j += 1)
      for (let i = 0; i < P; i += 1) {
        const idx = i + j * P;
        if (tile.kind[idx] !== 4) continue;
        const z = tile.z[idx];
        if (tile.water[idx] >= z) continue;
        if (z + 1 > box.z1 || z + 7 < box.z0) continue;
        const top = tile.top[idx];
        const x = chunk.wx(i);
        const y = chunk.wy(j);
        const t0 = tempAt(x, y);
        const pad = i === 0 || j === 0 || i === P - 1 || j === P - 1;
        const hx = wrap ? wrap.v(x) : x;
        const hy = wrap ? wrap.v(y) : y;
        const h = hash32(seed, wrap ? wrap.vi(x) : x, wrap ? wrap.vi(y) : y, 0x6c0) / 4294967296;
        let stem = 0;
        let stemM = 0;
        let head = 0;
        let headM = 0;
        if (MEADOW[top]) {
          const tl = t0 - Math.max(0, z / 8) / LAPSE;
          // wild flowers first (drifts, a species per drift)
          const f = season.flower(top === MAT.GRASS_SPRING ? MAT.GRASS : top, tl, patchNoise(seed, hx, hy, 48, 0x62), h, patchNoise(seed, hx, hy, 192, 0x64));
          if (f) {
            stemM = MAT.GRASS_TALL;
            headM = f;
            stem = f === MAT.FLOWER_LUPIN ? 2 + ((h * 97) % 3 | 0) : f === MAT.HEATHER_BLOOM ? 0 : 1 + ((h * 53) % 2 | 0);
            head = f === MAT.FLOWER_LUPIN ? 2 : 1;
          } else {
            // tall grass in drifts: rank where the meadow is unmown and lush, short on dry ground
            const lush = smoothstep(0.25, 0.8, patchNoise(seed, hx, hy, 96, 0x71));
            const g = hash32(seed, wrap ? wrap.vi(x) : x, wrap ? wrap.vi(y) : y, 0x6c1) / 4294967296;
            if (g < 0.05 + 0.33 * lush) {
              stemM = bladeOf(top, season.id);
              stem = 1 + Math.floor(g * 97) % (lush > 0.5 ? 4 : 2);
            }
          }
        } else if (FLOOR[top]) {
          // the forest floor: dwarf shrubs, moss cushions, herbs, debris
          const tl = t0 - Math.max(0, z / 8) / LAPSE;
          const boreal = tl < 0.45;
          const carpet = patchNoise(seed, hx, hy, 72, 0x73);
          const s = season.id;
          if (top === MAT.HEATHER) {
            // heather in low clumps, in bloom late in the summer
            if (h < 0.45) {
              stemM = MAT.HEATHER;
              stem = 1 + ((h * 131) % 2 | 0);
              if (s === "summer" && carpet > 0.5 && h < 0.2) {
                head = 1;
                headM = MAT.HEATHER_BLOOM;
              }
            }
          } else if (boreal && carpet > 0.42 && h < 0.62 * smoothstep(0.42, 0.68, carpet)) {
            // blueberry and lingonberry carpets (lingonberry on the drier, lichen-grey patches)
            const lingon = patchNoise(seed, hx, hy, 40, 0x75) > 0.62;
            stemM = s === "autumn" ? (h < 0.3 ? MAT.LEAVES_RED : MAT.TUNDRA_AUTUMN) : lingon ? MAT.LINGON : MAT.BLUEBERRY;
            stem = 1 + ((h * 97) % 2 | 0);
            if ((s === "summer" || s === "autumn") && h < 0.009) {
              head = 1;
              headM = lingon ? MAT.BERRY_RED : MAT.BERRY_BLUE;
            }
          } else if (top === MAT.MOSS && patchNoise(seed, hx, hy, 20, 0x74) > 0.55 && h < 0.4) {
            // moss cushions over stones and old stumps
            stemM = MAT.MOSS_BRIGHT;
            stem = 1;
          } else if (!boreal && carpet > 0.5 && h < 0.3) {
            // herbs and woodland grasses in the broadleaf wood
            stemM = h < 0.12 ? MAT.FERN : MAT.GRASS_DARK;
            stem = 1 + ((h * 53) % 2 | 0);
          } else if (h > 0.984) {
            // debris: stones, twigs and cones, now and then a mushroom
            const q = (h - 0.984) / 0.016;
            if (q > 0.62) {
              stemM = q > 0.85 ? MAT.ROCK_LIGHT : MAT.STONE;
              stem = 1;
            } else if (q > 0.3) {
              stemM = boreal ? MAT.TWIGS : MAT.DEADWOOD;
              stem = 1;
            } else if ((s === "autumn" && q < 0.06) || (s === "summer" && q < 0.02)) {
              stemM = MAT.MUSHROOM_STEM;
              stem = 1;
              head = 1;
              headM = boreal && q < 0.012 ? MAT.MUSHROOM_RED : MAT.MUSHROOM_BROWN;
            }
          } else if (h < 0.035) {
            stemM = top === MAT.NEEDLE_LITTER || top === MAT.MOSS ? MAT.GRASS_DRY : MAT.GRASS_DARK;
            stem = 1 + Math.floor(h * 1000) % 2;
          }
        }
        if (!stem && !head && WETEDGE[top]) {
          // reeds on the edge of the water
          const wet = pad
            ? wetAt(x - 1, y) || wetAt(x + 1, y) || wetAt(x, y - 1) || wetAt(x, y + 1)
            : tile.water[idx - 1] > tile.z[idx - 1] || tile.water[idx + 1] > tile.z[idx + 1] || tile.water[idx - P] > tile.z[idx - P] || tile.water[idx + P] > tile.z[idx + P];
          // (in clumps along the bank, not a hedge)
          const clump = patchNoise(seed, hx, hy, 24, 0x72);
          if (wet && h < 0.5 * smoothstep(0.35, 0.7, clump)) {
            stemM = season.id === "winter" || season.id === "autumn" ? MAT.GRASS_STRAW : h < 0.08 ? MAT.GRASS_TALL : MAT.REED;
            stem = 2 + Math.floor(h * 997) % 6;
          }
        }
        if (!stem && !head) continue;
        const [k0, k1] = chunk.rangeZ(z + 1, z + stem + head);
        for (let k = k0; k <= k1; k += 1) {
          const di = idx + k * P2;
          if (d[di] !== 0) continue;
          d[di] = chunk.wz(k) > z + stem ? headM : stemM;
        }
      }
  },
};
