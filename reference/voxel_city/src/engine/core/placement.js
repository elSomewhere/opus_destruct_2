import { VOXEL_SIZE } from "./units.js";

/**
 * Exact placements of local voxel lattices in the world
 * (ANGLED_WORLD_PLAN.md §4.1-4.2).
 *
 * Every rotation is quantized to a Pythagorean triple p² + q² = r², so its
 * cosine and sine are the rationals q / r and p / r: no sin, cos or atan2
 * anywhere, and both mapping directions stay in integer arithmetic, bit
 * for bit the same in every JS engine and in C++ (structvox).
 *
 *   yaw    about the world's up axis, an index into YAWS (132 exact yaws:
 *          the four proper rotations and 32 angles in every quadrant),
 *          optionally followed by a second one, `yaw2` (a bay turned on a
 *          turned building): the exact product of the two rotations, a
 *          triple of its own (yawProduct), still in integers
 *   pitch  about the local v axis, a signed index into PITCHES (a positive
 *          pitch climbs along +u: road grades)
 *   roll   about the local u axis, an index into YAWS (tilted boulders,
 *          fallen logs)
 *
 * The rotation local -> world is M / D = Rz(yaw) · Ry(pitch) · Rx(roll),
 * an integer matrix M over the common denominator D (the product of the
 * three hypotenuses). World frame: x east, y south, z up (right-handed); a
 * yaw turns +u from east towards south, so the four proper rotations are
 * the canonical building frame's fronts: N = 0°, E = 90°, S = 180°, W = 270°.
 *
 * Geometry. `origin` is the world point (integer voxel coordinates) where
 * the corner of local cell (0, 0, 0) lies. A world voxel (x, y, z) belongs
 * to the local cell holding its centre:
 *   (u, v, w) = floor(Mᵀ · (2 (p - origin) + 1) / 2D)
 * and a local cell's centre lies at origin + M · (2 (u, v, w) + 1) / 2D.
 * Membership tests compare the integer numerators against the scaled
 * extent, so no tolerance constants are needed.
 *
 * The four proper rotations (yaw 0, 33, 66, 99 with no pitch or roll) take
 * a fast path that evaluates exactly the canonical frame's expressions
 * (buildings/frame.js), fractional arguments included, so axis-aligned
 * output stays bit-identical to the engine before this module; on integer
 * cells it agrees with the general maps (test/placement.test.js).
 */

/**
 * Primitive Pythagorean triples [p, q, r] with r ≤ 100, by angle atan(p / q):
 * 8.80° to 43.60° in steps of 1-5°. There are exactly 16, all ≥ 8°.
 * (45° itself has no exact rational rotation.)
 */
export const YAW_TRIPLES = [
  [13, 84, 85],
  [11, 60, 61],
  [9, 40, 41],
  [16, 63, 65],
  [7, 24, 25],
  [12, 35, 37],
  [5, 12, 13],
  [36, 77, 85],
  [39, 80, 89],
  [8, 15, 17],
  [33, 56, 65],
  [28, 45, 53],
  [3, 4, 5],
  [48, 55, 73],
  [65, 72, 97],
  [20, 21, 29],
];

/**
 * Pitch family (2n, n² - 1, n² + 1): grade 2n / (n² - 1), within a hair of
 * the road classes' grade limits (network/roadLevel.js): arterial 8%,
 * collector 10%, local 12%, village 13% / alley 14%, lane 16%, steepest 20%.
 */
export const PITCH_N = [25, 20, 16, 14, 12, 10];

function gcd(a, b) {
  a = Math.abs(a);
  b = Math.abs(b);
  while (b) [a, b] = [b, a % b];
  return a;
}

/** Yaws of one quadrant [0°, 90°): identity, the 16 triples, then their mirrors (90° - θ), by angle. */
const QUADRANT = [[1, 0, 1], ...YAW_TRIPLES.map(([p, q, r]) => [q, p, r]), ...YAW_TRIPLES.slice().reverse().map(([p, q, r]) => [p, q, r])];

/** Every exact yaw: { c, s, r } with c² + s² = r² (cos = c / r, sin = s / r), by angle from 0 (east) to 360°. */
export const YAWS = [];
for (let k = 0; k < 4; k += 1) {
  for (const [c0, s0, r] of QUADRANT) {
    let c = c0;
    let s = s0;
    // (0 - s: a quarter turn of a zero sine is +0, never -0)
    for (let t = 0; t < k; t += 1) [c, s] = [0 - s, c];
    YAWS.push(Object.freeze({ c, s, r }));
  }
}
Object.freeze(YAWS);

/** Yaws per quarter turn; the proper rotations are YAW_QUARTER * k. */
export const YAW_QUARTER = QUADRANT.length;

/** Yaw index of a canonical front (the direction +u points to is the front's clockwise neighbour). */
export const FRONT_YAW = Object.freeze({ N: 0, E: YAW_QUARTER, S: 2 * YAW_QUARTER, W: 3 * YAW_QUARTER });

/** Pitches: index 0 level, 1..6 the grades of PITCH_N (reduced to primitive triples); negative indices descend along +u. */
export const PITCHES = Object.freeze([
  Object.freeze({ c: 1, s: 0, r: 1, n: 0 }),
  ...PITCH_N.map((n) => {
    const g = gcd(gcd(2 * n, n * n - 1), n * n + 1);
    return Object.freeze({ c: (n * n - 1) / g, s: (2 * n) / g, r: (n * n + 1) / g, n });
  }),
]);

/** Exact floor(a / b) for integers (|a| < 2^53, b > 0). */
export function floorDiv(a, b) {
  return Math.floor(a / b);
}

/** Exact ceil(a / b) for integers (|a| < 2^53, b > 0). */
export function ceilDiv(a, b) {
  return Math.ceil(a / b);
}

function yawEntry(i) {
  const y = YAWS[i];
  if (!y) throw new Error(`placement: no yaw ${i}`);
  return y;
}

/**
 * The exact yaw of table yaw `a` followed by table yaw `b`: { c, s, r }, the
 * product of their Gaussian integers (c + i s) reduced to a primitive
 * triple (its legs of opposite parity and r odd, as every table triple's:
 * a voxel centre never lands on a cell face). Angles add; nothing is
 * rounded.
 */
export function yawProduct(a, b) {
  const A = yawEntry(a);
  const B = yawEntry(b);
  const c = A.c * B.c - A.s * B.s;
  const s = A.c * B.s + A.s * B.c;
  const r = A.r * B.r;
  const g = gcd(gcd(c, s), r);
  // (+ 0: never -0)
  return { c: c / g + 0, s: s / g + 0, r: r / g };
}

/** Index of the table yaw equal to the exact rotation { c, s, r } (reduced), or -1. */
export function yawIndex({ c, s, r }) {
  return YAWS.findIndex((y) => y.c === c && y.s === s && y.r === r);
}

/** The exact direction { c, s, r } of yaw `yaw` followed by `yaw2` (0: none). */
export function yawVector(yaw, yaw2 = 0) {
  return yaw2 ? yawProduct(yaw, yaw2) : yawEntry(yaw);
}

function pitchEntry(i) {
  const p = PITCHES[Math.abs(i)];
  if (!p) throw new Error(`placement: no pitch ${i}`);
  return i < 0 ? { c: p.c, s: -p.s, r: p.r } : p;
}

/** 3x3 integer matrix product (row-major arrays). */
function mul3(a, b) {
  const o = new Array(9);
  for (let i = 0; i < 3; i += 1)
    for (let j = 0; j < 3; j += 1) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
  return o;
}

const matrices = new Map();

/**
 * The rotation of (yaw, pitch, roll) as an integer matrix over a common
 * denominator: { m (row-major, local -> world), d }. Shared and frozen.
 */
export function rotationMatrix(yaw = 0, pitch = 0, roll = 0, yaw2 = 0) {
  const key = `${yaw},${pitch},${roll},${yaw2}`;
  let rot = matrices.get(key);
  if (!rot) {
    rot = computeRotation(yaw, pitch, roll, yaw2);
    Object.freeze(rot.m);
    matrices.set(key, Object.freeze(rot));
  }
  return rot;
}

function computeRotation(yaw, pitch, roll, yaw2) {
  const Y = yawVector(yaw, yaw2);
  const P = pitchEntry(pitch);
  const R = yawEntry(roll);
  // yaw about z; pitch about v, +u climbing; roll about u (0 - s: never -0)
  const rz = [Y.c, 0 - Y.s, 0, Y.s, Y.c, 0, 0, 0, Y.r];
  const ry = [P.c, 0, 0 - P.s, 0, P.r, 0, P.s, 0, P.c];
  const rx = [R.r, 0, 0, 0, R.c, 0 - R.s, 0, R.s, R.c];
  // (+ 0: a sum of zero products is +0)
  const m = mul3(mul3(rz, ry), rx).map((v) => v + 0);
  let d = Y.r * P.r * R.r;
  // (keep the smallest denominator: a quarter turn or no turn at all is over 1)
  const g = m.reduce((acc, v) => gcd(acc, v), d);
  if (g > 1) {
    for (let k = 0; k < 9; k += 1) m[k] /= g;
    d /= g;
  }
  return { m, d };
}

/**
 * Unit quaternion { x, y, z, w } (w ≥ 0) of the rotation M / D. Every
 * component is the square root of an exact rational, taken once: IEEE 754
 * rounds division and sqrt correctly, so the result is bit-identical on
 * every platform and in every language (structvox's SourceGrid.rot).
 */
export function matrixQuat(m, d) {
  const [m00, m01, m02, m10, m11, m12, m20, m21, m22] = m;
  const d4 = 4 * d;
  const W = d + m00 + m11 + m22;
  const X = d + m00 - m11 - m22;
  const Y = d - m00 + m11 - m22;
  const Z = d - m00 - m11 + m22;
  const w = Math.sqrt(W / d4);
  let x = Math.sqrt(X / d4);
  let y = Math.sqrt(Y / d4);
  let z = Math.sqrt(Z / d4);
  if (W > 0) {
    // signs from the antisymmetric part (exact integer tests)
    if (m21 - m12 < 0) x = -x;
    if (m02 - m20 < 0) y = -y;
    if (m10 - m01 < 0) z = -z;
  } else {
    // a half turn: the axis up to sign; its largest component is positive
    if (X >= Y && X >= Z) {
      if (m01 + m10 < 0) y = -y;
      if (m02 + m20 < 0) z = -z;
    } else if (Y >= Z) {
      if (m01 + m10 < 0) x = -x;
      if (m12 + m21 < 0) z = -z;
    } else {
      if (m02 + m20 < 0) x = -x;
      if (m12 + m21 < 0) y = -y;
    }
  }
  return { x: x + 0, y: y + 0, z: z + 0, w };
}

/**
 * Index of the exact yaw closest to direction (dx, dy) among `allowed`
 * (yaw indices; default all), by the largest cosine. Ties go to the lower
 * index. Pure arithmetic: no atan2.
 */
export function nearestYaw(dx, dy, allowed = null) {
  let best = 0;
  let bestCos = -Infinity;
  const n = allowed ? allowed.length : YAWS.length;
  for (let k = 0; k < n; k += 1) {
    const i = allowed ? allowed[k] : k;
    const y = YAWS[i];
    const cos = (y.c * dx + y.s * dy) / y.r;
    if (cos > bestCos) {
      bestCos = cos;
      best = i;
    }
  }
  return best;
}

/** The four proper rotations, as yaw indices. */
export const CARDINAL_YAWS = Object.freeze([0, YAW_QUARTER, 2 * YAW_QUARTER, 3 * YAW_QUARTER]);
const ALL_YAWS = Object.freeze(YAWS.map((_, i) => i));

/** Yaw indices a config allows: all exact yaws, or (angles off / "cardinal") the proper rotations only. */
export function allowedYaws(config) {
  const a = config?.world?.angles;
  if (!a?.enabled || a.yawSet === "cardinal") return CARDINAL_YAWS;
  return ALL_YAWS;
}

export class Placement {
  /**
   * @param origin  world point (integer voxels) of local cell (0, 0, 0)'s corner
   * @param yaw, pitch, roll  table indices (0 = identity)
   * @param yaw2  a second table yaw after `yaw` (their exact product), 0 for none
   * @param h  voxel size (m); only the world's is supported yet
   * @param priority  which part owns a deliberate overlap (structvox `priority`)
   * @param anchored  hint: ground / support (no bonds between anchored voxels)
   * @param extent  optional local box {u0, v0, w0, u1, v1, w1} (inclusive cells)
   */
  constructor({ origin = { x: 0, y: 0, z: 0 }, yaw = 0, yaw2 = 0, pitch = 0, roll = 0, h = VOXEL_SIZE, priority = 0, anchored = false, extent = null } = {}) {
    if (h !== VOXEL_SIZE) throw new Error("placement: only the world voxel size is supported");
    // (a product that is a table yaw is that yaw: one name for every rotation)
    if (yaw2) {
      const k = yawIndex(yawProduct(yaw, yaw2));
      if (k >= 0) [yaw, yaw2] = [k, 0];
    }
    this.origin = { x: origin.x, y: origin.y, z: origin.z ?? 0 };
    this.yaw = yaw;
    /** a second table yaw after `yaw` (0: none) */
    this.yaw2 = yaw2;
    this.pitch = pitch;
    this.roll = roll;
    this.h = h;
    this.priority = priority;
    this.anchored = anchored;
    this.extent = extent;
    const { m, d } = rotationMatrix(yaw, pitch, roll, yaw2);
    this.m = m;
    this.d = d;
    /** only a yaw: x, y map on their own and z = w + origin.z */
    this.flat = pitch === 0 && roll === 0;
    // quarter turns of an axis-aligned placement (-1: not one), and its pivot:
    // the world cell of local cell (0, 0)
    this.q = this.flat && yaw2 === 0 && yaw % YAW_QUARTER === 0 ? yaw / YAW_QUARTER : -1;
    if (this.q >= 0) {
      this.px = this.origin.x - (this.q === 1 || this.q === 2 ? 1 : 0);
      this.py = this.origin.y - (this.q === 2 || this.q === 3 ? 1 : 0);
    }
  }

  /**
   * An axis-aligned placement from its pivot: the world cell (px, py) of
   * local cell (0, 0) and the quarter turns q (0 N, 1 E, 2 S, 3 W). The
   * pivot is kept as given, so the fast path evaluates the canonical
   * frame's expressions exactly.
   */
  static cardinal(px, py, q, opts = {}) {
    const o = { x: px + (q === 1 || q === 2 ? 1 : 0), y: py + (q === 2 || q === 3 ? 1 : 0), z: opts.z ?? 0 };
    const p = new Placement({ ...opts, origin: o, yaw: q * YAW_QUARTER });
    p.px = px;
    p.py = py;
    return p;
  }

  /** Axis-aligned: one of the four proper rotations (a world-grid part). */
  get axisAligned() {
    return this.q >= 0;
  }

  /** World voxel (x, y) -> local cell [u, v] of a placement with only a yaw. */
  toLocalXY(x, y) {
    switch (this.q) {
      case 0:
        return [x - this.px, y - this.py];
      case 1:
        return [y - this.py, this.px - x];
      case 2:
        return [this.px - x, this.py - y];
      case 3:
        return [this.py - y, x - this.px];
      default: {
        const m = this.m;
        const dx = 2 * (x - this.origin.x) + 1;
        const dy = 2 * (y - this.origin.y) + 1;
        const d2 = 2 * this.d;
        return [floorDiv(m[0] * dx + m[3] * dy, d2), floorDiv(m[1] * dx + m[4] * dy, d2)];
      }
    }
  }

  /**
   * Local (u, v) -> world [x, y] of a placement with only a yaw: the world
   * cell of a local cell (the canonical frame's point map on the fast path,
   * fractions pass through), else the world cell holding the cell's centre.
   */
  toWorldXY(u, v) {
    switch (this.q) {
      case 0:
        return [this.px + u, this.py + v];
      case 1:
        return [this.px - v, this.py + u];
      case 2:
        return [this.px - u, this.py - v];
      case 3:
        return [this.px + v, this.py - u];
      default: {
        const m = this.m;
        const du = 2 * u + 1;
        const dv = 2 * v + 1;
        const d2 = 2 * this.d;
        return [this.origin.x + floorDiv(m[0] * du + m[1] * dv, d2), this.origin.y + floorDiv(m[3] * du + m[4] * dv, d2)];
      }
    }
  }

  /** Local direction (du, dv) -> world [dx, dy] (exact on the fast path, else divided by D). */
  dirToWorldXY(du, dv) {
    switch (this.q) {
      case 0:
        return [du, dv];
      case 1:
        return [-dv, du];
      case 2:
        return [-du, -dv];
      case 3:
        return [dv, -du];
      default: {
        const m = this.m;
        return [(m[0] * du + m[1] * dv) / this.d, (m[3] * du + m[4] * dv) / this.d];
      }
    }
  }

  /** World voxel -> local cell [u, v, w]. */
  toLocal(x, y, z) {
    if (this.flat) {
      const [u, v] = this.toLocalXY(x, y);
      return [u, v, z - this.origin.z];
    }
    const m = this.m;
    const o = this.origin;
    const dx = 2 * (x - o.x) + 1;
    const dy = 2 * (y - o.y) + 1;
    const dz = 2 * (z - o.z) + 1;
    const d2 = 2 * this.d;
    return [floorDiv(m[0] * dx + m[3] * dy + m[6] * dz, d2), floorDiv(m[1] * dx + m[4] * dy + m[7] * dz, d2), floorDiv(m[2] * dx + m[5] * dy + m[8] * dz, d2)];
  }

  /** Local cell -> the world voxel holding its centre [x, y, z]. */
  toWorld(u, v, w) {
    if (this.flat) {
      const [x, y] = this.toWorldXY(u, v);
      return [x, y, w + this.origin.z];
    }
    const [X, Y, Z] = this.toWorldScaled(u, v, w);
    const d2 = 2 * this.d;
    const o = this.origin;
    return [o.x + floorDiv(X, d2), o.y + floorDiv(Y, d2), o.z + floorDiv(Z, d2)];
  }

  /**
   * Local cell centre -> world point relative to the origin as exact
   * integers over 2D: [X, Y, Z] with world = origin + [X, Y, Z] / 2D.
   */
  toWorldScaled(u, v, w) {
    const m = this.m;
    const du = 2 * u + 1;
    const dv = 2 * v + 1;
    const dw = 2 * w + 1;
    return [m[0] * du + m[1] * dv + m[2] * dw, m[3] * du + m[4] * dv + m[5] * dw, m[6] * du + m[7] * dv + m[8] * dw];
  }

  /**
   * Inclusive world voxel box holding every world voxel whose centre falls
   * in the local box `e` {u0, v0, w0, u1, v1, w1} (inclusive cells): exact
   * for the proper rotations, else the (exact, integer) bounds of the
   * rotated box, which may take in a sliver more.
   */
  localBoundsToWorldAABB(e) {
    const w0 = e.w0 ?? 0;
    const w1 = e.w1 ?? 0;
    if (this.q >= 0) {
      const [ax, ay] = this.toWorldXY(e.u0, e.v0);
      const [bx, by] = this.toWorldXY(e.u1, e.v1);
      return { x0: Math.min(ax, bx), y0: Math.min(ay, by), z0: w0 + this.origin.z, x1: Math.max(ax, bx), y1: Math.max(ay, by), z1: w1 + this.origin.z };
    }
    // corners of the continuous box [u0, u1 + 1) x ... in world numerators over D
    const m = this.m;
    const lo = [Infinity, Infinity, Infinity];
    const hi = [-Infinity, -Infinity, -Infinity];
    for (const u of [e.u0, e.u1 + 1])
      for (const v of [e.v0, e.v1 + 1])
        for (const w of [w0, w1 + 1])
          for (let a = 0; a < 3; a += 1) {
            const n = m[a * 3] * u + m[a * 3 + 1] * v + m[a * 3 + 2] * w;
            if (n < lo[a]) lo[a] = n;
            if (n > hi[a]) hi[a] = n;
          }
    // world voxel p is in when its centre p + 1/2 lies within [lo, hi] / D
    const d = this.d;
    const o = this.origin;
    const cell = (n, up) => (up ? ceilDiv(2 * n - d, 2 * d) : floorDiv(2 * n - d, 2 * d));
    return { x0: o.x + cell(lo[0], true), y0: o.y + cell(lo[1], true), z0: o.z + cell(lo[2], true), x1: o.x + cell(hi[0], false), y1: o.y + cell(hi[1], false), z1: o.z + cell(hi[2], false) };
  }

  /** World box of the placement's own extent (null without one). */
  worldAABB() {
    return this.extent ? this.localBoundsToWorldAABB(this.extent) : null;
  }

  /** Unit quaternion {x, y, z, w} of the rotation (bit-identical everywhere). */
  toQuat() {
    return matrixQuat(this.m, this.d);
  }

  /**
   * structvox `SourceGrid { id, origin, rot, voxel_size, priority }`
   * (ANGLED_WORLD_PLAN.md §8), rot a unit quaternion. structvox centres a
   * grid's voxel p at `origin + R h p` and the world's voxel w at `h w`,
   * where this engine centres world voxel w at h (w + 1/2): so its origin is
   * h (origin + (R - I) / 2 · (1, 1, 1)), and each local voxel lands in the
   * world voxel `toWorld` names (docs/GRIDS.md of structvox, Game::far_mesh).
   */
  toSourceGrid(id) {
    const o = this.origin;
    const m = this.m;
    const d = this.d;
    // (an exact rational per axis, one division, and h a power of two)
    const axis = (a, oa) => ((2 * d * oa + m[a * 3] + m[a * 3 + 1] + m[a * 3 + 2] - d) / (2 * d)) * this.h;
    return { id, origin: [axis(0, o.x), axis(1, o.y), axis(2, o.z)], rot: this.toQuat(), voxel_size: this.h, priority: this.priority };
  }
}
