// Site kinds for the stages of the site layer and the site links (tests/city/site_kinds.hpp is the
// C++ twin). The reference's kinds (sites/militaryBase.js, researchComplex.js, mountainBase.js)
// need the cell network, the archetypes and the kit's surface structures, which come later; these
// stand in for them with the same kinds of placement and complexes of the same shapes:
//
//   testBase    a compound on a level pad: one sector entered from a bunker; its plan's bounds
//               reach beyond the blend rect; ground: a chequer on the pad; port: the anteroom of
//               the deepest level (militaryBase)
//   testCampus  four sectors joined by a tram, two portals; no bounds, no ground; port: the
//               station nearest the other site (researchComplex)
//   testHold    a custom placement on mountain ground (place): a round apron, a ramp, the site's
//               own pad and a road (a path pad); ground by pad; two sectors and a tram, a custom
//               volume (a vault); port: the nearest station (mountainBase)
//   testRuin    a few walls; neither ground nor port
//
// registerSiteKinds() registers them in SITES (the reference's SITES holds no kinds unless the
// kinds' modules are imported, which these stages do not).
import { REF } from "./rec.mjs";
import { shaftRect } from "./complexes.mjs";

const { SITES } = await import(REF + "world/sites.js");
const { planComplex, emitComplex, LEVEL_GAP } = await import(REF + "sites/complex.js");
const { box, finishStructure } = await import(REF + "sites/kit.js");
const { Rng, hashFloat } = await import(REF + "core/hash.js");
const { vx } = await import(REF + "core/units.js");
const { MAT } = await import(REF + "voxel/materials.js");
const { P, P2 } = await import(REF + "voxel/chunk.js");

/** The station of a complex's tram nearest a point (the reference kinds' port). */
function nearestStation(u, toward) {
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

// ------------------------------------------------------------ testBase

function basePlan(world, site) {
  const rng = new Rng(site.seed);
  const r = site.rect;
  const W = r.x1 - r.x0;
  const H = r.y1 - r.y0;
  const bunker = { x0: Math.round(r.x0 + W * 0.72), y0: Math.round(r.y0 + H * 0.05), x1: Math.round(r.x0 + W * 0.98), y1: Math.round(r.y0 + H * 0.32) };
  const b = site.blend;
  return { bunker, levels: rng.int(2, 3), bounds: { x0: b.x0 - vx(30), y0: b.y0, x1: b.x1, y1: b.y1 + vx(20) }, under: null };
}

function baseStructure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [] };
  const r = site.rect;
  const z = site.padZ;
  box(lists.details, r.x0, r.y0, z + 1, r.x1, r.y0, z + 22, MAT.CHAINLINK);
  box(lists.details, r.x1, r.y1, z + 1, r.x0, r.y1, z + 22, MAT.CHAINLINK);
  const bk = p.bunker;
  box(lists.shells, bk.x0, bk.y0, z + 1, bk.x1, bk.y1, z + 40, MAT.CONCRETE_DARK);
  box(lists.carves, bk.x0 + 4, bk.y0 + 4, z + 2, bk.x1 - 4, bk.y1 - 4, z + 36, 0);
  const bounds = { x0: r.x0 + vx(8), y0: r.y0 + vx(8), x1: r.x1 - vx(8), y1: r.y1 - vx(8) };
  const under = planComplex(rng, {
    sectors: [{ bounds, z0: z + 1 - vx(14), levels: p.levels, theme: "military", rooms: [9, 14] }],
    entries: [{ rect: shaftRect(Math.round((bk.x0 + bk.x1) / 2), bk.y0 + 12), zTop: z + 1, sector: 0, dir: -1, openTop: true }],
    tram: null,
  });
  p.under = under;
  emitComplex(lists, under, rng);
  p.structure = finishStructure(lists);
  return p.structure;
}

function baseGround(site, x, y, out) {
  if (!out.inside) return;
  out.mat = ((x >> 5) + (y >> 5)) & 1 ? MAT.CONCRETE : MAT.CONCRETE_LIGHT;
  out.sub = MAT.CONCRETE;
}

function basePort(world, site) {
  baseStructure(world, site);
  const u = site.plan.under;
  const lv = u.levels[u.levels.length - 1];
  const q = lv.rooms[0];
  return { x: Math.round((q.x0 + q.x1) / 2), y: Math.round((q.y0 + q.y1) / 2), z: lv.zf };
}

// ------------------------------------------------------------ testCampus

const CAMPUS_THEMES = ["lab", "containment", "power", "barracks"];

function campusPlan(world, site) {
  const rng = new Rng(site.seed);
  const r = site.rect;
  const iw = r.x1 - r.x0;
  const ih = r.y1 - r.y0;
  const at = (fx, fy) => ({ x: Math.round(r.x0 + iw * fx), y: Math.round(r.y0 + ih * fy) });
  const portals = [at(0.18, 0.38), at(0.66, 0.4)].map((q) => ({ x0: q.x, y0: q.y, x1: q.x + vx(12), y1: q.y + vx(15) }));
  return { portals, themes: rng.shuffle(CAMPUS_THEMES.slice()), levels: [rng.int(2, 3), rng.int(2, 3), rng.int(2, 3), rng.int(2, 3)], under: null };
}

function campusStructure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [] };
  const r = site.rect;
  const z = site.padZ;
  for (const q of p.portals) {
    box(lists.shells, q.x0, q.y0, z + 1, q.x1, q.y1, z + 40, MAT.CONCRETE_DARK);
    box(lists.carves, q.x0 + 4, q.y0 + 4, z + 2, q.x1 - 4, q.y1 - 4, z + 36, 0);
  }
  const pad = vx(10);
  const mx = Math.round((r.x0 + r.x1) / 2);
  const my = Math.round((r.y0 + r.y1) / 2);
  const quads = [
    { x0: r.x0 + pad, y0: r.y0 + pad, x1: mx - pad / 2, y1: my - pad / 2 },
    { x0: mx + pad / 2, y0: r.y0 + pad, x1: r.x1 - pad, y1: my - pad / 2 },
    { x0: r.x0 + pad, y0: my + pad / 2, x1: mx - pad / 2, y1: r.y1 - pad },
    { x0: mx + pad / 2, y0: my + pad / 2, x1: r.x1 - pad, y1: r.y1 - pad },
  ];
  const top = z + 1;
  const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(16) - k * vx(9), levels: p.levels[k], theme: p.themes[k], rooms: [8, 12] }));
  let deepest = Infinity;
  for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * LEVEL_GAP);
  const entries = p.portals.map((q) => {
    const cx = Math.round((q.x0 + q.x1) / 2);
    let sector = quads.findIndex((b) => cx >= b.x0 && cx <= b.x1 && q.y0 >= b.y0 && q.y0 <= b.y1);
    if (sector < 0) sector = 0;
    return { rect: shaftRect(cx, q.y0 + 12), zTop: top, sector, dir: -1, openTop: true };
  });
  const seen = new Set();
  const uniq = entries.filter((e) => (seen.has(e.sector) ? false : (seen.add(e.sector), true)));
  const under = planComplex(rng, { sectors, entries: uniq, tram: { z: deepest - LEVEL_GAP } });
  p.under = under;
  emitComplex(lists, under, rng);
  p.structure = finishStructure(lists);
  return p.structure;
}

function campusPort(world, site, toward) {
  campusStructure(world, site);
  return nearestStation(site.plan.under, toward);
}

// ------------------------------------------------------------ testHold

const ROAD_HALF = vx(3.5);

function holdPlace(world, cand) {
  const { rect, seed } = cand;
  const cx = Math.round((rect.x0 + rect.x1) / 2);
  const cy = Math.round((rect.y0 + rect.y1) / 2);
  const T = world.terrain;
  const tc = T.sample(cx, cy);
  if (tc.mountain < 0.2 || tc.u > 0.01) return null;
  const side = hashFloat(seed, 1) < 0.5 ? 1 : -1;
  const ax0 = side > 0 ? rect.x1 + vx(10) : rect.x0 - vx(54);
  const apron = { x0: ax0, y0: cy - vx(28), x1: ax0 + vx(44), y1: cy + vx(28) };
  const zf = Math.round(T.sample((apron.x0 + apron.x1) / 2, (apron.y0 + apron.y1) / 2).h);
  const zr = Math.round(tc.h);
  // a ramp from the site's pad towards the apron (along x), or out of its south side (along y)
  const alongY = hashFloat(seed, 2) < 0.4;
  const rx0 = side > 0 ? rect.x1 - vx(20) : apron.x1 - vx(10);
  const ramp = alongY ? { x0: cx - vx(4), y0: rect.y1 - vx(20), x1: cx + vx(4), y1: rect.y1 + vx(10) } : { x0: rx0, y0: cy - vx(4), x1: rx0 + vx(30), y1: cy + vx(4) };
  const pts = [];
  let x = side > 0 ? apron.x1 : apron.x0;
  let y = cy;
  for (let k = 0; k < 14; k += 1) {
    pts.push({ x, y, z: Math.round(T.sample(x, y).h) });
    x += side * vx(8);
    y += Math.round((hashFloat(seed, 10 + k) - 0.5) * vx(8));
  }
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const q of pts) {
    x0 = Math.min(x0, q.x);
    y0 = Math.min(y0, q.y);
    x1 = Math.max(x1, q.x);
    y1 = Math.max(y1, q.y);
  }
  const road = { rect: { x0: x0 - ROAD_HALF, y0: y0 - ROAD_HALF, x1: x1 + ROAD_HALF, y1: y1 + ROAD_HALF }, z: zf, path: pts, half: ROAD_HALF, margin: vx(9), round: true, road: true };
  const fp = {
    x0: Math.min(rect.x0, apron.x0, ramp.x0, road.rect.x0),
    y0: Math.min(rect.y0, apron.y0, ramp.y0, road.rect.y0),
    x1: Math.max(rect.x1, apron.x1, ramp.x1, road.rect.x1),
    y1: Math.max(rect.y1, apron.y1, ramp.y1, road.rect.y1),
  };
  return {
    padZ: zf,
    pads: [
      { rect: apron, z: zf, margin: vx(22), round: true },
      { rect: ramp, z: zr, margin: vx(10), ramp: alongY ? { axis: "y", a0: ramp.y0, a1: ramp.y1, z0: zr, z1: zf } : { axis: "x", a0: ramp.x0, a1: ramp.x1, z0: side > 0 ? zr : zf, z1: side > 0 ? zf : zr } },
      { rect, z: zr, margin: cand.margin },
      road,
    ],
    footprint: fp,
    side,
  };
}

function holdPlan(world, site) {
  const rng = new Rng(site.seed);
  const fp = site.placed.footprint;
  return { levels: [rng.int(2, 3), rng.int(2, 3)], bounds: { x0: fp.x0 - vx(8), y0: fp.y0 - vx(8), x1: fp.x1 + vx(8), y1: fp.y1 + vx(8) }, under: null };
}

function holdGround(site, x, y, out) {
  const pad = out.pad;
  if (pad?.road) {
    if (out.inside) {
      out.mat = pad._c < 0.7 && pad._s % 48 < 24 ? MAT.LINE_YELLOW : pad._c > ROAD_HALF - 1.2 ? MAT.LINE_WHITE : MAT.ASPHALT_WORN;
      out.sub = MAT.GRAVEL;
    } else {
      out.mat = out.natural - out.z > vx(1) ? MAT.ROCK : MAT.GRAVEL;
      out.sub = out.mat;
    }
    return;
  }
  if (pad?.ramp) {
    out.mat = MAT.PAVER_GRAY;
    out.sub = MAT.GRAVEL;
    return;
  }
  if (Math.abs(out.z - out.natural) > 2) {
    out.mat = out.natural - out.z > vx(1) ? MAT.ROCK : MAT.GRAVEL;
    out.sub = out.mat;
  }
}

/** A vault over the site's rect at its pad level: a floor, the air above, a lining at its edge. */
function holdVolume(site) {
  const r = site.rect;
  const z = site.padZ;
  const H = vx(10);
  return {
    bb: { x0: r.x0, y0: r.y0, z0: z - 1, x1: r.x1, y1: r.y1, z1: z + H + 1 },
    rasterize(chunk) {
      if (chunk.lod > 2) return;
      const data = chunk.data;
      for (let j = 0; j < P; j += 1)
        for (let i = 0; i < P; i += 1) {
          const x = chunk.wx(i);
          const y = chunk.wy(j);
          if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
          const edge = x < r.x0 + 16 || x > r.x1 - 16 || y < r.y0 + 16 || y > r.y1 - 16;
          for (let k = 0; k < P; k += 1) {
            const zz = chunk.wz(k);
            const idx = i + j * P + k * P2;
            if (zz === z) data[idx] = MAT.FLOOR_CONCRETE;
            else if (zz > z && zz <= z + H) {
              if (!edge) data[idx] = 0;
              else if (data[idx] !== 0) data[idx] = MAT.CONCRETE;
            }
          }
        }
    },
  };
}

function holdStructure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [], custom: [holdVolume(site)] };
  const cx = Math.round(site.center.x);
  const cy = Math.round(site.center.y);
  const halfL = vx(90);
  const halfW = vx(70);
  const quads = [
    { x0: cx + vx(6), y0: cy - halfW, x1: cx + halfL, y1: cy + halfW },
    { x0: cx - halfL, y0: cy - halfW, x1: cx - vx(6), y1: cy + halfW },
  ];
  const top = site.padZ;
  const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(18) - k * vx(8), levels: p.levels[k], theme: k === 0 ? "military" : "power", rooms: [8, 12] }));
  let deepest = Infinity;
  for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * vx(15));
  const px = cx - vx(30);
  const py = cy + vx(20);
  let sector = quads.findIndex((b) => px >= b.x0 && px <= b.x1 && py >= b.y0 && py <= b.y1);
  if (sector < 0) sector = 0;
  const under = planComplex(rng, { sectors, entries: [{ rect: shaftRect(px, py), zTop: top + 1, sector, dir: -1, openTop: true }], tram: { z: deepest - vx(15) } });
  p.under = under;
  emitComplex(lists, under, rng);
  p.structure = finishStructure(lists);
  return p.structure;
}

function holdPort(world, site, toward) {
  holdStructure(world, site);
  return nearestStation(site.plan.under, toward);
}

// ------------------------------------------------------------ testRuin

function ruinStructure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const lists = { shells: [], carves: [], details: [] };
  const r = site.rect;
  const z = site.padZ;
  box(lists.details, r.x0, r.y0, z + 1, r.x0 + 3, r.y1, z + 30, MAT.STONE);
  box(lists.details, r.x1, r.y0, z + 1, r.x1 - 3, r.y0 + 40, z + 18, MAT.STONE);
  box(lists.carves, r.x0, r.y0 + 20, z + 2, r.x0 + 3, r.y0 + 30, z + 20, 0);
  site.plan.structure = finishStructure(lists);
  return site.plan.structure;
}

export const SITE_KINDS = [
  { id: "testBase", label: "Test base", frequency: 0.4, minU: 0, maxU: 0.04, size: [230, 180], margin: 45, maxRelief: 30, plan: basePlan, structure: baseStructure, ground: baseGround, port: basePort },
  { id: "testCampus", label: "Test campus", frequency: 0.2, minU: 0, maxU: 0.05, size: [340, 280], margin: 50, maxRelief: 40, plan: campusPlan, structure: campusStructure, port: campusPort },
  { id: "testHold", label: "Test hold", frequency: 0.25, minU: 0, maxU: 0.02, size: [190, 190], margin: 30, place: holdPlace, plan: holdPlan, structure: holdStructure, ground: holdGround, port: holdPort },
  { id: "testRuin", label: "Test ruin", frequency: 0.1, minU: 0, maxU: 0.3, size: [100, 80], plan: () => ({}), structure: ruinStructure },
];

let registered = false;
/** Registers the kinds in SITES (once). */
export function registerSiteKinds() {
  if (registered) return SITES;
  registered = true;
  for (const k of SITE_KINDS) SITES.register(k);
  return SITES;
}
