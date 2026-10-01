// Stage "civicrules": the furnishing rules of civic rooms and shops (buildings/interior/
// civicRules.js makeCivicRules) run on sample rooms in a scripted room context: its own parts as
// furnish.js's RoomCtx has them (sideLen, sideDepth, cell, area, longSides), and wall / free /
// wallBehind answering from a hash of their arguments, logging every call with its options and
// building the prefab (with the options furnish.js would merge) from the room's stream. The
// rules' calls, options, draws and boxes are the record. tests/city/test_civicrules.cpp runs the
// same context.
import { REF, f, line, samples } from "../lib/rec.mjs";

const { makeCivicRules, RULE_PREFABS } = await import(REF + "buildings/interior/civicRules.js");
const { PREFABS } = await import(REF + "buildings/interior/prefabs.js");
const { CIVIC_PREFABS } = await import(REF + "buildings/interior/civicPrefabs.js");
const { Rng, hash32 } = await import(REF + "core/hash.js");

// (furnish.js's merged table: Object.assign(PREFABS, CIVIC_PREFABS, RULE_PREFABS))
const ALL = { ...PREFABS, ...CIVIC_PREFABS, ...RULE_PREFABS };
const SIDES = ["N", "E", "S", "W"];
const OPTS = ["w", "d", "monitor", "hood", "upper", "screen", "metal", "steel", "double", "band", "piano", "goods", "h", "top", "curtain", "seat", "frame"];
const fopt = (o) => (o === undefined ? "-" : OPTS.map((k) => f(o[k])).join(","));
const sum = (boxes) => {
  let s = 0;
  for (const q of boxes) s += q.a0 + 3 * q.a1 + 5 * q.b0 + 7 * q.b1 + 11 * q.z0 + 13 * q.z1 + 17 * q.m;
  return s;
};

class Ctx {
  constructor(room, rect, zf, rng, cars, salt, log) {
    this.cars = cars;
    this.room = room;
    this.r = rect;
    this.zf = zf;
    this.rng = rng;
    this.W = rect.x1 - rect.x0 + 1;
    this.H = rect.y1 - rect.y0 + 1;
    this.boxes = [];
    this.salt = salt;
    this.log = log;
    this.calls = 0;
  }

  sideLen(side) {
    return side === "N" || side === "S" ? this.W : this.H;
  }

  sideDepth(side) {
    return side === "N" || side === "S" ? this.H : this.W;
  }

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

  area() {
    return this.W * this.H;
  }

  longSides() {
    return this.W >= this.H ? this.rng.shuffle(["N", "S"]).concat(["E", "W"]) : this.rng.shuffle(["E", "W"]).concat(["N", "S"]);
  }

  wallBehind(side, a) {
    return ["wall", "wall", "door", "ext", "wall", "open", "wall", "door"][hash32(this.salt, side.charCodeAt(0), a, 7) & 7];
  }

  wall(key, opts = {}) {
    const pf = ALL[key];
    this.calls += 1;
    const sides = opts.sides ?? this.rng.shuffle(SIDES);
    const side = sides[0];
    const L = this.sideLen(side);
    const w = opts.w ?? pf.w;
    const d = (opts.d ?? pf.d) + (pf.dExtra ?? 0);
    const at = opts.at ?? "center";
    const a = at === "center" ? Math.round((L - w) / 2) : at === "start" ? 0 : at === "end" ? L - w : typeof at === "number" ? at : this.rng.int(0, Math.max(0, L - w));
    const ok = (hash32(this.salt, this.calls, w, d) & 3) !== 0 && w <= L;
    this.log.push(line("wall", key, sides.join(""), opts.at, opts.w, opts.d, opts.keepDepth, opts.pad, fopt(opts.prefab), a, ok));
    if (!ok) return null;
    const boxes = pf.build(this.rng, { ...(opts.prefab ?? {}), w, d: opts.d ?? pf.d });
    this.log.push(line("wb", boxes.length, sum(boxes)));
    return { key, side, a, b: 0, w, d };
  }

  free(key, opts = {}) {
    const pf = ALL[key];
    this.calls += 1;
    const side = opts.side ?? "N";
    const w = opts.w ?? pf.w;
    const d = opts.d ?? pf.d;
    const pad = opts.pad ?? pf.pad ?? 2;
    const L = this.sideLen(side);
    const D = this.sideDepth(side);
    const a = opts.a ?? Math.round((L - w) / 2);
    const b = opts.b ?? Math.round((D - d) / 2);
    const range = opts.exact ? 0 : opts.range ?? 12;
    const ok = hash32(this.salt, this.calls, a, b) % 3 !== 0 && w + 2 * pad <= L && d + 2 * pad <= D;
    this.log.push(line("free", key, side, opts.a, opts.b, opts.w, opts.d, opts.pad, opts.range, opts.exact, fopt(opts.prefab), range, ok));
    if (!ok) return null;
    const boxes = pf.build(this.rng, { w, d, ...(opts.prefab ?? {}) });
    this.log.push(line("fb", boxes.length, sum(boxes)));
    return { key, side, a, b, w, d };
  }
}

// furnish.js's base rules the civic ones build on
const BASE = {};
for (const type of ["office", "bedroom", "warehouse"])
  BASE[type] = (c) => {
    c.log.push(line("rule", type, c.rng.next()));
  };

export default function* civicrules() {
  const R = makeCivicRules(BASE);
  yield line("rules", Object.keys(R).join(","));
  const r = samples(29);
  for (const type of Object.keys(R)) {
    for (let i = 0; i < 10; i += 1) {
      const W = 6 + Math.floor(r() * 110);
      const H = 6 + Math.floor(r() * 90);
      const x0 = Math.floor(r() * 50);
      const y0 = Math.floor(r() * 50);
      const rect = { x0, y0, x1: x0 + W - 1, y1: y0 + H - 1 };
      const fire = r() < 0.5;
      const classical = r() < 0.5;
      const front = ["N", "S", undefined][Math.floor(r() * 3)];
      const cars = r() < 0.7;
      const zf = Math.floor(r() * 100);
      const salt = Math.floor(r() * 1e9);
      const rng = new Rng(Math.floor(r() * 4294967296));
      const room = { id: i, type, rects: [rect], fire, classical, ...(front ? { front } : {}) };
      const log = [];
      const c = new Ctx(room, rect, zf, rng, cars, salt, log);
      R[type](c);
      yield line("room", type, i, W, H, x0, y0, fire, classical, front, cars, zf, salt);
      yield* log;
      yield line("boxes", ...c.boxes.flatMap((q) => [q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, q.m]));
      yield line("end", rng.next());
    }
  }
}
