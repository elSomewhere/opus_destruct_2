import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { YAW_TRIPLES, YAWS, YAW_QUARTER, FRONT_YAW, CARDINAL_YAWS, PITCHES, Placement, rotationMatrix, matrixQuat, nearestYaw, allowedYaws, yawProduct, yawIndex, yawVector } from "../src/engine/core/placement.js";
import { Frame } from "../src/engine/buildings/frame.js";
import { makeConfig } from "../src/engine/config/defaults.js";
import { PRESETS, presetConfig } from "../src/engine/config/presets.js";
import { GOLDEN_PRESETS } from "../scripts/lib/golden.js";

const gcd = (a, b) => (b ? gcd(b, a % b) : Math.abs(a));
/** Exact angle order of two directions (c, s) in [0°, 360°): integer tests only. */
const half = (c, s) => (s > 0 || (s === 0 && c > 0) ? 0 : 1);
const before = (a, b) => half(a.c, a.s) < half(b.c, b.s) || (half(a.c, a.s) === half(b.c, b.s) && a.c * b.s - a.s * b.c > 0);

test("yaw table: exactly the 16 primitive Pythagorean triples with r ≤ 100, all at least 8°, by angle", () => {
  const all = [];
  for (let m = 2; m * m < 100; m += 1)
    for (let n = 1; n < m; n += 1) {
      if ((m - n) % 2 === 0 || gcd(m, n) !== 1) continue;
      const a = m * m - n * n;
      const b = 2 * m * n;
      const r = m * m + n * n;
      if (r <= 100) all.push([Math.min(a, b), Math.max(a, b), r]);
    }
  assert.equal(all.length, 16);
  const key = (t) => t.join(",");
  assert.deepEqual(new Set(YAW_TRIPLES.map(key)), new Set(all.map(key)));
  for (const [p, q, r] of YAW_TRIPLES) {
    assert.equal(p * p + q * q, r * r);
    // tan 8° < p / q  (tan 8° = 0.14054...)
    assert.ok(p / q > 0.1405);
  }
  // sorted by angle: p1 / q1 < p2 / q2
  for (let k = 1; k < YAW_TRIPLES.length; k += 1) {
    const [p1, q1] = YAW_TRIPLES[k - 1];
    const [p2, q2] = YAW_TRIPLES[k];
    assert.ok(p1 * q2 < p2 * q1);
  }
});

test("yaws: 132 exact rotations in strict angular order, the proper rotations at every quarter", () => {
  assert.equal(YAWS.length, 132);
  assert.equal(YAW_QUARTER, 33);
  for (const y of YAWS) assert.equal(y.c * y.c + y.s * y.s, y.r * y.r);
  for (let k = 1; k < YAWS.length; k += 1) assert.ok(before(YAWS[k - 1], YAWS[k]), `yaw ${k}`);
  assert.deepEqual([...CARDINAL_YAWS], [0, 33, 66, 99]);
  assert.deepEqual(CARDINAL_YAWS.map((i) => [YAWS[i].c, YAWS[i].s, YAWS[i].r]), [[1, 0, 1], [0, 1, 1], [-1, 0, 1], [0, -1, 1]]);
  assert.deepEqual({ ...FRONT_YAW }, { N: 0, E: 33, S: 66, W: 99 });
  // every yaw of the first quadrant has its mirror (90° - θ) and its quarter turns
  for (let i = 1; i < 33; i += 1) {
    const a = YAWS[i];
    const m = YAWS[33 - i];
    assert.deepEqual([m.c, m.s, m.r], [a.s, a.c, a.r]);
    for (let k = 1; k < 4; k += 1) {
      const b = YAWS[i + 33 * k];
      let [c, s] = [a.c, a.s];
      for (let t = 0; t < k; t += 1) [c, s] = [-s, c];
      assert.deepEqual([b.c, b.s, b.r], [c, s, a.r]);
    }
  }
});

test("pitches: the (2n, n² - 1, n² + 1) family lands on the road classes' grade limits", () => {
  assert.equal(PITCHES.length, 7);
  assert.deepEqual([PITCHES[0].c, PITCHES[0].s, PITCHES[0].r], [1, 0, 1]);
  const grades = [0.08, 0.1, 0.12, 0.13, 0.16, 0.2];
  for (let k = 1; k < PITCHES.length; k += 1) {
    const p = PITCHES[k];
    assert.equal(p.c * p.c + p.s * p.s, p.r * p.r);
    assert.equal(gcd(gcd(p.c, p.s), p.r), 1);
    const grade = p.s / p.c;
    assert.ok(Math.abs(grade - 2 * p.n / (p.n * p.n - 1)) < 1e-15);
    assert.ok(grade >= grades[k - 1] - 0.001 && grade < grades[k - 1] + 0.015, `pitch ${k}: ${grade}`);
  }
});

test("orthonormality: cos² + sin² within 1 ulp of 1, and every composed rotation exactly orthogonal in integers", () => {
  const ulp = 2 ** -52;
  for (const y of [...YAWS, ...PITCHES]) {
    const c = y.c / y.r;
    const s = y.s / y.r;
    assert.ok(Math.abs(c * c + s * s - 1) <= ulp, `${y.c},${y.s},${y.r}`);
  }
  // Mᵀ M = D² I exactly: yaw x pitch x roll compose without any rounding
  for (let yaw = 0; yaw < YAWS.length; yaw += 1)
    for (let pitch = -6; pitch <= 6; pitch += 1)
      for (const roll of [0, 5, 29, 71]) {
        const { m, d } = rotationMatrix(yaw, pitch, roll);
        for (let i = 0; i < 3; i += 1)
          for (let j = 0; j < 3; j += 1) {
            const dot = m[i] * m[j] + m[3 + i] * m[3 + j] + m[6 + i] * m[6 + j] + 0;
            assert.equal(dot, i === j ? d * d : 0);
          }
        // proper rotation: determinant D³ (beyond 2^53: BigInt)
        const b = m.map(BigInt);
        const det = b[0] * (b[4] * b[8] - b[5] * b[7]) - b[1] * (b[3] * b[8] - b[5] * b[6]) + b[2] * (b[3] * b[7] - b[4] * b[6]);
        assert.equal(det, BigInt(d) ** 3n);
      }
});

test("exact integer round-trip: local cell -> scaled world -> local over an 801 x 801 lattice, every yaw", () => {
  let mismatches = 0;
  for (let yaw = 0; yaw < YAWS.length; yaw += 1) {
    const { m, d } = rotationMatrix(yaw, 0, 0);
    const d2 = d * d;
    for (let u = -400; u <= 400; u += 1)
      for (let v = -400; v <= 400; v += 1) {
        // world numerators over 2D of the cell centre, then back through Mᵀ
        const X = m[0] * (2 * u + 1) + m[1] * (2 * v + 1);
        const Y = m[3] * (2 * u + 1) + m[4] * (2 * v + 1);
        const bu = (m[0] * X + m[3] * Y) / d2;
        const bv = (m[1] * X + m[4] * Y) / d2;
        if (bu !== 2 * u + 1 || bv !== 2 * v + 1) mismatches += 1;
      }
  }
  assert.equal(mismatches, 0);
  // and in 3D through pitch and roll
  const p = new Placement({ origin: { x: 5, y: -7, z: 3 }, yaw: 17, pitch: -3, roll: 40 });
  for (let u = -12; u <= 12; u += 3)
    for (let v = -12; v <= 12; v += 3)
      for (let w = -12; w <= 12; w += 3) {
        const [X, Y, Z] = p.toWorldScaled(u, v, w);
        const { m, d } = p;
        assert.deepEqual([(m[0] * X + m[3] * Y + m[6] * Z) / (d * d), (m[1] * X + m[4] * Y + m[7] * Z) / (d * d), (m[2] * X + m[5] * Y + m[8] * Z) / (d * d)], [2 * u + 1, 2 * v + 1, 2 * w + 1]);
      }
});

test("rasterizer map: every world voxel lands in the local cell holding its centre (integer test, no tolerance)", () => {
  for (const yaw of [1, 7, 16, 20, 32, 45, 80, 131]) {
    const p = new Placement({ origin: { x: 13, y: -9, z: 0 }, yaw });
    const { m, d } = p;
    for (let x = -60; x <= 60; x += 1)
      for (let y = -60; y <= 60; y += 1) {
        const [u, v] = p.toLocalXY(x, y);
        // the centre's local coordinates over 2D lie in [2D u, 2D (u + 1))
        const a = m[0] * (2 * (x - 13) + 1) + m[3] * (2 * (y + 9) + 1);
        const b = m[1] * (2 * (x - 13) + 1) + m[4] * (2 * (y + 9) + 1);
        assert.ok(a >= 2 * d * u && a < 2 * d * (u + 1) && b >= 2 * d * v && b < 2 * d * (v + 1));
      }
  }
});

test("the proper rotations' fast path agrees with the general integer maps on every cell", () => {
  for (const q of [0, 1, 2, 3]) {
    const p = Placement.cardinal(40, -25, q);
    const { m, d } = rotationMatrix(q * YAW_QUARTER, 0, 0);
    assert.equal(d, 1);
    const o = p.origin;
    for (let x = -30; x <= 90; x += 1)
      for (let y = -80; y <= 20; y += 1) {
        const dx = 2 * (x - o.x) + 1;
        const dy = 2 * (y - o.y) + 1;
        const general = [Math.floor((m[0] * dx + m[3] * dy) / 2), Math.floor((m[1] * dx + m[4] * dy) / 2)];
        assert.deepEqual(p.toLocalXY(x, y), general);
        const [u, v] = general;
        const wx = o.x + Math.floor((m[0] * (2 * u + 1) + m[1] * (2 * v + 1)) / 2);
        const wy = o.y + Math.floor((m[3] * (2 * u + 1) + m[4] * (2 * v + 1)) / 2);
        assert.deepEqual(p.toWorldXY(u, v), [wx, wy]);
        assert.deepEqual([wx, wy], [x, y]);
      }
  }
});

/** The canonical frame as it was before core/placement.js: the reference for bit-identity. */
class ReferenceFrame {
  constructor(R, front) {
    this.R = { ...R };
    this.front = front;
  }
  toWorld(u, v) {
    const R = this.R;
    switch (this.front) {
      case "N":
        return [R.x0 + u, R.y0 + v];
      case "S":
        return [R.x1 - u, R.y1 - v];
      case "W":
        return [R.x0 + v, R.y1 - u];
      default:
        return [R.x1 - v, R.y0 + u];
    }
  }
  fromWorld(x, y) {
    const R = this.R;
    switch (this.front) {
      case "N":
        return [x - R.x0, y - R.y0];
      case "S":
        return [R.x1 - x, R.y1 - y];
      case "W":
        return [R.y1 - y, x - R.x0];
      default:
        return [y - R.y0, R.x1 - x];
    }
  }
  dirToWorld(du, dv) {
    switch (this.front) {
      case "N":
        return [du, dv];
      case "S":
        return [-du, -dv];
      case "W":
        return [dv, -du];
      default:
        return [-dv, du];
    }
  }
}

test("Frame delegates to Placement bit for bit: integers, fractions, signed zeros, every front", () => {
  const same = (a, b) => a.length === b.length && a.every((v, k) => Object.is(v, b[k]));
  const R = { x0: -37, y0: 1200, x1: 81, y1: 1263 };
  const values = [0, -0, 1, -1, 7, 63, 0.5, 1.4, 2.8000000000000003, -3.3, 117.9, 1e-9];
  // (a front other than N, S, E, W never made a frame, and still does not)
  for (const front of [undefined, "X"]) assert.throws(() => new Frame(R, front));
  for (const front of ["N", "S", "E", "W"]) {
    const f = new Frame(R, front);
    const g = new ReferenceFrame(R, front);
    for (const a of values)
      for (const b of values) {
        assert.ok(same(f.toWorld(a, b), g.toWorld(a, b)), `toWorld ${front} ${a},${b}`);
        assert.ok(same(f.fromWorld(R.x0 + a, R.y0 + b), g.fromWorld(R.x0 + a, R.y0 + b)), `fromWorld ${front}`);
        assert.ok(same(f.dirToWorld(a, b), g.dirToWorld(a, b)), `dirToWorld ${front} ${a},${b}`);
      }
  }
});

test("composed yaws: two table yaws' exact product, a primitive triple of its own, in integers; a product in the table is that yaw", () => {
  const gcd = (a, b) => (b ? gcd(b, a % b) : Math.abs(a));
  let composite = 0;
  for (let a = 0; a < YAWS.length; a += 1)
    for (let b = 1; b < YAWS.length; b += 1) {
      const A = YAWS[a];
      const B = YAWS[b];
      const { c, s, r } = yawProduct(a, b);
      // a rotation: c² + s² = r², reduced, r odd and its legs of opposite parity (no voxel centre on a cell face)
      assert.equal(c * c + s * s, r * r);
      assert.equal(gcd(gcd(c, s), r), 1);
      assert.ok(r % 2 === 1 && (Math.abs(c) + Math.abs(s)) % 2 === 1, `${a}+${b}: ${c}, ${s}, ${r}`);
      // the angles add, exactly: cos(α + β) = (Ac Bc - As Bs) / (Ar Br), sin likewise
      // (+ 0: the integers, whatever the sign of a zero)
      assert.equal(c * A.r * B.r + 0, (A.c * B.c - A.s * B.s) * r + 0);
      assert.equal(s * A.r * B.r + 0, (A.c * B.s + A.s * B.c) * r + 0);
      const p = new Placement({ origin: { x: 17, y: -5, z: 3 }, yaw: a, yaw2: b });
      const k = yawIndex({ c, s, r });
      if (k >= 0) {
        // (one name for every rotation: a product in the table is that yaw, with its fast path when proper)
        assert.ok(p.yaw === k && p.yaw2 === 0);
      } else {
        composite += 1;
        assert.ok(p.yaw === a && p.yaw2 === b && p.q === -1);
        assert.deepEqual(yawVector(a, b), { c, s, r });
      }
      assert.equal(p.d, r);
      assert.deepEqual(Array.from(p.m), [c, 0 - s, 0, s, c, 0, 0, 0, r]);
    }
  assert.ok(composite > 10000, `${composite}`);
  // 3-4-5 then 4-3-5 is a quarter turn: the proper rotation itself
  const q = new Placement({ yaw: yawIndex({ c: 4, s: 3, r: 5 }), yaw2: yawIndex({ c: 3, s: 4, r: 5 }) });
  assert.ok(q.yaw === YAW_QUARTER && q.yaw2 === 0 && q.q === 1);
  // a composite placement maps a world voxel to the local cell holding its centre, both ways consistently
  const p = new Placement({ origin: { x: 1234, y: -777, z: 10 }, yaw: 13, yaw2: 127 });
  for (let v = -20; v <= 20; v += 3)
    for (let u = -20; u <= 20; u += 3) {
      const [x, y, z] = p.toWorld(u, v, 5);
      const [lu, lv, lw] = p.toLocal(x, y, z);
      // (the voxel holding a cell's centre lies within a cell of it: its centre is half a voxel away at most)
      assert.ok(Math.abs(lu - u) <= 1 && Math.abs(lv - v) <= 1 && lw === 5);
    }
});

test("quaternions: bit-identical to the half-angle form, exact for the proper rotations, and consistent with the matrix", () => {
  assert.deepEqual(new Placement().toQuat(), { x: 0, y: 0, z: 0, w: 1 });
  assert.deepEqual(new Placement({ yaw: 33 }).toQuat(), { x: 0, y: 0, z: Math.SQRT1_2, w: Math.SQRT1_2 });
  assert.deepEqual(new Placement({ yaw: 66 }).toQuat(), { x: 0, y: 0, z: 1, w: 0 });
  assert.deepEqual(new Placement({ yaw: 99 }).toQuat(), { x: 0, y: 0, z: -Math.SQRT1_2, w: Math.SQRT1_2 });
  // a yaw's quaternion is (0, 0, sin θ/2, cos θ/2), each a single correctly rounded sqrt
  for (let i = 0; i < YAWS.length; i += 1) {
    const { c, s, r } = YAWS[i];
    const q = new Placement({ yaw: i }).toQuat();
    const w = Math.sqrt((r + c) / (2 * r));
    const z = Math.sqrt((r - c) / (2 * r));
    assert.ok(Object.is(q.w, w) && Object.is(q.z, s < 0 ? -z : z) && q.x === 0 && q.y === 0, `yaw ${i}`);
  }
  // the quaternion rotates like the integer matrix
  for (const [yaw, pitch, roll] of [[5, 2, 0], [40, -4, 11], [70, 6, 90], [101, -1, 130], [66, 0, 66], [0, 3, 33]]) {
    const p = new Placement({ yaw, pitch, roll });
    const { x, y, z, w } = p.toQuat();
    assert.ok(Math.abs(x * x + y * y + z * z + w * w - 1) < 4e-16);
    for (const v of [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0.3, -0.7, 0.2]]) {
      // q v q*
      const tx = 2 * (y * v[2] - z * v[1]);
      const ty = 2 * (z * v[0] - x * v[2]);
      const tz = 2 * (x * v[1] - y * v[0]);
      const rq = [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)];
      const rm = [0, 1, 2].map((a) => (p.m[a * 3] * v[0] + p.m[a * 3 + 1] * v[1] + p.m[a * 3 + 2] * v[2]) / p.d);
      for (let a = 0; a < 3; a += 1) assert.ok(Math.abs(rq[a] - rm[a]) < 1e-14, `${yaw},${pitch},${roll}`);
    }
  }
  // the float64 bits of every yaw x pitch quaternion: sqrt and division are
  // correctly rounded (IEEE 754), so this fingerprint holds on every platform
  const h = createHash("sha256");
  const f = new Float64Array(4);
  for (let yaw = 0; yaw < YAWS.length; yaw += 1)
    for (let pitch = -6; pitch <= 6; pitch += 1) {
      const q = matrixQuat(rotationMatrix(yaw, pitch, 0).m, rotationMatrix(yaw, pitch, 0).d);
      f.set([q.x, q.y, q.z, q.w]);
      h.update(new Uint8Array(f.buffer));
    }
  assert.equal(h.digest("hex").slice(0, 16), QUAT_FINGERPRINT);
});

/** Recorded once from the tables; any change of the tables or of the quaternion formula moves it. */
const QUAT_FINGERPRINT = "97211fe21b7285f4";

test("world boxes: the AABB of a rotated local box holds every voxel whose centre is inside, within a voxel", () => {
  for (const yaw of [0, 9, 33, 50, 66, 88, 99, 120]) {
    const p = new Placement({ origin: { x: -4, y: 11, z: 0 }, yaw });
    const e = { u0: -3, v0: 2, u1: 17, v1: 9 };
    const bb = p.localBoundsToWorldAABB(e);
    let x0 = Infinity;
    let y0 = Infinity;
    let x1 = -Infinity;
    let y1 = -Infinity;
    for (let x = -60; x <= 60; x += 1)
      for (let y = -50; y <= 70; y += 1) {
        const [u, v] = p.toLocalXY(x, y);
        if (u < e.u0 || u > e.u1 || v < e.v0 || v > e.v1) continue;
        x0 = Math.min(x0, x);
        y0 = Math.min(y0, y);
        x1 = Math.max(x1, x);
        y1 = Math.max(y1, y);
      }
    assert.ok(bb.x0 <= x0 && bb.y0 <= y0 && bb.x1 >= x1 && bb.y1 >= y1, `yaw ${yaw}: contains`);
    assert.ok(x0 - bb.x0 <= 1 && y0 - bb.y0 <= 1 && bb.x1 - x1 <= 1 && bb.y1 - y1 <= 1, `yaw ${yaw}: tight`);
    if (yaw % 33 === 0) assert.deepEqual([bb.x0, bb.y0, bb.x1, bb.y1], [x0, y0, x1, y1]);
  }
});

test("structvox SourceGrid: every local voxel lands where toWorld puts it (Game::far_mesh's arithmetic)", () => {
  // structvox: grid voxel p centred at origin + R h p; world voxel of a point X: floor(X / h + 0.5)
  for (const [yaw, pitch, roll] of [[0, 0, 0], [33, 0, 0], [66, 0, 0], [7, 0, 0], [52, 0, 0], [120, 0, 0], [20, 3, 0], [90, -2, 17]]) {
    const p = new Placement({ origin: { x: 1203, y: -877, z: 64 }, yaw, pitch, roll, priority: 2 });
    const g = p.toSourceGrid(9);
    assert.equal(g.id, 9);
    assert.equal(g.priority, 2);
    assert.equal(g.voxel_size, 0.125);
    const { x, y, z, w } = g.rot;
    const R = [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)];
    let ties = 0;
    for (let u = -20; u <= 20; u += 3)
      for (let v = -20; v <= 20; v += 3)
        for (let k = -6; k <= 6; k += 3) {
          const q = [u, v, k];
          const want = p.toWorld(u, v, k);
          for (let a = 0; a < 3; a += 1) {
            const X = g.origin[a] + 0.125 * (R[a * 3] * q[0] + R[a * 3 + 1] * q[1] + R[a * 3 + 2] * q[2]);
            const f = X / 0.125 + 0.5;
            // (a centre within float error of a world voxel's face: either side is right)
            if (Math.abs(f - Math.round(f)) < 1e-9) {
              ties += 1;
              continue;
            }
            assert.equal(Math.floor(f), want[a], `${yaw},${pitch},${roll} (${u},${v},${k}) axis ${a}`);
          }
        }
    assert.ok(ties < 100);
  }
});

test("nearest yaw: pure arithmetic, exact on the table's own directions", () => {
  assert.equal(nearestYaw(1, 0), 0);
  assert.equal(nearestYaw(0, 1), 33);
  assert.equal(nearestYaw(-1, 0), 66);
  assert.equal(nearestYaw(0, -1), 99);
  for (let i = 0; i < YAWS.length; i += 1) assert.equal(nearestYaw(YAWS[i].c, YAWS[i].s), i);
  assert.equal(nearestYaw(1, 0.2, CARDINAL_YAWS), 0);
});

test("angles are off by default and in every preset; off allows the proper rotations only", () => {
  const cfg = makeConfig();
  assert.equal(cfg.world.angles.enabled, false);
  assert.deepEqual([...allowedYaws(cfg)], [0, 33, 66, 99]);
  assert.equal(allowedYaws(makeConfig({ world: { angles: { enabled: true } } })).length, 132);
  assert.equal(allowedYaws(makeConfig({ world: { angles: { enabled: true, yawSet: "cardinal" } } })).length, 4);
  for (const id of GOLDEN_PRESETS) {
    const p = PRESETS.get(id);
    for (const s of p.sizes ?? [null]) assert.equal(makeConfig(presetConfig(p.id, { size: s?.id ?? null })).world.angles.enabled, false, p.id);
  }
});

test("no transcendental functions on the generation path of placements", () => {
  const src = readFileSync(new URL("../src/engine/core/placement.js", import.meta.url), "utf8")
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\/\/.*$/gm, "");
  assert.doesNotMatch(src, /Math\.(sin|cos|tan|asin|acos|atan|atan2|exp|expm1|log|log2|log10|log1p|pow|hypot|cbrt|sinh|cosh|tanh)\b/);
  assert.doesNotMatch(src, /\*\*/);
});

test("oriented rects: exact corners and bounds, distances in the rect's own axes, a fit against a slanted street", async () => {
  const { obbCorners, obbBounds, obbDistance, obbContains, fitLocalRect } = await import("../src/engine/core/obb.js");
  const p = new Placement({ origin: { x: 100, y: 200, z: 0 }, yaw: nearestYaw(4, 3) });
  const r = { x0: 0, y0: 0, x1: 79, y1: 39 };
  assert.deepEqual(obbCorners(p, r), [[100, 200], [164, 248], [140, 280], [76, 232]]);
  const b = obbBounds(p, r);
  const e = p.localBoundsToWorldAABB({ u0: 0, v0: 0, u1: 79, v1: 39 });
  assert.deepEqual(b, { x0: e.x0, y0: e.y0, x1: e.x1, y1: e.y1 });
  // every voxel of the rect (the placement's integer map) lies in its bounds, inside, at distance 0
  let n = 0;
  for (let y = b.y0 - 4; y <= b.y1 + 4; y += 1)
    for (let x = b.x0 - 4; x <= b.x1 + 4; x += 1) {
      const [u, v] = p.toLocalXY(x, y);
      const inside = u >= 0 && u <= 79 && v >= 0 && v <= 39;
      assert.equal(obbContains(p, r, x, y), inside);
      if (!inside) continue;
      n += 1;
      assert.ok(x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 && obbDistance(p, r, x, y) === 0);
    }
  assert.equal(n, 80 * 40);
  // 3-4-5: a voxel 10 cells in front of the front, in its own axes
  const [fx, fy] = [100 + 0.8 * 40 + 0.6 * 10 - 0.5, 200 + 0.6 * 40 - 0.8 * 10 - 0.5];
  assert.ok(Math.abs(obbDistance(p, r, fx, fy) - 10) < 1e-9);
  // a lot north-east of a diagonal street along (4, 3): the fit fronts the street, inside the lot
  const q = new Placement({ yaw: nearestYaw(-4, -3) });
  const lot = [[0, 0], [400, 0], [400, 300]];
  const f = fitLocalRect(q, lot, { depth: [40, 120], minWidth: 40, frontSlack: 3 });
  assert.ok(f && f.y1 - f.y0 + 1 === 120 && f.x1 - f.x0 + 1 >= 200, JSON.stringify(f));
  for (const [x, y] of obbCorners(q, f)) {
    assert.ok(y >= -1e-9 && x <= 400 + 1e-9, "inside the lot");
    assert.ok(y <= 0.75 * x + 1e-9, "behind the street line");
  }
  // the front edge on the street line
  const [c0, c1] = obbCorners(q, f);
  assert.ok(Math.abs(c0[1] - 0.75 * c0[0]) < 1.5 && Math.abs(c1[1] - 0.75 * c1[0]) < 1.5);
});
