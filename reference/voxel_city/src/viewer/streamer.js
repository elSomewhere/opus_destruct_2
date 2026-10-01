import * as THREE from "three";
import { geometryFromMesh } from "./voxelMaterial.js";
import { partBit, partMatrix } from "./partBits.js";

const VOXEL = 0.125;
const CH = 32;

function geometryBytes(msg) {
  let bytes = 0;
  const count = (payload) => {
    if (!payload) return;
    for (const a of [payload.position, payload.normal, payload.color, payload.aux, payload.index]) bytes += a.byteLength;
  };
  for (const c of msg.chunks) {
    count(c.opaque);
    count(c.transparent);
  }
  for (const p of msg.parts ?? []) for (const c of p.chunks) {
    count(c.opaque);
    count(c.transparent);
  }
  return bytes;
}

/**
 * Quadtree LOD streaming of column tiles.
 *
 * A tile (lod, cx, cy) covers 32*2^lod LOD0 voxels on each side and all of
 * its chunks vertically. A tile is refined into its 4 children when the
 * camera is closer than `lodFactor * size` (with some hysteresis, so a tile
 * near the threshold does not flip back and forth). Parents stay visible
 * until all children are ready (and vice versa when zooming out), so there
 * are never holes while streaming.
 *
 * The distance includes the vertical distance to the tile's content (ground
 * and anything built under or on it). These ranges are remembered per tile key independently of the tile's lifetime:
 * if they were dropped with a refined (disposed) parent, the next selection
 * would fall back to a broader ancestor range, change its mind, reload the
 * parent and oscillate.
 *
 * The angled world in parts mode: a tile brings the parts at home in it,
 * meshed in their own lattices at its LOD; each is drawn with its exact
 * matrix, shown and dropped with its tile, and at LOD0 the walker collides
 * with it (its cells' bitsets, found through the exact inverse map).
 *
 * A tile whose worker failed is asked for again (up to MAX_TRIES), then
 * shown empty and counted (stats.failed, lastError). Below a tile's lowest
 * chunk the ground is solid (the walker cannot fall out of the world).
 * Debug views: tiles tinted by their LOD, parts tinted, tile borders.
 */

const MAX_TRIES = 3;
/** Tint colours by LOD (debug view "lod"), and of parts (debug view "parts"). */
export const LOD_TINTS = [0xe8505b, 0xf59e2e, 0xf5d547, 0x6cc551, 0x2fbfa8, 0x3fa7d6, 0x5b6fe0, 0x8e6bd8, 0xc65bd0, 0xd0d0d0];
export const PART_TINT = 0xff3df2;
export class TileStreamer {
  constructor({ scene, pool, materials, config }) {
    this.scene = scene;
    this.pool = pool;
    this.materials = materials;
    /** debug view: "none", "lod" (tiles tinted by LOD) or "parts" (parts tinted) */
    this.tint = "none";
    /** keep the tiles as they are while the camera moves (look at LOD boundaries from outside) */
    this.frozen = false;
    this.failed = 0;
    this.lastError = null;
    this.genMs = { n: 0, sum: 0, max: 0 };
    this.meshByLod = Array.from({ length: (config.streaming.maxLod ?? 5) + 1 }, () => ({ samples: 0, chunks: 0, ms: 0, vertices: 0, triangles: 0 }));
    this.borders = null;
    this.maxLod = config.streaming.maxLod ?? 5;
    this.lodFactor = config.streaming.lodFactor ?? 5;
    this.viewDistance = 1500; // meters
    this.tiles = new Map();
    this.desired = new Set();
    this.visible = new Set();
    this.roots = [];
    this.root = new THREE.Group();
    this.root.name = "tiles";
    scene.add(this.root);
    this.stats = { tiles: 0, ready: 0, queued: 0, loading: 0, triangles: 0, meshes: 0, failed: 0, byLod: [] };
    pool.onTile = (msg) => this.onTile(msg);
    this.lastSelect = null;
    /** ground ranges [lo, hi] (voxels) by tile key, kept after tiles are disposed */
    this.ranges = new Map();
    /** ground-only ranges by tile key (is the camera underground?) */
    this.grounds = new Map();
    /** tiles refined in the last selection (hysteresis) */
    this.refined = new Set();
    /** LOD0 parts by the chunk columns their boxes reach ("cx,cy" -> Set): the walker's collision */
    this.partCols = new Map();
  }

  key(lod, cx, cy) {
    return `${lod}:${cx}:${cy}`;
  }

  tileSize(lod) {
    return CH << lod; // voxels
  }

  /**
   * Ground height range [lo, hi] (voxels) of a tile, from the tile itself
   * or its nearest loaded ancestor (sea level until anything is known).
   */
  groundRange(lod, cx, cy) {
    for (let l = lod; l <= this.maxLod; l += 1) {
      const sh = l - lod;
      const r = this.ranges.get(this.key(l, cx >> sh, cy >> sh));
      if (r) return r;
    }
    return [0, 0];
  }

  /** Recompute the desired tile set for a camera position (meters). */
  select(cam) {
    const px = cam.x / VOXEL;
    const py = cam.y / VOXEL;
    const pz = cam.z / VOXEL;
    const desired = new Set();
    const L = this.maxLod;
    const rs = this.tileSize(L);
    const R = this.viewDistance / VOXEL;
    const t0x = Math.floor((px - R) / rs);
    const t1x = Math.floor((px + R) / rs);
    const t0y = Math.floor((py - R) / rs);
    const t1y = Math.floor((py + R) / rs);
    const K = this.lodFactor;
    // underground (basement, sewer, complex, tunnel): the view is enclosed,
    // so detail follows the horizontal distance alone
    let under = false;
    for (let l = 0; l <= L; l += 1) {
      const size = this.tileSize(l);
      const g = this.grounds.get(this.key(l, Math.floor(px / size), Math.floor(py / size)));
      if (g) {
        under = pz < g[0] - 16;
        break;
      }
    }
    this.underground = under;
    const wasRefined = this.refined;
    const refined = new Set();
    const visit = (lod, cx, cy) => {
      const size = this.tileSize(lod);
      const x0 = cx * size;
      const y0 = cy * size;
      const dx = Math.max(x0 - px, 0, px - (x0 + size));
      const dy = Math.max(y0 - py, 0, py - (y0 + size));
      // vertical distance to the tile's ground (mountains: the camera may be
      // kilometers above sea level yet right on the ground)
      const [glo, ghi] = this.groundRange(lod, cx, cy);
      const dz = under ? 0 : Math.max(glo - pz, 0, pz - ghi - 80);
      const d = Math.hypot(dx, dy, dz);
      const key = this.key(lod, cx, cy);
      // refine below K * size; once refined, merge back only beyond 1.2 x that
      const limit = K * size * (wasRefined.has(key) ? 1.2 : 1);
      if (lod > 0 && d < limit) {
        refined.add(key);
        for (let j = 0; j < 2; j += 1) for (let i = 0; i < 2; i += 1) visit(lod - 1, cx * 2 + i, cy * 2 + j);
        return;
      }
      desired.add(key);
      let t = this.tiles.get(this.key(lod, cx, cy));
      if (!t) {
        t = { key: this.key(lod, cx, cy), lod, cx, cy, state: "queued", group: null, prio: 0, solid: null };
        this.tiles.set(t.key, t);
      }
      // nearest first in absolute distance; coarse tiles break ties so the horizon fills early
      t.prio = d + lod * 4;
    };
    const roots = [];
    for (let ty = t0y; ty <= t1y; ty += 1) {
      for (let tx = t0x; tx <= t1x; tx += 1) {
        const cxm = (tx + 0.5) * rs;
        const cym = (ty + 0.5) * rs;
        if (Math.hypot(cxm - px, cym - py) > R + rs) continue;
        visit(L, tx, ty);
        roots.push([tx, ty]);
      }
    }
    this.desired = desired;
    this.refined = refined;
    this.roots = roots;
  }

  update(cam) {
    if (this.frozen) {
      this.dispatch();
      if (this.dirty) {
        this.dirty = false;
        this.resolveVisibility();
      }
      return;
    }
    // new ground heights can change which tiles to refine: reselect now and then
    const now = performance.now();
    if (this.rangeDirty && now - (this.lastRangeSelect ?? 0) > 300) {
      this.rangeDirty = false;
      this.lastRangeSelect = now;
      this.lastSelect = null;
    }
    const moved = !this.lastSelect || this.lastSelect.distanceTo(cam) > 2;
    if (moved) {
      this.select(cam);
      this.lastSelect = cam.clone();
      this.dirty = true;
    }
    this.dispatch();
    if (this.dirty) {
      this.dirty = false;
      this.resolveVisibility();
    }
  }

  dispatch() {
    let slots = this.pool.freeSlots();
    if (!slots) return;
    const queue = [];
    for (const k of this.desired) {
      const t = this.tiles.get(k);
      if (t && t.state === "queued") queue.push(t);
    }
    queue.sort((a, b) => a.prio - b.prio);
    for (const t of queue) {
      if (!slots) break;
      if (!this.pool.dispatchTile({ lod: t.lod, cx: t.cx, cy: t.cy, collision: t.lod === 0 })) break;
      t.state = "loading";
      slots -= 1;
    }
  }

  /** The material of a mesh at LOD `lod` (a part's or the world grid's) for the current debug view. */
  materialFor(transparent, lod, isPart) {
    const m = this.materials;
    if (this.tint === "lod") return m.tinted(transparent, LOD_TINTS[lod] ?? 0xc0c0c0);
    if (this.tint === "parts" && isPart) return m.tinted(transparent, PART_TINT);
    return transparent ? m.transparent : m.opaque;
  }

  /** Switch the debug view of every mesh loaded ("none", "lod", "parts"). */
  setTint(tint) {
    this.tint = tint;
    for (const t of this.tiles.values())
      t.group?.traverse((o) => {
        if (o.isMesh) o.material = this.materialFor(o.userData.transparent, o.userData.lod, o.userData.part);
      });
  }

  onTile(msg) {
    const t = this.tiles.get(this.key(msg.lod, msg.cx, msg.cy));
    if (!t) return;
    if (msg.failed) {
      // ask again (a worker that ran out of memory, a transient error); after MAX_TRIES show it empty
      t.tries = (t.tries ?? 0) + 1;
      this.lastError = msg.error ?? this.lastError;
      if (t.tries < MAX_TRIES) {
        t.state = "queued";
        return;
      }
      this.failed += 1;
      t.state = "ready";
      t.group = new THREE.Group();
      t.tris = 0;
      this.dirty = true;
      return;
    }
    if (msg.ms !== undefined) {
      this.genMs.n += 1;
      this.genMs.sum += msg.ms;
      this.genMs.max = Math.max(this.genMs.max, msg.ms);
    }
    if (msg.meshStats) {
      const s = this.meshByLod[msg.lod];
      s.samples += 1;
      s.chunks += msg.meshStats.chunks;
      s.ms += msg.meshStats.ms;
      s.vertices += msg.meshStats.vertices;
      s.triangles += msg.meshStats.triangles;
    }
    const group = new THREE.Group();
    const s = 1 << msg.lod;
    const scale = VOXEL * s;
    let tris = 0;
    const solid = new Map();
    const climb = new Map();
    for (const c of msg.chunks) {
      const ox = msg.cx * CH * scale;
      const oy = msg.cy * CH * scale;
      const oz = c.cz * CH * scale;
      for (const [payload, transparent] of [
        [c.opaque, false],
        [c.transparent, true],
      ]) {
        if (!payload) continue;
        const mesh = new THREE.Mesh(geometryFromMesh(payload), this.materialFor(transparent, msg.lod, false));
        mesh.userData = { transparent, lod: msg.lod, part: false };
        mesh.position.set(ox, oy, oz);
        mesh.scale.setScalar(scale);
        mesh.matrixAutoUpdate = false;
        mesh.updateMatrix();
        if (transparent) mesh.renderOrder = 1;
        else {
          mesh.castShadow = msg.lod <= 3;
          mesh.receiveShadow = true;
        }
        group.add(mesh);
        tris += payload.index.length / 3;
      }
      if (c.solid) solid.set(c.cz, c.solid);
      if (c.climb) climb.set(c.cz, c.climb);
    }
    // the parts at home in the tile, each in its own lattice
    const partEntries = [];
    for (const p of msg.parts ?? []) {
      const pg = new THREE.Group();
      pg.matrixAutoUpdate = false;
      pg.matrix.set(...partMatrix(p, VOXEL));
      const ps = VOXEL * (1 << p.lod);
      const bits = { solid: new Map(), climb: new Map() };
      for (const c of p.chunks) {
        for (const [payload, transparent] of [
          [c.opaque, false],
          [c.transparent, true],
        ]) {
          if (!payload) continue;
          const mesh = new THREE.Mesh(geometryFromMesh(payload), this.materialFor(transparent, msg.lod, true));
          mesh.userData = { transparent, lod: msg.lod, part: true, partId: p.id, partKind: p.kind };
          mesh.position.set(c.cx * CH * ps, c.cy * CH * ps, c.cz * CH * ps);
          mesh.scale.setScalar(ps);
          mesh.matrixAutoUpdate = false;
          mesh.updateMatrix();
          if (transparent) mesh.renderOrder = 1;
          else {
            mesh.castShadow = msg.lod <= 3;
            mesh.receiveShadow = true;
          }
          pg.add(mesh);
          tris += payload.index.length / 3;
        }
        if (c.solid) bits.solid.set(`${c.cx},${c.cy},${c.cz}`, c.solid);
        if (c.climb) bits.climb.set(`${c.cx},${c.cy},${c.cz}`, c.climb);
      }
      group.add(pg);
      if (msg.lod === 0 && bits.solid.size) partEntries.push({ ...p, bits });
    }
    for (const e of partEntries) this.addPartCols(e);
    group.visible = false;
    t.group = group;
    t.tris = tris;
    t.bytes = geometryBytes(msg);
    t.solid = solid;
    t.climb = climb;
    t.partEntries = partEntries;
    t.zlo = msg.zlo;
    t.zhi = msg.zhi;
    t.cz0 = msg.cz0;
    t.gzlo = msg.gzlo;
    t.gzhi = msg.gzhi;
    // the height range of everything in the tile: ground plus underground
    // features (a walker at a tram station 70 m down needs LOD0 there) and
    // what stands above; a new range only matters if it differs from what was assumed
    const lo = Math.min(msg.gzlo, msg.zlo ?? msg.gzlo);
    const hi = Math.max(msg.gzhi, msg.zhi ?? msg.gzhi);
    this.grounds.set(t.key, [msg.gzlo, msg.gzhi]);
    if (this.grounds.size > 200000) this.grounds.delete(this.grounds.keys().next().value);
    const known = this.ranges.get(t.key);
    if (!known || known[0] !== lo || known[1] !== hi) {
      this.ranges.set(t.key, [lo, hi]);
      if (this.ranges.size > 200000) this.ranges.delete(this.ranges.keys().next().value);
      this.rangeDirty = true;
    }
    t.state = "ready";
    this.root.add(group);
    this.dirty = true;
    // keep workers busy without waiting for the next frame
    this.dispatch();
  }

  isReady(key) {
    const t = this.tiles.get(key);
    return !!t && t.state === "ready";
  }

  /**
   * Collect tiles that cover the region of (lod,cx,cy). Returns true when the
   * region is fully covered by ready tiles.
   */
  cover(lod, cx, cy, out) {
    const k = this.key(lod, cx, cy);
    if (this.desired.has(k)) {
      if (this.isReady(k)) {
        out.push(k);
        return true;
      }
      // zoom-out case: finer tiles from before may still cover it (any depth: no hole while the coarse tile loads)
      if (lod > 0) {
        const tmp = [];
        if (this.coverLoaded(lod, cx, cy, tmp, lod)) {
          out.push(...tmp);
          return true;
        }
      }
      return false;
    }
    if (lod === 0) return false;
    const tmp = [];
    let all = true;
    for (let j = 0; j < 2; j += 1)
      for (let i = 0; i < 2; i += 1) if (!this.cover(lod - 1, cx * 2 + i, cy * 2 + j, tmp)) all = false;
    if (all) {
      out.push(...tmp);
      return true;
    }
    if (this.isReady(k)) {
      out.push(k);
      return true;
    }
    out.push(...tmp);
    return false;
  }

  /** Cover a region with already-loaded descendants (bounded depth). */
  coverLoaded(lod, cx, cy, out, depth) {
    if (depth <= 0 || lod === 0) return false;
    for (let j = 0; j < 2; j += 1) {
      for (let i = 0; i < 2; i += 1) {
        const c = this.key(lod - 1, cx * 2 + i, cy * 2 + j);
        if (this.isReady(c)) out.push(c);
        else if (!this.coverLoaded(lod - 1, cx * 2 + i, cy * 2 + j, out, depth - 1)) return false;
      }
    }
    return true;
  }

  resolveVisibility() {
    const L = this.maxLod;
    const show = [];
    for (const [cx, cy] of this.roots) this.cover(L, cx, cy, show);
    const next = new Set(show);
    for (const k of this.visible) {
      if (!next.has(k)) {
        const t = this.tiles.get(k);
        if (t?.group) t.group.visible = false;
      }
    }
    let tris = 0;
    let meshes = 0;
    let gpuBytes = 0;
    for (const k of next) {
      const t = this.tiles.get(k);
      if (t?.group) {
        t.group.visible = true;
        tris += t.tris;
        gpuBytes += t.bytes ?? 0;
        meshes += t.group.children.length;
      }
    }
    this.visible = next;
    // dispose tiles that are neither desired nor visible
    let queued = 0;
    let loading = 0;
    let ready = 0;
    const byLod = Array.from({ length: this.maxLod + 1 }, () => ({ shown: 0, ready: 0, queued: 0, loading: 0 }));
    for (const k of next) {
      const t = this.tiles.get(k);
      if (t) byLod[t.lod].shown += 1;
    }
    for (const [k, t] of this.tiles) {
      if (t.state === "queued") queued += 1;
      if (t.state === "loading") loading += 1;
      if (t.state === "ready") ready += 1;
      byLod[t.lod][t.state] += 1;
      if (this.desired.has(k) || next.has(k)) continue;
      if (t.state === "loading") continue;
      if (t.group) {
        t.group.traverse((m) => m.geometry?.dispose());
        this.root.remove(t.group);
      }
      for (const e of t.partEntries ?? []) this.removePartCols(e);
      this.tiles.delete(k);
    }
    const meshByLod = this.meshByLod.map((s) => ({ ...s, msPerChunk: s.chunks ? s.ms / s.chunks : 0, verticesPerChunk: s.chunks ? s.vertices / s.chunks : 0, trianglesPerChunk: s.chunks ? s.triangles / s.chunks : 0 }));
    this.stats = { tiles: this.tiles.size, ready, queued, loading, triangles: tris, meshes, gpuBytes, failed: this.failed, byLod, meshByLod, genMs: this.genMs.n ? this.genMs.sum / this.genMs.n : 0, genMsMax: this.genMs.max };
    if (this.borders) this.drawBorders();
  }

  /** Debug view: the borders of the tiles shown, at their top, coloured by LOD. */
  showBorders(on) {
    if (!on) {
      if (this.borders) {
        this.borders.geometry.dispose();
        this.scene.remove(this.borders);
        this.borders = null;
      }
      return;
    }
    if (!this.borders) {
      this.borders = new THREE.LineSegments(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ vertexColors: true, depthTest: false, transparent: true, opacity: 0.85 }));
      this.borders.renderOrder = 10;
      this.borders.frustumCulled = false;
      this.scene.add(this.borders);
    }
    this.drawBorders();
  }

  drawBorders() {
    const pos = [];
    const col = [];
    const c = new THREE.Color();
    for (const k of this.visible) {
      const t = this.tiles.get(k);
      if (!t || t.zhi === undefined) continue;
      const size = this.tileSize(t.lod) * VOXEL;
      const x0 = t.cx * size;
      const y0 = t.cy * size;
      const z = (Math.min(t.zhi, t.gzhi + 16) + 1) * VOXEL;
      c.set(LOD_TINTS[t.lod] ?? 0xc0c0c0);
      const pts = [x0, y0, x0 + size, y0, x0 + size, y0 + size, x0, y0 + size];
      for (let i = 0; i < 4; i += 1) {
        const j = (i + 1) % 4;
        pos.push(pts[2 * i], pts[2 * i + 1], z, pts[2 * j], pts[2 * j + 1], z);
        col.push(c.r, c.g, c.b, c.r, c.g, c.b);
      }
    }
    const g = this.borders.geometry;
    g.setAttribute("position", new THREE.Float32BufferAttribute(pos, 3));
    g.setAttribute("color", new THREE.Float32BufferAttribute(col, 3));
    g.computeBoundingSphere();
  }

  /** The lowest content (m) of the LOD 0 tile under a point, or null: a walker below it has fallen out. */
  floorAt(xm, ym) {
    const t = this.tiles.get(this.key(0, Math.floor(xm / VOXEL / CH), Math.floor(ym / VOXEL / CH)));
    if (!t || t.state !== "ready" || t.zlo === undefined) return null;
    return t.zlo * VOXEL;
  }

  /** Walk-mode collision: is the LOD0 voxel (x,y,z) solid? null if unknown. */
  solidAt(x, y, z) {
    return this.bitAt("solid", x, y, z);
  }

  /** Is the LOD0 voxel (x,y,z) climbable (ladder)? null if unknown. */
  climbAt(x, y, z) {
    return this.bitAt("climb", x, y, z);
  }

  bitAt(layer, x, y, z) {
    const cx = Math.floor(x / CH);
    const cy = Math.floor(y / CH);
    // (the parts reaching this column: a pitched street, a turned building's walls)
    const parts = this.partCols.get(`${cx},${cy}`);
    if (parts) for (const p of parts) if (partBit(p, layer, x, y, z)) return true;
    const t = this.tiles.get(this.key(0, cx, cy));
    if (!t || t.state !== "ready" || !t[layer]) return null;
    const cz = Math.floor(z / CH);
    // (below the tile's lowest chunk: the ground's fill, solid)
    if (t.cz0 !== undefined && cz < t.cz0) return layer === "solid";
    const bits = t[layer].get(cz);
    if (!bits) return false;
    const lx = x - cx * CH;
    const ly = y - cy * CH;
    const lz = z - cz * CH;
    const n = lx + ly * CH + lz * CH * CH;
    return (bits[n >>> 5] & (1 << (n & 31))) !== 0;
  }

  /** Index a LOD0 part by the chunk columns its box reaches. */
  addPartCols(e) {
    const b = e.aabb;
    for (let cy = Math.floor(b.y0 / CH); cy <= Math.floor(b.y1 / CH); cy += 1)
      for (let cx = Math.floor(b.x0 / CH); cx <= Math.floor(b.x1 / CH); cx += 1) {
        const k = `${cx},${cy}`;
        if (!this.partCols.has(k)) this.partCols.set(k, new Set());
        this.partCols.get(k).add(e);
      }
  }

  removePartCols(e) {
    const b = e.aabb;
    for (let cy = Math.floor(b.y0 / CH); cy <= Math.floor(b.y1 / CH); cy += 1)
      for (let cx = Math.floor(b.x0 / CH); cx <= Math.floor(b.x1 / CH); cx += 1) {
        const set = this.partCols.get(`${cx},${cy}`);
        if (!set) continue;
        set.delete(e);
        if (!set.size) this.partCols.delete(`${cx},${cy}`);
      }
  }

  dispose() {
    this.showBorders(false);
    for (const t of this.tiles.values()) {
      if (t.group) t.group.traverse((m) => m.geometry?.dispose());
    }
    this.partCols.clear();
    this.scene.remove(this.root);
    this.tiles.clear();
  }
}
