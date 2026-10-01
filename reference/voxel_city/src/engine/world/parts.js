import { CHUNK } from "../core/units.js";

/**
 * Oriented parts (ANGLED_WORLD_PLAN.md §4.3): the unit of orientation. A
 * part is one coherent object in a lattice of its own (a turned building,
 * a pitched road segment, a wing): in structvox one oriented grid, here a
 * record { id, key, kind, placement, extent, aabb, home, reach, anchored,
 * priority } on the cell plan that made it (`plan.parts`). Everything
 * axis-aligned stays in the world grid and is no part at all.
 *
 * The physics budget (§3). Every part is a resident grid there, and the
 * cost of the resident grids is global (`near_grids` scans them all), so a
 * cell grants parts from a budget, in a fixed order, from its own list
 * only (no neighbour ever asked):
 *   - `angles.partArea` (m²): one part per that much of the cell on
 *     average, so any 96 m disc (structvox's load radius) holds ~7 at most
 *     on average, the operating point of structvox's turned city (1
 *     building in 8 on 24 m lots). Each cell is cut into budget squares of
 *     `angles.partCluster` times that area, each holding that many parts:
 *     the same density, and a run of road pieces up a steep street, or a
 *     row of turned buildings, may share a square;
 *   - `angles.maxResident`: the parts in any disc of `angles.residentRadius`
 *     (96 m, structvox's load radius: the grids resident round a player,
 *     whose high-water mark sets the scan, at home in the chunks whose
 *     centres the disc holds), whichever cells they are of. A cell keeps,
 *     in every such disc, at most floor(maxResident / t) of its parts,
 *     where t counts the cells whose parts the disc could hold (whose
 *     chunks' centres it reaches: `cellsNear`, from the arterial lattice's
 *     lines, pure, no neighbour asked anything), so no disc ever holds
 *     more than maxResident: 8 of one cell deep inside it, 4 of each of
 *     two cells across a border, 2 of each at a corner. The discs that
 *     matter when a part is granted are those holding its home; they are
 *     checked where the count and t can be highest, the vertices of the
 *     arrangement of the circles round the cell's parts and the edges of
 *     the cells' reach (`resident`);
 *   - `angles.maxPartsPerChunk`: parts at home in one chunk.
 * A part is at home in one chunk, owned by its cell (the cell holding the
 * chunk's last voxel), so the caps are exact without neighbours; and it
 * reaches at most PART_REACH chunks from it horizontally (structvox's far
 * tier gathers a tile's grids from home chunks that close: Game::far_mesh).
 */

/** Chunks a part may reach from its home chunk, horizontally (structvox Game::far_mesh kMargin). */
export const PART_REACH = 4;

/** Diameter (voxels) of the resident disc of a config (angles.residentRadius, m; structvox's 96 m load radius by default). */
export const residentD = (config) => 2 * (config?.world?.angles?.residentRadius ?? 96) * 8;

/**
 * Stable part id (u32, never 0): the canonical cell (11 bits per axis, so
 * ids repeat only 2,000 cells apart, never resident together) and the
 * part's index in the cell's grant order (8 bits).
 */
export function partId(ci, cj, k) {
  if (k < 0 || k > 255) throw new Error(`parts: index ${k} out of range`);
  return (((ci & 2047) << 19) | ((cj & 2047) << 8) | k) + 1;
}

/** Priority classes of part kinds: a building owns what it shares with its wings, both what they share with a road piece or a garage's ramp. */
const PRIORITY_CLASS = { road: 1, ramp: 1, wing: 2, building: 3 };

/**
 * A part's structvox priority (GridDesc::priority, docs/GRIDS.md §3: of two
 * grids the higher owns their overlap, displacing the other's voxels there;
 * the world grid has priority 0, and a grid of negative priority yields to
 * it). Unique, from its id: a building's above any wing's (a wing is cast
 * into its building's wall, which owns it), both above any road piece's (a
 * canopy over a sidewalk belongs to the building), all above the world
 * grid (a turned building or a pitched slab displaces the ground it is cast
 * into). A part cast into what the world grid holds (`yieldsToGrid`: a
 * wing of a building square to the grid, a garage's ramp between its decks)
 * yields to it: its priority is negative, the lower the class the lower.
 * Fits a structvox i32.
 */
export function partPriority(kind, id, { yieldsToGrid = false } = {}) {
  const p = (PRIORITY_CLASS[kind] ?? 1) * 2 ** 24 + (id % 2 ** 24);
  return yieldsToGrid ? -(2 ** 26 - p) : p;
}

/** Slack (voxels) of the resident check's geometry, always in the cap's favour (a centre that far out still counts). */
const EPS = 1e-6;

/** Chunk coordinate of a world voxel coordinate. */
const chunkOf = (v) => Math.floor(v / CHUNK);

/**
 * The cell owning a chunk: the one holding the chunk's last voxel (its max
 * corner). A partition of the chunks, in which a chunk straddling an
 * arterial line belongs to the cell east or south of it: the cell that
 * owns the road on that line (a cell owns its W and N edge roads).
 */
const ownerCorner = (c) => c * CHUNK + CHUNK - 1;

/**
 * Home chunk of a part whose base centre is world voxel (x, y, z), moved
 * into a chunk owned by its cell (cell rect [x0, x1) x [y0, y1), as the
 * arterial lines are).
 */
export function homeChunk(x, y, z, cellRect) {
  let cx = chunkOf(x);
  let cy = chunkOf(y);
  while (ownerCorner(cx) < cellRect.x0) cx += 1;
  while (ownerCorner(cx) >= cellRect.x1) cx -= 1;
  while (ownerCorner(cy) < cellRect.y0) cy += 1;
  while (ownerCorner(cy) >= cellRect.y1) cy -= 1;
  return { cx, cy, cz: chunkOf(z) };
}

/** Horizontal reach (chunks) of a world box from a home chunk. */
export function reachOf(aabb, home) {
  return Math.max(home.cx - chunkOf(aabb.x0), chunkOf(aabb.x1) - home.cx, home.cy - chunkOf(aabb.y0), chunkOf(aabb.y1) - home.cy);
}

/**
 * A part record. `placement` (core/placement.js) with its local `extent`
 * {u0, v0, w0, u1, v1, w1} (inclusive cells); `cell` { i, j, ci, cj, rect }
 * (ci, cj canonical); `index` its place in the cell's grant order.
 */
export function makePart({ cell, index, key, kind, placement, extent, anchored = false }) {
  const aabb = placement.localBoundsToWorldAABB(extent);
  // home: the world voxel under the middle of the extent, at its base
  const [hx, hy, hz] = placement.toWorld(Math.floor((extent.u0 + extent.u1) / 2), Math.floor((extent.v0 + extent.v1) / 2), extent.w0 ?? 0);
  const home = homeChunk(hx, hy, hz, cell.rect);
  return {
    id: partId(cell.ci, cell.cj, index),
    // (the owner cell: its plan holds what the part is of)
    cell: [cell.i, cell.j],
    key,
    kind,
    placement,
    extent,
    aabb,
    home,
    // (the world voxel under the middle of its base: the budget square it takes)
    base: { x: hx, y: hy, z: hz },
    reach: reachOf(aabb, home),
    anchored,
    priority: placement.priority,
  };
}

/**
 * The parts budget of one cell (see above). Candidates ask in a fixed
 * order; `grant` says whether a part at home in chunk `home` whose base
 * centre is (x, y) fits, and books it if so. Off (angles disabled): none.
 * `lattice` (the world's ArterialGrid) gives the cells around, for the
 * resident cap across borders; without it the cell is taken to be alone.
 */
export class PartBudget {
  constructor(config, cellRect, { lattice = null } = {}) {
    const a = config.world.angles;
    this.enabled = !!a?.enabled;
    this.rect = cellRect;
    this.maxPerChunk = a?.maxPartsPerChunk ?? 1;
    // squares of at least cluster x partArea (m²), within the cell, holding cluster parts each
    this.cluster = Math.max(1, a?.partCluster ?? 4);
    const side = Math.sqrt((a?.partArea ?? 3600) * this.cluster) * 8;
    this.nx = Math.max(1, Math.floor((cellRect.x1 - cellRect.x0) / side));
    this.ny = Math.max(1, Math.floor((cellRect.y1 - cellRect.y0) / side));
    // (8-bit part indices: at most 255 parts per cell)
    this.limit = Math.min(255, this.nx * this.ny * this.cluster);
    this.used = new Uint8Array(this.nx * this.ny);
    this.chunks = new Map();
    this.count = 0;
    // (the granted parts' home chunks and the cells each can reach, for the resident cap)
    this.maxResident = a?.maxResident ?? 8;
    this.R = residentD(config) / 2;
    this.lattice = lattice;
    this.homes = [];
  }

  /**
   * Where each cell whose parts a disc of the resident radius centred near
   * point (x, y) could hold keeps its homes: [x0, y0, x1, y1] (voxels, the
   * centres of the chunks it owns lie within), this cell's first.
   */
  cellsNear(x, y) {
    const g = (c) => [c.x0 - CHUNK / 2, c.y0 - CHUNK / 2, c.x1 + CHUNK / 2, c.y1 + CHUNK / 2];
    if (!this.lattice) return [g(this.rect)];
    const r = 2 * this.R + CHUNK;
    const L = this.lattice;
    const out = [g(this.rect)];
    for (let i = L.indexAt(0, x - r); i <= L.indexAt(0, x + r); i += 1)
      for (let j = L.indexAt(1, y - r); j <= L.indexAt(1, y + r); j += 1) {
        const c = L.cellRect(i, j);
        if (c.x0 === this.rect.x0 && c.y0 === this.rect.y0) continue;
        out.push(g(c));
      }
    return out;
  }

  /**
   * Is the resident cap kept with a part at home in `home` added (see
   * above)? Every disc of radius R holding its home's centre h: the count
   * of this cell's homes it holds (h's among them) at most
   * floor(maxResident / t), t the cells whose reach [x0 - R, x1 + R] x
   * [y0 - R, y1 + R] (corners rounded) holds its centre. The centres with a
   * given set of homes and cells form a convex region, bounded by the
   * circles of radius R round those homes and the edges of those reaches;
   * one of its vertices, or the one home it is the disc of, holds at least
   * as many of both. So the centres checked are the homes and every
   * crossing of those circles, lines and corner circles within R of h
   * (exact up to EPS, counted in the cap's favour).
   */
  resident(home) {
    const R = this.R;
    const hx = home.cx * CHUNK + CHUNK / 2;
    const hy = home.cy * CHUNK + CHUNK / 2;
    const pts = [[hx, hy]];
    for (const q of this.homes) if ((q.x - hx) ** 2 + (q.y - hy) ** 2 <= 4 * R * R) pts.push([q.x, q.y]);
    const cells = this.cellsNear(hx, hy);
    // (a disc with every one of them and every cell round could still hold them all)
    if (pts.length <= Math.floor(this.maxResident / cells.length)) return true;
    const circles = pts.slice();
    const vlines = [];
    const hlines = [];
    for (const [x0, y0, x1, y1] of cells) {
      vlines.push(x0 - R, x1 + R);
      hlines.push(y0 - R, y1 + R);
      circles.push([x0, y0], [x1, y0], [x0, y1], [x1, y1]);
    }
    const R2 = (R + EPS) ** 2;
    const over = (cx, cy) => {
      if ((cx - hx) ** 2 + (cy - hy) ** 2 > R2) return false;
      let n = 0;
      for (const [x, y] of pts) if ((x - cx) ** 2 + (y - cy) ** 2 <= R2) n += 1;
      let t = 0;
      for (const [x0, y0, x1, y1] of cells) {
        const dx = Math.max(x0 - cx, 0, cx - x1);
        const dy = Math.max(y0 - cy, 0, cy - y1);
        if (dx * dx + dy * dy <= R2) t += 1;
      }
      return n > Math.floor(this.maxResident / t);
    };
    for (const [x, y] of pts) if (over(x, y)) return false;
    // circles with circles (all of radius R)
    for (let a = 0; a < circles.length; a += 1)
      for (let b = a + 1; b < circles.length; b += 1) {
        const [ax, ay] = circles[a];
        const [bx, by] = circles[b];
        const dx = bx - ax;
        const dy = by - ay;
        const d2 = dx * dx + dy * dy;
        if (d2 === 0 || d2 > 4 * R * R) continue;
        const k = Math.sqrt(Math.max(0, R * R / d2 - 0.25));
        const mx = ax + dx / 2;
        const my = ay + dy / 2;
        if (over(mx - dy * k, my + dx * k) || over(mx + dy * k, my - dx * k)) return false;
      }
    // circles with the reaches' edges, and the edges with each other
    for (const [cx, cy] of circles) {
      for (const x of vlines) {
        const e = R * R - (x - cx) ** 2;
        if (e >= 0 && (over(x, cy - Math.sqrt(e)) || over(x, cy + Math.sqrt(e)))) return false;
      }
      for (const y of hlines) {
        const e = R * R - (y - cy) ** 2;
        if (e >= 0 && (over(cx - Math.sqrt(e), y) || over(cx + Math.sqrt(e), y))) return false;
      }
    }
    for (const x of vlines) for (const y of hlines) if (over(x, y)) return false;
    return true;
  }

  /** Budget square of a point in the cell. */
  square(x, y) {
    const r = this.rect;
    const i = Math.min(this.nx - 1, Math.max(0, Math.floor(((x - r.x0) * this.nx) / (r.x1 - r.x0))));
    const j = Math.min(this.ny - 1, Math.max(0, Math.floor(((y - r.y0) * this.ny) / (r.y1 - r.y0))));
    return i + j * this.nx;
  }

  /** Would a part at home in `home` with base centre (x, y) fit? */
  fits(x, y, home) {
    if (!this.enabled || this.count >= this.limit) return false;
    if (this.used[this.square(x, y)] >= this.cluster) return false;
    if ((this.chunks.get(`${home.cx},${home.cy},${home.cz}`) ?? 0) >= this.maxPerChunk) return false;
    return this.resident(home);
  }

  /** Book a part if it fits; returns its index in the cell's grant order, or -1. */
  grant(x, y, home) {
    if (!this.fits(x, y, home)) return -1;
    this.used[this.square(x, y)] += 1;
    const k = `${home.cx},${home.cy},${home.cz}`;
    this.chunks.set(k, (this.chunks.get(k) ?? 0) + 1);
    this.homes.push({ x: home.cx * CHUNK + CHUNK / 2, y: home.cy * CHUNK + CHUNK / 2 });
    return this.count++;
  }
}

/**
 * A road cut into the pieces an inclined road surface is made of (S4: one
 * anchored, pitched part per piece): every straight segment of its
 * centre line (a part is straight) in as few equal pieces as keep each,
 * right-of-way included, within PART_REACH chunks of its home chunk (in
 * a chunk its cell owns, `cellRect`). Keys `road id/p<segment>.<piece>`
 * are stable. Cached on the road: [{ key, seg, k, s0, s1, a, b, aabb, home, reach }]
 * (s0, s1 arc lengths along the road; home.cz is left to whoever knows the level).
 */
export function roadPieces(road, cellRect) {
  if (road._pieces) return road._pieces;
  const out = [];
  let acc = 0;
  for (let si = 0; si < road.pts.length - 1; si += 1) {
    const p = road.pts[si];
    const q = road.pts[si + 1];
    const len = Math.sqrt((q.x - p.x) ** 2 + (q.y - p.y) ** 2);
    if (len < 1e-6) continue;
    const at = (t) => ({ x: p.x + ((q.x - p.x) * t) / len, y: p.y + ((q.y - p.y) * t) / len });
    // (the piece is the right-of-way between its two end cross-sections)
    const nx = (-(q.y - p.y) / len) * road.hr;
    const ny = ((q.x - p.x) / len) * road.hr;
    const piece = (n, k) => {
      const a = at((len * k) / n);
      const b = at((len * (k + 1)) / n);
      const xs = [a.x + nx, a.x - nx, b.x + nx, b.x - nx];
      const ys = [a.y + ny, a.y - ny, b.y + ny, b.y - ny];
      const aabb = { x0: Math.floor(Math.min(...xs)), y0: Math.floor(Math.min(...ys)), x1: Math.ceil(Math.max(...xs)), y1: Math.ceil(Math.max(...ys)) };
      const home = homeChunk((a.x + b.x) / 2, (a.y + b.y) / 2, 0, cellRect);
      return { key: `${road.id}/p${si}.${k}`, seg: si, k, s0: acc + (len * k) / n, s1: acc + (len * (k + 1)) / n, a, b, aabb, home, reach: reachOf(aabb, home) };
    };
    // (shorter pieces help only while a piece's length, not the road's
    // width, sets its reach: pieces of 8 m at least; a piece that still
    // reaches further keeps its true reach, and stays in the world grid)
    const nMax = Math.max(1, Math.floor(len / 64));
    let best = 1;
    let bestReach = Infinity;
    for (let n = 1; n <= nMax; n += 1) {
      let r = 0;
      for (let k = 0; k < n; k += 1) r = Math.max(r, piece(n, k).reach);
      if (r < bestReach) {
        best = n;
        bestReach = r;
      }
      if (r <= PART_REACH) break;
    }
    for (let k = 0; k < best; k += 1) out.push(piece(best, k));
    acc += len;
  }
  road._pieces = out;
  return out;
}

/** Parts whose world box overlaps a rect (voxels), from the cells that can reach it. */
export function partsIn(world, rect, out = []) {
  const m = PART_REACH * CHUNK + CHUNK;
  const seen = new Set();
  for (const { i, j } of world.cellsOverlapping({ x0: rect.x0 - m, y0: rect.y0 - m, x1: rect.x1 + m, y1: rect.y1 + m })) {
    for (const p of world.cellPlan(i, j).parts) {
      const b = p.aabb;
      if (seen.has(p) || b.x1 < rect.x0 || b.x0 > rect.x1 || b.y1 < rect.y0 || b.y0 > rect.y1) continue;
      seen.add(p);
      out.push(p);
    }
  }
  return out;
}

/**
 * The parts at home in a chunk: structvox's `ChunkSource::grids(chunk)`
 * (with `part.placement.toSourceGrid(part.id)`). Only the chunk's own cell
 * can have homed them there.
 */
export function partsHomedIn(world, cx, cy, cz) {
  const c = world.cellAt(ownerCorner(cx), ownerCorner(cy));
  return world.cellPlan(c.i, c.j).parts.filter((p) => p.home.cx === cx && p.home.cy === cy && p.home.cz === cz);
}
