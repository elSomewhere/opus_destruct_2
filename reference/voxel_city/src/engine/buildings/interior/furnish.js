import { PREFABS, carBoxes } from "./prefabs.js";
import { CIVIC_PREFABS } from "./civicPrefabs.js";
import { makeCivicRules, RULE_PREFABS } from "./civicRules.js";
import { ROOM0, EXT, DOOR, OUT } from "./grid.js";
import { MAT } from "../../voxel/materials.js";

/**
 * Room furnishing. For each room a small occupancy grid is kept; doors
 * reserve a clearance zone in front of them, tall furniture avoids
 * exterior walls (windows) and walls with doors, and items are placed by
 * room-type rules (living: sofa facing a TV with a rug and coffee table
 * between; bedroom: bed centred on a wall with nightstands; kitchen:
 * counter run with sink/stove and a fridge; offices: desk clusters...).
 *
 * Output: canonical boxes with absolute z.
 */

const SIDES = ["N", "E", "S", "W"];

class RoomCtx {
  constructor(grid, room, rect, zf, rng, cars = false) {
    this.cars = cars;
    this.grid = grid;
    this.room = room;
    this.r = rect;
    this.zf = zf;
    this.rng = rng;
    this.W = rect.x1 - rect.x0 + 1;
    this.H = rect.y1 - rect.y0 + 1;
    this.occ = new Uint8Array(this.W * this.H);
    this.boxes = [];
    this.placed = [];
    this.reserveDoors();
    this.markLeaves();
  }

  sideLen(side) {
    return side === "N" || side === "S" ? this.W : this.H;
  }

  sideDepth(side) {
    return side === "N" || side === "S" ? this.H : this.W;
  }

  /** local (side, a, b) -> canonical cell */
  cell(side, a, b) {
    const r = this.r;
    switch (side) {
      case "N":
        return [r.x0 + a, r.y0 + b];
      case "S":
        return [r.x1 - a, r.y1 - b];
      case "W":
        return [r.x0 + b, r.y1 - a];
      default:
        return [r.x1 - b, r.y0 + a];
    }
  }

  inRect(u, v) {
    return u >= this.r.x0 && u <= this.r.x1 && v >= this.r.y0 && v <= this.r.y1;
  }

  isOcc(u, v) {
    if (!this.inRect(u, v)) return true;
    if (this.grid.get(u, v) !== ROOM0 + this.room.id) return true;
    return this.occ[u - this.r.x0 + (v - this.r.y0) * this.W] !== 0;
  }

  mark(u0, v0, u1, v1, val = 1) {
    for (let v = Math.max(v0, this.r.y0); v <= Math.min(v1, this.r.y1); v += 1)
      for (let u = Math.max(u0, this.r.x0); u <= Math.min(u1, this.r.x1); u += 1) this.occ[u - this.r.x0 + (v - this.r.y0) * this.W] = val;
  }

  reserveDoors() {
    const g = this.grid;
    const lab = ROOM0 + this.room.id;
    this.doorFaces = [];
    for (const d of g.doors) {
      if (d.a === this.room.id || d.b === this.room.id) {
        // room cells directly inside the opening
        const face = [];
        if (d.orient === "h") {
          for (const v of [d.v0 - 1, d.v1 + 1]) for (let u = d.u0; u <= d.u1; u += 1) if (g.get(u, v) === lab && this.inRect(u, v)) face.push([u, v]);
        } else {
          for (const u of [d.u0 - 1, d.u1 + 1]) for (let v = d.v0; v <= d.v1; v += 1) if (g.get(u, v) === lab && this.inRect(u, v)) face.push([u, v]);
        }
        if (face.length) this.doorFaces.push(face);
      }
      if (d.a !== this.room.id && d.b !== this.room.id) continue;
      const leaf = g.doorLeafRect(d);
      if (leaf) {
        // keep a margin free around the open leaf; the leaf itself is an obstacle
        this.mark(leaf.x0 - 1, leaf.y0 - 1, leaf.x1 + 1, leaf.y1 + 1, 2);
        this.leaves = this.leaves ?? [];
        this.leaves.push(leaf);
      }
      const w = d.width;
      // (a door raised to its street keeps its steps down free too)
      const depth = Math.max(8, w + 2, 2 * (d.sill ?? 0) + 4);
      if (d.orient === "h") {
        // room on low side (v < v0) or high side (v > v1)?
        if (g.get(d.u0, d.v0 - 1) === lab) this.mark(d.u0 - 2, d.v0 - depth, d.u1 + 2, d.v0 - 1, 2);
        if (g.get(d.u0, d.v1 + 1) === lab) this.mark(d.u0 - 2, d.v1 + 1, d.u1 + 2, d.v1 + depth, 2);
      } else {
        if (g.get(d.u0 - 1, d.v0) === lab) this.mark(d.u0 - depth, d.v0 - 2, d.u0 - 1, d.v1 + 2, 2);
        if (g.get(d.u1 + 1, d.v0) === lab) this.mark(d.u1 + 1, d.v0 - 2, d.u1 + depth, d.v1 + 2, 2);
      }
    }
  }

  markLeaves() {
    for (const l of this.leaves ?? []) this.mark(l.x0, l.y0, l.x1, l.y1, 1);
    this.reserveJunctions();
  }

  /**
   * A turned building (the angled world: doorExtra > 0): where this rect
   * meets another rect of the same room, a way through as wide as its doors
   * (the walker keeps to the world's axes, so the canonical gap must be
   * wider), kept free and joined to the doors like one of them.
   */
  reserveJunctions() {
    const extra = this.grid.doorExtra ?? 0;
    if (!extra || this.room.rects.length < 2) return;
    const r = this.r;
    const depth = 4 + extra;
    for (const o of this.room.rects) {
      if (o === r) continue;
      const face = [];
      const u0 = Math.max(r.x0, o.x0);
      const u1 = Math.min(r.x1, o.x1);
      const v0 = Math.max(r.y0, o.y0);
      const v1 = Math.min(r.y1, o.y1);
      if (u0 <= u1 && (o.y1 + 1 === r.y0 || o.y0 - 1 === r.y1)) {
        const v = o.y1 + 1 === r.y0 ? r.y0 : r.y1;
        for (let u = u0; u <= u1; u += 1) face.push([u, v]);
        this.mark(u0, v === r.y0 ? v : v - depth + 1, u1, v === r.y0 ? v + depth - 1 : v, 2);
      } else if (v0 <= v1 && (o.x1 + 1 === r.x0 || o.x0 - 1 === r.x1)) {
        const u = o.x1 + 1 === r.x0 ? r.x0 : r.x1;
        for (let v = v0; v <= v1; v += 1) face.push([u, v]);
        this.mark(u === r.x0 ? u : u - depth + 1, v0, u === r.x0 ? u + depth - 1 : u, v1, 2);
      }
      if (face.length) this.doorFaces.push(face);
    }
  }

  /**
   * Can a 0.5 m walker still get between every pair of doors of this room?
   * (In a turned building the walker keeps to the world's axes: across the
   * turn it needs a wider square of the canonical grid, 4 + doorExtra.)
   */
  doorsConnected() {
    if (this.doorFaces.length < 2) return this.doorFaces.length === 0 || this.faceReachable(this.doorFaces[0]);
    const W = this.W;
    const H = this.H;
    const lab = ROOM0 + this.room.id;
    const S = 4 + (this.grid.doorExtra ?? 0);
    const free = new Uint8Array(W * H);
    for (let y = 0; y < H; y += 1)
      for (let x = 0; x < W; x += 1) free[x + y * W] = this.occ[x + y * W] !== 1 && this.grid.get(this.r.x0 + x, this.r.y0 + y) === lab ? 1 : 0;
    const fit = new Uint8Array(W * H);
    for (let y = 0; y + S - 1 < H; y += 1)
      for (let x = 0; x + S - 1 < W; x += 1) {
        let ok = 1;
        for (let dy = 0; dy < S && ok; dy += 1) for (let dx = 0; dx < S; dx += 1) if (!free[x + dx + (y + dy) * W]) { ok = 0; break; }
        fit[x + y * W] = ok;
      }
    const touches = (face) => {
      const out = [];
      for (const [u, v] of face) {
        const cx = u - this.r.x0;
        const cy = v - this.r.y0;
        for (let dy = 1 - S; dy <= 0; dy += 1)
          for (let dx = 1 - S; dx <= 0; dx += 1) {
            const x = cx + dx;
            const y = cy + dy;
            if (x >= 0 && y >= 0 && x < W && y < H && fit[x + y * W]) out.push(x + y * W);
          }
      }
      return out;
    };
    const seen = new Uint8Array(W * H);
    const q = touches(this.doorFaces[0]);
    if (!q.length) return false;
    for (const k of q) seen[k] = 1;
    for (let h = 0; h < q.length; h += 1) {
      const k = q[h];
      const x = k % W;
      const y = (k - x) / W;
      for (const [nx, ny] of [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]]) {
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        const nk = nx + ny * W;
        if (fit[nk] && !seen[nk]) {
          seen[nk] = 1;
          q.push(nk);
        }
      }
    }
    for (let f = 1; f < this.doorFaces.length; f += 1) if (!touches(this.doorFaces[f]).some((k) => seen[k])) return false;
    return true;
  }

  faceReachable(face) {
    void face;
    return true;
  }

  save() {
    return { occ: this.occ.slice(), n: this.boxes.length, p: this.placed.length };
  }

  undo(s) {
    this.occ.set(s.occ);
    this.boxes.length = s.n;
    this.placed.length = s.p;
  }

  /** What's behind the wall at local a on `side`: 'ext' | 'door' | 'wall' | 'open'. */
  wallBehind(side, a) {
    const [u, v] = this.cell(side, a, 0);
    const [du, dv] = side === "N" ? [0, -1] : side === "S" ? [0, 1] : side === "W" ? [-1, 0] : [1, 0];
    const l = this.grid.get(u + du, v + dv);
    if (l === EXT) return "ext";
    if (l === DOOR) return "door";
    if (l === OUT) return "ext";
    if (l >= ROOM0) return "open";
    return "wall";
  }

  wallOk(side, a0, a1, tall) {
    for (let a = a0 - 1; a <= a1 + 1; a += 1) {
      if (a < 0 || a >= this.sideLen(side)) continue;
      const w = this.wallBehind(side, a);
      if (w === "door" || w === "open") return false;
      if (tall && w === "ext" && a >= a0 && a <= a1) return false;
    }
    return true;
  }

  footprintFree(side, a0, b0, w, d) {
    for (let b = b0; b < b0 + d; b += 1)
      for (let a = a0; a < a0 + w; a += 1) {
        const [u, v] = this.cell(side, a, b);
        if (this.isOcc(u, v)) return false;
      }
    return true;
  }

  /** Emit prefab boxes anchored at (side, a, b). */
  emit(side, a, b, boxes) {
    for (const q of boxes) {
      const [u0, v0] = this.cell(side, a + q.a0, b + q.b0);
      const [u1, v1] = this.cell(side, a + q.a1, b + q.b1);
      this.boxes.push({
        x0: Math.min(u0, u1),
        y0: Math.min(v0, v1),
        x1: Math.max(u0, u1),
        y1: Math.max(v0, v1),
        z0: this.zf + q.z0,
        z1: this.zf + q.z1,
        m: q.m,
      });
    }
  }

  occupy(side, a, b, w, d, pad = 0) {
    const [u0, v0] = this.cell(side, a - pad, b - pad);
    const [u1, v1] = this.cell(side, a + w - 1 + pad, b + d - 1 + pad);
    this.mark(Math.min(u0, u1), Math.min(v0, v1), Math.max(u0, u1), Math.max(v0, v1));
  }

  /**
   * Place a prefab against a wall.
   * opts: sides (preference order), at ('center'|'start'|'end'|'random'|number), w (for runs), prefabOpts
   */
  wall(key, opts = {}) {
    const pf = PREFABS[key];
    const sides = opts.sides ?? this.rng.shuffle(SIDES);
    for (const side of sides) {
      const L = this.sideLen(side);
      const w = opts.w ?? pf.w;
      const d = (opts.d ?? pf.d) + (pf.dExtra ?? 0);
      if (w > L || d > this.sideDepth(side) - (opts.keepDepth ?? 6)) continue;
      const positions = [];
      for (let a = 0; a + w <= L; a += 1) positions.push(a);
      const at = opts.at ?? "center";
      const target = at === "center" ? (L - w) / 2 : at === "start" ? 0 : at === "end" ? L - w : typeof at === "number" ? at : this.rng.int(0, L - w);
      positions.sort((x, y) => Math.abs(x - target) - Math.abs(y - target));
      for (const a of positions) {
        if (!this.wallOk(side, a, a + w - 1, pf.tall)) continue;
        if (!this.footprintFree(side, a, 0, w, d)) continue;
        const snap = this.save();
        const boxes = pf.build(this.rng, { ...(opts.prefab ?? {}), w, d: opts.d ?? pf.d });
        this.emit(side, a, 0, boxes);
        this.occupy(side, a, 0, w, d, opts.pad ?? 0);
        if (!this.doorsConnected()) {
          this.undo(snap);
          continue;
        }
        const rec = { key, side, a, b: 0, w, d };
        this.placed.push(rec);
        return rec;
      }
    }
    return null;
  }

  /** Free-standing prefab at a target point (local to side frame N by default). */
  free(key, opts = {}) {
    const pf = PREFABS[key];
    const side = opts.side ?? "N";
    const w = opts.w ?? pf.w;
    const d = opts.d ?? pf.d;
    const pad = opts.pad ?? pf.pad ?? 2;
    const L = this.sideLen(side);
    const D = this.sideDepth(side);
    if (w + 2 * pad > L || d + 2 * pad > D) return null;
    const ta = opts.a ?? Math.round((L - w) / 2);
    const tb = opts.b ?? Math.round((D - d) / 2);
    const cands = [];
    const range = opts.exact ? 0 : opts.range ?? 12;
    for (let db = -range; db <= range; db += 2) for (let da = -range; da <= range; da += 2) cands.push([ta + da, tb + db]);
    cands.sort((p, q) => Math.hypot(p[0] - ta, p[1] - tb) - Math.hypot(q[0] - ta, q[1] - tb));
    for (const [a, b] of cands) {
      if (a - pad < 0 || b - pad < 0 || a + w + pad > L || b + d + pad > D) continue;
      if (!pf.flat && !this.footprintFree(side, a - pad, b - pad, w + 2 * pad, d + 2 * pad)) continue;
      if (pf.flat && !this.footprintFree(side, a, b, w, d)) continue;
      const snap = this.save();
      this.emit(side, a, b, pf.build(this.rng, { w, d, ...(opts.prefab ?? {}) }));
      if (!pf.flat) {
        this.occupy(side, a, b, w, d, Math.max(0, pad - 1));
        if (!this.doorsConnected()) {
          this.undo(snap);
          continue;
        }
      }
      return { key, side, a, b, w, d };
    }
    return null;
  }

  /** Put a prefab against the wall opposite a placed item, facing it. */
  opposite(rec, key, opts = {}) {
    const opp = { N: "S", S: "N", E: "W", W: "E" }[rec.side];
    const L = this.sideLen(opp);
    const w = PREFABS[key].w;
    // mirror the along-wall coordinate (the opposite side runs the other way)
    const center = L - (rec.a + rec.w / 2);
    return this.wall(key, { ...opts, sides: [opp], at: Math.max(0, Math.round(center - w / 2)) });
  }

  area() {
    return this.W * this.H;
  }

  longSides() {
    return this.W >= this.H ? this.rng.shuffle(["N", "S"]).concat(["E", "W"]) : this.rng.shuffle(["E", "W"]).concat(["N", "S"]);
  }

  shortSides() {
    return this.W >= this.H ? this.rng.shuffle(["E", "W"]) : this.rng.shuffle(["N", "S"]);
  }
}

// ------------------------------------------------------------------ rules

const RULES = {
  living(c) {
    const sofa = c.wall("sofa", { sides: c.longSides(), at: "center" });
    if (sofa) {
      c.opposite(sofa, "tvUnit");
      const b = sofa.d + 4;
      const midA = sofa.a + Math.round((sofa.w - 14) / 2);
      c.free("rug", { side: sofa.side, a: midA, b: b - 2, exact: true, pad: 0 });
      c.free("coffeeTable", { side: sofa.side, a: sofa.a + 4, b, range: 2, pad: 1 });
    }
    if (c.area() > 700) c.wall("armchair", { at: "start" });
    c.wall("bookshelf", { at: "random" });
    c.wall("plant", { at: "end" });
    if (c.rng.chance(0.6)) c.wall("floorLamp", { at: "start" });
  },
  bedroom(c) {
    const double = c.W >= 22 && c.H >= 22;
    const bed = c.wall(double ? "bedDouble" : "bedSingle", { sides: c.rng.shuffle(["N", "E", "S", "W"]), at: "center", keepDepth: 6 });
    if (bed) {
      c.wall("nightstand", { sides: [bed.side], at: bed.a - 5 });
      if (double) c.wall("nightstand", { sides: [bed.side], at: bed.a + bed.w + 1 });
    }
    c.wall("wardrobe", { at: "start" });
    if (c.area() > 650) c.wall("desk", { at: "random" });
    else if (c.rng.chance(0.5)) c.wall("dresser", { at: "random" });
    if (c.rng.chance(0.5)) c.free("rug", { pad: 0, range: 4 });
  },
  kitchen(c) {
    const sides = c.longSides();
    let run = null;
    for (const side of sides) {
      const L = c.sideLen(side);
      const w = Math.min(L - 7, 34);
      if (w < 10) continue;
      run = c.wall("counterRun", { sides: [side], w, at: "start", prefab: {} });
      if (run) {
        c.wall("fridge", { sides: [side], at: run.a + run.w + 1 });
        break;
      }
    }
    if (!run) c.wall("fridge", {});
    if (c.area() > 600) c.free("diningTable", { range: 8 });
  },
  dining(c) {
    c.free("diningTable", { range: 6 });
    c.wall("dresser", { at: "center" });
    c.wall("plant", { at: "start" });
  },
  studio(c) {
    const bed = c.wall("bedDouble", { at: "start" }) ?? c.wall("bedSingle", { at: "start" });
    for (const side of c.longSides()) {
      if (bed && side === bed.side) continue;
      const w = Math.min(c.sideLen(side) - 8, 20);
      if (w >= 10 && c.wall("counterRun", { sides: [side], w, at: "end" })) break;
    }
    c.wall("fridge", { at: "end" });
    c.wall("sofa", { at: "random" }) ?? c.wall("armchair", { at: "random" });
    c.wall("wardrobe", { at: "random" });
    c.free("coffeeTable", { range: 10 });
  },
  bath(c) {
    if (c.sideLen("N") >= 15 || c.sideLen("E") >= 15) c.wall("bathtub", { sides: c.shortSides().length ? c.longSides() : undefined, at: "end" });
    else c.wall("shower", { at: "end" });
    c.wall("toilet", { at: "start" });
    c.wall("basin", { at: "center" });
    if (c.rng.chance(0.4)) c.wall("washer", { at: "random" });
  },
  wc(c) {
    c.wall("toilet", { at: "center" });
    c.wall("basin", { at: "center" });
  },
  foyer(c) {
    if (c.area() > 200) c.wall("dresser", { at: "random" });
    if (c.rng.chance(0.5)) c.wall("plant", { at: "random" });
  },
  hall(c) {
    if (c.area() > 400 && c.rng.chance(0.5)) c.wall("bookshelf", { at: "random" });
    if (c.rng.chance(0.4)) c.wall("plant", { at: "random" });
  },
  study(c) {
    c.wall("desk", { at: "center" });
    c.wall("bookshelf", { at: "start" });
    c.wall("bookshelf", { at: "end" });
    c.wall("armchair", { at: "random" });
  },
  closet(c) {
    c.wall("shelfUnit", { at: "start" });
    c.wall("shelfUnit", { at: "end" });
  },
  pantry(c) {
    c.wall("shelfUnit", { at: "start", prefab: { goods: MAT.GOODS } });
    c.wall("shelfUnit", { at: "end" });
  },
  laundry(c) {
    for (let k = 0; k < 3; k += 1) c.wall("washer", { at: "start" });
    c.wall("shelfUnit", { at: "end" });
  },
  storage(c) {
    for (let k = 0; k < 6; k += 1) if (!c.wall("shelfUnit", { at: "random", prefab: { metal: true } })) break;
    for (let k = 0; k < 3; k += 1) c.wall("boxes", { at: "random" });
  },
  backroom(c) {
    RULES.storage(c);
  },
  mechanical(c) {
    c.free("machine", { range: 12 });
  },
  trash(c) {
    for (let k = 0; k < 3; k += 1) c.wall("boxes", { at: "random" });
  },
  bike(c) {
    for (let k = 0; k < 4; k += 1) c.wall("bench", { at: "random" });
  },
  lobby(c) {
    if (c.area() > 3000) c.free("receptionDesk", { range: 20 });
    c.wall("mailboxes", { at: "random" });
    c.wall("bench", { at: "random" });
    c.wall("plant", { at: "start" });
    c.wall("plant", { at: "end" });
  },
  reception(c) {
    c.free("receptionDesk", { range: 8 });
    c.wall("bench", { at: "random" });
    c.wall("plant", { at: "start" });
  },
  landing(c) {
    if (c.rng.chance(0.3)) c.wall("plant", { at: "random" });
  },
  corridor(c) {
    if (c.area() > 1500 && c.rng.chance(0.4)) c.wall("plant", { at: "random" });
  },
  office(c) {
    c.wall("desk", { at: "center" });
    c.wall("bookshelf", { at: "start" }) ?? c.wall("shelfUnit", { at: "start" });
    c.wall("plant", { at: "end" });
  },
  meeting(c) {
    const w = Math.max(10, Math.min(c.W - 14, 40));
    const d = Math.max(6, Math.min(c.H - 16, 12));
    if (c.W >= c.H) c.free("meetingTable", { w, d, range: 4 });
    else c.free("meetingTable", { side: "E", w: Math.max(10, Math.min(c.H - 14, 40)), d: Math.max(6, Math.min(c.W - 16, 12)), range: 4 });
    c.wall("tvUnit", { at: "center" });
  },
  breakroom(c) {
    const run = c.wall("counterRun", { w: Math.min(24, c.sideLen("N") - 8), at: "start" });
    if (run) c.wall("fridge", { sides: [run.side], at: run.a + run.w + 1 });
    for (let k = 0; k < 3; k += 1) c.free("cafeTable", { range: 14, a: 10 + k * 10 });
  },
  restroom(c) {
    const side = c.longSides()[0];
    for (let k = 0; k < 5; k += 1) if (!c.wall("stallToilet", { sides: [side], at: "start" })) break;
    const opp = { N: "S", S: "N", E: "W", W: "E" }[side];
    for (let k = 0; k < 3; k += 1) if (!c.wall("basin", { sides: [opp], at: "start" })) break;
  },
  openOffice(c) {
    // grid of desk clusters, leaving aisles near the walls
    const pf = { w: 24, d: 22 };
    const nx = Math.floor((c.W - 8) / (pf.w + 8));
    const ny = Math.floor((c.H - 8) / (pf.d + 8));
    for (let j = 0; j < ny; j += 1) {
      for (let i = 0; i < nx; i += 1) {
        const a = 6 + i * (pf.w + 8);
        const b = 6 + j * (pf.d + 8);
        c.free("officeDesks", { a, b, exact: true, pad: 1 });
      }
    }
    for (let k = 0; k < 4; k += 1) c.wall("plant", { at: "random" });
    c.wall("shelfUnit", { at: "random" });
  },
  parking(c) {
    // stall rows along the long axis with a drive aisle
    const side = c.W >= c.H ? "N" : "W";
    const L = c.sideLen(side);
    const D = c.sideDepth(side);
    const rows = D >= 100 ? [2, D - 42] : D >= 50 ? [2] : [];
    for (const b of rows) {
      for (let a = 4; a + 18 < L; a += 20) {
        const [u, v] = c.cell(side, a - 1, b);
        const [u2, v2] = c.cell(side, a - 1, b + 38);
        c.boxes.push({ x0: Math.min(u, u2), y0: Math.min(v, v2), x1: Math.max(u, u2), y1: Math.max(v, v2), z0: c.zf - 1, z1: c.zf - 1, m: MAT.LINE_WHITE });
        if (c.rng.chance(0.6) && c.cars) c.free("car", { side, a: a + 2, b: b + 2, exact: true, pad: 0 });
      }
    }
  },
  classroom(c) {
    // board on the wall facing the class, teacher's desk before it, rows of desks
    const side = c.W >= c.H ? (c.wallBehind("W", Math.floor(c.H / 2)) === "wall" ? "W" : "E") : c.wallBehind("N", Math.floor(c.W / 2)) === "ext" ? "S" : "N";
    c.wall("chalkboard", { sides: [side], w: Math.min(32, c.sideLen(side) - 12), at: "center", keepDepth: 2 });
    const L = c.sideLen(side);
    const D = c.sideDepth(side);
    c.free("desk", { side, a: Math.round(L / 2) - 5, b: 6, exact: false, range: 4, prefab: { monitor: false } });
    for (let b = 20; b + 8 < D - 6; b += 11)
      for (let a = 6; a + 6 < L - 6; a += 9) c.free("schoolDesk", { side, a, b, exact: true });
    c.wall("shelfUnit", { at: "random" });
    c.wall("plant", { at: "random" });
  },
  gym(c) {
    // court markings and a hoop at each end
    const r = c.r;
    const line = (x0, y0, x1, y1) => c.boxes.push({ x0, y0, x1, y1, z0: c.zf - 1, z1: c.zf - 1, m: MAT.LINE_WHITE });
    const m = 8;
    line(r.x0 + m, r.y0 + m, r.x1 - m, r.y0 + m);
    line(r.x0 + m, r.y1 - m, r.x1 - m, r.y1 - m);
    line(r.x0 + m, r.y0 + m, r.x0 + m, r.y1 - m);
    line(r.x1 - m, r.y0 + m, r.x1 - m, r.y1 - m);
    const cx = Math.round((r.x0 + r.x1) / 2);
    line(cx, r.y0 + m, cx, r.y1 - m);
    const along = c.W >= c.H ? ["W", "E"] : ["N", "S"];
    for (const sd of along) c.wall("hoop", { sides: [sd], at: "center", keepDepth: 2 });
    c.wall("bench", { at: "random" });
  },
  cafeteria(c) {
    for (let b = 10; b + 8 < c.H - 6; b += 14) for (let a = 10; a + 8 < c.W - 8; a += 14) c.free("cafeTable", { a, b, exact: true });
    c.wall("counterRun", { w: Math.min(24, c.sideLen("N") - 8), at: "start" });
  },
  library(c) {
    for (let k = 0; k < 8; k += 1) if (!c.wall("bookshelf", { at: "random" })) break;
    for (let a = 12; a + 10 < c.W - 10; a += 20) c.free("diningTable", { a, b: Math.round(c.H / 2) - 4, range: 6 });
    c.wall("plant", { at: "start" });
  },
  teachers(c) {
    RULES.breakroom(c);
  },
  deck(c) {
    // garage deck: stalls and pillars were laid out by the planner
    const room = c.room;
    const r = c.r;
    const within = (q) => q.x0 >= r.x0 && q.x1 <= r.x1 && q.y0 >= r.y0 && q.y1 <= r.y1;
    const line = (x0, y0, x1, y1) => c.boxes.push({ x0, y0, x1, y1, z0: c.zf - 1, z1: c.zf - 1, m: MAT.LINE_WHITE });
    for (const s of room.stalls ?? []) {
      if (!within(s)) continue;
      line(s.x0, s.y0, s.x1, s.y0);
      line(s.x0, s.y1 + 1, s.x1, s.y1 + 1);
      if (!s.car || !c.cars) continue;
      // nose-in: the car's front (b = 0) faces the back of the stall
      for (const q of carBoxes(c.rng)) {
        const va = s.y0 + 3 + q.a0;
        const vb = s.y0 + 3 + q.a1;
        const ua = s.noseU > 0 ? s.x0 + 2 + q.b0 : s.x1 - 2 - q.b0;
        const ub = s.noseU > 0 ? s.x0 + 2 + q.b1 : s.x1 - 2 - q.b1;
        c.boxes.push({ x0: Math.min(ua, ub), x1: Math.max(ua, ub), y0: Math.min(va, vb), y1: Math.max(va, vb), z0: c.zf + q.z0, z1: c.zf + q.z1, m: q.m });
      }
    }
    for (const p of room.pillars ?? []) {
      if (!within(p)) continue;
      c.boxes.push({ ...p, z0: c.zf, z1: c.zf + (room.deckH ?? 22) - 3, m: MAT.CONCRETE });
      c.boxes.push({ ...p, z0: c.zf, z1: c.zf + 5, m: MAT.HAZARD_YELLOW });
    }
  },
  retail(c) {
    const cols = Math.floor((c.W - 10) / 22);
    for (let i = 0; i < cols; i += 1) c.free("gondola", { side: "N", a: 8 + i * 22, b: Math.round(c.H * 0.45), range: 4, pad: 3 });
    for (let k = 0; k < 4; k += 1) c.wall("shelfUnit", { at: "random" });
    c.wall("checkout", { at: "start" });
    c.wall("plant", { at: "end" });
  },
  cafe(c) {
    const side = c.longSides()[1];
    c.wall("barCounter", { sides: [side], w: Math.min(c.sideLen(side) - 10, 26), at: "center" });
    for (let k = 0; k < 8; k += 1) c.free("cafeTable", { range: 30, a: 6 + ((k * 13) % Math.max(8, c.W - 12)), b: 6 + ((k * 7) % Math.max(8, c.H - 12)) });
    c.wall("plant", { at: "start" });
  },
  restaurant(c) {
    RULES.cafe(c);
  },
  warehouse(c) {
    const side = c.W >= c.H ? "N" : "W";
    const L = c.sideLen(side);
    const D = c.sideDepth(side);
    for (let b = 10; b + 9 < D - 10; b += 34) {
      for (let a = 10; a + 22 < L - 10; a += 24) c.free("rack", { side, a, b, exact: true, pad: 0, prefab: { h: 40 } });
    }
    for (let k = 0; k < 6; k += 1) c.free("crates", { range: 40, a: c.rng.int(4, Math.max(5, L - 12)), b: c.rng.int(4, Math.max(5, D - 12)) });
  },
  barn(c) {
    // stalls along one long wall, stacked hay bales along the other, a feed alley between
    const side = c.W >= c.H ? "N" : "W";
    const opp = side === "N" ? "S" : "E";
    const L = c.sideLen(side);
    for (let a = 8; a + 20 < L - 8; a += 22) c.free("stall", { side, a, b: 2, exact: true, pad: 0 });
    for (let a = 10; a + 12 < L - 10; a += 14) c.free("hayStack", { side: opp, a, b: 2, exact: true, pad: 0 });
  },
  factory(c) {
    const side = c.W >= c.H ? "N" : "W";
    const L = c.sideLen(side);
    const D = c.sideDepth(side);
    for (let b = 14; b + 12 < D - 10; b += 30) for (let a = 14; a + 16 < L - 10; a += 30) c.free("machine", { side, a, b, exact: true });
    for (let k = 0; k < 5; k += 1) c.free("crates", { range: 30, a: c.rng.int(4, Math.max(5, L - 12)), b: c.rng.int(4, Math.max(5, D - 12)) });
  },
  mailroom(c) {
    c.wall("mailboxes", { at: "start" });
    c.wall("shelfUnit", { at: "end" });
  },
  security(c) {
    c.wall("desk", { at: "center" });
    c.wall("lockers", { at: "start" });
  },
  nave(c) {
    // altar at the far wall, two blocks of pews facing it either side of a central aisle
    const L = c.sideLen("N");
    const D = c.sideDepth("N");
    c.wall("altar", { sides: ["S"], at: "center", keepDepth: 2 });
    const bw = Math.min(28, Math.floor((L - 12 - 8) / 2));
    if (bw < 8) return;
    for (let b = 14; b + 5 < D - 22; b += 8) {
      c.free("pew", { side: "N", a: 4, b, w: bw, exact: true, pad: 0 });
      c.free("pew", { side: "N", a: L - 4 - bw, b, w: bw, exact: true, pad: 0 });
    }
  },
};

Object.assign(PREFABS, CIVIC_PREFABS, RULE_PREFABS);
Object.assign(RULES, makeCivicRules(RULES));

/** Rooms made of several rects (open plans, halls round an annex) furnish every rect. */
const MULTI_RECT = new Set(["openOffice", "parking", "deck", "warehouse", "factory", "selfStorage", "coldStore", "distribution", "timberYard", "sales", "marketHall", "exhibit", "gallery"]);

export function furnishFloor(world, plan, F, env, rng) {
  const grid = F.grid;
  const out = [];
  const zf = F.z + 2;
  for (const room of grid.rooms) {
    const rule = RULES[room.type];
    if (!rule || room.ceiling === 0) continue;
    // largest rect of the room first; open plans furnish every rect
    const rects = room.rects.slice().sort((a, b) => (b.x1 - b.x0) * (b.y1 - b.y0) - (a.x1 - a.x0) * (a.y1 - a.y0));
    const list = MULTI_RECT.has(room.type) ? rects : rects.slice(0, 1);
    for (const r of list) {
      if (r.x1 - r.x0 < 5 || r.y1 - r.y0 < 5) continue;
      const c = new RoomCtx(grid, room, r, zf, rng.fork(`${room.id}:${r.x0}:${r.y0}`), !!world.config.vehicles?.parked);
      rule(c);
      out.push(...c.boxes);
    }
  }
  return out;
}
