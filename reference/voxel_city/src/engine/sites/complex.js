import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { rSubtractAll } from "../core/rect.js";
import { makeStair, stairBoxes, stairDims, nearLanding } from "../buildings/interior/stairs.js";
import { Registry } from "../world/registry.js";

/**
 * Underground complexes (research labs, bunkers, mountain bases): a
 * reusable planner + emitter for Black Mesa / Doom style layouts.
 *
 *   sectors   each a stack of levels 15 m apart around a central stair
 *             shaft, laid out as rooms of several shapes (rect, octagon,
 *             round, cross) joined by corridors (spanning tree + loops)
 *   themes    decide room types and the big signature rooms of a sector
 *             (test chamber, reactor, hangar hall with waste channels, mess
 *             hall) and its colours (keycard frames, accent lights)
 *   atriums   a hall spanning two levels, overlooked by a catwalk ring
 *   ladders   shortcuts between levels through the floors of rooms
 *   tram      a transit loop at the bottom joining every sector's station
 *   entries   stair shafts from surface buildings down to a sector
 *
 * The emitter writes three box lists (shells: replace solid only, carves,
 * details) in world voxels; sites rasterize them through a spatial grid.
 * New themes register in COMPLEX_THEMES.
 */

export const COMPLEX_THEMES = new Registry("complexTheme");

COMPLEX_THEMES.register({
  id: "military",
  weights: [["lab", 4], ["storage", 3], ["server", 2], ["barracks", 2], ["armory", 1], ["control", 2], ["generator", 1], ["medical", 1]],
  big: (k, last) => (last ? "reactor" : "hangarHall"),
  frame: MAT.HAZARD_YELLOW,
  accent: MAT.EMERGENCY_RED,
  shapes: 0,
});
COMPLEX_THEMES.register({
  id: "lab",
  weights: [["lab", 5], ["cleanroom", 2], ["server", 2], ["office", 2], ["containment", 2], ["storage", 1], ["control", 1]],
  big: (k) => (k === 0 ? "atrium" : "testChamber"),
  frame: MAT.SIGN_BLUE,
  accent: MAT.NEON_CYAN,
  shapes: 0.45,
});
COMPLEX_THEMES.register({
  id: "power",
  weights: [["generator", 3], ["control", 2], ["storage", 2], ["server", 1], ["office", 1]],
  big: (k, last) => (last ? "reactor" : "hangarHall"),
  frame: MAT.HAZARD_YELLOW,
  accent: MAT.EMERGENCY_RED,
  shapes: 0.25,
});
COMPLEX_THEMES.register({
  id: "barracks",
  weights: [["barracks", 4], ["mess", 2], ["armory", 2], ["medical", 1], ["office", 1], ["storage", 1]],
  big: (k) => (k === 0 ? "atrium" : "messHall"),
  frame: MAT.SIGN_GREEN,
  accent: MAT.LIGHT_STRIP,
  shapes: 0.2,
});
COMPLEX_THEMES.register({
  id: "hangar",
  weights: [["storage", 3], ["armory", 2], ["control", 1], ["generator", 1], ["office", 1]],
  big: () => "hangarHall",
  frame: MAT.HAZARD_YELLOW,
  accent: MAT.EMERGENCY_RED,
  shapes: 0.1,
});
COMPLEX_THEMES.register({
  id: "containment",
  weights: [["containment", 4], ["lab", 2], ["security", 2], ["control", 1], ["medical", 1]],
  big: (k) => (k === 0 ? "testChamber" : "atrium"),
  frame: MAT.SIGN_RED,
  accent: MAT.CRYSTAL_VIOLET,
  shapes: 0.5,
});

export const LEVEL_GAP = vx(15);
const CW = vx(3); // corridor width

const ROOM_STYLE = {
  lab: { wall: MAT.PANEL_WHITE, floor: MAT.FLOOR_EPOXY, h: 32 },
  cleanroom: { wall: MAT.PANEL_WHITE, floor: MAT.FLOOR_TILE_WHITE, h: 30 },
  storage: { wall: MAT.CONCRETE, floor: MAT.FLOOR_CONCRETE, h: 32 },
  server: { wall: MAT.METAL_PANEL_DARK, floor: MAT.FLOOR_TILE_DARK, h: 30 },
  barracks: { wall: MAT.PAINT_SAGE, floor: MAT.FLOOR_LINOLEUM, h: 28 },
  mess: { wall: MAT.PAINT_CREAM, floor: MAT.FLOOR_TILE_GRAY, h: 30 },
  messHall: { wall: MAT.PAINT_CREAM, floor: MAT.FLOOR_TILE_GRAY, h: 40 },
  armory: { wall: MAT.METAL_PANEL, floor: MAT.FLOOR_CONCRETE, h: 28 },
  security: { wall: MAT.METAL_PANEL, floor: MAT.FLOOR_TILE_DARK, h: 28 },
  control: { wall: MAT.PANEL_GRAPHITE, floor: MAT.FLOOR_TILE_DARK, h: 30 },
  office: { wall: MAT.PAINT_WHITE, floor: MAT.FLOOR_CARPET_GRAY, h: 28 },
  generator: { wall: MAT.CORRUGATED, floor: MAT.FLOOR_EPOXY, h: 40 },
  medical: { wall: MAT.WALL_TILE_WHITE, floor: MAT.FLOOR_TILE_WHITE, h: 28 },
  containment: { wall: MAT.METAL_PANEL_DARK, floor: MAT.FLOOR_EPOXY, h: 32 },
  reactor: { wall: MAT.METAL_PANEL_DARK, floor: MAT.FLOOR_EPOXY, h: 64 },
  hangarHall: { wall: MAT.CONCRETE_DARK, floor: MAT.FLOOR_CONCRETE, h: 56 },
  testChamber: { wall: MAT.PANEL_GRAPHITE, floor: MAT.FLOOR_EPOXY, h: 96 },
  atrium: { wall: MAT.PANEL_WHITE, floor: MAT.FLOOR_TERRAZZO, h: LEVEL_GAP + 40 },
  atriumTop: { wall: MAT.PANEL_WHITE, floor: MAT.GRATE_STEEL, h: 40 },
  hall: { wall: MAT.METAL_PANEL, floor: MAT.FLOOR_EPOXY, h: 28 },
  station: { wall: MAT.TUNNEL_TILE, floor: MAT.FLOOR_TERRAZZO, h: 44 },
};

export function segRect(x0, y0, x1, y1, w) {
  const h = Math.floor(w / 2);
  return { x0: Math.min(x0, x1) - h, y0: Math.min(y0, y1) - h, x1: Math.max(x0, x1) + h, y1: Math.max(y0, y1) + h };
}

const overlaps = (a, b, pad = 0) => a.x0 - pad <= b.x1 && b.x0 <= a.x1 + pad && a.y0 - pad <= b.y1 && b.y0 <= a.y1 + pad;

/** The floor plan of a room as rects (octagons and circles as rows). */
export function shapeRects(q) {
  if (q.rects) return q.rects;
  const w = q.x1 - q.x0 + 1;
  const h = q.y1 - q.y0 + 1;
  let out;
  if (q.shape === "octagon") {
    const c = Math.max(4, Math.round(Math.min(w, h) * 0.27));
    out = [{ x0: q.x0, y0: q.y0 + c, x1: q.x1, y1: q.y1 - c }];
    for (let i = 0; i < c; i += 1) {
      out.push({ x0: q.x0 + c - i, y0: q.y0 + i, x1: q.x1 - c + i, y1: q.y0 + i });
      out.push({ x0: q.x0 + c - i, y0: q.y1 - i, x1: q.x1 - c + i, y1: q.y1 - i });
    }
  } else if (q.shape === "round") {
    const cx = (q.x0 + q.x1) / 2;
    const cy = (q.y0 + q.y1) / 2;
    const rx = w / 2;
    const ry = h / 2;
    out = [];
    for (let y = q.y0; y <= q.y1; y += 1) {
      const t = (y + 0.5 - cy) / ry;
      const half = rx * Math.sqrt(Math.max(0, 1 - t * t));
      if (half < 1) continue;
      out.push({ x0: Math.round(cx - half), y0: y, x1: Math.round(cx + half) - 1, y1: y });
    }
  } else if (q.shape === "cross") {
    const bw = Math.round(w * 0.2);
    const bh = Math.round(h * 0.2);
    out = [
      { x0: q.x0, y0: q.y0 + bh, x1: q.x1, y1: q.y1 - bh },
      { x0: q.x0 + bw, y0: q.y0, x1: q.x1 - bw, y1: q.y0 + bh - 1 },
      { x0: q.x0 + bw, y0: q.y1 - bh + 1, x1: q.x1 - bw, y1: q.y1 },
    ];
  } else out = [{ x0: q.x0, y0: q.y0, x1: q.x1, y1: q.y1 }];
  q.rects = out;
  return out;
}

function shrink(q, d) {
  return { ...q, x0: q.x0 + d, y0: q.y0 + d, x1: q.x1 - d, y1: q.y1 - d, rects: undefined };
}

// ------------------------------------------------------------ planning

/**
 * Stair stack through a list of walking levels (U-stair with virtual
 * stories of ~30 voxels; every real level is a landing with an opening).
 */
export function mkShaft(rect, levelsZ, dir = 1, openTop = false) {
  const zs = levelsZ.slice().sort((a, b) => a - b);
  const flights = [];
  let f = 0;
  for (let g = 0; g + 1 < zs.length; g += 1) {
    const rise = zs[g + 1] - zs[g];
    const n = Math.max(1, Math.round(rise / 30));
    let z = zs[g];
    for (let k = 0; k < n; k += 1) {
      const h = k === n - 1 ? zs[g + 1] - z : Math.round(rise / n);
      flights.push({ f: f++, z0: z - 1, H: h });
      z += h;
    }
  }
  const st = makeStair({ rect, axis: "v", dir, laneLow: true, f0: 0, f1: f });
  st.flights = flights;
  return { rect, st, dir, openTop, zLow: zs[0], zHigh: zs[zs.length - 1], levels: zs };
}

/** Anteroom in front of a shaft's near end. */
function anteroom(sr, dir) {
  return dir > 0
    ? { x0: sr.x0 - 16, y0: sr.y0 - vx(7), x1: sr.x1 + 16, y1: sr.y0 - 2, type: "hall", fixed: true }
    : { x0: sr.x0 - 16, y0: sr.y1 + 2, x1: sr.x1 + 16, y1: sr.y1 + vx(7), type: "hall", fixed: true };
}

/**
 * Plan a complex.
 * spec: {
 *   sectors: [{ bounds, z0 (level-0 walking z), levels, theme, rooms: [min,max] }],
 *   entries: [{ rect (stair shaft), zTop (walking z at the top), sector, dir, openTop }],
 *   tram: { z } | null
 * }
 */
export function planComplex(rng, spec) {
  const sd = stairDims(30);
  const out = { sectors: [], levels: [], shafts: [], ladders: [], tram: null, bounds: null };
  const entryRects = spec.entries.map((e) => e.rect);
  for (let si = 0; si < spec.sectors.length; si += 1) {
    const S = spec.sectors[si];
    const theme = COMPLEX_THEMES.get(S.theme ?? "military");
    const b = S.bounds;
    const cx = Math.round((b.x0 + b.x1) / 2);
    const cy = Math.round((b.y0 + b.y1) / 2);
    const cRect = { x0: cx - 8, y0: cy - Math.round(sd.L / 2), x1: cx - 8 + sd.W - 1, y1: cy - Math.round(sd.L / 2) + sd.L - 1 };
    const sector = { id: si, theme: theme.id, bounds: b, center: { x: cx, y: cy }, shaftRect: cRect, levels: [], themeDef: theme };
    const entries = spec.entries.filter((e) => e.sector === si);
    const lvZ = (k) => S.z0 - k * LEVEL_GAP;
    let atrium = null;
    for (let k = 0; k < S.levels; k += 1) {
      const zf = lvZ(k);
      const rooms = [anteroom(cRect, 1)];
      if (k === 0) for (const e of entries) rooms.push(anteroom(e.rect, e.dir));
      const keepRects = [cRect, ...(k === 0 ? entries.map((e) => e.rect) : [])];
      const keep = keepRects.map((q) => ({ x0: q.x0 - 8, y0: q.y0 - 8, x1: q.x1 + 8, y1: q.y1 + 8 }));
      // an atrium opens on level k as a catwalk ring over its hall on level k + 1
      if (atrium && k === atrium.k + 1) rooms.push({ x0: atrium.rect.x0, y0: atrium.rect.y0, x1: atrium.rect.x1, y1: atrium.rect.y1, type: "atrium", shape: atrium.rect.shape, fixed: true });
      const [rMin, rMax] = S.rooms ?? [9, 14];
      const target = rng.int(rMin, rMax);
      const bigType = theme.big(k, k === S.levels - 1);
      let bigPlaced = rooms.some((r) => r.type === "atrium") && bigType === "atrium";
      for (let t = 0; t < 500 && rooms.length < target + 2; t += 1) {
        const big = !bigPlaced || (rooms.length < 3 && rng.chance(0.4));
        const type = !bigPlaced ? bigType : big ? rng.pick(["hangarHall", "generator", "storage"]) : rng.weighted(theme.weights);
        if (type === "atrium" && (k + 1 >= S.levels || atrium)) {
          bigPlaced = true;
          continue;
        }
        const sq = type === "testChamber" || type === "reactor";
        const w = sq ? vx(rng.float(26, 32)) : big ? vx(rng.float(20, 30)) : vx(rng.float(8, 16));
        const h = sq ? w : big ? vx(rng.float(16, 26)) : vx(rng.float(7, 14));
        const x0 = Math.round(rng.float(b.x0, b.x1 - w) / 8) * 8;
        const y0 = Math.round(rng.float(b.y0, b.y1 - h) / 8) * 8;
        const q = { x0, y0, x1: x0 + w - 1, y1: y0 + h - 1 };
        const pad = vx(5);
        if ([...rooms, ...keep].some((o) => overlaps(q, o, pad))) continue;
        // on levels below an atrium keep its footprint free
        if (atrium && k > atrium.k + 1 && overlaps(q, atrium.rect, pad)) continue;
        q.type = type === "atrium" ? "atriumTop" : type;
        if (type === "atrium") q.lower = lvZ(k + 1);
        if (type === "testChamber") q.shape = "octagon";
        else if (type === "atrium") q.shape = rng.chance(0.5) ? "octagon" : "rect";
        else if (type === "reactor" || type === "hangarHall" || type === "messHall") q.shape = "rect";
        else if (rng.chance(theme.shapes) && Math.min(w, h) >= vx(9)) q.shape = rng.weighted([["octagon", 3], ["round", 1.5], ["cross", 1]]);
        else q.shape = "rect";
        if (!bigPlaced) {
          bigPlaced = true;
          if (type === "atrium") atrium = { k, rect: q };
        }
        rooms.push(q);
      }
      const level = { sector: si, k, zf, rooms, corridors: [], blocked: [], keep: keepRects, theme: theme.id };
      routeCorridors(rng, level, keep);
      sector.levels.push(level);
      out.levels.push(level);
    }
    // the sector shaft links its levels (and the tram station below)
    const shaftLevels = sector.levels.map((l) => l.zf);
    if (spec.tram) shaftLevels.push(spec.tram.z);
    if (shaftLevels.length > 1) out.shafts.push(mkShaft(cRect, shaftLevels));
    for (const e of entries) out.shafts.unshift(mkShaft(e.rect, [lvZ(0), e.zTop], e.dir, e.openTop ?? true));
    planLadders(sector, out.ladders, [cRect, ...entryRects]);
    out.sectors.push(sector);
  }
  if (spec.tram) out.tram = planTram(rng, spec.tram, out.sectors);
  let bb = null;
  for (const s of out.sectors) bb = bb ? { x0: Math.min(bb.x0, s.bounds.x0), y0: Math.min(bb.y0, s.bounds.y0), x1: Math.max(bb.x1, s.bounds.x1), y1: Math.max(bb.y1, s.bounds.y1) } : { ...s.bounds };
  out.bounds = bb;
  return out;
}

/**
 * Corridors: nearest pairs of rooms first (Kruskal with union-find), each
 * joined by an L or Z route that avoids shafts and open atrium floors,
 * until every room is connected; then a few loops. Rooms that no route can
 * reach are dropped, so a level never contains a sealed room.
 */
function routeCorridors(rng, level, keep) {
  const rooms = level.rooms;
  const cen = rooms.map((q) => ({ x: Math.round((q.x0 + q.x1) / 2), y: Math.round((q.y0 + q.y1) / 2) }));
  // corridors meet an atrium's upper level at its catwalk ring (on the side
  // facing the other room), never crossing the open centre
  const port = (i, j) => {
    const q = rooms[i];
    if (q.type !== "atriumTop") return cen[i];
    const dx = cen[j].x - cen[i].x;
    const dy = cen[j].y - cen[i].y;
    if (Math.abs(dx) > Math.abs(dy)) return { x: dx > 0 ? q.x1 - 6 : q.x0 + 6, y: cen[i].y };
    return { x: cen[i].x, y: dy > 0 ? q.y1 - 6 : q.y0 + 6 };
  };
  const blockers = keep.map((q) => ({ x0: q.x0 - 6, y0: q.y0 - 6, x1: q.x1 + 6, y1: q.y1 + 6 }));
  for (const r of rooms) if (r.type === "atriumTop") blockers.push({ x0: r.x0 + 20, y0: r.y0 + 20, x1: r.x1 - 20, y1: r.y1 - 20 });
  const clear = (segs) => segs.every((g) => !blockers.some((b) => overlaps(g, b)));
  const route = (a, b) => {
    const A = port(a, b);
    const B = port(b, a);
    const routes = [
      [segRect(A.x, A.y, B.x, A.y, CW), segRect(B.x, A.y, B.x, B.y, CW)],
      [segRect(A.x, A.y, A.x, B.y, CW), segRect(A.x, B.y, B.x, B.y, CW)],
    ];
    for (const off of [-1, 1, -2, 2, -3, 3, -4, 4, -6, 6]) {
      const my = Math.round((A.y + B.y) / 2) + off * vx(12);
      routes.push([segRect(A.x, A.y, A.x, my, CW), segRect(A.x, my, B.x, my, CW), segRect(B.x, my, B.x, B.y, CW)]);
      const mx = Math.round((A.x + B.x) / 2) + off * vx(12);
      routes.push([segRect(A.x, A.y, mx, A.y, CW), segRect(mx, A.y, mx, B.y, CW), segRect(mx, B.y, B.x, B.y, CW)]);
    }
    return routes.find(clear) ?? null;
  };
  const parent = rooms.map((_, i) => i);
  const find = (i) => (parent[i] === i ? i : (parent[i] = find(parent[i])));
  const pairs = [];
  for (let a = 0; a < rooms.length; a += 1)
    for (let b = a + 1; b < rooms.length; b += 1) pairs.push([a, b, Math.abs(cen[a].x - cen[b].x) + Math.abs(cen[a].y - cen[b].y)]);
  pairs.sort((p, q) => p[2] - q[2]);
  for (const [a, b] of pairs) {
    if (find(a) === find(b)) continue;
    const r = route(a, b);
    if (!r) {
      level.blocked.push([a, b]);
      continue;
    }
    level.corridors.push(...r);
    parent[find(a)] = find(b);
  }
  // a few loops for Doom-like circulation
  for (let e = 0; e < Math.round(rooms.length * 0.3); e += 1) {
    const a = rng.int(0, rooms.length - 1);
    const b = rng.int(0, rooms.length - 1);
    if (a === b) continue;
    const r = route(a, b);
    if (r) level.corridors.push(...r);
  }
  // keep only rooms connected to the shaft's anteroom (room 0)
  const root = find(0);
  const keepRooms = rooms.filter((q, i) => find(i) === root || q.fixed);
  if (keepRooms.length !== rooms.length) {
    const dropped = rooms.filter((q) => !keepRooms.includes(q));
    level.rooms = keepRooms;
    level.corridors = level.corridors.filter((c) => !dropped.some((q) => overlaps(c, q)) || keepRooms.some((q) => overlaps(c, q)));
  }
}

/** Ladder shafts between consecutive levels of a sector, through the floor of a room. */
function planLadders(sector, ladders, shaftRects) {
  const levels = sector.levels;
  const avoid = shaftRects.map((q) => ({ x0: q.x0 - 24, y0: q.y0 - 24, x1: q.x1 + 24, y1: q.y1 + 24 }));
  const free = (r) => !r.fixed && !["reactor", "testChamber", "atrium", "atriumTop"].includes(r.type);
  for (let k = 0; k + 1 < levels.length; k += 1) {
    let best = null;
    for (const a of levels[k].rooms) {
      if (!free(a)) continue;
      for (const b of levels[k + 1].rooms) {
        if (b.fixed || b.type === "atriumTop") continue;
        const m = b.type === "reactor" || b.type === "testChamber" ? 12 : 9;
        const o = { x0: Math.max(a.x0, b.x0) + m, y0: Math.max(a.y0, b.y0) + m, x1: Math.min(a.x1, b.x1) - m, y1: Math.min(a.y1, b.y1) - m };
        if (o.x1 - o.x0 < 12 || o.y1 - o.y0 < 12) continue;
        for (const [sx0, sy0] of [[o.x0, o.y0], [o.x1 - 7, o.y0], [o.x0, o.y1 - 7], [o.x1 - 7, o.y1 - 7]]) {
          const q = { x0: sx0, y0: sy0, x1: sx0 + 7, y1: sy0 + 7 };
          // inside both room shapes (octagons, circles)
          const inA = shapeRects(a).some((r) => r.x0 <= q.x0 - 2 && r.x1 >= q.x1 + 2 && r.y0 <= q.y0 - 2 && r.y1 >= q.y0 - 2) && shapeRects(a).some((r) => r.x0 <= q.x0 - 2 && r.x1 >= q.x1 + 2 && r.y0 <= q.y1 + 2 && r.y1 >= q.y1 + 2);
          const inB = shapeRects(b).some((r) => r.x0 <= q.x0 && r.x1 >= q.x1 && r.y0 <= q.y0 && r.y1 >= q.y0) && shapeRects(b).some((r) => r.x0 <= q.x0 && r.x1 >= q.x1 && r.y0 <= q.y1 && r.y1 >= q.y1);
          if (!inA || !inB) continue;
          if (avoid.some((v) => overlaps(q, v))) continue;
          const area = (o.x1 - o.x0) * (o.y1 - o.y0);
          if (!best || area > best.area) best = { area, rect: q };
          break;
        }
      }
    }
    if (!best) continue;
    ladders.push({ rect: best.rect, zTop: levels[k].zf, zBot: levels[k + 1].zf, upper: k, lower: k + 1, sector: sector.id });
    const clearZone = { x0: best.rect.x0 - 14, y0: best.rect.y0 - 14, x1: best.rect.x1 + 14, y1: best.rect.y1 + 14 };
    (levels[k].keepClear ??= []).push(clearZone);
    (levels[k + 1].keepClear ??= []).push(clearZone);
  }
}

/**
 * Tram loop: a station under every sector's shaft (its anteroom widened
 * into a platform hall) and an L-shaped tunnel from each station to the
 * next, closing the loop.
 */
function planTram(rng, tram, sectors) {
  const z = tram.z;
  const stations = sectors.map((s) => {
    const r = s.shaftRect;
    // platform hall in front of the shaft's near end (north side), track along its far side
    const hall = { x0: r.x0 - vx(14), x1: r.x1 + vx(14), y0: r.y0 - vx(12), y1: r.y0 - 2, type: "station", fixed: true };
    return { sector: s.id, hall, track: { x: Math.round((hall.x0 + hall.x1) / 2), y: hall.y0 + vx(2.5) } };
  });
  // loop order: by angle around the centroid
  const cx = stations.reduce((a, s) => a + s.track.x, 0) / stations.length;
  const cy = stations.reduce((a, s) => a + s.track.y, 0) / stations.length;
  stations.sort((p, q) => Math.atan2(p.track.y - cy, p.track.x - cx) - Math.atan2(q.track.y - cy, q.track.x - cx));
  const TW = vx(6);
  const segs = [];
  const routes = [];
  if (stations.length > 1) {
    for (let k = 0; k < stations.length; k += 1) {
      if (stations.length === 2 && k === 1) break;
      const A = stations[k];
      const B = stations[(k + 1) % stations.length];
      // trains run along the platforms: leave and enter each station along x;
      // between rows the line swings round outside the station halls
      let pts;
      if (Math.abs(A.track.y - B.track.y) < 4) pts = [A.track, { x: B.track.x, y: A.track.y }];
      else {
        const side = (A.track.x + B.track.x) / 2 >= cx ? 1 : -1;
        const xs = side > 0 ? Math.max(A.hall.x1, B.hall.x1) + vx(12) : Math.min(A.hall.x0, B.hall.x0) - vx(12);
        pts = [A.track, { x: xs, y: A.track.y }, { x: xs, y: B.track.y }, B.track];
      }
      routes.push(pts);
      for (let q = 0; q + 1 < pts.length; q += 1) segs.push(segRect(pts[q].x, pts[q].y, pts[q + 1].x, pts[q + 1].y, TW));
    }
  }
  void rng;
  return { z, stations, segs, routes };
}

// ------------------------------------------------------------ emission

/**
 * Emit a planned complex into box lists. out: { shells, carves, details }.
 */
export function emitComplex(out, cx, rng) {
  const tramStations = new Map((cx.tram?.stations ?? []).map((s) => [s.sector, s]));
  for (const lv of cx.levels) emitLevel(out, lv, rng, cx.sectors[lv.sector]?.themeDef);
  if (cx.tram) emitTram(out, cx.tram, tramStations);
  for (const L of cx.ladders) emitLadder(out.details, L);
  for (const sh of cx.shafts) emitShaft(out.details, sh);
}

function emitLevel(out, lv, rng, theme) {
  const { shells, carves, details } = out;
  const zf = lv.zf;
  const frameMat = theme?.frame ?? MAT.HAZARD_YELLOW;
  const accent = theme?.accent ?? MAT.EMERGENCY_RED;
  const push = (list, q, z0, z1, m, mode = 0) => list.push({ x0: q.x0, y0: q.y0, x1: q.x1, y1: q.y1, z0, z1, m, mode });
  const cut = lv.rooms.flatMap((q) => shapeRects(q));
  // corridors run centre to centre; only the pieces between rooms are built
  const pieces = rSubtractAll(lv.corridors, cut).filter((c) => c.x1 >= c.x0 && c.y1 >= c.y0);
  for (const q of lv.rooms) {
    const st = ROOM_STYLE[q.type] ?? ROOM_STYLE.hall;
    const rects = shapeRects(q);
    const zBase = q.type === "atriumTop" ? q.lower : zf;
    for (const r of rects) {
      push(shells, { x0: r.x0 - 2, y0: r.y0 - 2, x1: r.x1 + 2, y1: r.y1 + 2 }, zBase - 3, zf + st.h + 2, st.wall, 2);
      push(carves, r, zBase + 1, zf + st.h, 0);
      if (q.type !== "atriumTop") push(details, r, zf, zf, st.floor);
      // wall band and ceiling lights
      push(details, { x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 }, zf + 1, zf + 1, q.type === "testChamber" ? MAT.HAZARD_BLACK : MAT.HAZARD_YELLOW, 2);
      if (r.y1 - r.y0 > 4) for (let y = r.y0 + 12; y < r.y1 - 4; y += 28) for (let x = r.x0 + 6; x < r.x1 - 6; x += 20) details.push({ x0: x, y0: y, x1: x + 6, y1: y + 1, z0: zf + st.h, z1: zf + st.h, m: MAT.LIGHT_STRIP, mode: 0 });
    }
    // props, kept clear of corridor mouths and ladders
    const entries = [];
    for (const c of pieces) {
      if (!overlaps(c, q, 3)) continue;
      entries.push({ x0: Math.max(c.x0, q.x0 - 3) - 14, y0: Math.max(c.y0, q.y0 - 3) - 14, x1: Math.min(c.x1, q.x1 + 3) + 14, y1: Math.min(c.y1, q.y1 + 3) + 14 });
    }
    for (const k of lv.keepClear ?? []) entries.push(k);
    const props = [];
    roomProps(props, q, zf, rng);
    const inside = (b) => rects.some((r) => b.x0 >= r.x0 && b.x1 <= r.x1 && b.y0 >= r.y0 && b.y1 <= r.y1);
    for (const b of props) {
      if (entries.some((e) => overlaps(b, e))) continue;
      if (q.shape && q.shape !== "rect" && !inside(b)) continue;
      details.push(b);
    }
    if (q.type === "hangarHall") nukageChannel(details, q, zf);
    if (q.type === "reactor") catwalk(details, q, zf, pieces, 28);
    if (q.type === "testChamber") testChamber(details, q, zf, pieces);
    if (q.type === "atriumTop") atriumRing(details, q, zf, accent);
    if (q.type === "containment") containmentCells(details, q, zf, entries);
  }
  for (const c of pieces) {
    push(shells, { x0: c.x0 - 2, y0: c.y0 - 2, x1: c.x1 + 2, y1: c.y1 + 2 }, zf - 3, zf + 28 + 2, MAT.TUNNEL_WALL, 2);
    push(carves, c, zf + 1, zf + 26, 0);
    push(details, c, zf, zf, MAT.FLOOR_EPOXY);
    const long = c.x1 - c.x0 > c.y1 - c.y0;
    const mid = long ? Math.round((c.y0 + c.y1) / 2) : Math.round((c.x0 + c.x1) / 2);
    if (long) for (let x = c.x0 + 8; x < c.x1; x += 32) details.push({ x0: x, y0: mid, x1: x + 4, y1: mid, z0: zf + 26, z1: zf + 26, m: MAT.LIGHT_STRIP, mode: 0 });
    else for (let y = c.y0 + 8; y < c.y1; y += 32) details.push({ x0: mid, y0: y, x1: mid, y1: y + 4, z0: zf + 26, z1: zf + 26, m: MAT.LIGHT_STRIP, mode: 0 });
    // emergency / accent lamps on alternating walls
    if (long) for (let x = c.x0 + 24; x < c.x1 - 8; x += 64) details.push({ x0: x, y0: (x >> 6) & 1 ? c.y0 : c.y1, x1: x + 1, y1: (x >> 6) & 1 ? c.y0 : c.y1, z0: zf + 19, z1: zf + 20, m: accent, mode: 0 });
    else for (let y = c.y0 + 24; y < c.y1 - 8; y += 64) details.push({ x0: (y >> 6) & 1 ? c.x0 : c.x1, y0: y, x1: (y >> 6) & 1 ? c.x0 : c.x1, y1: y + 1, z0: zf + 19, z1: zf + 20, m: accent, mode: 0 });
    // keycard door frames where the corridor meets a room
    for (const q of lv.rooms) {
      if (q.fixed) continue;
      if (!overlaps(c, q, 2)) continue;
      doorFrame(details, c, q, zf, frameMat);
    }
  }
}

/** Coloured door frame across the corridor end that touches room q. */
function doorFrame(details, c, q, zf, m) {
  const push = (x0, y0, z0, x1, y1, z1, mm) => details.push({ x0, y0, z0, x1, y1, z1, m: mm, mode: 0 });
  const vertical = c.y1 - c.y0 > c.x1 - c.x0;
  // only a piece spanning the full corridor width gets a frame (subtracting
  // round rooms leaves thin slivers whose posts would wall the corridor off)
  if ((vertical ? c.x1 - c.x0 : c.y1 - c.y0) < CW - 4) return;
  if (vertical) {
    const y = c.y1 + 1 >= q.y0 - 2 && c.y1 < q.y0 ? c.y1 : c.y0 - 1 <= q.y1 + 2 && c.y0 > q.y1 ? c.y0 : null;
    if (y === null) return;
    push(c.x0, y, zf + 1, c.x0, y, zf + 22, m);
    push(c.x1, y, zf + 1, c.x1, y, zf + 22, m);
    push(c.x0, y, zf + 22, c.x1, y, zf + 24, m);
    push(c.x0 + 1, y, zf + 25, c.x1 - 1, y, zf + 26, MAT.CONCRETE_DARK);
  } else {
    const x = c.x1 + 1 >= q.x0 - 2 && c.x1 < q.x0 ? c.x1 : c.x0 - 1 <= q.x1 + 2 && c.x0 > q.x1 ? c.x0 : null;
    if (x === null) return;
    push(x, c.y0, zf + 1, x, c.y0, zf + 22, m);
    push(x, c.y1, zf + 1, x, c.y1, zf + 22, m);
    push(x, c.y0, zf + 22, x, c.y1, zf + 24, m);
    push(x, c.y0 + 1, zf + 25, x, c.y1 - 1, zf + 26, MAT.CONCRETE_DARK);
  }
}

/** Sunken channel of glowing waste across a hall with two railed bridges (2 voxels deep). */
function nukageChannel(details, q, zf) {
  const push = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  const alongX = q.x1 - q.x0 >= q.y1 - q.y0;
  const len = alongX ? q.x1 - q.x0 : q.y1 - q.y0;
  const c0 = (alongX ? q.y0 : q.x0) + 24;
  const c1 = c0 + 15;
  const R = (a0, a1, b0, b1) => (alongX ? [a0, b0, a1, b1] : [b0, a0, b1, a1]);
  const [x0, y0, x1, y1] = R(alongX ? q.x0 : q.y0, alongX ? q.x1 : q.y1, c0, c1);
  push(x0, y0, zf - 3, x1, y1, zf - 2, MAT.METAL_BLACK);
  push(x0, y0, zf - 1, x1, y1, zf - 1, MAT.NUKAGE);
  push(x0, y0, zf, x1, y1, zf, 0);
  for (const e of [c0 - 1, c1 + 1]) {
    const [a, b, c, d] = R(alongX ? q.x0 : q.y0, alongX ? q.x1 : q.y1, e, e);
    push(a, b, zf, c, d, zf, MAT.HAZARD_YELLOW);
  }
  const base = alongX ? q.x0 : q.y0;
  for (const t of [Math.round(len / 3), Math.round((2 * len) / 3)]) {
    const [bx0, by0, bx1, by1] = R(base + t - 6, base + t + 5, c0, c1);
    push(bx0, by0, zf, bx1, by1, zf, MAT.GRATE_STEEL);
    push(bx0, by0, zf - 2, bx1, by1, zf - 1, MAT.STEEL_BEAM);
    for (const side of [t - 6, t + 5]) {
      const [rx0, ry0, rx1, ry1] = R(base + side, base + side, c0, c1);
      push(rx0, ry0, zf + 7, rx1, ry1, zf + 7, MAT.RAILING);
      const [px0, py0, px1, py1] = R(base + side, base + side, c0, c0);
      push(px0, py0, zf + 1, px1, py1, zf + 6, MAT.RAILING);
      const [qx0, qy0, qx1, qy1] = R(base + side, base + side, c1, c1);
      push(qx0, qy0, zf + 1, qx1, qy1, zf + 6, MAT.RAILING);
    }
  }
}

/**
 * Catwalk ring along the walls of a tall hall (any shape), reached by a
 * straight stair along the flat south (or north) wall where no corridor
 * enters; railings on the inner edge, open where the stair lands.
 */
function catwalk(details, q, zf, corridors, H) {
  const push = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  const W = 8;
  const zc = zf + H;
  const w = q.x1 - q.x0 + 1;
  const h = q.y1 - q.y0 + 1;
  const c = q.shape === "octagon" ? Math.max(4, Math.round(Math.min(w, h) * 0.27)) : 0;
  const sx = q.x0 + c + W + 4;
  const topX = sx + 2 * H;
  if (topX + 12 > q.x1 - c - W) return;
  const hits = (y0, y1) => corridors.some((k) => k.x0 <= topX + 12 && sx - 4 <= k.x1 && k.y0 <= y1 + 4 && y0 - 4 <= k.y1);
  let south = true;
  if (hits(q.y1 - W - 10, q.y1 + 3)) {
    if (hits(q.y0 - 3, q.y0 + W + 10)) return;
    south = false;
  }
  const outer = shapeRects(q);
  const inner = shapeRects(shrink(q, W));
  for (const r of rSubtractAll(outer, inner)) {
    if (r.x1 < r.x0 || r.y1 < r.y0) continue;
    push(r.x0, r.y0, zc - 1, r.x1, r.y1, zc - 1, MAT.STEEL_BEAM);
    push(r.x0, r.y0, zc, r.x1, r.y1, zc, MAT.GRATE_STEEL);
  }
  const sy0 = south ? q.y1 - W - 10 : q.y0 + W;
  const sy1 = south ? q.y1 - W : q.y0 + W + 10;
  for (let k = 1; k <= H; k += 1) {
    const x = sx + 2 * (k - 1);
    push(x, sy0, zf + 1, x + 1, sy1, zf + k, MAT.STAIR_CONCRETE);
  }
  const landing = { x0: topX, y0: sy0, x1: topX + 9, y1: sy1 };
  push(landing.x0, landing.y0, zc - 1, landing.x1, landing.y1, zc, MAT.GRATE_STEEL);
  const edgeY = south ? sy0 : sy1;
  for (let k = 1; k <= H; k += 4) push(sx + 2 * (k - 1), edgeY, zf + k + 1, sx + 2 * (k - 1), edgeY, zf + k + 7, MAT.RAILING);
  // inner railing: the 1-voxel ring just inside the walkway, gap at the landing
  const ring = rSubtractAll(inner, shapeRects(shrink(q, W + 1)));
  const gap = { x0: landing.x0 - 1, y0: landing.y0 - 3, x1: landing.x1 + 1, y1: landing.y1 + 3 };
  for (const r of rSubtractAll(ring, [gap])) {
    if (r.x1 < r.x0 || r.y1 < r.y0) continue;
    push(r.x0, r.y0, zc + 7, r.x1, r.y1, zc + 7, MAT.RAILING);
    push(r.x0, r.y0, zc + 3, r.x1, r.y1, zc + 3, MAT.RAILING);
  }
  for (let x = q.x0 + c + 16; x < q.x1 - c - 8; x += 32) push(x, q.y0 + 2, zc - 2, x + 1, q.y0 + 2, zc - 2, MAT.LAMP_CAGE);
}

/**
 * Test chamber (Black Mesa style): an octagonal hall 12 m tall around a
 * pit with a glowing anomaly on emitter pylons, a gallery ring halfway up
 * with a stair, observation slits and warning lights.
 */
function testChamber(details, q, zf, corridors) {
  const push = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  const cx = Math.round((q.x0 + q.x1) / 2);
  const cy = Math.round((q.y0 + q.y1) / 2);
  const R = Math.round(Math.min(q.x1 - q.x0, q.y1 - q.y0) * 0.16);
  // pit: carved below the floor, lined, with a hazard rim
  for (let dy = -R; dy <= R; dy += 1) {
    const half = Math.round(Math.sqrt(Math.max(0, R * R - dy * dy)));
    push(cx - half, cy + dy, zf - 60, cx + half, cy + dy, zf, 0);
    push(cx - half - 2, cy + dy, zf, cx - half - 1, cy + dy, zf, MAT.HAZARD_YELLOW);
    push(cx + half + 1, cy + dy, zf, cx + half + 2, cy + dy, zf, MAT.HAZARD_YELLOW);
  }
  push(cx - R - 3, cy - R - 3, zf - 64, cx + R + 3, cy + R + 3, zf - 61, MAT.METAL_BLACK);
  // the anomaly: a glowing crystal column floating over the pit, held by emitter arms
  for (let z = zf + 10; z <= zf + 34; z += 1) {
    const r = Math.max(1, Math.round(5 - Math.abs(z - zf - 22) / 3));
    push(cx - r, cy - r, z, cx + r, cy + r, z, (z & 3) === 0 ? MAT.CRYSTAL_VIOLET : MAT.CRYSTAL_CYAN);
  }
  for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
    const L = R + 18;
    for (let t = 8; t <= L; t += 1) push(cx + dx * t, cy + dy * t, zf + 22, cx + dx * t, cy + dy * t, zf + 23, MAT.METAL_CHROME);
    push(cx + dx * L - 2, cy + dy * L - 2, zf + 1, cx + dx * L + 2, cy + dy * L + 2, zf + 24, MAT.METAL_PANEL_DARK);
    push(cx + dx * 8, cy + dy * 8, zf + 22, cx + dx * 8, cy + dy * 8, zf + 23, MAT.NEON_CYAN);
  }
  catwalk(details, q, zf, corridors, 40);
  // warning beacons
  for (const [x, y] of [[q.x0 + 20, q.y0 + 20], [q.x1 - 20, q.y0 + 20], [q.x0 + 20, q.y1 - 20], [q.x1 - 20, q.y1 - 20]]) push(x, y, zf + 60, x + 1, y + 1, zf + 61, MAT.EMERGENCY_RED);
}

/** Upper floor of an atrium: a catwalk ring along the walls over the open hall below. */
function atriumRing(details, q, zf, accent) {
  const push = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  const W = 12;
  const outer = shapeRects(q);
  const inner = shapeRects(shrink(q, W));
  const band = rSubtractAll(outer, inner).filter((r) => r.x1 >= r.x0 && r.y1 >= r.y0);
  for (const r of band) {
    push(r.x0, r.y0, zf - 1, r.x1, r.y1, zf - 1, MAT.STEEL_BEAM);
    push(r.x0, r.y0, zf, r.x1, r.y1, zf, MAT.GRATE_STEEL);
  }
  const ring = rSubtractAll(inner, shapeRects(shrink(q, W + 1))).filter((r) => r.x1 >= r.x0 && r.y1 >= r.y0);
  for (const r of ring) {
    push(r.x0, r.y0, zf + 7, r.x1, r.y1, zf + 7, MAT.RAILING);
    push(r.x0, r.y0, zf + 3, r.x1, r.y1, zf + 3, MAT.RAILING);
  }
  // hanging accent lights over the void
  const cx = Math.round((q.x0 + q.x1) / 2);
  const cy = Math.round((q.y0 + q.y1) / 2);
  push(cx - 1, cy - 1, zf + 12, cx + 1, cy + 1, zf + 30, MAT.STEEL_BEAM);
  push(cx - 4, cy - 4, zf + 8, cx + 4, cy + 4, zf + 11, accent);
}

/** Containment cells: glass-fronted cells along one wall, some with a glowing specimen. */
function containmentCells(details, q, zf, entries) {
  const push = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  if (q.shape && q.shape !== "rect") return;
  for (let x = q.x0 + 2; x + 18 < q.x1 - 2; x += 20) {
    const cell = { x0: x, y0: q.y0, x1: x + 18, y1: q.y0 + 16 };
    if (entries.some((e) => overlaps(cell, e))) continue;
    push(x, q.y0, zf + 1, x, q.y0 + 16, zf + 26, MAT.METAL_PANEL_DARK);
    push(x + 18, q.y0, zf + 1, x + 18, q.y0 + 16, zf + 26, MAT.METAL_PANEL_DARK);
    push(x, q.y0 + 16, zf + 1, x + 18, q.y0 + 16, zf + 26, MAT.GLASS_TINT);
    if (((x >> 3) & 3) === 1) push(x + 7, q.y0 + 6, zf + 1, x + 11, q.y0 + 10, zf + 8, MAT.CRYSTAL_VIOLET);
  }
}

function roomProps(out, q, zf, rng) {
  const b = (x0, y0, z0, x1, y1, z1, m) => out.push({ x0, y0, z0: zf + z0, x1, y1, z1: zf + z1, m, mode: 0 });
  const w = q.x1 - q.x0;
  const h = q.y1 - q.y0;
  const cx = Math.round((q.x0 + q.x1) / 2);
  const cy = Math.round((q.y0 + q.y1) / 2);
  switch (q.type) {
    case "server":
      for (let y = q.y0 + 8; y + 5 < q.y1 - 6; y += 14) {
        b(q.x0 + 6, y, 1, q.x1 - 6, y + 4, 16, MAT.METAL_PANEL_DARK);
        for (let x = q.x0 + 7; x < q.x1 - 6; x += 3) b(x, y, 3 + (x % 4), x, y, 3 + (x % 4), MAT.SIGNAL_GREEN);
      }
      break;
    case "lab":
    case "cleanroom":
      for (let x = q.x0 + 6; x + 10 < q.x1 - 4; x += 18) {
        b(x, q.y0 + 2, 1, x + 10, q.y0 + 6, 6, MAT.LAMINATE_WHITE);
        b(x + 3, q.y0 + 2, 7, x + 6, q.y0 + 2, 10, MAT.SCREEN);
      }
      for (let x = q.x0 + 10; x + 8 < q.x1 - 8; x += 20) {
        b(x, cy - 4, 1, x + 8, cy + 4, 1, MAT.METAL_BLACK);
        b(x + 1, cy - 3, 2, x + 7, cy + 3, 18, q.type === "cleanroom" ? MAT.GLASS : MAT.GLASS_GREEN);
        b(x, cy - 4, 19, x + 8, cy + 4, 20, MAT.METAL_BLACK);
      }
      break;
    case "storage":
    case "armory":
      for (let k = 0; k < Math.max(3, Math.round((w * h) / 900)); k += 1) {
        const x = rng.int(q.x0 + 2, q.x1 - 10);
        const y = rng.int(q.y0 + 2, q.y1 - 10);
        b(x, y, 1, x + 7, y + 7, rng.int(4, 10), q.type === "armory" ? MAT.CONTAINER_GREEN : MAT.CARDBOARD);
      }
      break;
    case "barracks":
      for (let x = q.x0 + 2; x + 8 < q.x1; x += 12) {
        b(x, q.y0 + 1, 1, x + 7, q.y0 + 16, 3, MAT.MATTRESS);
        b(x, q.y0 + 1, 10, x + 7, q.y0 + 16, 12, MAT.MATTRESS);
        b(x, q.y0 + 1, 1, x, q.y0 + 1, 14, MAT.STEEL_BEAM);
        b(x + 7, q.y0 + 16, 1, x + 7, q.y0 + 16, 14, MAT.STEEL_BEAM);
      }
      break;
    case "mess":
    case "messHall":
      for (let y = q.y0 + 10; y + 6 < q.y1 - 6; y += 16) for (let x = q.x0 + 8; x + 20 < q.x1 - 6; x += 28) {
        b(x, y, 5, x + 20, y + 4, 5, MAT.LAMINATE_GRAY);
        b(x + 2, y - 3, 3, x + 18, y - 2, 3, MAT.METAL_BLACK);
        b(x + 2, y + 6, 3, x + 18, y + 7, 3, MAT.METAL_BLACK);
      }
      b(q.x0 + 4, q.y1 - 6, 1, q.x1 - 4, q.y1 - 2, 7, MAT.APPLIANCE_STEEL);
      break;
    case "security":
      b(cx - 10, cy - 3, 1, cx + 10, cy + 3, 7, MAT.PANEL_GRAPHITE);
      b(cx - 8, cy - 3, 8, cx + 8, cy - 3, 12, MAT.SCREEN);
      break;
    case "office":
      for (let x = q.x0 + 4; x + 10 < q.x1 - 4; x += 14) {
        b(x, cy - 3, 5, x + 10, cy + 2, 5, MAT.WOOD_MED);
        b(x + 3, cy - 3, 6, x + 6, cy - 3, 9, MAT.SCREEN);
      }
      break;
    case "control":
      for (let x = q.x0 + 4; x + 8 < q.x1 - 4; x += 10) {
        b(x, q.y0 + 4, 1, x + 8, q.y0 + 8, 6, MAT.PANEL_GRAPHITE);
        b(x + 1, q.y0 + 4, 7, x + 7, q.y0 + 4, 11, MAT.SCREEN);
      }
      b(q.x0 + 4, q.y1 - 1, 8, q.x1 - 4, q.y1 - 1, 20, MAT.SCREEN);
      break;
    case "generator":
    case "hangarHall":
      for (let k = 0; k < 3; k += 1) {
        const x = q.x0 + 8 + k * Math.round((w - 20) / 3);
        const y = cy - 8;
        b(x, y, 1, x + 14, y + 16, 14, MAT.HAZARD_YELLOW);
        b(x + 2, y + 2, 15, x + 12, y + 14, 18, MAT.PIPE);
        b(x + 6, y + 6, 19, x + 8, y + 8, 40, MAT.PIPE);
      }
      break;
    case "reactor": {
      for (let dz = 1; dz < 60; dz += 1) {
        const rr = dz < 8 ? 26 : 18;
        b(cx - rr, cy - rr * 0.4, dz, cx + rr, cy + rr * 0.4, dz, MAT.CONCRETE_DARK);
        b(cx - rr * 0.4, cy - rr, dz, cx + rr * 0.4, cy + rr, dz, MAT.CONCRETE_DARK);
        b(cx - rr * 0.75, cy - rr * 0.75, dz, cx + rr * 0.75, cy + rr * 0.75, dz, MAT.CONCRETE_DARK);
      }
      for (const dz of [14, 30, 46]) b(cx - 19, cy - 19, dz, cx + 19, cy + 19, dz, MAT.NEON_CYAN);
      for (let k = 0; k < 6; k += 1) b(q.x0 + 12, q.y0 + 12 + k * 8, 20 + k * 3, q.x1 - 12, q.y0 + 13 + k * 8, 21 + k * 3, MAT.PIPE);
      break;
    }
    case "medical":
      for (let x = q.x0 + 3; x + 8 < q.x1; x += 14) b(x, q.y0 + 2, 1, x + 7, q.y0 + 16, 4, MAT.BEDSHEET_WHITE);
      break;
    case "atrium":
      // planters and benches around a central sculpture
      b(cx - 6, cy - 6, 1, cx + 6, cy + 6, 3, MAT.PLANT_POT);
      b(cx - 4, cy - 4, 4, cx + 4, cy + 4, 10, MAT.PLANT);
      for (const [dx, dy] of [[-24, 0], [24, 0], [0, -24], [0, 24]]) b(cx + dx - 6, cy + dy - 1, 1, cx + dx + 6, cy + dy + 1, 3, MAT.WOOD_MED);
      break;
    default:
      break;
  }
}

/** Tram tunnel loop and station halls. */
function emitTram(out, tram, stations) {
  const { shells, carves, details } = out;
  const z = tram.z;
  const push = (list, q, z0, z1, m, mode = 0) => list.push({ x0: q.x0, y0: q.y0, x1: q.x1, y1: q.y1, z0, z1, m, mode });
  for (const s of stations.values()) {
    const q = s.hall;
    push(shells, { x0: q.x0 - 2, y0: q.y0 - 2, x1: q.x1 + 2, y1: q.y1 + 2 }, z - 3, z + 46, MAT.TUNNEL_TILE, 2);
    push(carves, q, z + 1, z + 44, 0);
    push(details, q, z, z, MAT.FLOOR_TERRAZZO);
    // platform edge along the track side (a hand's width from the car) and a bench row
    push(details, { x0: q.x0, y0: s.track.y + 12, x1: q.x1, y1: s.track.y + 12 }, z, z, MAT.PLATFORM_EDGE);
    for (let x = q.x0 + 12; x + 10 < q.x1 - 8; x += 28) details.push({ x0: x, y0: q.y1 - 4, x1: x + 10, y1: q.y1 - 3, z0: z + 1, z1: z + 3, m: MAT.WOOD_MED, mode: 0 });
    for (let x = q.x0 + 8; x < q.x1; x += 24) details.push({ x0: x, y0: q.y0 + 20, x1: x + 6, y1: q.y0 + 21, z0: z + 44, z1: z + 44, m: MAT.LIGHT_STRIP, mode: 0 });
    details.push({ x0: q.x0 + 30, y0: q.y1 - 1, x1: q.x0 + 44, y1: q.y1 - 1, z0: z + 16, z1: z + 22, m: MAT.SIGNAGE_BLUE, mode: 0 });
  }
  for (const c of tram.segs) {
    push(shells, { x0: c.x0 - 2, y0: c.y0 - 2, x1: c.x1 + 2, y1: c.y1 + 2 }, z - 4, z + 42, MAT.TUNNEL_WALL, 2);
    push(carves, c, z + 1, z + 40, 0);
    push(details, c, z, z, MAT.BALLAST);
  }
  for (const pts of tram.routes ?? []) emitTrack(details, pts, z);
  // a tram car waiting at the first station
  const first = [...stations.values()][0];
  if (first && tram.segs.length) {
    const t = first.track;
    const b = (x0, y0, z0, x1, y1, z1, m) => details.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
    b(t.x - 60, t.y - 10, z + 3, t.x + 60, t.y + 10, z + 24, MAT.PANEL_WHITE);
    b(t.x - 58, t.y - 10, z + 14, t.x + 58, t.y - 10, z + 20, MAT.GLASS_TINT);
    b(t.x - 58, t.y + 10, z + 14, t.x + 58, t.y + 10, z + 20, MAT.GLASS_TINT);
    b(t.x - 60, t.y - 10, z + 8, t.x + 60, t.y - 10, z + 9, MAT.SIGN_BLUE);
    b(t.x - 60, t.y + 10, z + 8, t.x + 60, t.y + 10, z + 9, MAT.SIGN_BLUE);
  }
}

/**
 * Rails (gauge 12 voxels) and sleepers along an axis-aligned polyline, with
 * the rails curving round each corner on a quarter circle (radius R), so the
 * track runs continuously from station to station.
 */
function emitTrack(details, pts, z) {
  const R = 24;
  const push = (x0, y0, x1, y1, zz, m) => details.push({ x0: Math.min(x0, x1), y0: Math.min(y0, y1), x1: Math.max(x0, x1), y1: Math.max(y0, y1), z0: zz, z1: zz, m, mode: 0 });
  const dirOf = (a, b) => [Math.sign(b.x - a.x), Math.sign(b.y - a.y)];
  for (let k = 0; k + 1 < pts.length; k += 1) {
    const a = pts[k];
    const b = pts[k + 1];
    const [dx, dy] = dirOf(a, b);
    // straight part between the curves
    const sa = k > 0 ? R : 0;
    const sb = k + 2 < pts.length ? R : 0;
    const ax = a.x + dx * sa;
    const ay = a.y + dy * sa;
    const bx = b.x - dx * sb;
    const by = b.y - dy * sb;
    if ((bx - ax) * dx + (by - ay) * dy < 0) continue;
    for (const o of [-6, 6]) push(ax - dy * o, ay + dx * o, bx - dy * o, by + dx * o, z + 1, MAT.RAIL_STEEL);
    const len = Math.abs(bx - ax) + Math.abs(by - ay);
    for (let t = 0; t <= len; t += 1) {
      const px = ax + dx * t;
      const py = ay + dy * t;
      if ((((dx ? px : py) % 5) + 5) % 5 !== 0) continue;
      push(px - dy * 9, py + dx * 9, px + dy * 9 + dx, py - dx * 9 + dy, z, MAT.RAIL_TIE);
    }
    for (let t = 8; t < len; t += 48) push(ax + dx * t - dy, ay + dy * t + dx, ax + dx * (t + 4) + dy, ay + dy * (t + 4) - dx, z + 40, MAT.LIGHT_STRIP);
  }
  // quarter-circle curves at the corners
  for (let k = 1; k + 1 < pts.length; k += 1) {
    const [d1x, d1y] = dirOf(pts[k - 1], pts[k]);
    const [d2x, d2y] = dirOf(pts[k], pts[k + 1]);
    const c = pts[k];
    const ox = c.x - d1x * R + d2x * R;
    const oy = c.y - d1y * R + d2y * R;
    const a0 = Math.atan2(c.y - d1y * R - oy, c.x - d1x * R - ox);
    const a1 = Math.atan2(c.y + d2y * R - oy, c.x + d2x * R - ox);
    let da = a1 - a0;
    if (da > Math.PI) da -= 2 * Math.PI;
    if (da < -Math.PI) da += 2 * Math.PI;
    for (const r of [R - 6, R + 6]) {
      const n = Math.ceil(Math.abs(da) * r * 1.5);
      for (let q = 0; q <= n; q += 1) {
        const ang = a0 + (da * q) / n;
        const x = Math.round(ox + Math.cos(ang) * r);
        const y = Math.round(oy + Math.sin(ang) * r);
        push(x, y, x, y, z + 1, MAT.RAIL_STEEL);
      }
    }
    for (let q = 0; q <= 5; q += 1) {
      const ang = a0 + (da * q) / 5;
      const x0 = Math.round(ox + Math.cos(ang) * (R - 9));
      const y0 = Math.round(oy + Math.sin(ang) * (R - 9));
      const x1 = Math.round(ox + Math.cos(ang) * (R + 9));
      const y1 = Math.round(oy + Math.sin(ang) * (R + 9));
      const steps = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0));
      for (let t = 0; t <= steps; t += 1) {
        const x = Math.round(x0 + ((x1 - x0) * t) / steps);
        const y = Math.round(y0 + ((y1 - y0) * t) / steps);
        push(x, y, x, y, z, MAT.RAIL_TIE);
      }
    }
  }
}

/** Ladder shaft through rock and the upper floor, rungs on the west wall, railing around the hole. */
function emitLadder(details, L) {
  const r = L.rect;
  const push = (x0, y0, z0, x1, y1, z1, m, mode = 0) => details.push({ x0, y0, z0, x1, y1, z1, m, mode });
  push(r.x0 - 2, r.y0 - 2, L.zBot + 28, r.x1 + 2, r.y1 + 2, L.zTop - 1, MAT.CONCRETE, 2);
  push(r.x0, r.y0, L.zBot + 1, r.x1, r.y1, L.zTop, 0);
  for (let z = L.zBot + 1; z <= L.zTop; z += 1) {
    push(r.x0, r.y0 + 1, z, r.x0, r.y0 + 1, z, MAT.LADDER);
    push(r.x0, r.y1 - 1, z, r.x0, r.y1 - 1, z, MAT.LADDER);
    if ((z - L.zBot) % 2 === 0) push(r.x0, r.y0 + 2, z, r.x0, r.y1 - 2, z, MAT.LADDER);
  }
  const zt = L.zTop + 1;
  push(r.x1 + 1, r.y0 - 1, zt + 6, r.x1 + 1, r.y1 + 1, zt + 6, MAT.RAILING);
  push(r.x0, r.y0 - 1, zt + 6, r.x1 + 1, r.y0 - 1, zt + 6, MAT.RAILING);
  push(r.x0, r.y1 + 1, zt + 6, r.x1 + 1, r.y1 + 1, zt + 6, MAT.RAILING);
  for (const [x, y] of [[r.x1 + 1, r.y0 - 1], [r.x1 + 1, r.y1 + 1], [r.x0, r.y0 - 1], [r.x0, r.y1 + 1], [r.x1 + 1, r.y0 + 3]]) push(x, y, zt, x, y, zt + 5, MAT.RAILING);
  push(r.x0 - 1, r.y0 - 1, L.zTop, r.x0 - 1, r.y1 + 1, L.zTop, MAT.HAZARD_YELLOW);
  push(r.x1, r.y1, L.zBot + 24, r.x1, r.y1, L.zBot + 24, MAT.LAMP_CAGE);
}

/**
 * Stair shafts are emitted last and re-assert their walls and void so
 * corridors or rooms can never cap or cut a stairwell.
 */
export function emitShaft(details, sh) {
  const r = sh.rect;
  const ringTop = sh.openTop ? sh.zHigh - 1 : sh.zHigh + 30;
  details.push({ x0: r.x0 - 2, y0: r.y0 - 2, x1: r.x1 + 2, y1: r.y1 + 2, z0: sh.zLow - 3, z1: ringTop, m: MAT.CONCRETE, mode: 0 });
  details.push({ ...r, z0: sh.zLow, z1: sh.zHigh + 26, m: 0, mode: 0 });
  for (const zl of sh.levels) {
    if (sh.openTop && zl === sh.zHigh) continue;
    if (sh.dir > 0) details.push({ x0: r.x0 + 3, y0: r.y0 - 3, x1: r.x1 - 3, y1: r.y0 - 1, z0: zl + 1, z1: zl + 18, m: 0, mode: 0 });
    else details.push({ x0: r.x0 + 3, y0: r.y1 + 1, x1: r.x1 - 3, y1: r.y1 + 3, z0: zl + 1, z1: zl + 18, m: 0, mode: 0 });
  }
  if (sh.openTop) {
    const z = sh.zHigh + 1;
    const farY = sh.dir > 0 ? r.y1 + 1 : r.y0 - 1;
    details.push({ x0: r.x0 - 1, y0: Math.min(r.y0, r.y1) - 1, x1: r.x0 - 1, y1: r.y1 + 1, z0: z, z1: z + 7, m: MAT.RAILING, mode: 0 });
    details.push({ x0: r.x1 + 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1, z0: z, z1: z + 7, m: MAT.RAILING, mode: 0 });
    details.push({ x0: r.x0 - 1, y0: farY, x1: r.x1 + 1, y1: farY, z0: z, z1: z + 7, m: MAT.RAILING, mode: 0 });
  }
  const mats = { tread: MAT.STAIR_CONCRETE, landing: MAT.STAIR_CONCRETE, divider: MAT.CONCRETE_LIGHT, rail: MAT.RAILING };
  for (const b of stairBoxes(sh.st, mats)) details.push({ ...b, mode: 0 });
  const land = nearLanding(sh.st);
  for (const fl of sh.st.flights) details.push({ ...land, z0: fl.z0 - 1, z1: fl.z0 + 1, m: MAT.STAIR_CONCRETE, mode: 0 });
  details.push({ ...land, z0: sh.zHigh - 2, z1: sh.zHigh, m: MAT.STAIR_CONCRETE, mode: 0 });
  details.push({ ...r, z0: sh.zLow - 2, z1: sh.zLow, m: MAT.FLOOR_CONCRETE, mode: 0 });
  for (let z = sh.zLow + 20; z < sh.zHigh; z += 30) details.push({ x0: r.x0 + 6, y0: r.y1, x1: r.x1 - 6, y1: r.y1, z0: z, z1: z, m: MAT.LIGHT_STRIP, mode: 0 });
}
