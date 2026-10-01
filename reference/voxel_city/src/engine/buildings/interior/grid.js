import { YAWS } from "../../core/placement.js";
/**
 * FloorGrid: one floor plan as a label grid in the building's canonical
 * frame (u along the street facade, v from the front). One cell = one voxel
 * column (12.5 cm).
 *
 *   OUT  outside the footprint
 *   EXT  exterior wall (2 cells thick)
 *   WALL interior partition (1 cell thick, also the default "unassigned")
 *   DOOR carved opening in a wall
 *   16+  room index
 *
 * Rooms are painted as rects; everything interior that no room claims stays
 * WALL. Doors are found on real shared walls, so a planned layout is valid
 * only if the grid says two rooms really touch.
 */
export const OUT = 0;
export const EXT = 1;
export const WALL = 2;
export const DOOR = 3;
export const ROOM0 = 16;

export const EXT_T = 2;

/**
 * Extra width (cells) of every door of a turned building (the angled
 * world): a doorway in a turned wall must pass the viewer's 0.5 m square
 * (4 cells), which walks the world's axes, across the turn (4 (|cos| +
 * |sin| - 1) cells more), between jambs stepped by the world grid (one
 * cell). From the exact yaw triple: 2 cells at 8.8°-16°, 3 near 37°-53°.
 */
export function doorExtraOf(env) {
  if (!env.turn) return 0;
  const Y = YAWS[env.turn.yaw];
  return Math.ceil((4 * (Math.abs(Y.c) + Math.abs(Y.s) - Y.r)) / Y.r) + 1;
}

export class FloorGrid {
  /** `cut(u, v)`: cells of the footprint's rects that are outside nonetheless (a chamfered corner, buildings/chamfer.js). */
  constructor(U, V, footprint, cut = null) {
    this.U = U;
    this.V = V;
    // (a turned building's doors are wider, doorExtraOf)
    this.doorExtra = 0;
    this.cells = new Uint16Array(U * V);
    this.rooms = [];
    this.doors = [];
    this.footprint = footprint.map((r) => ({ ...r }));
    for (const r of footprint) this.fill(r, WALL);
    // (the exterior wall follows a cut: its cells are outside before the ring is found; rooms keep off them)
    this.cut = cut;
    if (cut) for (let v = 0; v < V; v += 1) for (let u = 0; u < U; u += 1) if (cut(u, v)) this.cells[u + v * U] = OUT;
    // exterior wall ring: interior cells within EXT_T of the outside
    const c = this.cells;
    const ext = [];
    for (let v = 0; v < V; v += 1) {
      for (let u = 0; u < U; u += 1) {
        if (c[u + v * U] === OUT) continue;
        let edge = false;
        for (let dv = -EXT_T; dv <= EXT_T && !edge; dv += 1) {
          for (let du = -EXT_T; du <= EXT_T; du += 1) {
            if (this.get(u + du, v + dv) === OUT) {
              edge = true;
              break;
            }
          }
        }
        if (edge) ext.push(u + v * U);
      }
    }
    for (const k of ext) c[k] = EXT;
  }

  get(u, v) {
    if (u < 0 || v < 0 || u >= this.U || v >= this.V) return OUT;
    return this.cells[u + v * this.U];
  }

  set(u, v, val) {
    if (u < 0 || v < 0 || u >= this.U || v >= this.V) return;
    this.cells[u + v * this.U] = val;
  }

  fill(r, val) {
    const x0 = Math.max(0, r.x0);
    const y0 = Math.max(0, r.y0);
    const x1 = Math.min(this.U - 1, r.x1);
    const y1 = Math.min(this.V - 1, r.y1);
    for (let v = y0; v <= y1; v += 1) this.cells.fill(val, x0 + v * this.U, x1 + v * this.U + 1);
  }

  /** Interior rects of the footprint (inset by the exterior wall). */
  innerRects() {
    return this.footprint.map((r) => ({ x0: r.x0 + EXT_T, y0: r.y0 + EXT_T, x1: r.x1 - EXT_T, y1: r.y1 - EXT_T }));
  }

  /** Is the rect entirely interior (WALL-labelled, unclaimed) cells? */
  isFree(r) {
    if (r.x0 < 0 || r.y0 < 0 || r.x1 >= this.U || r.y1 >= this.V || r.x1 < r.x0 || r.y1 < r.y0) return false;
    for (let v = r.y0; v <= r.y1; v += 1) {
      for (let u = r.x0; u <= r.x1; u += 1) if (this.cells[u + v * this.U] !== WALL) return false;
    }
    return true;
  }

  addRoom(type, rects, props = {}) {
    const id = this.rooms.length;
    const valid = rects.filter((r) => r.x1 >= r.x0 && r.y1 >= r.y0);
    const room = { id, type, rects: valid.map((r) => ({ ...r })), ...props };
    this.rooms.push(room);
    for (const r of valid) {
      if (this.cut) this.paintInside(r, ROOM0 + id);
      else this.fill(r, ROOM0 + id);
    }
    return room;
  }

  /** Label a rect's cells that are inside the walls (a cut floor: its rooms stop at the exterior wall along the cut). */
  paintInside(r, val) {
    for (let v = Math.max(0, r.y0); v <= Math.min(this.V - 1, r.y1); v += 1)
      for (let u = Math.max(0, r.x0); u <= Math.min(this.U - 1, r.x1); u += 1) {
        const k = u + v * this.U;
        if (this.cells[k] !== OUT && this.cells[k] !== EXT) this.cells[k] = val;
      }
  }

  /** Grow a room by painting more rects (only over free interior cells). */
  extendRoom(room, rect) {
    if (!this.isFree(rect)) return false;
    room.rects.push({ ...rect });
    this.fill(rect, ROOM0 + room.id);
    return true;
  }

  roomAt(u, v) {
    const l = this.get(u, v);
    return l >= ROOM0 ? this.rooms[l - ROOM0] : null;
  }

  area(room) {
    let a = 0;
    for (const r of room.rects) a += (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
    return a;
  }

  snapshot() {
    return { cells: this.cells.slice(), rooms: this.rooms.length, doors: this.doors.length };
  }

  restore(s) {
    this.cells.set(s.cells);
    this.rooms.length = s.rooms;
    this.doors.length = s.doors;
  }

  /**
   * Straight wall runs separating room `a` from room `b` (b = null: outside).
   * Returns [{orient:'v'|'h', fixed, t0, t1, thick, sideA}] where for 'v'
   * the wall occupies columns fixed..fixed+thick-1 (a on the low side if
   * sideA==='low'), running along v from t0..t1.
   */
  wallRuns(a, b) {
    const la = ROOM0 + a.id;
    const lb = b ? ROOM0 + b.id : OUT;
    const thick = b ? 1 : EXT_T;
    const wallLab = b ? WALL : EXT;
    const runs = [];
    const push = (orient, fixed, t, sideA) => {
      const last = runs[runs.length - 1];
      if (last && last.orient === orient && last.fixed === fixed && last.sideA === sideA && last.t1 === t - 1) last.t1 = t;
      else runs.push({ orient, fixed, t0: t, t1: t, thick, sideA });
    };
    const isWall = (u, v) => this.get(u, v) === wallLab;
    for (const r of a.rects) {
      // east side
      for (let v = r.y0; v <= r.y1; v += 1) {
        if (this.get(r.x1, v) !== la) continue;
        let ok = true;
        for (let k = 1; k <= thick; k += 1) if (!isWall(r.x1 + k, v)) ok = false;
        if (ok && this.get(r.x1 + thick + 1, v) === lb) push("v", r.x1 + 1, v, "low");
      }
      // west side
      for (let v = r.y0; v <= r.y1; v += 1) {
        if (this.get(r.x0, v) !== la) continue;
        let ok = true;
        for (let k = 1; k <= thick; k += 1) if (!isWall(r.x0 - k, v)) ok = false;
        if (ok && this.get(r.x0 - thick - 1, v) === lb) push("v", r.x0 - thick, v, "high");
      }
      // south side (+v)
      for (let u = r.x0; u <= r.x1; u += 1) {
        if (this.get(u, r.y1) !== la) continue;
        let ok = true;
        for (let k = 1; k <= thick; k += 1) if (!isWall(u, r.y1 + k)) ok = false;
        if (ok && this.get(u, r.y1 + thick + 1) === lb) push("h", r.y1 + 1, u, "low");
      }
      // north side (-v)
      for (let u = r.x0; u <= r.x1; u += 1) {
        if (this.get(u, r.y0) !== la) continue;
        let ok = true;
        for (let k = 1; k <= thick; k += 1) if (!isWall(u, r.y0 - k)) ok = false;
        if (ok && this.get(u, r.y0 - thick - 1) === lb) push("h", r.y0 - thick, u, "high");
      }
    }
    return runs;
  }

  /**
   * Carve a door between rooms a and b (b null = exterior). Options:
   *   width   opening width in cells (default 7 = 0.875 m)
   *   margin  min distance from wall corners (default 2)
   *   place   'center' | 'start' | 'end' | 'near' | 'auto'
   *   near    {u, v} target point when place === 'near'
   *   kind    semantic door kind for rendering (interior, entry, entrance, stair, closet, opening)
   * Returns the door or null if no wall run is long enough.
   */
  addDoor(a, b, opts = {}) {
    if (!b && (opts.place ?? "auto") !== "near") {
      // exterior doors face the street unless told otherwise
      const r = a.rects[0];
      opts = { ...opts, place: "near", near: { u: (r.x0 + r.x1) / 2, v: -12 } };
    }
    const width = (opts.width ?? 7) + this.doorExtra;
    const margin = opts.margin ?? 2;
    const runs = this.wallRuns(a, b).filter((r) => r.t1 - r.t0 + 1 >= width + 2 * margin);
    if (!runs.length) return null;
    let best = null;
    let bestScore = Infinity;
    for (const run of runs) {
      const lo = run.t0 + margin;
      const hi = run.t1 - margin - width + 1;
      const candidates = [];
      const place = opts.place ?? "auto";
      if (place === "center" || place === "auto") candidates.push(Math.round((lo + hi) / 2));
      if (place === "start" || place === "auto") candidates.push(lo);
      if (place === "end" || place === "auto") candidates.push(hi);
      if (place === "near") {
        const target = run.orient === "v" ? opts.near.v : opts.near.u;
        candidates.push(Math.max(lo, Math.min(hi, Math.round(target - width / 2))));
      }
      for (const t of candidates) {
        let score = 0;
        if (place === "near") {
          const cu = run.orient === "v" ? run.fixed : t + width / 2;
          const cv = run.orient === "v" ? t + width / 2 : run.fixed;
          score = Math.abs(cu - opts.near.u) + Math.abs(cv - opts.near.v);
        } else if (place === "auto") {
          // prefer near a corner (leaves wall for furniture), long runs
          score = Math.min(t - lo, hi - t) - (run.t1 - run.t0) * 0.01;
        }
        // avoid crowding existing doors
        for (const d of this.doors) {
          const du = Math.abs((d.u0 + d.u1) / 2 - (run.orient === "v" ? run.fixed : t + width / 2));
          const dv = Math.abs((d.v0 + d.v1) / 2 - (run.orient === "v" ? t + width / 2 : run.fixed));
          if (du + dv < width + 4) score += 50;
        }
        if (score < bestScore) {
          bestScore = score;
          best = { run, t };
        }
      }
    }
    if (!best) return null;
    const { run, t } = best;
    let door;
    if (run.orient === "v") {
      door = { u0: run.fixed, u1: run.fixed + run.thick - 1, v0: t, v1: t + width - 1, orient: "v" };
    } else {
      door = { u0: t, u1: t + width - 1, v0: run.fixed, v1: run.fixed + run.thick - 1, orient: "h" };
    }
    door.a = a.id;
    door.b = b ? b.id : -1;
    door.kind = opts.kind ?? (b ? "interior" : "entrance");
    door.sideA = run.sideA;
    door.width = width;
    door.leaf = opts.leaf ?? (door.kind === "opening" ? "none" : "wood");
    if (opts.height) door.height = opts.height;
    door.id = this.doors.length;
    for (let v = door.v0; v <= door.v1; v += 1) for (let u = door.u0; u <= door.u1; u += 1) this.set(u, v, DOOR);
    this.doors.push(door);
    return door;
  }

  /**
   * Where an open door leaf rests: flat against the wall next to the opening
   * on the hinge side, inside room a (or inside for exterior doors). Returns
   * a canonical rect (1 cell thick) or null when there is no free wall face.
   */
  doorLeafRect(d) {
    if (d.leaf === "none" || d.kind === "opening" || d.kind === "elevator" || d.leaf === "rollup") return null;
    const len = d.width - 1;
    const roomLab = d.b === -1 ? ROOM0 + d.a : ROOM0 + d.a;
    const tries = (d.id & 1) === 0 ? [0, 1] : [1, 0];
    for (const which of tries) {
      let r;
      if (d.orient === "h") {
        const low = this.get(d.u0, d.v0 - 1) === roomLab;
        const vFace = low ? d.v0 - 1 : d.v1 + 1;
        r = which === 0 ? { x0: d.u0 - len, x1: d.u0 - 1, y0: vFace, y1: vFace } : { x0: d.u1 + 1, x1: d.u1 + len, y0: vFace, y1: vFace };
      } else {
        const low = this.get(d.u0 - 1, d.v0) === roomLab;
        const uFace = low ? d.u0 - 1 : d.u1 + 1;
        r = which === 0 ? { x0: uFace, x1: uFace, y0: d.v0 - len, y1: d.v0 - 1 } : { x0: uFace, x1: uFace, y0: d.v1 + 1, y1: d.v1 + len };
      }
      let ok = true;
      for (let v = r.y0; v <= r.y1 && ok; v += 1) for (let u = r.x0; u <= r.x1 && ok; u += 1) if (this.get(u, v) !== roomLab) ok = false;
      if (ok) return r;
    }
    return null;
  }

  /** Room pairs connected by a door (adjacency list incl. -1 = outside). */
  doorGraph() {
    const adj = new Map();
    const add = (x, y) => {
      if (!adj.has(x)) adj.set(x, new Set());
      adj.get(x).add(y);
    };
    for (const d of this.doors) {
      add(d.a, d.b);
      add(d.b, d.a);
    }
    return adj;
  }
}
