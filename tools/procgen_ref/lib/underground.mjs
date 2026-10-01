// The worlds and records of the underground stages (subway, sewers): stations, tunnels, sewer
// plans as lines, column tiles and chunk fills of the stages' own (tests/city/underground_records.hpp
// is the C++ twin).
import { REF, f, line } from "./rec.mjs";
import { pureTerrain, withSeaTests } from "./worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { Rivers } = await import(REF + "nature/rivers.js");
const { Subway } = await import(REF + "underground/subway.js");
const { Sewers } = await import(REF + "underground/sewers.js");
const { LRU } = await import(REF + "core/lru.js");
const { ChunkBuffer, P, P2, P3 } = await import(REF + "voxel/chunk.js");
const { MAT } = await import(REF + "voxel/materials.js");
const { hash32 } = await import(REF + "core/hash.js");

/**
 * A World of World.js with what the underground reads of createWorld: the island sea tests (the
 * cell networks ask them: withSeaTests), the rivers, the subway (unless config.subway.enabled is
 * false), the sewers and blocksSurface, as createWorld.js makes them - and no lakes, highways or
 * harbour grading of the terrain: the cell networks' World (docs/CITY.md §6). The street level
 * (createWorld.js's streetLevel: the nearest road's graded level, else the terrain) is the
 * terrain's height here: the road levels are a stage of their own (network/roadLevel). Every cell
 * network is made once (a cache that drops nothing: road identity, docs/CITY.md §6) and so is every
 * station and sewer plan (they are pure: that only saves work); every terrain sample makes what it
 * reads first (pureTerrain).
 */
export function undergroundWorld(overrides) {
  const w = pureTerrain(withSeaTests(new World(overrides)));
  w.cellNets = new LRU(Infinity);
  w.rivers = new Rivers(w);
  w.streetLevel = (x, y) => w.terrain.sample(x, y).h;
  const cfg = w.config;
  w.subway = cfg.subway.enabled === false ? null : new Subway(w);
  w.sewers = new Sewers(w);
  w.blocksSurface = (x, y) => (w.subway ? w.subway.blocksSurface(x, y) : false) || w.sewers.blocksSurface(x, y);
  if (w.subway) w.subway.stations = new LRU(Infinity);
  w.sewers.plans = new LRU(Infinity);
  return w;
}

/** FNV-1a over a string's char codes (the records are ASCII). */
export function fnv(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i += 1) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

/** FNV-1a over a typed array's values (stages/chunk.mjs). */
export function digest(a) {
  let h = 2166136261;
  for (let i = 0; i < a.length; i += 1) h = Math.imul(h ^ a[i], 16777619);
  return h >>> 0;
}

// The fields the port's records have (underground/subway.hpp, underground/sewers.hpp): a field the
// reference gives one of them that is not listed here would be one the port lacks.
const BOX_KEYS = new Set(["x0", "x1", "y0", "y1", "z0", "z1", "m", "mode"]);
const BB_KEYS = new Set(["x0", "y0", "z0", "x1", "y1", "z1"]);
const STATION_KEYS = new Set(["axis", "i", "j", "x", "y", "zp", "zs", "boxes", "bb"]);
const TUNNEL_KEYS = new Set(["axis", "fixed", "l0", "l1", "s0", "s1", "z0", "z1"]);
const PLAN_KEYS = new Set(["runs", "nodes", "boxes", "openings", "bb"]);
const RUN_KEYS = new Set(["axis", "fixed", "l0", "l1", "z0", "z1", "cls"]);
const NODE_KEYS = new Set(["key", "x", "y", "z", "zr", "arms", "hall", "blocked", "qx", "qy", "shaft", "open", "R", "H", "stairTop"]);
const OPENING_KEYS = new Set(["x0", "y0", "x1", "y1", "top"]);
function checkKeys(what, rec, keys) {
  for (const k of Object.keys(rec)) if (!keys.has(k)) throw new Error(`${what}: a field the port's record lacks: ${k}`);
}

/** A box {x0, x1, y0, y1, z0, z1, m, mode}. */
export function fbox(q) {
  checkKeys("box", q, BOX_KEYS);
  return [q.x0, q.x1, q.y0, q.y1, q.z0, q.z1, q.m, q.mode].map(f).join(",");
}
/** Bounds {x0, y0, z0, x1, y1, z1}, or "-". */
export function fbb(b) {
  if (!b) return "-";
  checkKeys("bb", b, BB_KEYS);
  return [b.x0, b.y0, b.z0, b.x1, b.y1, b.z1].map(f).join(",");
}
/** Boxes: their count and the digest of their fields (fbox, joined by ";"). */
export const fboxes = (boxes) => `${boxes.length}:${fnv(boxes.map(fbox).join(";"))}`;

/** A station: its record (the boxes by count and digest), and every box when `full`. */
export function* stationLines(s, full) {
  checkKeys("station", s, STATION_KEYS);
  yield line("st", s.axis, s.i, s.j, s.x, s.y, s.zp, s.zs, fbb(s.bb), fboxes(s.boxes));
  if (full) for (const q of s.boxes) yield line("b", fbox(q));
}

/** A tunnel span (tunnelsNear). */
export function tunnelLine(t) {
  checkKeys("tunnel", t, TUNNEL_KEYS);
  return line("t", t.axis, t.fixed, t.l0, t.l1, t.s0, t.s1, t.z0, t.z1);
}

/** A sewer node (its arms in the order added; its stair's top on a hall). */
export function nodeLine(n) {
  checkKeys("node", n, NODE_KEYS);
  const top = n.stairTop ? [n.stairTop.x, n.stairTop.y, n.stairTop.z].map(f).join(",") : "-";
  return line("n", n.key, n.x, n.y, n.z, n.zr, [...n.arms].join(""), n.hall, n.blocked, n.qx, n.qy, n.shaft, n.open, n.R, n.H, top);
}

/** A cell's sewer plan: its header, runs, nodes, boxes (every box when `full`) and openings. */
export function* planLines(p, full) {
  checkKeys("plan", p, PLAN_KEYS);
  yield line("plan", p.runs.length, p.nodes.length, fboxes(p.boxes), p.openings.length, fbb(p.bb));
  for (const r of p.runs) {
    checkKeys("run", r, RUN_KEYS);
    yield line("r", r.axis, r.fixed, r.l0, r.l1, r.z0, r.z1, r.cls);
  }
  for (const n of p.nodes) yield nodeLine(n);
  if (full) for (const q of p.boxes) yield line("b", fbox(q));
  for (const o of p.openings) {
    checkKeys("opening", o, OPENING_KEYS);
    yield line("o", o.x0, o.y0, o.x1, o.y1, o.top.x, o.top.y, o.top.z);
  }
}

/**
 * A column tile (lod, cx, cy) of the stages' own (voxel/compose makes the real ones, a later stage
 * of the port): the ground's z of each padded column - the street level at the tile's corners,
 * bilinear, with a hashed jitter of -1 .. 2.
 */
export function makeTile(w, lod, cx, cy) {
  const s = 1 << lod;
  const half = s >> 1;
  const bx = (cx * 32 - 1) * s;
  const by = (cy * 32 - 1) * s;
  const e = (P - 1) * s;
  const c = [];
  for (const [dx, dy] of [[0, 0], [1, 0], [0, 1], [1, 1]]) c.push(Math.round(w.streetLevel(bx + half + dx * e, by + half + dy * e)));
  const z = new Int32Array(P * P);
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const u = i / (P - 1);
      const v = j / (P - 1);
      const h = hash32(w.seed, cx * P + i, cy * P + j, lod);
      z[i + j * P] = Math.round(c[0] * (1 - u) * (1 - v) + c[1] * u * (1 - v) + c[2] * (1 - u) * v + c[3] * u * v) + (h & 3) - 1;
    }
  return { lod, cx, cy, z };
}

/** The ground of a chunk before the underground: stone under the tile's ground, dirt near it, asphalt on it, water and air pockets. */
export function fill(ch, tile, seed) {
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const col = i + j * P;
      const gz = tile.z[col];
      for (let k = 0; k < P; k += 1) {
        const z = ch.wz(k);
        if (z > gz) continue;
        const h = hash32(seed, col, z, 77) & 63;
        ch.data[col + k * P2] = z === gz ? MAT.ASPHALT : h === 0 ? MAT.WATER : h === 1 ? 0 : gz - z < 3 ? MAT.DIRT : MAT.STONE;
      }
    }
}

/**
 * The chunk at `lod` holding the LOD 0 voxel (x, y, z), its ground filled from its column tile,
 * then drawn by raster(chunk, tile) - with no tile when `noTile`: its record.
 */
export function chunkLine(w, lod, x, y, z, raster, noTile = false) {
  const span = 32 << lod;
  const cx = Math.floor(x / span);
  const cy = Math.floor(y / span);
  const cz = Math.floor(z / span);
  const tile = makeTile(w, lod, cx, cy);
  const ch = new ChunkBuffer(lod, cx, cy, cz);
  fill(ch, tile, w.seed);
  const before = ch.data.slice();
  raster(ch, noTile ? null : tile);
  let changed = 0;
  let carved = 0;
  for (let i = 0; i < P3; i += 1)
    if (ch.data[i] !== before[i]) {
      changed += 1;
      if (ch.data[i] === 0) carved += 1;
    }
  return line("ch", lod, cx, cy, cz, noTile, digest(before), digest(ch.data), changed, carved);
}

/** The column tile rect (padded, voxels) of tile (lod, cx, cy). */
export function tileRect(lod, cx, cy) {
  const s = 1 << lod;
  const bx = (cx * 32 - 1) * s;
  const by = (cy * 32 - 1) * s;
  return { x0: bx, y0: by, x1: bx + P * s - 1, y1: by + P * s - 1 };
}
