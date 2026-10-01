/**
 * World worker. Each worker owns an independent World instance (identical
 * config => identical results) and serves stateless requests:
 *
 *   init     {config}
 *   tile     {id, lod, cx, cy, collision}  -> meshes for every chunk of a column tile
 *                                            (+ solid / climbable bitsets at LOD0), and
 *                                            in parts mode the parts at home in it, meshed
 *                                            in their own lattices (tileMessage)
 *   map      {id, rect}                    -> vector map data (roads, lots, buildings)
 *   building {id, buildingId}              -> interior plan for the inspector
 *   probe    {id, x, y}                    -> ground info at a point (spawn, HUD)
 *   overview {id, rect, w, h}              -> coarse biome / relief / settlement raster
 */
import { createWorld } from "../world/createWorld.js";
import { groundTile, buildChunk, tileContentRange } from "../voxel/compose.js";
import { meshChunkWith } from "../voxel/meshers.js";
import { IS_SOLID, IS_CLIMB } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { CHUNK } from "../core/units.js";
import { mapData, buildingData, probe, pois, overview, inspect } from "./queries.js";
import { partChunk } from "../world/partRaster.js";

let world = null;

/** 1 bit per voxel of the chunk interior where `table[material]` is set; null if none. */
function voxelBits(data, table) {
  const bits = new Uint32Array((CHUNK * CHUNK * CHUNK) / 32);
  let any = 0;
  let n = 0;
  for (let k = 1; k <= CHUNK; k += 1) {
    for (let j = 1; j <= CHUNK; j += 1) {
      let idx = 1 + j * P + k * P2;
      for (let i = 1; i <= CHUNK; i += 1, idx += 1, n += 1) {
        if (table[data[idx]]) {
          bits[n >>> 5] |= 1 << (n & 31);
          any = 1;
        }
      }
    }
  }
  return any ? bits : null;
}

/**
 * The parts at home in a tile's chunk columns (parts mode), meshed at the
 * tile's LOD in their own lattices: [{ id, m, d, origin, aabb, extent, lod,
 * chunks: [{ cx, cy, cz, opaque, transparent, solid, climb }] }] (lattice
 * chunk coordinates; the viewer places them by the part's matrix).
 */
export function tileParts(world, lod, cx, cy, collision, transfer) {
  if (world.config.world.angles?.partsMode !== "separate") return [];
  const n = 1 << lod;
  const span = CHUNK * n;
  const rect = { x0: cx * span, y0: cy * span, x1: cx * span + span - 1, y1: cy * span + span - 1 };
  const out = [];
  const seen = new Set();
  for (const c of world.cellsOverlapping(rect))
    for (const p of world.cellPlan(c.i, c.j).parts) {
      if (seen.has(p.id) || Math.floor(p.home.cx / n) !== cx || Math.floor(p.home.cy / n) !== cy) continue;
      seen.add(p.id);
      const e = p.extent;
      const g = CHUNK << lod;
      const chunks = [];
      for (let gz = Math.floor(e.w0 / g); gz <= Math.floor(e.w1 / g); gz += 1)
        for (let gy = Math.floor(e.v0 / g); gy <= Math.floor(e.v1 / g); gy += 1)
          for (let gx = Math.floor(e.u0 / g); gx <= Math.floor(e.u1 / g); gx += 1) {
            const pc = partChunk(world, p, lod, gx, gy, gz);
            const mesh = meshChunkWith(pc.data, { skirt: false, mesher: world.config.rendering?.mesher });
            const solid = collision && lod === 0 ? voxelBits(pc.data, IS_SOLID) : null;
            const climb = solid ? voxelBits(pc.data, IS_CLIMB) : null;
            if (!mesh.opaque && !mesh.transparent && !solid) continue;
            for (const q of [mesh.opaque, mesh.transparent]) if (q) transfer.push(q.position.buffer, q.normal.buffer, q.color.buffer, q.aux.buffer, q.index.buffer);
            if (solid) transfer.push(solid.buffer);
            if (climb) transfer.push(climb.buffer);
            chunks.push({ cx: gx, cy: gy, cz: gz, opaque: mesh.opaque, transparent: mesh.transparent, solid, climb });
          }
      const pl = p.placement;
      out.push({ id: p.id, m: Array.from(pl.m), d: pl.d, origin: { ...pl.origin }, aabb: p.aabb, extent: p.extent, lod, chunks });
    }
  return out;
}

/** A tile's message and the buffers it transfers. */
export function tileMessage(world, msg) {
  const { id, lod, cx, cy } = msg;
  const t0 = performance.now();
  const tile = groundTile(world, lod, cx, cy);
  const [lo, hi] = tileContentRange(world, tile);
  const span = CHUNK << lod;
  const cz0 = Math.floor(lo / span);
  const cz1 = Math.floor(hi / span);
  const chunks = [];
  const transfer = [];
  const meshStats = { ms: 0, chunks: 0, vertices: 0, triangles: 0 };
  for (let cz = cz0; cz <= cz1; cz += 1) {
    const chunk = buildChunk(world, lod, cx, cy, cz, tile);
    const mt = performance.now();
    const mesh = meshChunkWith(chunk.data, { skirt: lod >= 1, mesher: world.config.rendering?.mesher });
    meshStats.ms += performance.now() - mt;
    meshStats.chunks += 1;
    for (const pass of [mesh.opaque, mesh.transparent]) if (pass) {
      meshStats.vertices += pass.position.length / 3;
      meshStats.triangles += pass.index.length / 3;
    }
    let solid = null;
    let climb = null;
    if (msg.collision && lod === 0) {
      solid = voxelBits(chunk.data, IS_SOLID);
      if (solid) climb = voxelBits(chunk.data, IS_CLIMB);
    }
    if (!mesh.opaque && !mesh.transparent && !solid) continue;
    const entry = { cz, opaque: mesh.opaque, transparent: mesh.transparent, solid, climb };
    for (const m of [mesh.opaque, mesh.transparent]) {
      if (!m) continue;
      transfer.push(m.position.buffer, m.normal.buffer, m.color.buffer, m.aux.buffer, m.index.buffer);
    }
    if (solid) transfer.push(solid.buffer);
    if (climb) transfer.push(climb.buffer);
    chunks.push(entry);
  }
  const parts = tileParts(world, lod, cx, cy, !!msg.collision, transfer);
  return { message: { type: "tile", id, lod, cx, cy, cz0, cz1, zlo: lo, zhi: hi, gzlo: tile.zMin, gzhi: tile.zMax, chunks, parts, meshStats, ms: performance.now() - t0 }, transfer };
}

function handleTile(msg) {
  const { message, transfer } = tileMessage(world, msg);
  self.postMessage(message, transfer);
}

if (typeof self !== "undefined") self.onmessage = (e) => {
  const msg = e.data;
  try {
    switch (msg.type) {
      case "init":
        world = createWorld(msg.config ?? {});
        self.postMessage({ type: "ready", id: msg.id });
        break;
      case "tile":
        handleTile(msg);
        break;
      case "map":
        self.postMessage({ type: "map", id: msg.id, data: mapData(world, msg.rect, msg.detail) });
        break;
      case "overview": {
        const data = overview(world, msg.rect, msg.w, msg.h);
        self.postMessage({ type: "overview", id: msg.id, data }, [data.rgba.buffer]);
        break;
      }
      case "building":
        self.postMessage({ type: "building", id: msg.id, data: buildingData(world, msg.buildingId) });
        break;
      case "pois":
        self.postMessage({ type: "pois", id: msg.id, data: pois(world, msg.x, msg.y) });
        break;
      case "probe":
        self.postMessage({ type: "probe", id: msg.id, data: probe(world, msg.x, msg.y) });
        break;
      case "inspect":
        self.postMessage({ type: "inspect", id: msg.id, data: inspect(world, msg.x, msg.y, msg.z, msg.part ?? null) });
        break;
      default:
        break;
    }
  } catch (err) {
    self.postMessage({ type: "error", id: msg.id, error: String(err?.stack ?? err) });
  }
};
