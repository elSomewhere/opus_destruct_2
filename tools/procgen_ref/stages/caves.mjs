// Stage "caves": natural caves (nature/caves.js) of several worlds (charts, islands, cold, hot and
// mountain country; caves off, a shallow cave band): the cave regions and field at points, and
// caveSource's z range and rasterizer at LOD 0, 1 and 2 over column tiles of the stage's own
// making (voxel/compose makes the real ones, a later stage of the port): the terrain's height,
// bilinear between the samples at the tile's corners, with a hashed jitter; natural ground mostly,
// some other kinds of column, water and snow. Each tile's chunks (by the cave band's top, at the
// ground, two drawn in the band, one above) are filled with stone under the ground (dirt near it, the tile's
// top on it, water and air pockets) before the caves carve and decorate them. The worlds are
// World.js's with Caves and a LandCover (the source reads both); every terrain sample makes what
// it reads first (lib/worlds.mjs pureTerrain; docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, pureTerrain } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { Caves, caveSource } = await import(REF + "nature/caves.js");
const { LandCover } = await import(REF + "nature/landcover.js");
const { ChunkBuffer, P, P2, P3 } = await import(REF + "voxel/chunk.js");
const { MAT } = await import(REF + "voxel/materials.js");
const { hash32 } = await import(REF + "core/hash.js");

/** The worlds of lib/worlds.mjs this stage samples, and two of its own (tests/city/test_caves.cpp has the same). */
export const CAVE_KEYS = ["cities", "island:large", "nordicIsland:large", "desert", "wrapWorld:small", "planetNorth", "allMountains"];
export const CAVE_WORLDS = [
  ["cavesOff", '{"seed":5,"caves":{"enabled":false}}'],
  ["cavesShallow", '{"seed":6,"caves":{"depth":25}}'],
];

const NO_WATER = -2147483648;

/** FNV-1a over a typed array's values (stages/chunk.mjs). */
function digest(a) {
  let h = 2166136261;
  for (let i = 0; i < a.length; i += 1) h = Math.imul(h ^ a[i], 16777619);
  return h >>> 0;
}

/** A column tile (lod, cx, cy) of the stage's own: the fields caveSource reads. */
function makeTile(w, lod, cx, cy) {
  const s = 1 << lod;
  const half = s >> 1;
  const bx = (cx * 32 - 1) * s;
  const by = (cy * 32 - 1) * s;
  const e = (P - 1) * s;
  const c = [];
  for (const [dx, dy] of [[0, 0], [1, 0], [0, 1], [1, 1]]) c.push(Math.round(w.terrain.sample(bx + half + dx * e, by + half + dy * e).h));
  const n = P * P;
  const z = new Int32Array(n);
  const top = new Uint16Array(n);
  const water = new Int32Array(n).fill(NO_WATER);
  const kind = new Uint8Array(n);
  let zMin = Infinity;
  let zMax = -Infinity;
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const idx = i + j * P;
      const u = i / (P - 1);
      const v = j / (P - 1);
      const h = hash32(w.seed, cx * P + i, cy * P + j, lod);
      z[idx] = Math.round(c[0] * (1 - u) * (1 - v) + c[1] * u * (1 - v) + c[2] * (1 - u) * v + c[3] * u * v) + (h & 7) - 3;
      const q = (h >>> 3) & 63;
      kind[idx] = q < 3 ? 1 : q < 5 ? 2 : q < 6 ? 3 : q < 8 ? 6 : 4;
      top[idx] = q === 8 ? MAT.SNOW : q === 9 ? MAT.SNOW_WIND : MAT.GRASS;
      if (q === 10 || q === 11) water[idx] = z[idx] + 2;
      if (z[idx] < zMin) zMin = z[idx];
      if (z[idx] > zMax) zMax = z[idx];
    }
  return { lod, cx, cy, z, top, water, kind, zMin, zMax };
}

/** The ground of a chunk before the caves: stone under the tile's ground, dirt near it, its top on it, water and air pockets. */
function fill(ch, tile, seed) {
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const col = i + j * P;
      const gz = tile.z[col];
      for (let k = 0; k < P; k += 1) {
        const z = ch.wz(k);
        if (z > gz) continue;
        const h = hash32(seed, col, z, 77) & 63;
        ch.data[col + k * P2] = z === gz ? tile.top[col] : h === 0 ? MAT.WATER : h === 1 ? 0 : gz - z < 3 ? MAT.DIRT : MAT.STONE;
      }
    }
}

export default function* caves() {
  const r = samples(71);
  const worlds = [...allWorlds().filter(([key]) => CAVE_KEYS.includes(key)), ...CAVE_WORLDS.map(([key, json]) => [key, JSON.parse(json)])];
  for (const [key, overrides] of worlds) {
    const w = pureTerrain(new World(overrides));
    w.caves = new Caves(w);
    w.landCover = new LandCover(w);
    const cv = w.caves;
    yield line("caves", key, cv.cfg.enabled, cv.cfg.depth);
    for (let k = 0; k < 400; k += 1) {
      const x = (r() - 0.5) * 400000;
      const y = (r() - 0.5) * 400000;
      const z = (r() - 0.3) * 4000;
      const d = r() * 90;
      yield line("f", x, y, z, d, cv.region(x, y), cv.field(x, y, z, d));
    }
    for (let k = 0; k < 36; k += 1) {
      const lod = k % 3;
      const s = 1 << lod;
      // a column in a cave region (most of the time), or anywhere
      let x = 0;
      let y = 0;
      for (let tries = 0; tries < 20; tries += 1) {
        x = Math.round((r() - 0.5) * 400000);
        y = Math.round((r() - 0.5) * 400000);
        if (k % 6 === 5 || cv.region(x, y) > 0.2) break;
      }
      const span = 32 * s;
      const cx = Math.floor(x / span);
      const cy = Math.floor(y / span);
      const tile = makeTile(w, lod, cx, cy);
      const bx = (cx * 32 - 1) * s;
      const by = (cy * 32 - 1) * s;
      const rect = { x0: bx, y0: by, x1: bx + P * s - 1, y1: by + P * s - 1 };
      const zr = caveSource.zRange(w, rect, lod, tile);
      let ok = 0;
      for (let idx = 0; idx < P * P; idx += 1) if (cv.columnOk(tile, idx)) ok += 1;
      yield line("tile", lod, cx, cy, tile.zMin, tile.zMax, ok, zr ?? "-");
      // chunks: by the cave band's top, at the ground, two drawn in the band, one above the ground
      const lo = zr ? zr[0] : tile.zMin - 200;
      const cz0 = Math.floor((lo + 24) / span);
      const cz1 = Math.floor(tile.zMin / span);
      const cz2 = Math.floor((lo + r() * (tile.zMax - lo)) / span);
      const cz3 = Math.floor((tile.zMin - r() * cv.cfg.depth * 8) / span);
      const cz4 = Math.floor(tile.zMax / span) + 2;
      for (const cz of [cz0, cz1, cz2, cz3, cz4]) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        fill(ch, tile, w.seed);
        const before = ch.data.slice();
        caveSource.rasterize(w, ch, tile);
        let changed = 0;
        let carved = 0;
        for (let i = 0; i < P3; i += 1)
          if (ch.data[i] !== before[i]) {
            changed += 1;
            if (ch.data[i] === 0) carved += 1;
          }
        yield line("ch", lod, cx, cy, cz, digest(before), digest(ch.data), changed, carved);
      }
    }
  }
}
