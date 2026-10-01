import { MATERIALS, IS_TRANSPARENT } from "./materials.js";
import { P, P2 } from "./chunk.js";
import { CHUNK } from "../core/units.js";

const CORNERS = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]];
const EDGES = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];
const TETS = [[0, 5, 1, 6], [0, 1, 2, 6], [0, 2, 3, 6], [0, 3, 7, 6], [0, 7, 4, 6], [0, 4, 5, 6]];

class TriangleBuilder {
  constructor() {
    this.position = [];
    this.normal = [];
    this.color = [];
    this.aux = [];
    this.index = [];
  }

  triangle(a, b, c, mat, hint = null) {
    const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    const ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    let n = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
    if (hint && n[0] * hint[0] + n[1] * hint[1] + n[2] * hint[2] < 0) [b, c] = [c, b];
    const len = Math.hypot(...n) || 1;
    n = n.map((v) => Math.round((v / len) * 127));
    const base = this.position.length / 3;
    const m = MATERIALS[mat];
    for (const p of [a, b, c]) {
      this.position.push(...p);
      this.normal.push(...n);
      this.color.push(...m.rgb);
      this.aux.push(255, Math.round(m.emissive * 255), Math.round(Math.min(1, m.noise * 4) * 255), 255);
    }
    this.index.push(base, base + 1, base + 2);
  }

  finish() {
    if (!this.index.length) return null;
    const Index = this.position.length / 3 > 65535 ? Uint32Array : Uint16Array;
    return { position: Float32Array.from(this.position), normal: Int8Array.from(this.normal), color: Uint8Array.from(this.color), aux: Uint8Array.from(this.aux), index: Index.from(this.index) };
  }
}

const solid = (data, x, y, z) => {
  const m = data[x + y * P + z * P2];
  return m !== 0 && !IS_TRANSPARENT[m];
};

function cell(data, x, y, z) {
  const inside = new Array(8);
  let mat = 0;
  let count = 0;
  for (let k = 0; k < 8; k += 1) {
    const c = CORNERS[k];
    inside[k] = solid(data, x + c[0], y + c[1], z + c[2]);
    if (inside[k]) {
      count += 1;
      mat ||= data[x + c[0] + (y + c[1]) * P + (z + c[2]) * P2];
    }
  }
  return { inside, mat, count };
}

const midpoint = (x, y, z, a, b) => [(x + (CORNERS[a][0] + CORNERS[b][0]) * 0.5) - 0.5, (y + (CORNERS[a][1] + CORNERS[b][1]) * 0.5) - 0.5, (z + (CORNERS[a][2] + CORNERS[b][2]) * 0.5) - 0.5];

/** Binary marching cubes, decomposed into consistently oriented tetrahedra. */
export function meshMarchingCubes(data) {
  const out = new TriangleBuilder();
  for (let z = 1; z <= CHUNK; z += 1) for (let y = 1; y <= CHUNK; y += 1) for (let x = 1; x <= CHUNK; x += 1) {
    const c = cell(data, x, y, z);
    if (!c.count || c.count === 8) continue;
    for (const tet of TETS) {
      const hits = [];
      for (let a = 0; a < 4; a += 1) for (let b = a + 1; b < 4; b += 1) {
        const ca = tet[a];
        const cb = tet[b];
        if (c.inside[ca] !== c.inside[cb]) hits.push(midpoint(x, y, z, ca, cb));
      }
      if (hits.length < 3) continue;
      const hint = [0, 0, 0];
      for (let k = 0; k < 8; k += 1) if (c.inside[k]) for (let d = 0; d < 3; d += 1) hint[d] += 0.5 - CORNERS[k][d];
      out.triangle(hits[0], hits[1], hits[2], c.mat, hint);
      if (hits.length === 4) out.triangle(hits[0], hits[2], hits[3], c.mat, hint);
    }
  }
  return out.finish();
}

const key = (x, y, z) => x + y * 33 + z * 33 * 33;

/** Surface-nets dual contouring: one clamped vertex per active cell. */
export function meshDualContour(data) {
  const vertices = new Array(33 * 33 * 33);
  const cells = new Array(vertices.length);
  for (let z = 0; z <= CHUNK; z += 1) for (let y = 0; y <= CHUNK; y += 1) for (let x = 0; x <= CHUNK; x += 1) {
    const c = cell(data, x, y, z);
    if (!c.count || c.count === 8) continue;
    const p = [0, 0, 0];
    let n = 0;
    for (const [a, b] of EDGES) if (c.inside[a] !== c.inside[b]) {
      const q = midpoint(x, y, z, a, b);
      p[0] += q[0]; p[1] += q[1]; p[2] += q[2]; n += 1;
    }
    const k = key(x, y, z);
    vertices[k] = p.map((v) => v / n);
    cells[k] = c;
  }
  const out = new TriangleBuilder();
  const quad = (ids, mat, flip) => {
    const p = ids.map((i) => vertices[i]);
    if (p.some((v) => !v)) return;
    if (flip) { out.triangle(p[0], p[2], p[1], mat); out.triangle(p[0], p[3], p[2], mat); }
    else { out.triangle(p[0], p[1], p[2], mat); out.triangle(p[0], p[2], p[3], mat); }
  };
  for (let z = 1; z <= CHUNK; z += 1) for (let y = 1; y <= CHUNK; y += 1) for (let x = 1; x <= CHUNK; x += 1) {
    if (solid(data, x, y, z) !== solid(data, x + 1, y, z)) quad([key(x, y - 1, z - 1), key(x, y, z - 1), key(x, y, z), key(x, y - 1, z)], cells[key(x, y, z)]?.mat ?? data[x + y * P + z * P2], solid(data, x, y, z));
    if (solid(data, x, y, z) !== solid(data, x, y + 1, z)) quad([key(x - 1, y, z - 1), key(x - 1, y, z), key(x, y, z), key(x, y, z - 1)], cells[key(x, y, z)]?.mat ?? data[x + y * P + z * P2], !solid(data, x, y, z));
    if (solid(data, x, y, z) !== solid(data, x, y, z + 1)) quad([key(x - 1, y - 1, z), key(x, y - 1, z), key(x, y, z), key(x - 1, y, z)], cells[key(x, y, z)]?.mat ?? data[x + y * P + z * P2], solid(data, x, y, z));
  }
  return out.finish();
}

