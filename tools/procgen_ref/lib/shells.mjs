// Shared inputs of the building shell stages (massing, wings, garageramps, skybridges, grading; the
// C++ twin is tests/city/shell_records.hpp): the worlds a shell is drawn in (every season, explicit
// snow covers), scripted envelopes of every archetype, the chunks round a box and their digests.
import { REF, line } from "./rec.mjs";
import { scriptedLot } from "./buildings.mjs";

const { World } = await import(REF + "world/World.js");
const { ARCHETYPES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const { Rng } = await import(REF + "core/hash.js");

/**
 * The worlds of the shell stages (config overrides as JSON, parsed alike by both sides): summer,
 * winter, a cold spring and autumn (snow on roofs from the season), an explicit snow cover in
 * summer (patches) and one above 1 (every roof white).
 */
export const SHELL_WORLDS = [
  '{"seed":1}',
  '{"seed":2,"world":{"season":"winter"}}',
  '{"seed":3,"world":{"season":"spring","climate":{"temperature":0.2}}}',
  '{"seed":4,"world":{"season":"autumn","climate":{"temperature":0.1,"temperatureVar":0.02}}}',
  '{"seed":5,"world":{"climate":{"snowCover":0.6}}}',
  '{"seed":6,"world":{"season":"winter","climate":{"snowCover":1.4}}}',
  '{"seed":7,"world":{"season":"winter","climate":{"temperature":0.05}}}',
];

const worlds = new Map();
/** World k of SHELL_WORLDS (a World of World.js, made once). */
export function shellWorld(k) {
  let w = worlds.get(k);
  if (!w) worlds.set(k, (w = new World(JSON.parse(SHELL_WORLDS[k]))));
  return w;
}

// a flavor's church dome materials (and a list with a name the palette lacks)
const DOMES = [["GOLD", "DOME_GREEN", "DOME_BLUE"], ["DOME_SHINGLE", "DOME_GREEN"], ["NOPE"]];

/**
 * A scripted envelope of archetype `a` (an archetype record) in style `style` (null: the district's
 * pick, planBuildingEnvelope) on a scripted lot of a size it fits (lib/buildings.mjs scriptedLot,
 * plain or turned), in district d, in world w: { lot, env } (env null when none fits). Draws from r.
 */
export function shellEnvelope(r, a, style, d, w, k) {
  let U = 0;
  let V = 0;
  for (let t = 0; t < 40; t += 1) {
    U = 40 + Math.floor(r() * 560);
    V = 40 + Math.floor(r() * 560);
    if (a.fits(U, V)) break;
  }
  const lot = scriptedLot(r, U, V, k, d.id);
  const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 3000) - 200, config: w.config };
  if (r() < 0.2) extra.chapel = true;
  if (r() < 0.5) extra.dome = DOMES[Math.floor(r() * DOMES.length)];
  if (r() < 0.4) extra.pitchedCivic = 0.6;
  const rng = new Rng(Math.floor(r() * 4294967296));
  const env = style === null ? A.planBuildingEnvelope(lot, d, rng, extra) : A.planBuildingEnvelopeAs(lot, a.id, style, d, rng, extra);
  return { lot, env };
}

/** Every archetype, in registration order. */
export const archetypeList = () => ARCHETYPES.all();

/** A chunk's content: FNV-1a over its voxels, and the count of the solid ones. */
export function chunkDigest(c) {
  let h = 2166136261;
  let n = 0;
  const d = c.data;
  for (let i = 0; i < d.length; i += 1) {
    h = Math.imul(h ^ d[i], 16777619);
    if (d[i]) n += 1;
  }
  return [h >>> 0, n];
}

/**
 * The chunks (LOD 0 world chunk coordinates [lod, cx, cy, cz] for each lod of `lods`) holding
 * the given points (voxels [x, y, z]), each once per LOD in the points' order.
 */
export function chunksAt(lods, pts) {
  const out = [];
  for (const lod of lods) {
    const E = 32 << lod;
    const seen = new Set();
    for (const [x, y, z] of pts) {
      const c = [lod, Math.floor(x / E), Math.floor(y / E), Math.floor(z / E)];
      const key = c.join(",");
      if (seen.has(key)) continue;
      seen.add(key);
      out.push(c);
    }
  }
  return out;
}

// ---- sites: lots on scripted streets (the wings stage, the building source's)

const Fr = await import(REF + "buildings/frame.js");
const { trimToCuts } = await import(REF + "city/blockPoly.js");
const { roadLevelAt } = await import(REF + "network/roadLevel.js");
const { YAWS } = await import(REF + "core/placement.js");
const { vx } = await import(REF + "core/units.js");

/** Street classes of the scripted streets (the first five: a slanted street's). */
const CLASSES = ["local", "collector", "arterial", "village", "pedestrian", "lane", "alley"];
/** The archetypes that may carry wings, and some that may not. */
const WINGED = ["walkup", "midrise", "office", "rowhouse", "townhouse"];
const UNWINGED = ["house", "panelSlab", "garage", "school"];
const GAPS = [0, 0, 1, 3];

/**
 * A scripted site in cell (i, j) of World w (a World of World.js whose cellNet serves `cellRoads`,
 * a Map by `${i},${j}`): a lot of a winged archetype's size (now and then another's) at the cell's
 * centre, plain (any front) or turned (a table yaw), fronting scripted straight streets of the
 * classes' cross sections on some of its sides (front, left, right, back: their right-of-way edge on
 * the lot's edge or a little off it, owned by the cell or now and then by another), a plain lot now
 * and then cut back at a front corner by a slanted street (a block cut of a table yaw, now and then
 * off the table by a hair; the lot trimmed to it as cellPlan.js fitLots does, `whole` the lot as
 * planned) or taking its block whole (`whole: true`); its envelope (planBuildingEnvelopeAs) on the
 * ground level its frontage point's street gives (cellPlan.js lotGround). Draws from r.
 * Returns { lot, block, env, cellId, view }.
 */
export function wingSite(r, w, cellRoads, i, j, k, DS, styles, specs) {
  const rc = w.arterials.cellRect(i, j);
  const cx = Math.round((rc.x0 + rc.x1) / 2);
  const cy = Math.round((rc.y0 + rc.y1) / 2);
  const cellId = `C${i}_${j}`;
  const aid = r() < 0.85 ? WINGED[Math.floor(r() * WINGED.length)] : UNWINGED[Math.floor(r() * UNWINGED.length)];
  const a = ARCHETYPES.get(aid);
  let U = 0;
  let V = 0;
  for (let t = 0; t < 60; t += 1) {
    U = 48 + Math.floor(r() * 300);
    V = 64 + Math.floor(r() * 300);
    if (a.fits(U, V)) break;
  }
  const turned = r() < 0.25;
  const fi = Math.floor(r() * 4);
  const yaw = Math.floor(r() * 132);
  const lot = { id: `${cellId}/b${k}/l0`, district: null, corner: false, micro: false, block: `${cellId}/b${k}`, frontages: [] };
  if (turned) {
    lot.front = Fr.nominalFront(yaw);
    lot.turn = { yaw, origin: { x: cx - 100, y: cy - 100 }, ou: 0, ov: 0, U, V };
    lot.rect = { ...Fr.lotFrameOf(lot).R };
  } else {
    lot.front = "NESW"[fi];
    const ns = lot.front === "N" || lot.front === "S";
    const x0 = cx - Math.floor((ns ? U : V) / 2);
    const y0 = cy - Math.floor((ns ? V : U) / 2);
    lot.rect = { x0, y0, x1: x0 + (ns ? U : V) - 1, y1: y0 + (ns ? V : U) - 1 };
  }
  const frame = Fr.lotFrameOf(lot);
  const roads = [];
  const street = (pts, cls, owned) => {
    const s = specs[cls];
    const road = { id: `${cellId}/r${roads.length}`, cell: owned ? cellId : "C999_999", cls, pts, hc: s.hc, hr: s.hr, corner: s.corner, median: s.median, parking: s.parking,
      lanes: s.lanes, lane: s.lane, sidewalk: s.sidewalk, shoulder: s.shoulder };
    roads.push(road);
    return road;
  };
  // the streets along its sides
  for (const cs of ["F", "L", "R", "B"]) {
    const p = r();
    const alley = r() < 0.6;
    const ci = Math.floor(r() * CLASSES.length);
    const gap = GAPS[Math.floor(r() * GAPS.length)];
    const ext = 200 + Math.floor(r() * 400);
    const owned = r() < 0.85;
    if (!(cs === "F" ? p < 0.92 : cs === "B" ? p < 0.3 : p < 0.6)) continue;
    const cls = cs === "B" && alley ? "alley" : CLASSES[ci];
    const o = gap + specs[cls].hr;
    let pts;
    if (turned) {
      const P = (u, v) => {
        const [x, y] = frame.pointToWorld(u, v);
        return { x, y };
      };
      if (cs === "F") pts = [P(-ext, -o), P(U + ext, -o)];
      else if (cs === "B") pts = [P(-ext, V + o), P(U + ext, V + o)];
      else if (cs === "L") pts = [P(-o, -ext), P(-o, V + ext)];
      else pts = [P(U + o, -ext), P(U + o, V + ext)];
    } else {
      const R = lot.rect;
      const ws = frame.worldSide(cs);
      if (ws === "N") pts = [{ x: R.x0 - ext, y: R.y0 - o }, { x: R.x1 + 1 + ext, y: R.y0 - o }];
      else if (ws === "S") pts = [{ x: R.x0 - ext, y: R.y1 + 1 + o }, { x: R.x1 + 1 + ext, y: R.y1 + 1 + o }];
      else if (ws === "W") pts = [{ x: R.x0 - o, y: R.y0 - ext }, { x: R.x0 - o, y: R.y1 + 1 + ext }];
      else pts = [{ x: R.x1 + 1 + o, y: R.y0 - ext }, { x: R.x1 + 1 + o, y: R.y1 + 1 + ext }];
      if (cls !== "alley") lot.frontages.push({ side: ws, cls });
    }
    street(pts, cls, owned);
  }
  // a slanted street cutting a front corner off (plain lots)
  let block = r() < 0.5 ? null : { id: lot.block };
  const sp = r();
  const sc = r();
  const sy = r();
  const sq = r();
  const sd = r();
  const sh = r();
  const scls = CLASSES[Math.floor(r() * 5)];
  const sowned = r() < 0.85;
  if (!turned && sp < 0.35) {
    const R = lot.rect;
    const [wx, wy] = frame.toWorld(sc < 0.5 ? 0 : frame.U - 1, 0);
    const qx = wx === R.x0 ? R.x0 : R.x1 + 1;
    const qy = wy === R.y0 ? R.y0 : R.y1 + 1;
    const Y = YAWS[1 + Math.floor(sy * 32) + 33 * Math.floor(sq * 4)];
    const dx = Y.c / Y.r;
    const dy = Y.s / Y.r;
    let nx = -dy;
    let ny = dx;
    if (nx * ((R.x0 + R.x1 + 1) / 2 - qx) + ny * ((R.y0 + R.y1 + 1) / 2 - qy) < 0) {
      nx = -nx;
      ny = -ny;
    }
    if (sh < 0.1) nx += 1e-4;
    const hr = specs[scls].hr;
    const c = nx * qx + ny * qy + vx(2) + Math.floor(sd * vx(8));
    const id = `${cellId}/r${roads.length}`;
    const cut = { nx, ny, c, cls: scls, hr, id };
    const t = trimToCuts(R, [cut]);
    if (t) {
      const t0 = (c - hr - (nx * qx + ny * qy)) / (nx * nx + ny * ny);
      const px = qx + nx * t0;
      const py = qy + ny * t0;
      street([{ x: px - dx * 700, y: py - dy * 700 }, { x: px + dx * 700, y: py + dy * 700 }], scls, sowned);
      lot.whole = R;
      lot.rect = t.rect;
      for (const tr of t.trimmed) {
        if (!tr.cls || tr.cls === "alley") continue;
        lot.frontages = [...lot.frontages.filter((q) => q.side !== tr.side), { side: tr.side, cls: tr.cls, slant: tr.id }];
      }
      block = { id: lot.block, cuts: [cut] };
    }
  }
  const wh = r();
  if (lot.whole === undefined && !turned && wh < 0.1) lot.whole = true;
  lot.corner = lot.frontages.length >= 2;
  const d = DS[Math.floor(r() * DS.length)];
  lot.district = d.id;
  const style = styles[Math.floor(r() * styles.length)];
  cellRoads.set(`${i},${j}`, roads);
  const view = w.roadView(i, j);
  // (cellPlan.js lotGround: the street's level at the lot's frontage point)
  const R = lot.rect;
  const fp = turned
    ? Fr.lotFrameOf(lot).pointToWorld(U / 2, -4)
    : lot.front === "N"
      ? [(R.x0 + R.x1) / 2, R.y0 - 4]
      : lot.front === "S"
        ? [(R.x0 + R.x1) / 2, R.y1 + 4]
        : lot.front === "W"
          ? [R.x0 - 4, (R.y0 + R.y1) / 2]
          : [R.x1 + 4, (R.y0 + R.y1) / 2];
  const lv = roadLevelAt(w, view, fp[0], fp[1], 24);
  const groundZ = lv ? Math.round(lv.z) + (lv.sidewalk ? 1 : 0) : Math.round(w.terrain.sample(fp[0], fp[1]).h) + 1;
  const extra = { u: r(), core: r(), groundZ, config: w.config };
  const env = A.planBuildingEnvelopeAs(lot, aid, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
  // (scripted, beyond the archetypes: upper floors set back from the sides, or cantilevered over the
  // front - footprints no archetype draws, which a chamfer or a bay may not fit)
  const tm = r();
  if (env && env.roof.type === "flat" && env.floors >= 4 && env.tiers.length === 1 && tm < 0.2) {
    const m = env.tiers[0].rects[0];
    const up = tm < 0.1 ? { x0: m.x0 + 12, y0: m.y0, x1: m.x1 - 12, y1: m.y1 } : { x0: m.x0, y0: m.y0 - 12, x1: m.x1, y1: m.y1 };
    env.tiers = [{ f0: 0, f1: 1, rects: [m] }, { f0: 2, f1: env.floors - 1, rects: [up] }];
  }
  return { lot, block, env, cellId, view };
}

/** A lot's record: its rect, front, turn, frontages, whole and block cuts. */
export function lotLine(tag, lot, block) {
  const turn = lot.turn ? [lot.turn.yaw, lot.turn.origin.x, lot.turn.origin.y, lot.turn.U, lot.turn.V].join("/") : "-";
  const fronts = lot.frontages.map((q) => `${q.side}${q.cls}${q.slant !== undefined ? `@${q.slant}` : ""}`).join(";") || "-";
  const whole = lot.whole === undefined ? "-" : lot.whole === true ? "true" : `${lot.whole.x0},${lot.whole.y0},${lot.whole.x1},${lot.whole.y1}`;
  const cuts = block?.cuts ? block.cuts.map((q) => [q.nx, q.ny, q.c, q.cls, q.hr, q.id].join(",")).join(";") : block ? "none" : "-";
  return line(tag, lot.id, `${lot.rect.x0},${lot.rect.y0},${lot.rect.x1},${lot.rect.y1}`, lot.front, turn, fronts, whole, cuts, lot.corner, lot.district);
}

export { line };
