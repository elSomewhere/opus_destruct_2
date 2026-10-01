import { SITES } from "../world/sites.js";
import { DISTRICTS } from "../world/registry.js";
import { Rng } from "../core/hash.js";
import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { fence, gateBooth, watchtowers, radarMast, fuelTanks, truck, portalBlock, finishStructure, planGate } from "./kit.js";
import { planBuildingEnvelopeAs } from "../buildings/archetypes.js";
import { stairDims } from "../buildings/interior/stairs.js";
import { planComplex, emitComplex } from "./complex.js";

/**
 * Military research base: a fenced compound in the countryside.
 *
 * Surface: perimeter fence with a gate + guard booth facing the nearest
 * road (joined by a driveway), watchtowers, a radar mast, fuel tanks, a
 * helipad, an HQ, barracks and hangars (regular archetypes => full,
 * validated interiors) and a concrete bunker that houses the stairs down.
 *
 * Underground: 2-3 levels 14 m apart, each a Doom-like layout of rooms
 * (labs, server rooms, reactor hall, barracks, armory, storage, control)
 * joined by a spanning tree of corridors plus a few loops; a stair shaft
 * links the levels and an entrance shaft climbs to the bunker.
 */

DISTRICTS.register({
  id: "military",
  label: "Military",
  color: "#6f7d5a",
  streets: { pattern: "none", block: [[9999, 9999], [9999, 9999]], pedestrianChance: 0, mergeChance: 0, localClass: "local" },
  blockUse: [["rural", 1]],
  lots: { mode: "none", width: [0, 0], alleyChance: 0 },
  archetypes: [],
  floors: [2, 3],
  styles: [["concrete", 1]],
});

const FIRST_DEPTH = vx(14);

function planBase(world, site) {
  const rng = new Rng(site.seed);
  const r = site.rect;
  const W = r.x1 - r.x0;
  const H = r.y1 - r.y0;
  const { gateSide, gate, drive } = planGate(world, site);
  // internal ring road
  const ringIn = vx(12);
  const ringW = vx(7);
  const ring = { x0: r.x0 + ringIn, y0: r.y0 + ringIn, x1: r.x1 - ringIn, y1: r.y1 - ringIn };
  // building pads inside the ring (canonical: front faces the ring road)
  const inner = { x0: ring.x0 + ringW + vx(4), y0: ring.y0 + ringW + vx(4), x1: ring.x1 - ringW - vx(4), y1: ring.y1 - ringW - vx(4) };
  const iw = inner.x1 - inner.x0;
  const ih = inner.y1 - inner.y0;
  const lots = [];
  const lotRect = (fx0, fy0, fx1, fy1) => ({
    x0: Math.round(inner.x0 + iw * fx0),
    y0: Math.round(inner.y0 + ih * fy0),
    x1: Math.round(inner.x0 + iw * fx1),
    y1: Math.round(inner.y0 + ih * fy1),
  });
  lots.push({ kind: "office", rect: lotRect(0.0, 0.0, 0.3, 0.3), front: "N" });
  lots.push({ kind: "walkup", rect: lotRect(0.36, 0.0, 0.62, 0.22), front: "N" });
  lots.push({ kind: "warehouse", rect: lotRect(0.0, 0.45, 0.42, 1.0), front: "W" });
  lots.push({ kind: "warehouse", rect: lotRect(0.48, 0.55, 0.78, 1.0), front: "S" });
  const bunker = lotRect(0.72, 0.05, 0.98, 0.32);
  const helipad = { x: Math.round(inner.x0 + iw * 0.62), y: Math.round(inner.y0 + ih * 0.38), r: vx(10) };
  const apron = lotRect(0.44, 0.26, 0.98, 0.5);
  const plan = {
    gateSide,
    gate,
    drive,
    ring,
    ringW,
    inner,
    lots,
    bunker,
    helipad,
    apron,
    bounds: {
      x0: Math.min(site.blend.x0, drive.x0),
      y0: Math.min(site.blend.y0, drive.y0),
      x1: Math.max(site.blend.x1, drive.x1),
      y1: Math.max(site.blend.y1, drive.y1),
    },
    levels: rng.int(2, 3),
    under: null,
  };
  void W;
  void H;
  return plan;
}

/** Surface envelopes for the cell plan (full interiors via archetypes). */
function surface(world, site) {
  const rng = new Rng(site.seed ^ 0x51ed);
  const district = DISTRICTS.get("military");
  const envs = [];
  site.plan.lots.forEach((l, k) => {
    const lot = {
      id: `C${site.cell.i}_${site.cell.j}/${site.type}${site.a}_${site.b}/l${k}`,
      rect: l.rect,
      front: l.front,
      frontages: [{ side: l.front, cls: "local" }],
      corner: false,
      district: "military",
      groundZ: site.padZ,
      block: site.id,
      cell: `C${site.cell.i}_${site.cell.j}`,
    };
    const env = planBuildingEnvelopeAs(lot, l.kind, "concrete", district, rng.fork(`b${k}`), { u: 0, core: 0.3, groundZ: site.padZ, config: world.config });
    if (env) envs.push({ lot, env });
  });
  return envs;
}

function ground(site, x, y, out) {
  const p = site.plan;
  const inRect = (q) => x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1;
  if (inRect(p.drive)) {
    const c = p.gateSide === "N" || p.gateSide === "S" ? x - (p.drive.x0 + p.drive.x1) / 2 : y - (p.drive.y0 + p.drive.y1) / 2;
    out.mat = Math.abs(c) < 0.6 ? MAT.LINE_YELLOW : MAT.ASPHALT_WORN;
    out.sub = MAT.GRAVEL;
    return;
  }
  if (!out.inside) return;
  const r = p.ring;
  const onRing =
    x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1 && (x < r.x0 + p.ringW || x > r.x1 - p.ringW || y < r.y0 + p.ringW || y > r.y1 - p.ringW);
  if (onRing) {
    out.mat = MAT.ASPHALT;
    out.sub = MAT.GRAVEL;
    return;
  }
  const h = p.helipad;
  const dh = Math.hypot(x - h.x, y - h.y);
  if (dh < h.r) {
    const hx = Math.abs(x - h.x);
    const hy = Math.abs(y - h.y);
    const isH = dh > h.r - 3 || (hy < vx(3.5) && (Math.abs(hx - vx(2.5)) < 3 || (hx < vx(2.5) && hy < 3)));
    out.mat = isH ? MAT.LINE_WHITE : MAT.CONCRETE_DARK;
    out.sub = MAT.CONCRETE;
    return;
  }
  if (inRect(p.apron) || inRect(p.bunker)) {
    out.mat = ((x >> 5) + (y >> 5)) & 1 ? MAT.CONCRETE : MAT.CONCRETE_LIGHT;
    out.sub = MAT.CONCRETE;
    return;
  }
  for (const l of p.lots) {
    const q = l.rect;
    if (x >= q.x0 - vx(6) && x <= q.x1 + vx(6) && y >= q.y0 - vx(6) && y <= q.y1 + vx(6)) {
      out.mat = MAT.CONCRETE;
      out.sub = MAT.CONCRETE;
      return;
    }
  }
  out.mat = MAT.GRASS_DRY;
  out.sub = MAT.DIRT;
}

// ------------------------------------------------------------ structures

/** Surface props + bunker + underground complex as world boxes (lazy). */
function structure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [] };
  const { details } = lists;
  const r = site.rect;
  const z = site.padZ;
  fence(details, r, z, p.gateSide, p.gate);
  gateBooth(details, p.gate, p.gateSide, z);
  watchtowers(details, r, z);
  radarMast(details, p.apron.x1 - vx(4), p.apron.y0 + vx(4), z);
  fuelTanks(details, p.apron.x0 + vx(8), p.apron.y1 - vx(6), z);
  if (world.config.vehicles?.parked) for (let k = 0; k < 4; k += 1) truck(details, p.apron.x0 + vx(6) + k * vx(5), p.apron.y0 + vx(4), z + 1, rng);
  // bunker over the entrance shaft, then the complex below
  const under = planUnderground(site, rng);
  site.plan.under = under;
  portalBlock(lists, p.bunker, z);
  emitComplex(lists, under, rng);
  site.plan.structure = finishStructure(lists);
  return site.plan.structure;
}

// ------------------------------------------------------------ underground

/** The base's complex: one military sector under the compound, entered from the bunker. */
function planUnderground(site, rng) {
  const p = site.plan;
  const r = site.rect;
  const bounds = { x0: r.x0 + vx(8), y0: r.y0 + vx(8), x1: r.x1 - vx(8), y1: r.y1 - vx(8) };
  const sd = stairDims(30);
  const bk = p.bunker;
  // entrance stair centered in the bunker, near end facing the blast door (south)
  const ex0 = Math.round((bk.x0 + bk.x1 - sd.W) / 2);
  const eRect = { x0: ex0, y0: bk.y0 + 12, x1: ex0 + sd.W - 1, y1: bk.y0 + 12 + sd.L - 1 };
  return planComplex(rng, {
    sectors: [{ bounds, z0: site.padZ + 1 - FIRST_DEPTH, levels: p.levels, theme: "military", rooms: [9, 14] }],
    entries: [{ rect: eRect, zTop: site.padZ + 1, sector: 0, dir: -1, openTop: true }],
    tram: null,
  });
}

function pois(world, site) {
  const p = site.plan;
  const g = p.gate;
  const out2 = { N: [0, -40], S: [0, 40], W: [-40, 0], E: [40, 0] }[p.gateSide];
  structure(world, site);
  const u = p.under;
  const deep = u.levels[u.levels.length - 1];
  const sh = u.shafts[u.shafts.length - 1];
  return [
    { label: "Military base gate", x: (g.x0 + g.x1) / 2 + out2[0], y: (g.y0 + g.y1) / 2 + out2[1], z: null },
    { label: "Military base, deepest level", x: sh.rect.x0 + 8, y: sh.rect.y0 - 28, z: deep.zf + 1 },
  ];
}

/** Link-tunnel port: the anteroom of the deepest level. */
function port(world, site) {
  structure(world, site);
  const u = site.plan.under;
  const lv = u.levels[u.levels.length - 1];
  const q = lv.rooms[0];
  return { x: Math.round((q.x0 + q.x1) / 2), y: Math.round((q.y0 + q.y1) / 2), z: lv.zf };
}

SITES.register({
  id: "militaryBase",
  label: "Military research base",
  frequency: 0.45,
  minU: 0,
  maxU: 0.04,
  size: [230, 180],
  margin: 45,
  maxRelief: 30,
  plan: planBase,
  surface,
  ground,
  structure,
  pois,
  port,
});
