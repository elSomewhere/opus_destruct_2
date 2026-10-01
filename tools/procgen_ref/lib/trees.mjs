// Trees for the stages of nature/trees.js (trees, treemodels, treevox; tests/city/tree_records.hpp
// is the C++ twin): sampled tree records of every kind, their fields and their models' parts as
// record fields, and the seasonal looks the emitters give them.
import { REF, f } from "./rec.mjs";

const { TREE_KINDS } = await import(REF + "nature/trees.js");
const { YAWS, PITCHES } = await import(REF + "core/placement.js");
const { Season } = await import(REF + "world/season.js");

/** Every kind of TREE_KINDS, in its key order, then a kind the tables do not know. */
export const KINDS = [...Object.keys(TREE_KINDS), "unknownKind"];
/** The understory's kinds (the forest gives them the smaller reach, U_R - 1). */
const UNDER = new Set(["fern", "berry", "log", "stump", "shrub", "shrubDry", "hazel", "juniper"]);
/** The tilts the forest lays a wild log at (forest.js LOG_TILTS): level, the grades, the yaw table's first angles. */
const LOG_TILTS = [...PITCHES, ...YAWS.slice(1, 9)];

/**
 * A tree of `kind` at (x, y, z) from 24 draws of r: its size from the kind's ranges as the
 * emitters scale them (a sapling, a young tree, a grown one, up to a third broader), a seed (now
 * and then negative or beyond 32 bits), open-grown or not; wild or not (with or without its amp,
 * and the forest's reach, a tight one or none); a log's lie (yaw, tilt, plate) given or not.
 */
export function sampleTree(r, kind, x, y, z) {
  const u = [];
  for (let k = 0; k < 24; k += 1) u.push(r());
  const spec = TREE_KINDS[kind] ?? TREE_KINDS.oak;
  const g = u[0] < 0.1 ? 0.12 + 0.2 * u[1] : u[0] < 0.3 ? 0.4 + 0.45 * u[1] : 0.9 + 0.2 * u[1];
  const h = Math.max(2, Math.round(Math.round((spec.h[0] + (spec.h[1] - spec.h[0]) * u[2]) * 8) * g));
  const rr = Math.round((spec.r[0] + (spec.r[1] - spec.r[0]) * u[3]) * 8) * (u[4] < 0.3 ? 1 : 0.4 + 0.95 * u[4]);
  const seed = u[5] < 0.06 ? Math.floor((u[6] - 0.5) * 2 ** 36) : Math.floor(u[6] * 4294967296);
  const t = { x, y, z, h, r: rr, kind, seed, open: u[7] < 0.4 };
  if (u[8] < 0.5) {
    t.wild = true;
    if (u[9] < 0.35) t.amp = u[10] < 0.25 ? 0 : u[10] < 0.4 ? 0.5 : u[10];
    if (u[11] < 0.5) t.reach = UNDER.has(kind) ? 27 : 57;
    else if (u[11] < 0.75) t.reach = 4 + Math.floor(u[12] * 40);
  }
  if (kind === "log") {
    if (u[13] < 0.5) t.yaw = Math.floor(u[14] * 132);
    if (u[15] < 0.6) {
      const T = LOG_TILTS[Math.floor(u[16] * LOG_TILTS.length)];
      t.tilt = u[17] < 0.5 ? { c: T.c, s: -T.s, r: T.r } : T;
    }
    if (u[18] < 0.6) t.plate = u[19] < 0.5;
  }
  if (u[20] < 0.05) t.r = u[21] * 1.2;
  return t;
}

/** A tree's fields: kind, x, y, z, h, r, seed, open, wild, amp, reach, yaw, tilt (c,s,r), plate ("-": undefined). */
export function treeFields(t) {
  return [t.kind, t.x, t.y, t.z, t.h, t.r, t.seed, t.open, t.wild, t.amp, t.reach, t.yaw, t.tilt ? `${t.tilt.c},${t.tilt.s},${t.tilt.r}` : undefined, t.plate];
}

const KEYS = {
  0: ["k", "x", "y", "z", "rx", "rz", "mix", "bb"],
  1: ["k", "ax", "ay", "az", "dx", "dy", "dz", "L2", "r0", "r1", "mat", "trunk", "twig", "birch", "foot", "moss", "stump", "bb"],
  2: ["k", "x", "y", "z0", "z1", "zLive", "r", "tier", "lobes", "phase", "lean", "lx", "ly", "zf", "hh", "ax", "ay", "asym", "bb"],
  3: ["k", "x", "y", "z", "rx", "rz", "drop", "mix", "bb"],
  4: ["k", "x", "y", "z", "r", "h", "n", "phase", "bb"],
  5: ["k", "x", "y", "z", "nx", "ny", "nz", "r", "th", "bb"],
};
const TAG = ["b", "l", "c", "u", "f", "p"];

/** A model part as a record line's fields (every field it may carry, "-" where it has none). */
export function partFields(p) {
  const keys = KEYS[p.k];
  for (const key of Object.keys(p)) if (!keys.includes(key)) throw new Error(`trees: a part with an unknown field ${key}`);
  const b = p.bb;
  const flags = new Set(["trunk", "twig", "birch", "moss", "stump", "lean"]);
  return [TAG[p.k], ...keys.slice(1, -1).map((key) => (flags.has(key) ? !!p[key] : p[key])), b.x0, b.y0, b.z0, b.x1, b.y1, b.z1];
}

/** A part as text (the trees stage digests these). */
export const partText = (p) => partFields(p).map(f).join(" ");

/** The seasons a tree's look is taken from: every season under four climates (none, explicit snow covers). */
export const LOOK_CLIMATES = [null, { snowCover: 0.5 }, { temperature: 0.3, temperatureVar: 0.04, snowCover: 0.2 }, { snowCover: 1 }];
export const SEASONS = LOOK_CLIMATES.flatMap((cl) => ["spring", "summer", "autumn", "winter"].map((id) => new Season({ world: { season: id, climate: cl } })));

/** FNV-1a over a string's character codes. */
export function fnv(s, h = 2166136261) {
  for (let i = 0; i < s.length; i += 1) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}
