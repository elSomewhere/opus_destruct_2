/**
 * Golden output: hashes of what a preset generates at fixed sample points,
 * the regression oracle for `world.angles.enabled = false`
 * (ANGLED_WORLD_PLAN.md §5.1). With angles off every existing preset must
 * reproduce the hashes recorded before the angled world existed, bit for
 * bit: the cell plans (roads, blocks, lots, buildings, spaces, via
 * `mapData`), the ground tiles and the voxel chunks at LOD 0, 2, 5 and 8.
 *
 * Used by scripts/golden.js (write / check) and test/golden.test.js.
 */
import { frameOf } from "../../src/engine/buildings/frame.js";
import { createHash } from "node:crypto";
import { createWorld } from "../../src/engine/world/createWorld.js";
import { presetConfig, PRESETS } from "../../src/engine/config/presets.js";
import { buildChunk, groundTile, tileContentRange } from "../../src/engine/voxel/compose.js";
import { mapData } from "../../src/engine/stream/queries.js";
import { CHUNK } from "../../src/engine/core/units.js";
import { partChunk } from "../../src/engine/world/partRaster.js";
import { createSvxSource } from "../../src/engine/svx/source.js";

/** Sample points (m): the spawn town's centre, its streets, the outskirts, open country, far away. */
export const GOLDEN_POINTS = [
  [0, 0],
  [180, -140],
  [1400, 900],
  [-3200, 2600],
  [7000, -5200],
];

/** LODs sampled at every point (LOD 0 builds the interiors). */
export const GOLDEN_LODS = [0, 2, 5, 8];

/** At most this many chunks per column tile (the lowest first: ground, basements, ground floors). */
const MAX_CHUNKS = 10;

/**
 * The presets of the axis-aligned world, recorded before the angled world
 * existed; they keep `world.angles.enabled = false` for good. Angled
 * variants are presets of their own and are not part of this oracle.
 */
export const GOLDEN_PRESETS = ["cities", "infiniteCity", "wrapWorld", "island", "nordicIsland", "nordicTown", "oldHarbourTown", "whiteSeaTown", "planetEquator", "planetNorth"];

/** Every golden preset and size variant: [key, preset id, size id | null]. */
export function goldenConfigs() {
  const out = [];
  for (const id of GOLDEN_PRESETS) {
    const p = PRESETS.get(id);
    if (p.sizes) for (const s of p.sizes) out.push([`${p.id}:${s.id}`, p.id, s.id]);
    else out.push([p.id, p.id, null]);
  }
  return out;
}

function digest(...parts) {
  const h = createHash("sha256");
  for (const p of parts) {
    if (p === null || p === undefined) h.update("\u0000");
    else if (ArrayBuffer.isView(p)) h.update(new Uint8Array(p.buffer, p.byteOffset, p.byteLength));
    else h.update(typeof p === "string" ? p : JSON.stringify(p));
  }
  return h.digest("hex").slice(0, 16);
}

/** Hashes of one column tile: its ground tile and its lowest chunks. */
function tileHash(world, lod, x, y) {
  const span = CHUNK << lod;
  const cx = Math.floor(x / span);
  const cy = Math.floor(y / span);
  const t = groundTile(world, lod, cx, cy);
  const [lo, hi] = tileContentRange(world, t);
  const s = 1 << lod;
  const cz0 = Math.floor(lo / (CHUNK * s));
  const cz1 = Math.min(Math.floor(hi / (CHUNK * s)), cz0 + MAX_CHUNKS - 1);
  const chunks = [];
  for (let cz = cz0; cz <= cz1; cz += 1) chunks.push(buildChunk(world, lod, cx, cy, cz, t).data);
  return digest(t.z, t.top, t.sub, t.kind, t.water, `${lo},${hi}`, ...chunks);
}

/** Buildings sampled at LOD 0 (interiors, fixtures, furniture): spread over the spawn town's by id. */
export const GOLDEN_BUILDINGS = 4;
/** Street doors off their building's level sampled at LOD 0, and how many of the spawn town's buildings are searched for them. */
export const GOLDEN_DOORS = 2;
const GOLDEN_DOOR_SCAN = 80;

/** { sample key -> hash } of one preset configuration. */
export function goldenHashes(presetId, sizeId, { points = GOLDEN_POINTS, lods = GOLDEN_LODS, buildings = GOLDEN_BUILDINGS } = {}) {
  const world = createWorld(presetConfig(presetId, { size: sizeId }));
  const out = {};
  for (const [mx, my] of points) {
    const x = Math.round(mx * 8);
    const y = Math.round(my * 8);
    const key = `${mx},${my}`;
    // the cell plans round the point (a 240 m square)
    out[`plan@${key}`] = digest(mapData(world, { x0: x - 960, y0: y - 960, x1: x + 960, y1: y + 960 }));
    for (const lod of lods) out[`lod${lod}@${key}`] = tileHash(world, lod, x, y);
  }
  // the LOD 0 tile at the centre of a few of the spawn town's buildings
  const envs = world.envelopesIn({ x0: -960, y0: -960, x1: 960, y1: 960 }).sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  for (let k = 0; k < Math.min(buildings, envs.length); k += 1) {
    const env = envs[Math.floor((k * envs.length) / buildings)];
    const b = env.bounds;
    out[`building${k}`] = digest(env.id, tileHash(world, 0, Math.floor((b.x0 + b.x1) / 2), Math.floor((b.y0 + b.y1) / 2)));
  }
  // the LOD 0 tile at a few street doors off their building's level (a slope: a door raised to its
  // street, steps from the street's level, buildings/interior/plan.js streetLevels), among the first
  // of the spawn town's buildings
  let n = 0;
  for (const env of envs.slice(0, GOLDEN_DOOR_SCAN)) {
    if (n >= GOLDEN_DOORS) break;
    const d = world.buildingPlan(env)?.floorByIndex.get(0)?.grid.doors.find((q) => q.street !== undefined);
    if (!d) continue;
    const [x, y] = frameOf(env).toWorld(d.u0, d.v0);
    out[`door${n}`] = digest(env.id, d.u0, d.v0, d.street, d.sill ?? 0, tileHash(world, 0, x, y));
    n += 1;
  }
  return out;
}

/**
 * The angled presets (ANGLED_WORLD_PLAN.md): not part of the axis-aligned
 * oracle, but recorded as well (test/golden/angled.json), so that no change
 * to the angled world goes unnoticed: a change that means to alter it
 * records it again, and the samples that moved say what it touched.
 */
export const ANGLED_PRESETS = ["angledCities", "angledInfiniteCity", "angledNordicTown", "angledOldHarbourTown"];

/** Every angled preset and size variant: [key, preset id, size id | null]. */
export function angledConfigs() {
  const out = [];
  for (const id of ANGLED_PRESETS) {
    const p = PRESETS.get(id);
    if (p.sizes) for (const s of p.sizes) out.push([`${p.id}:${s.id}`, p.id, s.id]);
    else out.push([p.id, p.id, null]);
  }
  return out;
}

/** A part's record as the export gives it (world/parts.js): what structvox would be told. */
function partRecord(p) {
  const pl = p.placement;
  return [p.id, p.key, p.kind, pl.origin.x, pl.origin.y, pl.origin.z, pl.yaw, pl.yaw2, pl.pitch, pl.roll, pl.priority, !!p.anchored, p.extent, p.home, p.members ?? null];
}

/** Hashes of a part's content in its own lattice: its lowest chunks at LOD 0 (8 at most) and one at LOD 2. */
function partHash(world, p) {
  const e = p.extent;
  const chunks = [];
  for (let cz = Math.floor(e.w0 / CHUNK); cz <= Math.floor(e.w1 / CHUNK) && chunks.length < 8; cz += 1)
    for (let cy = Math.floor(e.v0 / CHUNK); cy <= Math.floor(e.v1 / CHUNK) && chunks.length < 8; cy += 1)
      for (let cx = Math.floor(e.u0 / CHUNK); cx <= Math.floor(e.u1 / CHUNK) && chunks.length < 8; cx += 1) chunks.push(partChunk(world, p, 0, cx, cy, cz).data);
  const c2 = partChunk(world, p, 2, Math.floor(e.u0 / (CHUNK * 4)), Math.floor(e.v0 / (CHUNK * 4)), Math.floor(e.w0 / (CHUNK * 4))).data;
  return digest(partRecord(p), ...chunks, c2);
}

/**
 * { sample key -> hash } of an angled preset: the axis-aligned samples
 * (goldenHashes, parts drawn into the world grid), the parts of the 3 x 3
 * cells round the origin, the content of two parts of every kind in their
 * own lattices, and the tiles round two points in parts mode (the world
 * grid leaving the parts out).
 */
export function angledHashes(presetId, sizeId) {
  const out = goldenHashes(presetId, sizeId);
  const world = createWorld(presetConfig(presetId, { size: sizeId }));
  const parts = [];
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...world.cellPlan(i, j).parts);
  parts.sort((a, b) => a.id - b.id);
  out.parts = digest(parts.map(partRecord));
  const byKind = new Map();
  for (const p of parts) byKind.set(p.kind, [...(byKind.get(p.kind) ?? []), p]);
  for (const [kind, list] of [...byKind].sort()) for (const k of [0, Math.floor(list.length / 2)].filter((q, n, a) => a.indexOf(q) === n)) out[`part:${kind}${k}`] = partHash(world, list[k]);
  const cfg = presetConfig(presetId, { size: sizeId });
  cfg.world.angles = { ...cfg.world.angles, partsMode: "separate" };
  const sep = createWorld(cfg);
  for (const [mx, my] of GOLDEN_POINTS.slice(0, 2)) for (const lod of [0, 2]) out[`separate:lod${lod}@${mx},${my}`] = tileHash(sep, lod, Math.round(mx * 8), Math.round(my * 8));
  // the structvox export (src/engine/svx): chunks round the origin at the ground with their layers and isolated
  // voxels, the grids at the homes of a part of every kind, and those parts' voxels
  const src = createSvxSource(presetConfig(presetId, { size: sizeId }));
  const gz = Math.floor(src.spawn().pos[2] / 0.125 / CHUNK);
  for (const [cx, cy] of [[0, 0], [-1, -1], [3, -2]])
    for (const cz of [gz - 1, gz, gz + 1]) {
      const layers = ["look", "flora", "water"].map((l) => src.generateLayer(cx, cy, cz, l));
      out[`svx:${cx},${cy},${cz}`] = digest(src.generate(cx, cy, cz).vox, ...layers, src.isolated(cx, cy, cz));
    }
  for (const [kind, list] of [...byKind].sort()) {
    const p = list[0];
    const g = src.generateGrid(p.id);
    out[`svx:grid:${kind}`] = digest(src.grids(p.home.cx, p.home.cy, p.home.cz), ...(g?.chunks ?? []).flatMap((c) => [[c.cx, c.cy, c.cz], c.vox, c.look, c.flora]));
  }
  return out;
}
