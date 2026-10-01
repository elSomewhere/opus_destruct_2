import { createWorld } from "../world/createWorld.js";
import { makeConfig } from "../config/defaults.js";
import { buildChunk, groundChunk, groundTile, tileContentRange } from "../voxel/compose.js";
import { P, P2 } from "../voxel/chunk.js";
import { CHUNK, VOXEL_SIZE } from "../core/units.js";
import { matrixQuat } from "../core/placement.js";
import { partsHomedIn, partId } from "../world/parts.js";
import { partChunk } from "../world/partRaster.js";
import { probe } from "../stream/queries.js";
import { MAT } from "../voxel/materials.js";
import { CLASSIFY, CLASS, vox, svxMaterials } from "./materials.js";

/**
 * The city as a structvox chunk source (opus_destruct_2,
 * core/include/svx/world/source.hpp ChunkSource and
 * game/include/svx/game/source.hpp GameSource): the same calls, the same
 * data, in plain arrays a host copies into its own types, so that the merge
 * is an adapter and nothing else (docs/MERGE_SVX.md).
 *
 * Conventions (structvox docs/GRIDS.md, svx/world/grid.hpp):
 *   - a chunk is 32^3 voxels, index (x * 32 + y) * 32 + z;
 *   - a voxel byte is 0 air, else (1 + physics class) | 0x80 anchored
 *     (svx/materials.js), with its look, flora and water in layers;
 *   - world voxel (x, y, z) is the city's voxel (x, y, z): structvox's voxel
 *     p is the cube h (p - 1/2) .. h (p + 1/2), so a city point q (voxel
 *     units, the corner of voxel 0 at 0) is at h (q - 1/2) metres;
 *   - z is up (the city's y runs south, x east: the same numbers, the same
 *     rotations);
 *   - an oriented grid's voxel p is centred at origin + R(rot) (h p): a
 *     part's local cell p is the grid's voxel p (grids).
 *
 * The world grid is generated in parts mode (world.angles.partsMode
 * "separate"): it leaves out what the parts hold (Rule A), and every part is
 * a grid of its own. Pure functions of the chunk (or the grid id): any
 * number of sources over the same config, in any worker, agree.
 */

/** structvox's reach of voxel coordinates (svx/world/grid.hpp kVoxelLimit). */
export const SVX_VOXEL_LIMIT = (1 << 20) - 4096;
const C3 = CHUNK * CHUNK * CHUNK;
const svxIndex = (x, y, z) => (x * CHUNK + y) * CHUNK + z;

/**
 * A part's frame as structvox places a grid: { origin [x, y, z] (m), rot
 * {x, y, z, w} } with its voxel p centred at origin + R(rot) (h p).
 */
export function gridFrame(placement, h = VOXEL_SIZE) {
  const { m, d, origin } = placement;
  // the centre of local cell 0, in city voxel units, then in metres
  const c = [0, 3, 6].map((r, a) => [origin.x, origin.y, origin.z][a] + (m[r] + m[r + 1] + m[r + 2]) / (2 * d));
  return { origin: c.map((q) => h * (q - 0.5)), rot: matrixQuat(m, d) };
}

/**
 * The export's options:
 *   flora   "layer" (decorative plants air, in an air-bound layer: until the
 *           core has a decorative flag), "solid" (the foliage class) or
 *           "none" (left out)
 *   props   "isolated" (props and furniture as isolated voxels the host
 *           writes after generation, `isolated`) or "solid" (as structure)
 */
export function createSvxSource(config, { flora = "layer", props = "isolated" } = {}) {
  const cfg = makeConfig(config);
  cfg.world.angles = { ...cfg.world.angles, partsMode: "separate" };
  const world = createWorld(cfg);
  const h = VOXEL_SIZE;
  const gridCache = new Map();
  let hint = { i: 0, j: 0 };
  const last = new Map();

  /** The export of one chunk: { vox, look, flora, water, isolated, any }. */
  function exportChunk(cx, cy, cz) {
    const key = `${cx},${cy},${cz}`;
    if (last.has(key)) return last.get(key);
    const out = { vox: new Uint8Array(C3), look: null, flora: null, water: null, isolated: null, any: false };
    const t = groundTile(world, 0, cx, cy);
    // (above what the column holds: air, cheaply)
    if (cz * CHUNK <= tileContentRange(world, t)[1]) fill(out, t, cx, cy, cz);
    if (last.size > 64) last.delete(last.keys().next().value);
    last.set(key, out);
    return out;
  }

  function fill(out, t, cx, cy, cz) {
    const chunk = buildChunk(world, 0, cx, cy, cz, t, { isolated: props === "isolated" });
    const ground = groundChunk(world, 0, cx, cy, cz, t);
    const d = chunk.data;
    const gd = ground.data;
    const iso = chunk.iso;
    for (let k = 1; k <= CHUNK; k += 1) {
      const z = chunk.wz(k);
      for (let j = 1; j <= CHUNK; j += 1)
        for (let i = 1; i <= CHUNK; i += 1) {
          const idx = i + j * P + k * P2;
          const m = d[idx];
          if (!m) continue;
          const c = CLASSIFY[m];
          const s = svxIndex(i - 1, j - 1, k - 1);
          if (c.liquid) {
            (out.water ??= new Uint8Array(C3))[s] = 255;
            continue;
          }
          // the ground's own voxel, where it still stands: anchored (a bridge's deck, piers and railings are structure)
          const col = i + j * P;
          const deck = t.deck && t.deck[col] && z > t.bed[col];
          if (gd[idx] === m && !deck && m !== MAT.RAILING) {
            out.vox[s] = vox(c.g, true);
            (out.look ??= new Uint8Array(C3))[s] = c.gLook;
            out.any = true;
            continue;
          }
          if (c.flora && flora !== "solid") {
            if (flora === "layer") (out.flora ??= new Uint8Array(C3))[s] = 1 + c.floraIdx;
            continue;
          }
          if (iso && iso[idx] === m) {
            (out.isolated ??= []).push(s, vox(c.s, false), c.sLook);
            continue;
          }
          out.vox[s] = vox(c.s, false);
          (out.look ??= new Uint8Array(C3))[s] = c.sLook;
          out.any = true;
        }
    }
  }

  /** A part's SourceGrid record. */
  function sourceGrid(p) {
    const f = gridFrame(p.placement, h);
    return { id: p.id, origin: f.origin, rot: f.rot, voxelSize: h, priority: p.priority, anchored: !!p.anchored, kind: p.kind, key: p.key };
  }

  /** The part behind a grid id: the one grids() listed, else the cell of its id nearest the last asked. */
  function partOf(id) {
    const p = gridCache.get(id);
    if (p) return p;
    const k = (id - 1) & 255;
    const cj = ((id - 1) >>> 8) & 2047;
    const ci = ((id - 1) >>> 19) & 2047;
    const near = (want, at) => at + ((((want - (world.arterials.canon(at) & 2047)) % 2048) + 3072) % 2048) - 1024;
    const i = near(ci, hint.i);
    const j = near(cj, hint.j);
    if (partId(world.arterials.canon(i), world.arterials.canon(j), k) !== id) return null;
    return world.cellPlan(i, j).parts.find((q) => q.id === id) ?? null;
  }

  return {
    world,
    materials: svxMaterials,

    /** World extent in chunks: [lo, hi) per axis (ChunkSource::chunk_lo / chunk_hi). */
    chunkLo() {
      return extent(world)[0];
    },
    chunkHi() {
      return extent(world)[1];
    },

    /** ChunkSource::generate: the chunk's voxel bytes and whether any is solid. */
    generate(cx, cy, cz) {
      const e = exportChunk(cx, cy, cz);
      return { vox: e.vox, any: e.any };
    },

    /** ChunkSource::generate_layer: "look" (solid-bound), "flora" and "water" (air-bound); null when the chunk holds none. */
    generateLayer(cx, cy, cz, layer) {
      const e = exportChunk(cx, cy, cz);
      return layer === "look" ? e.look : layer === "flora" ? e.flora : layer === "water" ? e.water : null;
    },

    /**
     * The chunk's isolated voxels (props and furniture, ANGLED_WORLD_PLAN.md
     * §4.6: the host writes them with kEditIsolated in WorldSystem::
     * on_generated): a flat [index, vox, look, ...] or null.
     */
    isolated(cx, cy, cz) {
      return exportChunk(cx, cy, cz).isolated;
    },

    /** ChunkSource::grids: the parts at home in the chunk. */
    grids(cx, cy, cz) {
      const c = world.cellAt(cx * CHUNK + CHUNK - 1, cy * CHUNK + CHUNK - 1);
      hint = { i: c.i, j: c.j };
      return partsHomedIn(world, cx, cy, cz).map((p) => {
        gridCache.set(p.id, p);
        if (gridCache.size > 4096) gridCache.delete(gridCache.keys().next().value);
        return sourceGrid(p);
      });
    },

    /** The SourceGrid of a grid id (for a host that keeps only ids), or null. */
    grid(id) {
      const p = partOf(id);
      return p ? sourceGrid(p) : null;
    },

    /**
     * ChunkSource::generate_grid: a part's voxels in its own lattice, by
     * chunks of its lattice (VoxelGrid's): { h, chunks: [{ cx, cy, cz, vox,
     * look, flora }] } (a road slab's voxels anchored), or null.
     */
    generateGrid(id) {
      const p = partOf(id);
      if (!p) return null;
      const e = p.extent;
      const chunks = [];
      for (let gz = Math.floor(e.w0 / CHUNK); gz <= Math.floor(e.w1 / CHUNK); gz += 1)
        for (let gy = Math.floor(e.v0 / CHUNK); gy <= Math.floor(e.v1 / CHUNK); gy += 1)
          for (let gx = Math.floor(e.u0 / CHUNK); gx <= Math.floor(e.u1 / CHUNK); gx += 1) {
            const c = partChunk(world, p, 0, gx, gy, gz);
            const q = { cx: gx, cy: gy, cz: gz, vox: new Uint8Array(C3), look: new Uint8Array(C3), flora: null };
            let any = false;
            for (let k = 1; k <= CHUNK; k += 1)
              for (let j = 1; j <= CHUNK; j += 1)
                for (let i = 1; i <= CHUNK; i += 1) {
                  const m = c.data[i + j * P + k * P2];
                  if (!m) continue;
                  const cl = CLASSIFY[m];
                  const s = svxIndex(i - 1, j - 1, k - 1);
                  if (cl.liquid) continue;
                  if (cl.flora && flora !== "solid") {
                    if (flora === "layer") (q.flora ??= new Uint8Array(C3))[s] = 1 + cl.floraIdx;
                    continue;
                  }
                  q.vox[s] = vox(cl.s, p.anchored);
                  q.look[s] = cl.sLook;
                  any = true;
                }
            if (any || q.flora) chunks.push(q);
          }
      return chunks.length ? { h, chunks } : null;
    },

    /**
     * ChunkSource::region: the unit a chunk's changes come back with - the
     * city block of the building most of its column holds, else of its
     * centre, else 8 x 8 columns - so no building returns in half. A key
     * below 2^53.
     */
    region(cx, cy) {
      const x0 = cx * CHUNK;
      const y0 = cy * CHUNK;
      const col = { x0, y0, x1: x0 + CHUNK - 1, y1: y0 + CHUNK - 1 };
      let best = null;
      let bestArea = 0;
      for (const env of world.envelopesIn(col)) {
        const r = env.R;
        const a = Math.max(0, Math.min(r.x1, col.x1) - Math.max(r.x0, col.x0) + 1) * Math.max(0, Math.min(r.y1, col.y1) - Math.max(r.y0, col.y0) + 1);
        if (a > bestArea || (a === bestArea && best && env.id < best.id)) {
          best = env;
          bestArea = a;
        }
      }
      // (a lot's id is its block's and "/l<k>")
      const lot = best?.lot;
      let blockId = lot && lot.lastIndexOf("/l") > 0 ? lot.slice(0, lot.lastIndexOf("/l")) : (lot ?? null);
      if (!blockId) {
        const c = world.cellAt(x0 + 16, y0 + 16);
        blockId = world.cellPlan(c.i, c.j).blockAt(x0 + 16, y0 + 16)?.id ?? null;
      }
      if (!blockId) return 2 ** 52 + ((cx >> 3) & 0xffff) * 65536 + ((cy >> 3) & 0xffff);
      return fnv(String(blockId)) * 2 ** 20 + 1;
    },

    /**
     * GameSource::coarse: the far render tier, n coarse cells of `factor`^3
     * voxels from voxel `lo` (index (x * n1 + y) * n2 + z): the world grid at
     * the LOD of that size (parts are splatted by the host from their grids).
     */
    coarse(lo, n, factor) {
      const lod = Math.round(Math.log2(factor));
      if (1 << lod !== factor) throw new Error("svx coarse: factor must be a power of two");
      const out = new Uint8Array(n[0] * n[1] * n[2]);
      const span = CHUNK;
      const chunks = new Map();
      for (let x = 0; x < n[0]; x += 1)
        for (let y = 0; y < n[1]; y += 1)
          for (let z = 0; z < n[2]; z += 1) {
            const X = Math.floor(lo[0] / factor) + x;
            const Y = Math.floor(lo[1] / factor) + y;
            const Z = Math.floor(lo[2] / factor) + z;
            const ccx = Math.floor(X / span);
            const ccy = Math.floor(Y / span);
            const ccz = Math.floor(Z / span);
            const key = `${ccx},${ccy},${ccz}`;
            let ch = chunks.get(key);
            if (!ch) chunks.set(key, (ch = buildChunk(world, lod, ccx, ccy, ccz)));
            const m = ch.data[X - ccx * span + 1 + (Y - ccy * span + 1) * P + (Z - ccz * span + 1) * P2];
            if (!m) continue;
            const c = CLASSIFY[m];
            if (c.liquid || (c.flora && flora !== "solid")) continue;
            out[(x * n[1] + y) * n[2] + z] = vox(c.s, false);
          }
      return out;
    },

    /** GameSource::spawn_pos / spawn_dir: at the origin, on the ground (metres). */
    spawn() {
      const p = probe(world, 0, 0);
      return { pos: [-0.5 * h, -0.5 * h, h * (p.groundZ + 1) - 0.5 * h], dir: [1, 0, 0] };
    },
  };
}

/** FNV-1a of a string (u32). */
function fnv(str) {
  let h = 0x811c9dc5;
  for (let k = 0; k < str.length; k += 1) h = Math.imul(h ^ str.charCodeAt(k), 0x01000193);
  return h >>> 0;
}

/** The world's extent in chunks: [[lo x, y, z], [hi x, y, z]) within structvox's reach. */
function extent(world) {
  const lim = Math.floor(SVX_VOXEL_LIMIT / CHUNK) - 1;
  const w = world.config.world;
  let half = lim;
  // (an island and its shelf; anything else reaches as far as structvox does)
  if (w.mode === "island") half = Math.min(lim, Math.ceil((w.island.radius * Math.max(1, w.island.elongation) * 1.5 * 8) / CHUNK));
  const peak = w.mode === "island" ? w.island.peak : 6000;
  const zHi = Math.min(lim, Math.ceil(((peak + 800) * 8) / CHUNK));
  const zLo = -Math.min(lim, Math.ceil((600 * 8) / CHUNK));
  return [
    [-half, -half, zLo],
    [half, half, zHi],
  ];
}

export { CLASS, CLASSIFY };
