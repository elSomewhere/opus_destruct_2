import { SITES } from "../world/sites.js";
import { DISTRICTS } from "../world/registry.js";
import { Rng } from "../core/hash.js";
import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { planBuildingEnvelopeAs } from "../buildings/archetypes.js";
import { stairDims } from "../buildings/interior/stairs.js";
import { planComplex, emitComplex, LEVEL_GAP } from "./complex.js";
import { planGate, fence, gateBooth, watchtowers, radome, dishArray, fuelTanks, truck, portalBlock, finishStructure } from "./kit.js";

/**
 * Research complex (Black Mesa style): a campus on the surface over a deep
 * multi-sector facility.
 *
 * Surface: fenced campus with a gate, ring road, an HQ and lab buildings
 * (office archetype => full validated interiors), a hangar, a parking
 * garage, radomes and a dish array, and portal blocks housing the stairs
 * down.
 *
 * Underground: four sectors in a 2 x 2 grid under the campus (lab,
 * containment, power, barracks themes), each 2-3 levels at staggered
 * depths, joined by a tram loop at the bottom; two portals lead down to
 * the first sectors, the rest are reached by tram.
 */

DISTRICTS.register({
  id: "research",
  label: "Research campus",
  color: "#6a8aa8",
  streets: { pattern: "none", block: [[9999, 9999], [9999, 9999]], pedestrianChance: 0, mergeChance: 0, localClass: "local" },
  blockUse: [["rural", 1]],
  lots: { mode: "none", width: [0, 0], alleyChance: 0 },
  archetypes: [],
  floors: [3, 6],
  styles: [["glass", 2], ["futurist", 1], ["concrete", 1]],
});

const THEMES = ["lab", "containment", "power", "barracks"];

function plan(world, site) {
  const rng = new Rng(site.seed);
  const r = site.rect;
  const { gateSide, gate, drive } = planGate(world, site, vx(4.5));
  const ringIn = vx(12);
  const ringW = vx(8);
  const ring = { x0: r.x0 + ringIn, y0: r.y0 + ringIn, x1: r.x1 - ringIn, y1: r.y1 - ringIn };
  const inner = { x0: ring.x0 + ringW + vx(4), y0: ring.y0 + ringW + vx(4), x1: ring.x1 - ringW - vx(4), y1: ring.y1 - ringW - vx(4) };
  const iw = inner.x1 - inner.x0;
  const ih = inner.y1 - inner.y0;
  const at = (fx0, fy0, fx1, fy1) => ({
    x0: Math.round(inner.x0 + iw * fx0),
    y0: Math.round(inner.y0 + ih * fy0),
    x1: Math.round(inner.x0 + iw * fx1),
    y1: Math.round(inner.y0 + ih * fy1),
  });
  const lots = [
    { kind: "office", rect: at(0.0, 0.0, 0.3, 0.3), front: "N", style: "glass" },
    { kind: "office", rect: at(0.36, 0.0, 0.62, 0.27), front: "N", style: rng.pick(["futurist", "glass"]) },
    { kind: "office", rect: at(0.7, 0.0, 1.0, 0.3), front: "N", style: "concrete" },
    { kind: "warehouse", rect: at(0.0, 0.6, 0.38, 1.0), front: "W", style: "industrial" },
    { kind: "garage", rect: at(0.44, 0.66, 0.7, 1.0), front: "S", style: "parkingDeck" },
  ];
  // portal blocks over the stairs down to sectors 0 and 1
  const pw = vx(12);
  const ph = vx(15);
  const portals = [at(0.18, 0.38, 0.18, 0.38), at(0.66, 0.4, 0.66, 0.4)].map((q) => ({ x0: q.x0, y0: q.y0, x1: q.x0 + pw, y1: q.y0 + ph }));
  const helipad = { ...at(0.45, 0.46, 0.45, 0.46), r: vx(10) };
  const radomes = [at(0.86, 0.5, 0.86, 0.5), at(0.94, 0.7, 0.94, 0.7)];
  const dishes = at(0.74, 0.88, 0.74, 0.88);
  const tanks = at(0.78, 0.34, 0.78, 0.34);
  return {
    gateSide,
    gate,
    drive,
    ring,
    ringW,
    inner,
    lots,
    portals,
    helipad: { x: helipad.x0, y: helipad.y0, r: helipad.r },
    radomes: radomes.map((q) => ({ x: q.x0, y: q.y0 })),
    dishes: { x: dishes.x0, y: dishes.y0 },
    tanks: { x: tanks.x0, y: tanks.y0 },
    bounds: {
      x0: Math.min(site.blend.x0, drive.x0),
      y0: Math.min(site.blend.y0, drive.y0),
      x1: Math.max(site.blend.x1, drive.x1),
      y1: Math.max(site.blend.y1, drive.y1),
    },
    themes: rng.shuffle(THEMES.slice()),
    levels: [rng.int(2, 3), rng.int(2, 3), rng.int(2, 3), rng.int(2, 3)],
    under: null,
  };
}

function surface(world, site) {
  const rng = new Rng(site.seed ^ 0x51ed);
  const district = DISTRICTS.get("research");
  const envs = [];
  site.plan.lots.forEach((l, k) => {
    const lot = {
      id: `C${site.cell.i}_${site.cell.j}/${site.type}${site.a}_${site.b}/l${k}`,
      rect: l.rect,
      front: l.front,
      frontages: [{ side: l.front, cls: "local" }],
      corner: false,
      district: "research",
      groundZ: site.padZ,
      block: site.id,
      cell: `C${site.cell.i}_${site.cell.j}`,
    };
    const env = planBuildingEnvelopeAs(lot, l.kind, l.style, district, rng.fork(`b${k}`), { u: 0.2, core: 0.45, groundZ: site.padZ, config: world.config });
    if (env) envs.push({ lot, env });
  });
  return envs;
}

function ground(site, x, y, out) {
  const p = site.plan;
  const inRect = (q, m = 0) => x >= q.x0 - m && x <= q.x1 + m && y >= q.y0 - m && y <= q.y1 + m;
  if (inRect(p.drive)) {
    const c = p.gateSide === "N" || p.gateSide === "S" ? x - (p.drive.x0 + p.drive.x1) / 2 : y - (p.drive.y0 + p.drive.y1) / 2;
    out.mat = Math.abs(c) < 0.6 ? MAT.LINE_YELLOW : MAT.ASPHALT;
    out.sub = MAT.GRAVEL;
    return;
  }
  if (!out.inside) return;
  const r = p.ring;
  const onRing = inRect(r) && (x < r.x0 + p.ringW || x > r.x1 - p.ringW || y < r.y0 + p.ringW || y > r.y1 - p.ringW);
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
  for (const q of p.portals) {
    if (inRect(q, vx(4))) {
      out.mat = ((x >> 4) + (y >> 4)) & 1 ? MAT.CONCRETE : MAT.CONCRETE_LIGHT;
      out.sub = MAT.CONCRETE;
      return;
    }
  }
  for (const l of p.lots) {
    if (inRect(l.rect, vx(5))) {
      out.mat = MAT.PLAZA_STONE;
      out.sub = MAT.CONCRETE;
      return;
    }
  }
  // lawns crossed by paths between the buildings
  const px = ((x % 160) + 160) % 160;
  const py = ((y % 160) + 160) % 160;
  out.mat = px < 12 || py < 12 ? MAT.PAVER_GRAY : MAT.GRASS_LAWN;
  out.sub = MAT.DIRT;
}

/** The facility: 2 x 2 sectors under the campus, a tram loop, portals to sectors 0 and 1. */
function planUnderground(site, rng) {
  const p = site.plan;
  const r = site.rect;
  const pad = vx(10);
  const mx = Math.round((r.x0 + r.x1) / 2);
  const my = Math.round((r.y0 + r.y1) / 2);
  const quads = [
    { x0: r.x0 + pad, y0: r.y0 + pad, x1: mx - pad / 2, y1: my - pad / 2 },
    { x0: mx + pad / 2, y0: r.y0 + pad, x1: r.x1 - pad, y1: my - pad / 2 },
    { x0: r.x0 + pad, y0: my + pad / 2, x1: mx - pad / 2, y1: r.y1 - pad },
    { x0: mx + pad / 2, y0: my + pad / 2, x1: r.x1 - pad, y1: r.y1 - pad },
  ];
  const top = site.padZ + 1;
  const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(16) - k * vx(9), levels: p.levels[k], theme: p.themes[k], rooms: [8, 12] }));
  let deepest = Infinity;
  for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * LEVEL_GAP);
  const sd = stairDims(30);
  // portal shafts: portal k sits over the sector whose bounds contain it
  const entries = p.portals.map((q) => {
    const cx = Math.round((q.x0 + q.x1) / 2);
    const rect = { x0: cx - Math.floor(sd.W / 2), x1: cx - Math.floor(sd.W / 2) + sd.W - 1, y0: q.y0 + 12, y1: q.y0 + 12 + sd.L - 1 };
    let sector = quads.findIndex((b) => cx >= b.x0 && cx <= b.x1 && q.y0 >= b.y0 && q.y0 <= b.y1);
    if (sector < 0) sector = 0;
    return { rect, zTop: top, sector, dir: -1, openTop: true };
  });
  // at most one portal per sector
  const seen = new Set();
  const uniq = entries.filter((e) => (seen.has(e.sector) ? false : (seen.add(e.sector), true)));
  return planComplex(rng, { sectors, entries: uniq, tram: { z: deepest - LEVEL_GAP } });
}

function structure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [] };
  const { details } = lists;
  const z = site.padZ;
  fence(details, site.rect, z, p.gateSide, p.gate, 26);
  gateBooth(details, p.gate, p.gateSide, z);
  watchtowers(details, site.rect, z);
  for (const q of p.radomes) radome(details, q.x, q.y, z, 44);
  dishArray(details, p.dishes.x, p.dishes.y, z, 3, 72);
  fuelTanks(details, p.tanks.x, p.tanks.y, z, 2);
  if (world.config.vehicles?.parked) for (let k = 0; k < 3; k += 1) truck(details, p.helipad.x + vx(14) + k * vx(5), p.helipad.y - vx(6), z + 1, rng);
  const under = planUnderground(site, rng);
  site.plan.under = under;
  for (const q of p.portals) portalBlock(lists, q, z, MAT.SIGN_BLUE);
  emitComplex(lists, under, rng);
  site.plan.structure = finishStructure(lists);
  return site.plan.structure;
}

function pois(world, site) {
  const p = site.plan;
  const g = p.gate;
  const out2 = { N: [0, -40], S: [0, 40], W: [-40, 0], E: [40, 0] }[p.gateSide];
  structure(world, site);
  const u = p.under;
  const q = p.portals[0];
  const out = [
    { label: "Research complex gate", x: (g.x0 + g.x1) / 2 + out2[0], y: (g.y0 + g.y1) / 2 + out2[1], z: null },
    { label: "Research complex, portal to the labs", x: (q.x0 + q.x1) / 2, y: q.y1 + 24, z: null },
  ];
  for (const sec of u.sectors) {
    const lv = sec.levels[0];
    const ante = lv.rooms[0];
    out.push({ label: `Research complex, ${sec.theme} sector`, x: (ante.x0 + ante.x1) / 2, y: (ante.y0 + ante.y1) / 2, z: lv.zf + 1 });
  }
  const st = u.tram?.stations[0];
  if (st) out.push({ label: "Research complex, tram station", x: (st.hall.x0 + st.hall.x1) / 2, y: (st.hall.y0 + st.hall.y1) / 2, z: u.tram.z + 1 });
  return out;
}

/** Link-tunnel port: the tram station nearest the linked site. */
function port(world, site, toward) {
  structure(world, site);
  const u = site.plan.under;
  if (!u.tram?.stations.length) return null;
  let best = null;
  for (const st of u.tram.stations) {
    const x = Math.round((st.hall.x0 + st.hall.x1) / 2);
    const y = Math.round((st.hall.y0 + st.hall.y1) / 2);
    const d = toward ? Math.hypot(toward.x - x, toward.y - y) : 0;
    if (!best || d < best.d) best = { x, y, z: u.tram.z, d };
  }
  return best;
}

SITES.register({
  id: "researchComplex",
  label: "Research complex",
  frequency: 0.25,
  minU: 0,
  maxU: 0.05,
  size: [340, 280],
  margin: 50,
  maxRelief: 40,
  plan,
  surface,
  ground,
  structure,
  pois,
  port,
});

export { structure as researchStructure };
