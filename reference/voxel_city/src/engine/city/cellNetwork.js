import { Rng, hashFloat, hashString } from "../core/hash.js";
import { SimplexNoise } from "../core/noise.js";
import { vx } from "../core/units.js";
import { roadSpecs } from "../network/roadClasses.js";
import { DISTRICTS } from "../world/registry.js";
import { classifyDistrict } from "./districts.js";
import { flavoredDistrictId, flavorOf } from "./flavors.js";
import { STREET_PATTERNS, PATTERN_FAMILY } from "./streets.js";
import { diagonalsOn, diagonalPieces } from "./diagonals.js";
import { rectPoly, splitPoly, lineCrosses, polyBlock, polyCentroid } from "./blockPoly.js";

/**
 * Stage 1 of an arterial cell: its road network, sub-cells (districts) and
 * blocks. Depends only on global line functions and macro fields, so any cell
 * can be produced independently of its neighbours.
 *
 * Ownership: a cell owns the arterial segments on its WEST and NORTH edges;
 * E/S edge cross-sections are recomputed (same pure function) for insets.
 */

const STRIP_BY_DISTRICT = {
  downtown: "pits",
  midtown: "pits",
  mixed: "pits",
  residential: "grass",
  suburban: "grass",
  industrial: "none",
  heavyIndustry: "none",
  village: "grass",
  microdistrict: "grass",
  port: "none",
  projects: "grass",
};

let wobbleNoise = null;
let wobbleSeed = null;

/** Angled streets on (ANGLED_WORLD_PLAN.md S1)? */
function angledRoads(config) {
  const a = config.world.angles;
  return !!(a?.enabled && a.features?.roads !== false);
}

/** Wobble of country roads (x the axis-aligned world's) and of village streets through the angled world. */
const ANGLED_WOBBLE = { rural: 1.7, village: 0.45, main: 0.35 };

/** Urbanization a diagonal boulevard needs somewhere along its piece of a cell. */
const DIAGONAL_U = 0.3;

/** Deterministic description of one arterial-grid edge. */
export function edgeInfo(world, axis, line, span) {
  const { arterials, fields, config } = world;
  const fixed = arterials.line(axis, line);
  const s0 = arterials.line(1 - axis, span);
  const s1 = arterials.line(1 - axis, span + 1);
  // (a wrapping world hashes the canonical line and span: every lap agrees)
  const cl = arterials.canon(line);
  const cs = arterials.canon(span);
  const sOff = arterials.line(1 - axis, span) - arterials.lineAt(1 - axis, cs);
  const mid = (s0 + s1) / 2;
  const px = axis === 0 ? fixed : mid;
  const py = axis === 0 ? mid : fixed;
  const um = fields.urban(px, py);
  const ua = um.u;
  const ub = fields.urban(axis === 0 ? fixed : s0, axis === 0 ? s0 : fixed).u;
  const uc = fields.urban(axis === 0 ? fixed : s1, axis === 0 ? s1 : fixed).u;
  const u = Math.max(ua, (ub + uc) / 2);
  // through a village the country road becomes its main street (sidewalks, a gentle bend)
  const village = um.settlement?.village ?? false;
  // a town's flavor may build narrow, gently bending main streets (old northern towns)
  const flavor = um.settlement && !village ? flavorOf(um.settlement) : null;
  const chance = config.city.ruralRoadChance ?? 0.6;
  let cls = null;
  if (village) cls = u > 0.12 ? "village" : u > 0.04 || hashFloat(config.seed, cl * 2 + axis, cs, 991) < chance ? "rural" : null;
  // (small places build two-lane main streets instead of avenues: city.mainRoad)
  else if (u > 0.2) {
    cls = flavor?.mainRoad ?? config.city.mainRoad ?? "arterial";
    // not every line is an avenue: away from the centre about half the lines
    // (never those with a subway under them) are two-lane collectors, so
    // the grid reads as a few main roads with streets between them
    const sub = config.subway.enabled !== false && Math.abs(line + axis * 7) % (config.subway.lineEvery ?? 2) === 0;
    if (cls === "arterial" && !sub && um.core < 0.3 && config.world.mode !== "infiniteCity" && hashFloat(config.seed, cl * 2 + axis, 0, 994) < 0.55) cls = "collector";
  }
  // (the lanes out of a small island town: not every edge round it)
  else if (u > 0.06) cls = !fields.island || hashFloat(config.seed, cl * 2 + axis, cs, 992) < 0.5 ? "rural" : null;
  else if (hashFloat(config.seed, cl * 2 + axis, cs, 991) < chance && !tooSteep(world, axis, fixed, s0, s1)) cls = "rural";
  if (fields.island) {
    // island: trunk roads join every place; nothing runs out over the sea
    if (!cls && fields.island.trunkEdges(world).has(`${axis}:${line}:${span}`)) cls = "rural";
    if (cls && (axis === 0 ? world.seaHitsSeg(fixed, s0, fixed, s1, 4) : world.seaHitsSeg(s0, fixed, s1, fixed, 4))) cls = null;
  }
  const specs = roadSpecs(config);
  const spec = cls ? specs[cls] : null;
  let pts;
  if (axis === 0) pts = [{ x: fixed, y: s0 }, { x: fixed, y: s1 }];
  else pts = [{ x: s0, y: fixed }, { x: s1, y: fixed }];
  // (the angled world: country and village roads wander more freely)
  const free = angledRoads(config) ? ANGLED_WOBBLE : null;
  if (cls === "rural") pts = wobble(world, axis, cl, fixed, s0, s1, (village ? 0.25 : 1) * (free ? free.rural : 1), sOff);
  else if (village) pts = wobble(world, axis, cl, fixed, s0, s1, free ? free.village : 0.2, sOff);
  else if (cls && flavor?.mainRoadWobble) pts = wobble(world, axis, cl, fixed, s0, s1, flavor.mainRoadWobble, sOff);
  // old-town main streets are cobbled
  const paving = cls && cls !== "rural" && flavor?.cobbleWithin && fields.settlementDistance(um.settlement, px, py) < flavor.cobbleWithin ? "cobble" : null;
  // how far the road bends off its line: blocks beside it keep that much further back
  let wob = 0;
  for (const p of pts) wob = Math.max(wob, Math.abs((axis === 0 ? p.x : p.y) - fixed));
  return { axis, line, span, fixed, s0, s1, cls, spec, hr: spec ? spec.hr : 0, wob, paving, pts, u };
}

/**
 * Country roads stay off terrain they could not climb: no road where the
 * ground along the edge rises more than ~14% between samples (mountain
 * flanks, canyon walls, ravines).
 */
function tooSteep(world, axis, fixed, s0, s1) {
  const mid = (s0 + s1) / 2;
  if (world.fields.mountainness(axis === 0 ? fixed : mid, axis === 0 ? mid : fixed) > 0.45) return true;
  const n = 6;
  const step = (s1 - s0) / n;
  let prev = null;
  for (let k = 0; k <= n; k += 1) {
    const s = s0 + k * step;
    const h = world.terrain.sample(axis === 0 ? fixed : s, axis === 0 ? s : fixed).h;
    if (prev !== null && Math.abs(h - prev) > 0.14 * step) return true;
    prev = h;
  }
  return false;
}

function wobble(world, axis, line, fixed, s0, s1, scale = 1, sOff = 0) {
  const seed = world.config.seed;
  if (wobbleSeed !== seed) {
    wobbleNoise = new SimplexNoise(seed ^ 0x51f15e);
    wobbleSeed = seed;
  }
  const amp = vx(world.config.city.ruralRoadWobble) * scale;
  const len = s1 - s0;
  const n = Math.max(4, Math.round(len / vx(24)));
  const pts = [];
  for (let k = 0; k <= n; k += 1) {
    const t = k / n;
    const s = s0 + len * t;
    const taper = Math.pow(Math.sin(Math.PI * t), 0.7);
    const off = amp * taper * wobbleNoise.fbm2(line * 7.13 + axis * 31.1, (s - sOff) / vx(420), 3);
    const f = Math.round(fixed + off);
    pts.push(axis === 0 ? { x: f, y: Math.round(s) } : { x: Math.round(s), y: f });
  }
  return pts;
}

/**
 * Does a sub-cell centre lie on a harbour front? Near a big lake's shore,
 * or on an island near the main town's harbour (one small port, not the
 * whole waterfront).
 */
function portNear(world, x, y) {
  if (world.lakes?.shoreNear(x, y, vx(420))) return true;
  const isl = world.fields.island;
  if (!isl) return false;
  const h = isl.harbour();
  if (!h) return false;
  // (a small town's harbour is a few quays, not half the town)
  const townR = isl.sites[0].radius;
  const reach = Math.min(420 + (0.18 * (isl.cfg.population ?? 20000)) / 100, townR * 0.5);
  if (Math.hypot(x / 8 - h.x, y / 8 - h.y) > reach) return false;
  return isl.coast(x / 8, y / 8) < Math.min(380, townR * 0.32);
}

export function planCellNetwork(world, i, j) {
  const { arterials, fields, config } = world;
  const seed = config.seed;
  // a wrapping world: the cell's canonical id seeds everything in it
  const id = `C${arterials.canon(i)}_${arterials.canon(j)}`;
  // (and positions that seed anything are taken relative to the canonical cell)
  const lapX = arterials.line(0, i) - arterials.lineAt(0, arterials.canon(i));
  const lapY = arterials.line(1, j) - arterials.lineAt(1, arterials.canon(j));
  const rect = arterials.cellRect(i, j);
  const specs = roadSpecs(config);
  const rng = Rng.from(seed, id, "network");
  const roads = [];
  const blocks = [];
  const subcells = [];

  const angled = angledRoads(config);
  let roadCount = 0;
  // (island: a road over the sea keeps its record and id but is not built)
  const makeRoad = (cls, pts, extra = {}, build = true) => {
    const spec = specs[cls];
    const road = {
      id: `${id}/r${roadCount++}`,
      cell: id,
      cls,
      pts,
      hc: spec.hc,
      hr: spec.hr,
      corner: spec.corner,
      median: spec.median,
      parking: spec.parking,
      lanes: spec.lanes,
      lane: spec.lane,
      sidewalk: spec.sidewalk,
      shoulder: spec.shoulder,
      // (the angled world: the cell that owns it, whose road view sees every road it meets, roadLevel.js)
      ...(angled ? { home: [i, j] } : {}),
      ...extra,
    };
    if (build) roads.push(road);
    return road;
  };

  const edges = {
    W: edgeInfo(world, 0, i, j),
    E: edgeInfo(world, 0, i + 1, j),
    N: edgeInfo(world, 1, j, i),
    S: edgeInfo(world, 1, j + 1, i),
  };
  for (const key of ["W", "N"]) {
    const e = edges[key];
    if (e.cls) makeRoad(e.cls, e.pts, { arterialEdge: key, ...(e.paving ? { paving: e.paving } : {}) });
  }

  // diagonal boulevards (city/diagonals.js): the pieces of the global lines
  // crossing this cell, built where they run through a town
  const diagonals = [];
  if (diagonalsOn(config)) {
    for (const d of diagonalPieces(config, rect)) {
      let best = null;
      for (let q = 0; q <= 4; q += 1) {
        const ur = fields.urban(d.a.x + ((d.b.x - d.a.x) * q) / 4, d.a.y + ((d.b.y - d.a.y) * q) / 4);
        if (!best || ur.u > best.u) best = ur;
      }
      if (best.u < DIAGONAL_U || !best.settlement || best.settlement.village || world.seaHitsSeg(d.a.x, d.a.y, d.b.x, d.b.y, 4)) continue;
      const flavor = flavorOf(best.settlement);
      const mx = (d.a.x + d.b.x) / 2;
      const my = (d.a.y + d.b.y) / 2;
      const cobble = flavor.cobbleWithin && fields.settlementDistance(best.settlement, mx, my) < flavor.cobbleWithin;
      const road = makeRoad(flavor.mainRoad ?? config.city.mainRoad ?? "arterial", [{ ...d.a }, { ...d.b }], { diagonal: { f: d.fam.f, k: d.k }, ...(cobble ? { paving: "cobble" } : {}) });
      diagonals.push({ ...d, road, dx: d.b.x - d.a.x, dy: d.b.y - d.a.y });
    }
  }

  // collectors split urban cells into up to four sub-cells
  const cx = (rect.x0 + rect.x1) / 2;
  const cy = (rect.y0 + rect.y1) / 2;
  const centerUrban = fields.urban(cx, cy);
  const w = rect.x1 - rect.x0;
  const h = rect.y1 - rect.y0;
  const xs = [rect.x0];
  const ys = [rect.y0];
  const thr = config.city.collectorUrbanThreshold;
  let colX = null;
  let colY = null;
  // villages keep their organic streets: no collector grid (nor do towns whose flavor says so)
  if (centerUrban.u > thr && !centerUrban.settlement?.village && flavorOf(centerUrban.settlement).collectors !== false) {
    if (w > vx(380)) {
      colX = Math.round((rect.x0 + w * (0.5 + (rng.next() - 0.5) * 0.24)) / 8) * 8;
      xs.push(colX);
    }
    if (h > vx(380)) {
      colY = Math.round((rect.y0 + h * (0.5 + (rng.next() - 0.5) * 0.24)) / 8) * 8;
      ys.push(colY);
    }
  }
  // a village centred in this cell gets its main street through the centre
  // (and a cross street when it is large): the houses gather around it
  let colCls = "collector";
  if (colX === null && colY === null) {
    for (const v of fields.nearestVillages(cx, cy)) {
      const m = vx(70);
      if (v.x < rect.x0 + m || v.x > rect.x1 - m || v.y < rect.y0 + m || v.y > rect.y1 - m) continue;
      const alongY = hashFloat(seed, v.cx ?? v.x, v.cy ?? v.y, 993) < 0.5;
      const big = v.radius > vx(420);
      if (alongY || big) {
        colX = Math.round(v.x / 8) * 8;
        xs.push(colX);
      }
      if (!alongY || big) {
        colY = Math.round(v.y / 8) * 8;
        ys.push(colY);
      }
      colCls = "village";
      break;
    }
  }
  xs.push(rect.x1);
  ys.push(rect.y1);
  // (a bending edge road: its blocks keep clear of the whole bend)
  const edgeSide = (e) => ({ cls: e.cls, hr: e.hr + (e.cls ? e.wob : 0), id: null });
  const roadSide = (r) => ({ cls: r.cls, hr: r.hr, id: r.id });
  // (island: a collector that would run out over the sea is not built; its sub-cells keep it as their side)
  // (the angled world: a village's own main street bends gently through it)
  const bend = (ax, ay, bx, by) =>
    ax === bx
      ? wobble(world, 0, arterials.canon(i) * 131 + arterials.canon(j) * 7 + 5000, ax, ay, by, ANGLED_WOBBLE.main, lapY)
      : wobble(world, 1, arterials.canon(i) * 131 + arterials.canon(j) * 7 + 6000, ay, ax, bx, ANGLED_WOBBLE.main, lapX);
  const collector = (ax, ay, bx, by) =>
    makeRoad(colCls, angled && colCls === "village" ? bend(ax, ay, bx, by) : [{ x: ax, y: ay }, { x: bx, y: by }], {}, !world.seaHitsSeg(ax, ay, bx, by, 3));

  // how the collectors divide a town cell: a plain cross, a cross whose
  // north-south street jogs where it meets the other, a T (one collector
  // stops at the other), or a single collector; not every cell is four
  // equal squares
  const subs = [];
  let colXRoad = null;
  let colYRoad = null;
  const layoutPick = colX !== null && colY !== null && colCls === "collector" ? rng.next() : -1;
  if (layoutPick >= 0.3) {
    colYRoad = collector(rect.x0, colY, rect.x1, colY);
    const side = { W: edgeSide(edges.W), E: edgeSide(edges.E), N: edgeSide(edges.N), S: edgeSide(edges.S) };
    const row = (y0, y1, x, road, top) => {
      const ns = top ? { N: side.N, S: roadSide(colYRoad) } : { N: roadSide(colYRoad), S: side.S };
      if (x === null) subs.push({ rect: { x0: rect.x0, y0, x1: rect.x1, y1 }, sides: { W: side.W, E: side.E, ...ns } });
      else {
        subs.push({ rect: { x0: rect.x0, y0, x1: x, y1 }, sides: { W: side.W, E: roadSide(road), ...ns } });
        subs.push({ rect: { x0: x, y0, x1: rect.x1, y1 }, sides: { W: roadSide(road), E: side.E, ...ns } });
      }
    };
    if (layoutPick < 0.62) {
      // a jog: the street continues 30-90 m to one side
      const off = vx(30 + 60 * rng.next()) * (rng.chance(0.5) ? 1 : -1);
      const xb = Math.round(Math.max(rect.x0 + w * 0.28, Math.min(rect.x1 - w * 0.28, colX + off)) / 8) * 8;
      const top = collector(colX, rect.y0, colX, colY);
      const bot = collector(xb, colY, xb, rect.y1);
      colXRoad = top;
      row(rect.y0, colY, colX, top, true);
      row(colY, rect.y1, xb, bot, false);
    } else if (layoutPick < 0.88) {
      // a T: the north-south collector stops at the other one (north or south half)
      const north = rng.chance(0.5);
      const stub = north ? collector(colX, rect.y0, colX, colY) : collector(colX, colY, colX, rect.y1);
      colXRoad = stub;
      row(rect.y0, colY, north ? colX : null, stub, true);
      row(colY, rect.y1, north ? null : colX, stub, false);
    } else {
      // a single east-west collector
      row(rect.y0, colY, null, null, true);
      row(colY, rect.y1, null, null, false);
    }
  } else {
    colXRoad = colX !== null ? collector(colX, rect.y0, colX, rect.y1) : null;
    colYRoad = colY !== null ? collector(rect.x0, colY, rect.x1, colY) : null;
    for (let b = 0; b < ys.length - 1; b += 1)
      for (let a = 0; a < xs.length - 1; a += 1)
        subs.push({
          rect: { x0: xs[a], y0: ys[b], x1: xs[a + 1], y1: ys[b + 1] },
          sides: {
            W: a === 0 ? edgeSide(edges.W) : roadSide(colXRoad),
            E: a === xs.length - 2 ? edgeSide(edges.E) : roadSide(colXRoad),
            N: b === 0 ? edgeSide(edges.N) : roadSide(colYRoad),
            S: b === ys.length - 2 ? edgeSide(edges.S) : roadSide(colYRoad),
          },
        });
  }

  // district of a point from its urban sample (macro fields + the settlement's flavor)
  const districtAt = (x, y, ur, key) => {
    const dn = fields.districtNoise(x, y);
    const ind = fields.industryNoise(x, y);
    const base = classifyDistrict({
      u: ur.u,
      core: ur.core,
      village: !!ur.settlement?.village,
      port: ur.u > 0.12 && portNear(world, x, y),
      dn,
      ind,
      seed,
      key,
    });
    return flavoredDistrictId(base, ur.settlement, ur.settlement ? { d: fields.settlementDistance(ur.settlement, x, y), dn, ind, u: ur.u, core: ur.core } : null);
  };
  const infinite = config.world.mode === "infiniteCity";

  let subIndex = 0;
  {
    for (const { rect: subRect, sides } of subs) {
      const sx = (subRect.x0 + subRect.x1) / 2;
      const sy = (subRect.y0 + subRect.y1) / 2;
      const ur = fields.urban(sx, sy);
      const subId = `${id}/s${subIndex}`;
      // island: a sub-cell that is mostly sea stays sea
      if (fields.island && world.seaShare(subRect) > 0.5) {
        subcells.push({ id: subId, rect: subRect, sides, district: "sea", u: ur.u, core: ur.core, settlement: ur.settlement?.id ?? null });
        subIndex += 1;
        continue;
      }
      // a sub-cell of open country on the edge of a town (its centre rural,
      // the town reaching into a corner of it) is laid out like the town's
      // outskirts there; its blocks are kept only where the town reaches
      let pu = ur;
      let px = sx;
      let py = sy;
      let fringe = false;
      if (ur.u < 0.14 && !ur.settlement?.village && !infinite) {
        const qs = [
          [subRect.x0 + 24, subRect.y0 + 24],
          [subRect.x1 - 24, subRect.y0 + 24],
          [subRect.x0 + 24, subRect.y1 - 24],
          [subRect.x1 - 24, subRect.y1 - 24],
          [sx, subRect.y0 + 24],
          [sx, subRect.y1 - 24],
          [subRect.x0 + 24, sy],
          [subRect.x1 - 24, sy],
        ];
        for (const [qx, qy] of qs) {
          const q = fields.urban(qx, qy);
          if (q.settlement && !q.settlement.village && q.u > 0.17 && q.u > pu.u) {
            pu = q;
            px = qx;
            py = qy;
            fringe = true;
          }
        }
      }
      const districtId = districtAt(px, py, pu, hashString(subId));
      const district0 = DISTRICTS.get(districtId);
      // a flavor may lay a district out in another street pattern (organic
      // old northern towns; `adaptive`: the whole town grows organically)
      const fl = pu.settlement && !pu.settlement.village ? flavorOf(pu.settlement) : null;
      // (a sub-cell the historic core reaches into grows organically too, block by block)
      let nearCore = false;
      if (fl?.oldCore && pu.settlement) {
        const S = pu.settlement;
        const qx = Math.max(subRect.x0, Math.min(subRect.x1, S.x));
        const qy = Math.max(subRect.y0, Math.min(subRect.y1, S.y));
        nearCore = fields.settlementDistance(S, qx, qy) < fl.oldCore;
      }
      const adaptive = !!fl && (fl.adaptive || nearCore);
      const pat = fl ? fl.patterns?.[districtId] ?? (adaptive && PATTERN_FAMILY[district0.streets.pattern] !== "none" ? "organic" : null) : null;
      const district = pat && pat !== district0.streets.pattern ? { ...district0, streets: { ...district0.streets, pattern: pat } } : district0;
      const sub = { id: subId, rect: subRect, sides, district: districtId, u: pu.u, core: pu.core, settlement: pu.settlement?.id ?? null, ...(fringe ? { fringe: true } : {}) };
      subcells.push(sub);
      const subRng = Rng.from(seed, subId, "streets");
      const strip = STRIP_BY_DISTRICT[districtId] ?? "pits";
      const paving = district.streets.paving ?? null;
      // the pattern's streets and blocks wait until every block has been
      // looked at: a street is built only where it serves a kept block
      const pendingRoads = [];
      const pendingBlocks = [];
      const emit = {
        road: (cls, ax, ay, bx, by, pav = paving) => {
          const r = makeRoad(cls, [{ x: ax, y: ay }, { x: bx, y: by }], { sub: subId, strip, ...(pav && cls !== "collector" ? { paving: pav } : {}) }, false);
          r.sea = world.seaHitsSeg(ax, ay, bx, by, 3);
          pendingRoads.push(r);
          return r;
        },
        // (a polygon block of the angled world: its bounding rect and sides, and the polygon, blockPoly.js)
        block: (r, bs, poly = null) => pendingBlocks.push(poly ? { r, s: bs, poly } : { r, s: bs }),
        // (adaptive patterns: the district of a part of the sub-cell, laid out organically)
        local: adaptive
          ? (r) => {
              // (the most urban of the centre and the corners: a part the town
              // only reaches into is still split, its blocks kept where built)
              let cx = (r.x0 + r.x1) / 2;
              let cy = (r.y0 + r.y1) / 2;
              let lu = fields.urban(cx, cy);
              if (lu.u < 0.25)
                for (const [qx, qy] of [[r.x0 + 16, r.y0 + 16], [r.x1 - 16, r.y0 + 16], [r.x0 + 16, r.y1 - 16], [r.x1 - 16, r.y1 - 16]]) {
                  const q = fields.urban(qx, qy);
                  if (q.u > lu.u + 0.05 && q.settlement && !q.settlement.village) [cx, cy, lu] = [qx, qy, q];
                }
              if (lu.u < 0.1) return DISTRICTS.get("rural");
              const d = DISTRICTS.maybe(districtAt(cx, cy, lu, hashString(`${subId}/${r.x0 - lapX},${r.y0 - lapY}`)));
              // (parks come from the block programs and landmarks: never a whole cell of lawn)
              if (!d || d.id === "park") return DISTRICTS.get("residential");
              return d.lots.mode === "micro" && Math.min(r.x1 - r.x0, r.y1 - r.y0) < vx(120) ? DISTRICTS.get("residential") : d;
            }
          : null,
      };
      // (the angled world: old-town cuts tilt now and then, except where a
      // diagonal boulevard runs through the sub-cell: no shallow crossings)
      const crossed = diagonals.some((d) => lineCrosses(rectPoly(subRect, sides), d.a, d.dx, d.dy));
      STREET_PATTERNS[district.streets.pattern](sub, district, subRng, emit, angled ? { angled: true, tilt: crossed ? 0 : 0.35 } : undefined);
      // a diagonal boulevard splits the blocks it runs through along its centre line
      for (const d of diagonals) {
        const next = [];
        for (const pb of pendingBlocks) {
          const poly = pb.poly ?? rectPoly(pb.r, pb.s);
          if (!lineCrosses(poly, d.a, d.dx, d.dy)) {
            next.push(pb);
            continue;
          }
          for (const child of splitPoly(poly, d.a, d.dx, d.dy, roadSide(d.road))) {
            if (!child) continue;
            const b = polyBlock(child);
            next.push(b.cuts.length ? { r: b.r, s: b.s, poly: child } : { r: b.r, s: b.s });
          }
        }
        pendingBlocks.length = 0;
        pendingBlocks.push(...next);
      }
      const family = PATTERN_FAMILY[district.streets.pattern];
      const kept = [];
      for (const pb of pendingBlocks) {
        const r = pb.r;
        // (island: blocks that are mostly sea are left to the sea)
        if (fields.island && world.seaShare(r) > 0.5) continue;
        let dId = districtId;
        if (family !== "none" && (!district.port || adaptive)) {
          // (a polygon block: its own middle, not its bounding rect's)
          const mid = pb.poly ? polyCentroid(pb.poly) : null;
          const bx = mid ? mid.x : (r.x0 + r.x1) / 2;
          const by = mid ? mid.y : (r.y0 + r.y1) / 2;
          const bu = fields.urban(bx, by);
          const key = hashString(`${subId}/${r.x0 - lapX},${r.y0 - lapY}`);
          // the town's edge frays: out on the rim a block is built only where
          // the town reaches (fields, meadows and woods take the rest)
          if (!infinite && bu.u < 0.6) {
            const reach = bu.u + 0.2 * fields.fringeNoise(bx, by) + 0.12 * ((key % 1000) / 1000 - 0.5);
            if (reach < 0.2 || bu.settlement?.village) continue;
          }
          // each block takes the district of its own place in the town (not
          // a park or open country, and superblocks only on big blocks)
          const own = districtAt(bx, by, bu, key);
          const od = DISTRICTS.maybe(own);
          const small = Math.min(r.x1 - r.x0, r.y1 - r.y0) < vx(70);
          if (od && own !== "rural" && PATTERN_FAMILY[od.streets.pattern] !== "none" && !(od.lots.mode === "micro" && small)) dId = own;
        }
        kept.push({ ...pb, district: dId });
      }
      // build the streets that front kept blocks, trimmed to the stretch they serve
      const extent = new Map();
      const grow = (rid, lo, hi) => {
        const e = extent.get(rid);
        if (e) {
          e[0] = Math.min(e[0], lo);
          e[1] = Math.max(e[1], hi);
        } else extent.set(rid, [lo, hi]);
      };
      // (a slanted street's stretch is measured along it, from its first point)
      const byId = new Map(pendingRoads.map((r) => [r.id, r]));
      const along = (r, x, y) => {
        const [p, q] = r.pts;
        return ((x - p.x) * (q.x - p.x) + (y - p.y) * (q.y - p.y)) / Math.hypot(q.x - p.x, q.y - p.y);
      };
      for (const k of kept) {
        if (k.poly) {
          // a polygon block: the stretch of each edge's street it really fronts
          // (measured the way that street is trimmed: along x, along y, or along itself)
          for (let e = 0; e < k.poly.length; e += 1) {
            const p = k.poly[e];
            const q = k.poly[(e + 1) % k.poly.length];
            const r = byId.get(p.side?.id);
            if (!r) continue;
            const [ra, rb] = r.pts;
            if (ra.y === rb.y) grow(r.id, Math.min(p.x, q.x), Math.max(p.x, q.x));
            else if (ra.x === rb.x) grow(r.id, Math.min(p.y, q.y), Math.max(p.y, q.y));
            else {
              const a = along(r, p.x, p.y);
              const b = along(r, q.x, q.y);
              grow(r.id, Math.min(a, b), Math.max(a, b));
            }
          }
          continue;
        }
        for (const side of ["N", "S", "W", "E"]) {
          const rid = k.s[side]?.id;
          if (!rid) continue;
          const horizontal = side === "N" || side === "S";
          grow(rid, horizontal ? k.r.x0 : k.r.y0, horizontal ? k.r.x1 : k.r.y1);
        }
      }
      for (const r of pendingRoads) {
        const e = extent.get(r.id);
        if (!e || r.sea) continue;
        const [p, q] = r.pts;
        if (p.x !== q.x && p.y !== q.y) {
          // a slanted street (the angled world): trimmed along itself
          const L = Math.hypot(q.x - p.x, q.y - p.y);
          const t0 = Math.max(0, e[0]);
          const t1 = Math.min(L, e[1]);
          if (t1 - t0 < 1) continue;
          const ux = (q.x - p.x) / L;
          const uy = (q.y - p.y) / L;
          if (t0 > 0 || t1 < L) r.pts = [{ x: p.x + ux * t0, y: p.y + uy * t0 }, { x: p.x + ux * t1, y: p.y + uy * t1 }];
        } else if (p.y === q.y) {
          const x0 = Math.max(Math.min(p.x, q.x), e[0]);
          const x1 = Math.min(Math.max(p.x, q.x), e[1]);
          r.pts = [{ x: x0, y: p.y }, { x: x1, y: p.y }];
        } else {
          const y0 = Math.max(Math.min(p.y, q.y), e[0]);
          const y1 = Math.min(Math.max(p.y, q.y), e[1]);
          r.pts = [{ x: p.x, y: y0 }, { x: p.x, y: y1 }];
        }
        delete r.sea;
        roads.push(r);
      }
      for (const k of kept) {
        const r = k.r;
        const s0 = k.s;
        const bd = k.district === districtId ? district : DISTRICTS.get(k.district);
        const pushBlock = (rr, ss, poly = null) => {
          const prop = {
            x0: rr.x0 + ss.W.hr,
            y0: rr.y0 + ss.N.hr,
            x1: rr.x1 - ss.E.hr - 1,
            y1: rr.y1 - ss.S.hr - 1,
          };
          blocks.push({
            id: `${id}/b${blocks.length}`,
            cell: id,
            sub: subId,
            district: k.district,
            rect: rr,
            sides: ss,
            prop,
            u: pu.u,
            core: pu.core,
            // (a polygon block: its centre-line polygon and the property half-planes of its slanted edges)
            ...(poly ? { poly, cuts: polyBlock(poly).cuts } : {}),
          });
        };
        if (k.poly) {
          pushBlock(r, s0, k.poly);
          continue;
        }
        // service alleys split long urban blocks along their long axis
        const alleyChance = bd.lots.alleyChance ?? 0;
        const w = r.x1 - r.x0;
        const h = r.y1 - r.y0;
        const longX = w >= h;
        const longLen = longX ? w : h;
        const shortLen = longX ? h : w;
        const bRng = Rng.from(seed, subId, "alley", r.x0 - lapX, r.y0 - lapY);
        if (alleyChance > 0 && longLen > vx(80) && shortLen > vx(52) && bRng.chance(alleyChance)) {
          const alleyCls = bd.streets.laneClass ?? "alley";
          const extra = { sub: subId, strip: "none", ...(bd.streets.paving ? { paving: bd.streets.paving } : {}) };
          if (longX) {
            const ay = Math.round((r.y0 + r.y1) / 2 / 8) * 8;
            const alley = makeRoad(alleyCls, [{ x: r.x0, y: ay }, { x: r.x1, y: ay }], extra);
            const as = { cls: alleyCls, hr: alley.hr, id: alley.id };
            pushBlock({ ...r, y1: ay }, { ...s0, S: as });
            pushBlock({ ...r, y0: ay }, { ...s0, N: as });
          } else {
            const ax = Math.round((r.x0 + r.x1) / 2 / 8) * 8;
            const alley = makeRoad(alleyCls, [{ x: ax, y: r.y0 }, { x: ax, y: r.y1 }], extra);
            const as = { cls: alleyCls, hr: alley.hr, id: alley.id };
            pushBlock({ ...r, x1: ax }, { ...s0, E: as });
            pushBlock({ ...r, x0: ax }, { ...s0, W: as });
          }
          continue;
        }
        pushBlock(r, s0);
      }
      subIndex += 1;
    }
  }
  // an old town's collectors are cobbled where it lies on both sides of them
  for (const [road, isX] of [[colXRoad, true], [colYRoad, false]]) {
    if (!road) continue;
    const both = subcells.filter((sc) => (isX ? sc.rect.x0 === colX || sc.rect.x1 === colX : sc.rect.y0 === colY || sc.rect.y1 === colY));
    const pav = both.length && both.every((sc) => DISTRICTS.maybe(sc.district)?.streets.paving) ? DISTRICTS.get(both[0].district).streets.paving : null;
    if (pav) road.paving = pav;
  }

  return { id, i, j, rect, edges, roads, blocks, subcells };
}
