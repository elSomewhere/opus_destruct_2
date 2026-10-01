import { Placement, YAWS, yawVector } from "../core/placement.js";
import { obbBounds, worldPointToLocal } from "../core/obb.js";

/**
 * Canonical building frame. Every building/interior algorithm works in a
 * rotated frame where:
 *   u runs along the street frontage, v runs from the FRONT (v = 0) to the back
 *   sides: F (front, v=0), B (back), L (u=0), R (u=U-1)
 * Frames are proper rotations of the world grid (no mirroring), so stairs,
 * hinges and furniture keep their handedness.
 *
 * Canonical rects use the same {x0,y0,x1,y1} shape as world rects with x=u,
 * y=v, so all rect utilities work in either space.
 *
 * A frame is a thin wrapper over an axis-aligned Placement
 * (core/placement.js): the front picks the quarter turn (N 0°, E 90°,
 * S 180°, W 270°) and the pivot is the world cell of canonical cell (0, 0).
 *
 * A turned frame (TurnedFrame, the angled world's turned buildings,
 * ANGLED_WORLD_PLAN.md S3) is the same over a placement with any yaw of the
 * table: canonical cell (u, v) is its local cell (u + ou, v + ov), so a lot
 * and the building on it share one placement and differ by an integer
 * offset. Build frames through `lotFrameOf` / `frameOf`, which carry the
 * turn (`lot.turn`, `env.turn`); both give the plain frame otherwise.
 */

const SIDE_MAP = {
  N: { F: "N", B: "S", L: "W", R: "E" },
  S: { F: "S", B: "N", L: "E", R: "W" },
  W: { F: "W", B: "E", L: "S", R: "N" },
  E: { F: "E", B: "W", L: "N", R: "S" },
};

/** Quarter turns of a front (anything else turns like E, as it always did). */
const QUARTER = { N: 0, E: 1, S: 2, W: 3 };

export class Frame {
  /** @param R world rect (inclusive) @param front world side of the street facade */
  constructor(R, front) {
    this.R = { ...R };
    this.front = front;
    const w = R.x1 - R.x0 + 1;
    const h = R.y1 - R.y0 + 1;
    if (front === "N" || front === "S") {
      this.U = w;
      this.V = h;
    } else {
      this.U = h;
      this.V = w;
    }
    this.sideMap = SIDE_MAP[front];
    this.inverseSide = Object.fromEntries(Object.entries(this.sideMap).map(([k, v]) => [v, k]));
    const q = QUARTER[front] ?? 1;
    this.placement = Placement.cardinal(q === 0 || q === 3 ? R.x0 : R.x1, q === 0 || q === 1 ? R.y0 : R.y1, q, { extent: { u0: 0, v0: 0, u1: this.U - 1, v1: this.V - 1 } });
  }

  /** canonical cell -> world cell */
  toWorld(u, v) {
    return this.placement.toWorldXY(u, v);
  }

  /** world cell -> canonical cell */
  fromWorld(x, y) {
    return this.placement.toLocalXY(x, y);
  }

  rectToWorld(r) {
    const [ax, ay] = this.toWorld(r.x0, r.y0);
    const [bx, by] = this.toWorld(r.x1, r.y1);
    return { x0: Math.min(ax, bx), y0: Math.min(ay, by), x1: Math.max(ax, bx), y1: Math.max(ay, by) };
  }

  rectFromWorld(r) {
    const [ax, ay] = this.fromWorld(r.x0, r.y0);
    const [bx, by] = this.fromWorld(r.x1, r.y1);
    return { x0: Math.min(ax, bx), y0: Math.min(ay, by), x1: Math.max(ax, bx), y1: Math.max(ay, by) };
  }

  /** canonical side (F/B/L/R) -> world side (N/S/E/W) */
  worldSide(cs) {
    return this.sideMap[cs];
  }

  canonSide(ws) {
    return this.inverseSide[ws];
  }

  /** canonical direction vector (du, dv) -> world (dx, dy) */
  dirToWorld(du, dv) {
    return this.placement.dirToWorldXY(du, dv);
  }
}

/** Canonical side vectors (outward normal). */
export const CSIDE_DIR = { F: [0, -1], B: [0, 1], L: [-1, 0], R: [1, 0] };
export const COPP = { F: "B", B: "F", L: "R", R: "L" };

/**
 * A turned frame: canonical (u, v) = local cell (u + ou, v + ov) of a
 * placement with a yaw (no pitch or roll), U x V cells. Membership of a
 * world voxel is the placement's exact integer map (`fromWorld`); a
 * canonical rect is no world rect any more, so `rectToWorld` gives the
 * world bounds of every voxel it holds, for culling and indexing only
 * (what draws it tests each voxel with `fromWorld`).
 */
export class TurnedFrame extends Frame {
  /** `front`: the nominal world side the facade faces (the nearest of N/E/S/W), for coarse decisions only. */
  constructor(placement, ou, ov, U, V, front) {
    const b = obbBounds(placement, { x0: ou, y0: ov, x1: ou + U - 1, y1: ov + V - 1 });
    super(b, front);
    this.U = U;
    this.V = V;
    this.placement = placement;
    this.ou = ou;
    this.ov = ov;
    this.turned = true;
  }

  /** The same placement with the canonical origin at this frame's (du, dv), U x V cells. */
  shifted(du, dv, U, V) {
    return new TurnedFrame(this.placement, this.ou + du, this.ov + dv, U, V, this.front);
  }

  /** The turn to record on a lot or an envelope: { yaw, origin, ou, ov }. */
  get turn() {
    const p = this.placement;
    return { yaw: p.yaw, ...(p.yaw2 ? { yaw2: p.yaw2 } : {}), origin: { x: p.origin.x, y: p.origin.y }, ou: this.ou, ov: this.ov };
  }

  toWorld(u, v) {
    return this.placement.toWorldXY(u + this.ou, v + this.ov);
  }

  fromWorld(x, y) {
    const [u, v] = this.placement.toLocalXY(x, y);
    return [u - this.ou, v - this.ov];
  }

  /** Canonical continuous point (u, v) -> world continuous point [x, y] (exact rationals). */
  pointToWorld(u, v) {
    const p = this.placement;
    const lu = u + this.ou;
    const lv = v + this.ov;
    return [p.origin.x + (p.m[0] * lu + p.m[1] * lv) / p.d, p.origin.y + (p.m[3] * lu + p.m[4] * lv) / p.d];
  }

  /** World continuous point -> canonical continuous point. */
  pointFromWorld(x, y) {
    const [u, v] = worldPointToLocal(this.placement, x, y);
    return [u - this.ou, v - this.ov];
  }

  /** World bounds (inclusive voxels) of every voxel a canonical rect holds (fractional rects: the cells they take in). */
  rectToWorld(r) {
    return obbBounds(this.placement, { x0: Math.ceil(r.x0) + this.ou, y0: Math.ceil(r.y0) + this.ov, x1: Math.floor(r.x1) + this.ou, y1: Math.floor(r.y1) + this.ov });
  }

  /** Canonical bounds of a world rect (its corners' cells): conservative. */
  rectFromWorld(r) {
    let x0 = Infinity;
    let y0 = Infinity;
    let x1 = -Infinity;
    let y1 = -Infinity;
    for (const [x, y] of [[r.x0, r.y0], [r.x1, r.y0], [r.x0, r.y1], [r.x1, r.y1]]) {
      const [u, v] = this.fromWorld(x, y);
      x0 = Math.min(x0, u);
      y0 = Math.min(y0, v);
      x1 = Math.max(x1, u);
      y1 = Math.max(y1, v);
    }
    return { x0, y0, x1, y1 };
  }

  /** Canonical direction -> world direction (unit for unit, exact rationals). */
  dirToWorld(du, dv) {
    return this.placement.dirToWorldXY(du, dv);
  }

  /**
   * Distance (voxels) from world voxel (x, y) to a canonical rect, between
   * cell centres as grading measures world rects (0 inside), in the frame's
   * own axes.
   */
  distance(r, x, y) {
    const [u, v] = this.pointFromWorld(x + 0.5, y + 0.5);
    const du = Math.max(r.x0 + 0.5 - u, 0, u - (r.x1 + 0.5));
    const dv = Math.max(r.y0 + 0.5 - v, 0, v - (r.y1 + 0.5));
    return Math.sqrt(du * du + dv * dv);
  }
}

/** The nearest proper front (N/E/S/W) to a turned front of yaw index `yaw` (followed by `yaw2`). */
export function nominalFront(yaw, yaw2 = 0) {
  if (yaw2) {
    // (the nearest axis to the product's direction: never a tie, no yaw is 45°)
    const { c, s } = yawVector(yaw, yaw2);
    return Math.abs(c) > Math.abs(s) ? (c > 0 ? "N" : "S") : s > 0 ? "E" : "W";
  }
  const q = Math.round(yaw / (YAWS.length / 4)) % 4;
  return ["N", "E", "S", "W"][q];
}

/** A turned frame from a recorded turn { yaw, yaw2?, origin, ou, ov }, U x V cells. */
export function turnedFrame(turn, U, V, front = nominalFront(turn.yaw, turn.yaw2)) {
  return new TurnedFrame(new Placement({ origin: { x: turn.origin.x, y: turn.origin.y, z: 0 }, yaw: turn.yaw, yaw2: turn.yaw2 ?? 0 }), turn.ou, turn.ov, U, V, front);
}

/** A lot's frame: turned when the lot is (`lot.turn`, with its U x V), else the plain frame of its rect. */
export function lotFrameOf(lot) {
  return lot.turn ? turnedFrame(lot.turn, lot.turn.U, lot.turn.V, lot.front) : new Frame(lot.rect, lot.front);
}

const frames = new WeakMap();

/** A building's frame (memoized): turned when the envelope is (`env.turn`), else the plain frame of env.R. */
export function frameOf(env) {
  let f = frames.get(env);
  if (!f) {
    f = env.turn ? turnedFrame(env.turn, env.U, env.V, env.front) : new Frame(env.R, env.front);
    frames.set(env, f);
  }
  return f;
}
