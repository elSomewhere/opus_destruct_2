// Stage "sitekinds": the reference's site kinds - sites/militaryBase.js, researchComplex.js,
// mountainBase.js - and the site kit (sites/kit.js) on the worlds of lib/worlds.mjs as createWorld
// makes them (with its highways and water, which keep sites away, and the terrain's harbour
// grading), on two worlds with parked vehicles (the kinds' trucks) and on a World of World.js (no
// water, no highways). First the kit's pieces on their own (fences on every side, their gates
// mid-side and at a corner, gate booths, towers, masts, radomes, dish arrays, tanks, trucks, portal
// blocks: every box). Then, over the lattice cells round the origin (and a few beyond, where a
// stronghold's flank search meets the sea, or ground rising beyond its apron): every site - its
// placement (the default level pad, or the stronghold's flank, apron, gate and service road down
// the flank), its kind's plan (gate and driveway, ring road, lots, bunker, portals, helipad,
// radomes; the stronghold's frame, cavern, yard and portal), its surface envelopes (the cell
// plan's: archetypes on its lots, the stronghold's on its cavern floor), sitesInCell, the ground at
// points over its pads and features (driveways and their centre lines, rings, helipads, aprons,
// lots, lawns, the road's centre line, edges and verges, the cut into the flank), its structure
// (boxes digested) and complex (digested), its link port toward nowhere and toward a point, the
// site source's z ranges round it, and chunks at LODs 0 to 5 over its features (the stronghold's
// cavern - floor, vault, walls - at LODs 0 to 3). Every terrain sample makes what it reads first
// (lib/worlds.mjs pureTerrain; docs/CITY.md §6): a stronghold's placement samples the terrain up
// to 5 km away along its road, and the records are what they are in any order.
//
// Every branch of the kinds and the kit runs but these, which no world reaches (none of the
// stage's, nor 8,000 more lattice cells of 16 mountain and fjord worlds searched) or which are
// dead: a stronghold's centre on mountain ground with 0.01 < u (place: the layer's urbanization
// window, u <= 0.02, is measured at nearly the same point); a stronghold building within 2 m of
// the vault, or a lot corner outside the cavern (its lots lie within 0.74 of the half extents:
// r < 0.98); a lot no envelope fits (the office and warehouse fit every stronghold's lots and the
// military base's four); no room in the yard for a truck; a research complex's or stronghold's
// portal over no sector or two over one (the portals are fixed fractions of the site: sectors 0
// and 1, and 0); a port without a tram station (both kinds plan a station per sector); and the
// cavern's hanging lamp masts (`hanger`: the cavern's centre, the site rect's, is always at x.5,
// y.5, so `la === 48 && lb === 1` never holds - dead).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, pureTerrain } from "../lib/worlds.mjs";
import { boxLines, complexLines, complexPoints, digest, groundChunk, textDigest } from "../lib/complexes.mjs";
import { envLine } from "../lib/buildings.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { World } = await import(REF + "world/World.js");
const { SiteLayer, siteSource } = await import(REF + "world/sites.js");
const K = await import(REF + "sites/kit.js");
const { Rng } = await import(REF + "core/hash.js");
const { MAT } = await import(REF + "voxel/materials.js");

/** Worlds with parked vehicles (trucks at the sites), probed round the origin only. */
export const PARKED_WORLDS = [
  ["parkedSpawnNear", '{"seed":8,"terrain":{"spawnMountains":[1,2]},"vehicles":{"parked":true}}'],
  ["parkedMountains", '{"seed":4,"terrain":{"mountainBelt":[0,0.01]},"vehicles":{"parked":true}}'],
];

/** Worlds of World.js (no createWorld: no water, no highways), probed round the origin only. */
export const BARE_WORLDS = [["bareMountains", '{"seed":4,"terrain":{"mountainBelt":[0,0.01]}}']];

/**
 * Lattice cells beyond a world's window where a stronghold's flank search meets what the window's
 * do not: the sea below its flank (islandDesert 4,5), ground rising again beyond its apron (both).
 */
export const FAR_CELLS = [
  ["islandDesert", [[4, 5]]],
  ["allMountains", [[-5, 0], [5, 1]]],
];

const R = (q) => [q.x0, q.y0, q.x1, q.y1];
const ids = (list) => list.map((s) => s.id).join(",") || "none";
const pt = (p) => `${p.x},${p.y}`;

/** The kit's pieces with scripted arguments (fences with their gate mid-side, or at a corner): every box. */
function* kitLines(r) {
  const sides = ["N", "S", "W", "E"];
  for (let k = 0; k < 16; k += 1) {
    const x0 = Math.round((r() - 0.5) * 20000);
    const y0 = Math.round((r() - 0.5) * 20000);
    const rect = { x0, y0, x1: x0 + 200 + Math.floor(r() * 900), y1: y0 + 200 + Math.floor(r() * 900) };
    const z = Math.round((r() - 0.5) * 400);
    const side = sides[k % 4];
    const ns = side === "N" || side === "S";
    const mid = ns ? Math.round((rect.x0 + rect.x1) / 2) : Math.round((rect.y0 + rect.y1) / 2);
    const at = side === "N" ? rect.y0 : side === "S" ? rect.y1 : side === "W" ? rect.x0 : rect.x1;
    const lo = k < 8 ? mid - 32 : k < 12 ? (ns ? rect.x0 : rect.y0) - 10 : (ns ? rect.x1 : rect.y1) - 54;
    const gate = ns ? { x0: lo, x1: lo + 64, y0: at, y1: at } : { y0: lo, y1: lo + 64, x0: at, x1: at };
    const skip = k < 4 ? undefined : { [sides[(k + 1) % 4]]: true };
    const out = [];
    if (skip) K.fence(out, rect, z, side, gate, 22 + (k % 3) * 2, skip);
    else K.fence(out, rect, z, side, gate);
    K.gateBooth(out, gate, side, z);
    yield line("kit", "fence", side, ...R(rect), z, skip ? Object.keys(skip)[0] : "-");
    yield* boxLines("B", out);
  }
  const x = Math.round((r() - 0.5) * 20000);
  const y = Math.round((r() - 0.5) * 20000);
  const z = Math.round((r() - 0.5) * 400);
  const rect = { x0: x, y0: y, x1: x + 640, y1: y + 480 };
  const pieces = [
    ["watchtowers", (o) => K.watchtowers(o, rect, z)],
    ["radarMast", (o) => K.radarMast(o, x, y, z)],
    ["radome", (o) => K.radome(o, x, y, z)],
    ["radome44", (o) => K.radome(o, x, y, z, 44)],
    ["dishArray", (o) => K.dishArray(o, x, y, z)],
    ["dishArray72", (o) => K.dishArray(o, x, y, z, 3, 72)],
    ["fuelTanks", (o) => K.fuelTanks(o, x, y, z)],
    ["fuelTanks2", (o) => K.fuelTanks(o, x, y, z, 2)],
  ];
  for (const [id, fn] of pieces) {
    const out = [];
    fn(out);
    yield line("kit", id, x, y, z);
    yield* boxLines("B", out);
  }
  const rng = new Rng(4242);
  for (let k = 0; k < 4; k += 1) {
    const out = [];
    K.truck(out, x + k * 40, y, z, rng);
    yield line("kit", "truck", k);
    yield* boxLines("B", out);
  }
  for (const accent of [undefined, MAT.SIGN_BLUE]) {
    const lists = { shells: [], carves: [], details: [] };
    if (accent === undefined) K.portalBlock(lists, rect, z);
    else K.portalBlock(lists, rect, z, accent);
    yield line("kit", "portalBlock", accent ?? "-");
    yield* boxLines("S", lists.shells);
    yield* boxLines("C", lists.carves);
    yield* boxLines("D", lists.details);
  }
}

function padLine(p) {
  const path = p.path ? `${p.path.length}:${p.path.map((q) => `${q.x},${q.y},${q.z}`).join(";")}` : "-";
  return line("pad", ...R(p.rect), p.z, p.margin, p.round, p.ramp ? "ramp" : "-", path, p.half, p.road);
}

function* siteLines(s) {
  const b = s.plan.bounds;
  yield line("site", s.id, s.type, s.a, s.b, s.cell.i, s.cell.j, ...R(s.cellRect), ...R(s.rect), ...R(s.blend), s.margin, s.padZ, s.seed, s.center.x, s.center.y, b ? R(b) : "-",
    R(s.placed.footprint), s.pads.length);
  yield* s.pads.map(padLine);
  const p = s.plan;
  const lots = (list) => list.map((l) => `${l.kind}/${R(l.rect).join(",")}/${l.front}/${l.style ?? "-"}`).join(" ");
  const heli = (h) => `${h.x},${h.y},${h.r}`;
  if (s.type === "mountainBase") {
    const pl = s.placed;
    const g = pl.gate;
    yield line("placed", pl.side, pl.dist, pl.L, pl.Wd, pl.H, pl.A, pl.B, R(pl.apron), R(pl.tunnel), R(pl.cavRect), `${g.u},${g.v},${g.side},${g.out.join(":")},${g.x},${g.y},${g.err}`, pl.road.length);
    const c = p.cavern;
    yield line("plan", p.flavor, p.frame.side, p.side, p.gateSide, R(p.gate), [c.cx, c.cy, c.A, c.B, c.H, c.Hw, c.zf], R(p.road), R(p.yard), R(p.portal), p.portals.length, heli(p.helipad),
      R(p.apron), R(p.tunnel), p.dist, p.themes.join(","), p.accent, p.clear.map(R).join(";"), R(p.bounds), p.levels);
  } else {
    const extra = s.type === "militaryBase" ? [R(p.bunker), R(p.apron)] : [p.portals.map(R).join(";"), p.radomes.map(pt).join(";"), pt(p.dishes), pt(p.tanks), p.themes.join(",")];
    yield line("plan", p.gateSide, R(p.gate), R(p.drive), R(p.ring), p.ringW, R(p.inner), heli(p.helipad), ...extra, R(p.bounds), p.levels);
  }
  yield line("lots", lots(p.lots));
}

/** Points (voxels) to ask the ground at, over a site's pads and features (draws from r). */
function probes(s, r) {
  const pts = [];
  const inRect = (q, n) => {
    for (let k = 0; k < n; k += 1) pts.push([Math.round(q.x0 + r() * (q.x1 - q.x0)), Math.round(q.y0 + r() * (q.y1 - q.y0))]);
  };
  const grow = (q, m) => ({ x0: q.x0 - m, y0: q.y0 - m, x1: q.x1 + m, y1: q.y1 + m });
  const p = s.plan;
  inRect(p.bounds ?? s.blend, 8);
  const h = p.helipad;
  inRect({ x0: h.x - h.r, y0: h.y - h.r, x1: h.x + h.r, y1: h.y + h.r }, 6);
  if (s.type === "mountainBase") {
    inRect(p.apron, 8);
    inRect(grow(p.apron, 176), 8);
    // the lane on the tunnel's axis, its yellow line
    const alongX = p.side === "E" || p.side === "W";
    for (let k = 0; k < 2; k += 1) {
      const t = r();
      pts.push(alongX ? [Math.round(p.apron.x0 + t * (p.apron.x1 - p.apron.x0)), Math.floor(s.center.y)] : [Math.floor(s.center.x), Math.round(p.apron.y0 + t * (p.apron.y1 - p.apron.y0))]);
    }
    // the road: its centre line, its edges and its verges
    const path = s.placed.road[0].path;
    for (let k = 0; k < 4; k += 1) {
      const m = Math.floor(r() * (path.length - 1));
      const a = path[m];
      const b = path[m + 1];
      const L = Math.hypot(b.x - a.x, b.y - a.y) || 1;
      const nx = -(b.y - a.y) / L;
      const ny = (b.x - a.x) / L;
      for (const off of [0, 27.5, -27.5, 40]) pts.push([a.x + nx * off, a.y + ny * off]);
    }
  } else {
    inRect(s.rect, 10);
    const d = p.drive;
    inRect(d, 3);
    const ns = p.gateSide === "N" || p.gateSide === "S";
    const t = r();
    pts.push(ns ? [(d.x0 + d.x1) / 2, Math.round(d.y0 + t * (d.y1 - d.y0))] : [Math.round(d.x0 + t * (d.x1 - d.x0)), (d.y0 + d.y1) / 2]);
    inRect(p.ring, 4);
    for (const l of p.lots) inRect(grow(l.rect, 40), 2);
    if (s.type === "militaryBase") {
      inRect(p.bunker, 2);
      inRect(p.apron, 2);
    } else for (const q of p.portals) inRect(grow(q, 32), 2);
  }
  return pts;
}

/** Points (x, y, z voxels) worth a chunk: the complex, the surface structures, the cavern. */
function chunkPoints(s, u) {
  const pts = complexPoints(u);
  const p = s.plan;
  const z = s.padZ;
  pts.push([Math.round(s.center.x), Math.round(s.center.y), z]);
  pts.push([Math.round((p.gate.x0 + p.gate.x1) / 2), Math.round((p.gate.y0 + p.gate.y1) / 2), z + 10]);
  pts.push([p.helipad.x, p.helipad.y, z]);
  if (s.type === "mountainBase") {
    const c = p.cavern;
    const [ax, ay] = p.frame.toWorld(p.dist, 0);
    pts.push([ax, ay, z + 20], [p.portal.x0, p.portal.y0, c.zf + 20], [p.road.x0, p.road.y0, c.zf + 30]);
  } else {
    const q = s.type === "militaryBase" ? p.bunker : p.portals[0];
    pts.push([q.x0, q.y0, z + 20], [s.rect.x0 + 6, s.rect.y0 + 6, z + 70]);
    if (s.type === "researchComplex") pts.push([p.radomes[0].x, p.radomes[0].y, z + 40], [p.dishes.x, p.dishes.y, z + 30]);
    else pts.push([p.apron.x1 - 32, p.apron.y0 + 32, z + 120]);
  }
  return pts;
}

/** A ground-filled chunk (its surface at z = surface) at (x, y, z), the site source rasterized into it. */
function* chunkLine(w, s, lod, x, y, z, surface) {
  const e = 32 << lod;
  const cx = Math.floor(x / e);
  const cy = Math.floor(y / e);
  const cz = Math.floor(z / e);
  const ch = groundChunk(lod, cx, cy, cz, surface);
  const s0 = 1 << lod;
  const bx = (cx * 32 - 1) * s0;
  const by = (cy * 32 - 1) * s0;
  const rect = { x0: bx, y0: by, x1: bx + 34 * s0 - 1, y1: by + 34 * s0 - 1 };
  const zr = siteSource.zRange(w, rect, lod);
  siteSource.rasterize(w, ch, { zMin: s.padZ - 6 });
  yield line("ch", s.id, lod, cx, cy, cz, zr ?? "-", ch.countNonAir(), digest(ch.data));
}

/** The sites of a world's lattice cells round the origin (and its cells of FAR_CELLS) and their records. */
function* worldLines(key, w, span, r) {
  const L = w.sites;
  yield line("world", key, L.cell, L.n, span);
  const cells = [];
  for (let b = -span; b <= span; b += 1) for (let a = -span; a <= span; a += 1) cells.push([a, b]);
  for (const [k, list] of FAR_CELLS) if (k === key) cells.push(...list);
  const found = [];
  for (const [a, b] of cells) {
    const s = L.siteAt(a, b);
    if (!s) {
      yield line("none", a, b);
      continue;
    }
    found.push(s);
    yield* siteLines(s);
  }
  for (const s of found) {
    // the surface envelopes (the cell plan's)
    for (const { lot, env } of s.def.surface(w, s))
      yield line("surf", lot.id, R(lot.rect), lot.front, lot.district, lot.groundZ, lot.block, lot.cell, lot.underground ?? false) + " " + envLine(env);
    yield line("cell", s.id, ids(L.sitesInCell(s.cell.i, s.cell.j)));
    // the ground over its pads and features
    const out = {};
    for (const [x, y] of probes(s, r)) {
      const nat = s.padZ + Math.round((r() - 0.5) * 160);
      const g = L.ground(x, y, nat, out);
      if (!g) {
        yield line("g", x, y, nat, "-");
        continue;
      }
      yield line("g", x, y, nat, g.z, g.mat, g.sub, g.site.id, g.site.pads.indexOf(g.pad), g.natural, g.inside, g.pad.path ? [g.pad._z, g.pad._s, g.pad._c] : "-");
    }
    // the structure and its complex
    const st = s.def.structure(w, s);
    let h = 2166136261;
    for (const q of st.boxes) h = textDigest(line(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode) + "\n", h);
    const u = s.plan.under;
    let hc = 2166136261;
    for (const l of complexLines(u)) hc = textDigest(l + "\n", hc);
    yield line("st", s.id, st.boxes.length, h, [st.bb.x0, st.bb.y0, st.bb.z0, st.bb.x1, st.bb.y1, st.bb.z1], (st.custom ?? []).length,
      `${u.sectors.length}/${u.levels.length}/${u.shafts.length}/${u.ladders.length}/${u.tram ? u.tram.stations.length : 0}`, hc);
    // its link port, toward nowhere and toward a point
    const toward = { x: Math.round(s.center.x + (r() - 0.5) * 80000), y: Math.round(s.center.y + (r() - 0.5) * 80000) };
    for (const t of [null, toward]) {
      const q = s.def.port(w, s, t);
      yield line("port", s.id, t ? pt(t) : "-", q ? [q.x, q.y, q.z, q.d] : "-");
    }
    // the site source's z ranges round it, and its chunks
    const sb = s.plan.bounds ?? s.blend;
    for (const [x, y] of [[sb.x0, sb.y0], [sb.x1, sb.y0], [sb.x0, sb.y1], [sb.x1, sb.y1], [Math.round(s.center.x), Math.round(s.center.y)]]) {
      const rect = { x0: x - 40, y0: y - 40, x1: x + 40, y1: y + 40 };
      yield line("zc", s.id, ...R(rect), siteSource.zRange(w, rect, 0) ?? "-");
    }
    const pts = chunkPoints(s, u);
    for (const lod of [0, 0, 0, 1, 2, 3, 5]) {
      const [x, y, z] = pts[Math.floor(r() * pts.length)];
      const e = 32 << lod;
      yield* chunkLine(w, s, lod, x + Math.floor((r() - 0.5) * e * 0.5), y + Math.floor((r() - 0.5) * e * 0.5), z + Math.floor((r() - 0.5) * e * 0.5), s.padZ);
    }
    if (s.type === "mountainBase") {
      // the cavern: its floor, its vault, its walls (LODs 0 to 3: the volume skips coarse ones),
      // under the mountain's rock (COVER over the vault: the volume lines and ribs only rock)
      const c = s.plan.cavern;
      const at = [
        [0, c.cx, c.cy, c.zf],
        [0, c.cx, c.cy, c.zf + c.H - 6],
        [0, c.cx + c.A * 0.6, c.cy + c.B * 0.3, c.zf + c.Hw + (c.H - c.Hw) * 0.6],
        [0, c.cx - c.A * 0.55, c.cy - c.B * 0.2, c.zf + c.H * 0.8],
        [0, c.cx - c.A, c.cy, c.zf + 4],
        [0, c.cx, c.cy + c.B, c.zf + 4],
        [0, c.cx + c.A, c.cy, c.zf + 50],
        [1, c.cx, c.cy, c.zf],
        [2, c.cx, c.cy, c.zf + c.H / 2],
        [3, c.cx, c.cy, c.zf],
      ];
      for (const [lod, x, y, z] of at) yield* chunkLine(w, s, lod, x, y, z, c.zf + c.H + 320);
    }
  }
}

export default function* sitekinds() {
  const r = samples(53);
  yield* kitLines(r);
  for (const [key, overrides] of allWorlds()) yield* worldLines(key, pureTerrain(createWorld(overrides)), 3, r);
  for (const [key, json] of PARKED_WORLDS) yield* worldLines(key, pureTerrain(createWorld(JSON.parse(json))), 1, r);
  // a World of World.js with the kinds: no highways or water to keep sites off, no isWet (a
  // stronghold's road runs on)
  for (const [key, json] of BARE_WORLDS) {
    const w = pureTerrain(new World(JSON.parse(json)));
    w.sites = new SiteLayer(w);
    yield* worldLines(key, w, 1, r);
  }
}
