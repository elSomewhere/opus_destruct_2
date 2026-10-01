import { groundTile } from "../voxel/compose.js";
import { CHUNK } from "../core/units.js";
import { P } from "../voxel/chunk.js";
import { frameOf } from "../buildings/frame.js";
import { flavorOf } from "../city/flavors.js";
import { BIOMES } from "../nature/biomes.js";
import { SNOWLINE, TREELINE } from "../nature/landcover.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";
import { buildChunk } from "../voxel/compose.js";
import { MATERIALS } from "../voxel/materials.js";
import { partsIn } from "../world/parts.js";
import { partChunk } from "../world/partRaster.js";
import { roadLevelAt } from "../network/roadLevel.js";
import { offsetAt } from "../network/highways.js";

/**
 * Read-only queries used by the viewer (map, inspector, spawn). They return
 * plain structured-cloneable data.
 */

export function mapData(world, rect) {
  const cells = world.cellsOverlapping(rect);
  const roads = [];
  const blocks = [];
  const lots = [];
  const buildings = [];
  const spaces = [];
  const subcells = [];
  for (const { i, j } of cells) {
    const plan = world.cellPlan(i, j);
    for (const r of plan.net.roads) {
      roads.push({ id: r.id, cls: r.cls, pts: r.pts.map((p) => [p.x, p.y]), hc: r.hc, hr: r.hr });
    }
    for (const s of plan.net.subcells) subcells.push({ id: s.id, rect: s.rect, district: s.district });
    for (const b of plan.net.blocks) blocks.push({ id: b.id, prop: b.prop, district: b.district });
    for (const l of plan.lots) lots.push({ id: l.id, rect: l.rect, front: l.front, building: l.building });
    for (const s of plan.spaces) spaces.push({ id: s.id, rect: s.rect, kind: s.kind });
    for (const b of plan.buildings) {
      const f = frameOf(b);
      // (a turned building of the angled world: each rect also as its turned outline, `poly`)
      const out = (r) => (f.turned ? { ...f.rectToWorld(r), poly: [f.pointToWorld(r.x0, r.y0), f.pointToWorld(r.x1 + 1, r.y0), f.pointToWorld(r.x1 + 1, r.y1 + 1), f.pointToWorld(r.x0, r.y1 + 1)] } : f.rectToWorld(r));
      buildings.push({
        id: b.id,
        archetype: b.archetype,
        style: b.style,
        floors: b.floors,
        basements: b.basements,
        height: (b.topZ - b.baseZ) / 8,
        tiers: b.tiers.map((t) => ({ f0: t.f0, f1: t.f1, rects: t.rects.map(out) })),
        annexes: b.annexes.map((a) => (a.canon ? out(a.canon) : a.world)),
        program: b.program,
      });
    }
  }
  const extra = {};
  if (world.rivers) {
    // coarse river mask (1 = channel) for the plan map
    const W = 220;
    const step = Math.max(8, Math.ceil((rect.x1 - rect.x0) / W));
    const w = Math.ceil((rect.x1 - rect.x0) / step);
    const h = Math.ceil((rect.y1 - rect.y0) / step);
    const mask = new Uint8Array(w * h);
    let any = false;
    for (let j = 0; j < h; j += 1)
      for (let i = 0; i < w; i += 1) {
        const ri = world.rivers.at(rect.x0 + (i + 0.5) * step, rect.y0 + (j + 0.5) * step);
        if (ri && ri.d < ri.half) {
          mask[i + j * w] = 1;
          any = true;
        }
      }
    if (any) extra.rivers = { x0: rect.x0, y0: rect.y0, step, w, h, mask };
  }
  extra.skybridges = cells.flatMap(({ i, j }) => (world.cellPlan(i, j).skybridges ?? []).map((b) => b.rect));
  if (world.highways) extra.highways = world.highways.mapData(rect);
  if (world.subway) extra.subway = world.subway.mapData(rect);
  if (world.sewers) extra.sewers = world.sewers.mapData(rect);
  if (world.sites) extra.sites = world.sites.mapData(rect);
  if (world.siteLinks) extra.siteLinks = world.siteLinks.mapData(rect);
  return { rect, roads, blocks, lots, buildings, spaces, subcells, ...extra };
}

export function buildingData(world, buildingId) {
  const env = world.envelope(buildingId);
  if (!env) return null;
  const plan = world.buildingPlan ? world.buildingPlan(env) : null;
  return {
    envelope: {
      id: env.id,
      archetype: env.archetype,
      style: env.style,
      floors: env.floors,
      basements: env.basements,
      R: env.R,
      front: env.front,
      U: env.U,
      V: env.V,
      tiers: env.tiers,
      program: env.program,
      storyH: env.storyH,
      baseZ: env.baseZ,
    },
    plan: plan ? serializePlan(plan) : null,
  };
}

function serializePlan(plan) {
  return {
    floors: plan.floors.map((f) => ({
      index: f.index,
      z: f.z,
      height: f.height,
      U: f.grid.U,
      V: f.grid.V,
      cells: f.grid.cells,
      rooms: f.grid.rooms.map((r) => ({ id: r.id, type: r.type, unit: r.unit ?? null, rects: r.rects })),
      doors: f.grid.doors,
      stairs: plan.stairs.filter((s) => s.floor === f.index).map((s) => ({ rect: s.rect, dir: s.dir })),
      ramps: (f.ramps ?? []).filter((r) => r.f === f.index).map((r) => ({ rect: r.rect })),
      furniture: f.furniture ? f.furniture.map((b) => [b.x0, b.y0, b.x1, b.y1, b.m]) : [],
    })),
    issues: plan.issues ?? [],
  };
}

/** Ground height & context at a point (voxel coords). */
export function probe(world, x, y) {
  const lod = 0;
  const cx = Math.floor(x / CHUNK);
  const cy = Math.floor(y / CHUNK);
  const tile = groundTile(world, lod, cx, cy);
  const i = x - (cx * CHUNK - 1);
  const j = y - (cy * CHUNK - 1);
  const idx = Math.max(0, Math.min(P - 1, i)) + Math.max(0, Math.min(P - 1, j)) * P;
  const { i: ci, j: cj } = world.cellAt(x, y);
  const net = world.cellNet(ci, cj);
  const sub = net.subcells.find((s) => x >= s.rect.x0 && x < s.rect.x1 && y >= s.rect.y0 && y < s.rect.y1);
  const ur = world.fields.urban(x, y);
  return {
    groundZ: tile.z[idx],
    water: tile.water[idx],
    district: sub?.district ?? null,
    u: ur.u,
    core: ur.core,
    settlement: ur.settlement?.id ?? null,
    cell: [ci, cj],
  };
}

/**
 * What is at a world voxel (x, y, z), for the viewer's inspector: the world
 * grid's material there, the parts whose cells hold it (and their material),
 * the building, road, highway and district round it. `part` (an id) names
 * the part a ray hit, when it was one.
 */
export function inspect(world, x, y, z, part = null) {
  const cx = Math.floor(x / CHUNK);
  const cy = Math.floor(y / CHUNK);
  const cz = Math.floor(z / CHUNK);
  const chunk = buildChunk(world, 0, cx, cy, cz);
  const at = (data, lx, ly, lz) => data[lx + 1 + (ly + 1) * P + (lz + 1) * P * P];
  const mat = (id) => (id ? { id, name: MATERIALS[id]?.name ?? `#${id}` } : null);
  const out = { voxel: [x, y, z], metres: [x * 0.125, y * 0.125, z * 0.125], grid: mat(at(chunk.data, x - cx * CHUNK, y - cy * CHUNK, z - cz * CHUNK)), parts: [] };
  // the parts holding this voxel (their own lattices, through the exact inverse map)
  for (const p of partsIn(world, { x0: x, y0: y, x1: x, y1: y })) {
    const b = p.aabb;
    if (z < b.z0 || z > b.z1) continue;
    const [u, v, w] = p.placement.toLocal(x, y, z);
    const pc = partChunk(world, p, 0, Math.floor(u / CHUNK), Math.floor(v / CHUNK), Math.floor(w / CHUNK));
    const m = at(pc.data, u - Math.floor(u / CHUNK) * CHUNK, v - Math.floor(v / CHUNK) * CHUNK, w - Math.floor(w / CHUNK) * CHUNK);
    if (m || p.id === part) out.parts.push({ id: p.id, kind: p.kind, key: p.key, cell: p.cell, home: p.home, local: [u, v, w], material: mat(m), hit: p.id === part });
  }
  // the building whose footprint holds it
  const c = world.cellAt(x, y);
  out.cell = [c.i, c.j];
  for (const env of world.cellPlan(c.i, c.j).buildings) {
    const [u, v] = frameOf(env).fromWorld(x, y);
    if (u < 0 || v < 0 || u >= env.U || v >= env.V) continue;
    out.building = { id: env.id, archetype: env.archetype, district: env.district ?? null, floors: env.floors ?? null, local: [Math.floor(u), Math.floor(v)], turned: !!frameOf(env).turned };
    break;
  }
  const r = roadLevelAt(world, world.roadView(c.i, c.j), x + 0.5, y + 0.5, 16);
  if (r && r.dist <= r.seg.hr + 2) out.road = { id: r.seg.road.id, cls: r.seg.road.cls, level: +r.z.toFixed(2), offset: +r.dist.toFixed(1), sidewalk: r.dist > r.seg.hc };
  if (world.highways) {
    const hw = world.highways;
    const n = hw.nearest(x + 0.5, y + 0.5, hw.edgesNear({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), hw.hw + 64);
    if (n) out.highway = { edge: n.edge.id, offset: +n.d.toFixed(1), arc: +(n.s * 0.125).toFixed(1), deck: +n.z.toFixed(1) };
  }
  Object.assign(out, probe(world, x, y));
  return out;
}

/**
 * Points of interest near the spawn for the viewer's "places" list.
 * Positions are world voxels; z is the walking level (feet) when relevant.
 */
export function pois(world, x = 0, y = 0) {
  const out = [];
  const add = (label, px, py, pz = null, mode = "walk") => out.push({ label, x: Math.round(px), y: Math.round(py), z: pz, mode });
  add(world.fields.island ? "Town (overview)" : "Downtown (overview)", x, y, null, "orbit");
  // street corner downtown
  const net = world.cellNet(0, 0);
  const art = net.roads.find((r) => r.cls === "arterial");
  if (art) add("Downtown street corner", art.pts[0].x + art.hr - 6, art.pts[0].y + art.hr + 40, null, "walk");
  // subway platform
  if (world.subway) {
    const st = world.subway.stationsNear({ x0: x - 6000, y0: y - 6000, x1: x + 6000, y1: y + 6000 }).sort((a, b) => Math.hypot(a.x - x, a.y - y) - Math.hypot(b.x - x, b.y - y))[0];
    if (st) {
      const off = st.axis === 0 ? [0, 200] : [200, 0];
      add("Subway platform", st.x + off[0], st.y + off[1], st.zp + 1, "walk");
      add("Subway entrance (street)", st.x + (st.axis === 0 ? world.subway.art.hc + 20 : 150), st.y + (st.axis === 0 ? 150 : world.subway.art.hc + 20), null, "walk");
    }
  }
  // sewers: the overflow hall stair on a sidewalk, and an open manhole
  if (world.sewers) {
    const hall = world.sewers.nearestHall(x, y);
    if (hall?.stairTop) add("Sewer overflow hall (stair)", hall.stairTop.x, hall.stairTop.y, hall.stairTop.z, "walk");
    const c = world.cellAt(x, y);
    const mh = world.sewers.cellPlan(c.i, c.j).nodes.filter((n) => n.open).sort((a, b) => Math.hypot(a.x - x, a.y - y) - Math.hypot(b.x - x, b.y - y))[0];
    if (mh) add("Open manhole (ladder down)", mh.x - mh.qx * 4, mh.y + mh.qy * 22, mh.zr + 1, "walk");
  }
  // a river bridge in the spawn city or the nearest town with a river
  if (world.rivers) {
    // walk along the river from an urban reach to the nearest street crossing it
    const bridgeNear = (px, py) => {
      for (let d = 0; d < 2400; d += 16)
        for (const [dx, dy] of [[d, 0], [-d, 0], [0, d], [0, -d]]) {
          const qx = Math.round(px + dx);
          const qy = Math.round(py + dy);
          const ri = world.rivers.at(qx, qy);
          if (!ri || ri.d > ri.half - 4) continue;
          const c = world.cellAt(qx, qy);
          const rs = makeRoadSample();
          sampleRoadSurface(world.roadView(c.i, c.j).near({ x0: qx - 1, y0: qy - 1, x1: qx + 1, y1: qy + 1 }), qx + 0.5, qy + 0.5, rs, world.seed);
          if (rs.kind === KIND.CARRIAGE) return [qx, qy];
        }
      return null;
    };
    let best = null;
    let tries = 0;
    for (let r = 0; r < 24000 && !best && tries < 12; r += 40)
      for (let a = 0; a < 48 && !best && tries < 12; a += 1) {
        const px = x + Math.cos((a / 48) * Math.PI * 2) * r * 8;
        const py = y + Math.sin((a / 48) * Math.PI * 2) * r * 8;
        const ri = world.rivers.at(px, py);
        if (!ri || ri.d >= 2 || ri.urban <= 0.5 || ri.half < 8) continue;
        tries += 1;
        best = bridgeNear(px, py);
      }
    if (best) add("River bridge", best[0], best[1], null, "walk");
  }
  // elevated highway deck: on its outer lane (right of its direction), under it, and a junction of highways
  if (world.highways) {
    const hw = world.highways;
    let best = null;
    let junction = null;
    for (const e of hw.edgesNear({ x0: x - 12000, y0: y - 12000, x1: x + 12000, y1: y + 12000 })) {
      for (let k = 1; k < e.pts.length; k += 1) {
        const p = e.pts[k];
        const d = Math.hypot(p.x - x, p.y - y);
        if (!best || d < best.d) best = { d, e, s: e.lengths[k] };
      }
      for (const j of e.junctions) if (j.degree >= 3 && (!junction || Math.hypot(j.x - x, j.y - y) < junction.d)) junction = { ...j, d: Math.hypot(j.x - x, j.y - y) };
    }
    if (best) {
      const lane = offsetAt(best.e, best.s, -(3 + 1.5 * world.config.highways.laneWidth * 8));
      add("Elevated highway (deck)", lane.x, lane.y, Math.round(lane.z) + 1, "walk");
      const under = offsetAt(best.e, best.s, 0);
      add("Under the highway", under.x, under.y, null, "walk");
    }
    if (junction) add("Highway junction", junction.x, junction.y, junction.z, "orbit");
  }
  // a park and a suburban house
  outer: for (let r = 0; r <= 6; r += 1)
    for (let i = -r; i <= r; i += 1)
      for (let j = -r; j <= r; j += 1) {
        if (Math.max(Math.abs(i), Math.abs(j)) !== r) continue;
        const plan = world.cellPlan(i, j);
        if (!out.some((p) => p.label === "City park")) {
          const park = plan.spaces.find((s) => s.kind === "park");
          if (park) add("City park", (park.rect.x0 + park.rect.x1) / 2, (park.rect.y0 + park.rect.y1) / 2 + 60, null, "walk");
        }
        if (!out.some((p) => p.label === "Suburban house")) {
          const h = plan.buildings.find((b) => b.archetype === "house");
          if (h) {
            const f = frameOf(h);
            const [hx, hy] = f.toWorld(h.entranceU ?? Math.floor(h.U / 2), -24);
            add("Suburban house", hx, hy, null, "walk");
          }
        }
        if (out.some((p) => p.label === "City park") && out.some((p) => p.label === "Suburban house")) break outer;
      }
  // the spawn city's civic buildings (nearest of each kind)
  if (!world.fields.island) {
    const near = [];
    const c = world.cellAt(x, y);
    for (let j = -2; j <= 2; j += 1) for (let i = -2; i <= 2; i += 1) near.push(...world.cellPlan(c.i + i, c.j + j).buildings);
    civicPois(near, x, y, add);
  }
  // highway interchange (ramps down to the street)
  if (world.highways) {
    let bestR = null;
    for (const e of world.highways.edgesNear({ x0: x - 12000, y0: y - 12000, x1: x + 12000, y1: y + 12000 })) {
      for (const r of world.highways.ramps(e)) {
        let k = 0;
        while (k < e.lengths.length - 2 && e.lengths[k + 1] < r.cross) k += 1;
        const t = (r.cross - e.lengths[k]) / (e.lengths[k + 1] - e.lengths[k] || 1);
        const px = e.pts[k].x + (e.pts[k + 1].x - e.pts[k].x) * t;
        const py = e.pts[k].y + (e.pts[k + 1].y - e.pts[k].y) * t;
        const d = Math.hypot(px - x, py - y);
        if (!bestR || d < bestR.d) bestR = { d, x: px, y: py, ramp: r };
      }
    }
    if (bestR) {
      add("Highway interchange", bestR.x + 40, bestR.y + 40, null, "orbit");
      // (a ramp's landing on its arterial: walk up it onto the deck)
      add("Highway ramp (from the street)", bestR.ramp.x, bestR.ramp.y, null, "walk");
    }
  }
  // the angled world's parts: the nearest of each kind, looked at from above
  if (world.config.world.angles?.enabled) {
    const LABELS = { building: "Turned building", row: "Row of turned buildings", road: "Pitched street", ramp: "Garage ramp", chamfer: "Chamfered corner", corner: "Corner bay", bay: "Canted bay", wing: "Wing" };
    const best = new Map();
    const c = world.cellAt(x, y);
    for (let j = -2; j <= 2; j += 1)
      for (let i = -2; i <= 2; i += 1)
        for (const p of world.cellPlan(c.i + i, c.j + j).parts) {
          const kind = p.kind === "building" ? (p.members?.length > 1 ? "row" : "building") : p.kind === "wing" ? (p.key.split("/").pop() ?? "wing") : p.kind;
          const b = p.aabb;
          const px = (b.x0 + b.x1) / 2;
          const py = (b.y0 + b.y1) / 2;
          const d = Math.hypot(px - x, py - y);
          if (!best.has(kind) || d < best.get(kind).d) best.set(kind, { d, x: px, y: py, z: b.z0 });
        }
    for (const [kind, label] of Object.entries(LABELS)) {
      const b = best.get(kind);
      if (b) out.push({ label: `${label} (part)`, x: Math.round(b.x), y: Math.round(b.y), z: b.z, mode: "orbit", distance: 60, group: "Angled world" });
    }
  }
  // other cities by flavor, nearest first
  const LABELS = { futuristic: "Futuristic city", historic: "Historic city", desert: "Desert town", nordic: "Northern town", soviet: "Soviet town (microdistricts)" };
  const cellS = world.config.world.settlementCell * 8;
  const near = world.fields.settlementsIn({ x0: x - 4 * cellS, y0: y - 4 * cellS, x1: x + 4 * cellS, y1: y + 4 * cellS }).filter((st) => Math.hypot(st.x - x, st.y - y) > 800);
  near.sort((p, q) => Math.hypot(p.x - x, p.y - y) - Math.hypot(q.x - x, q.y - y));
  for (const st of near) {
    const label = LABELS[flavorOf(st).id];
    if (!label || out.some((p) => p.label.startsWith(label))) continue;
    add(`${label} (${(Math.hypot(st.x - x, st.y - y) / 8000).toFixed(1)} km)`, st.x, st.y, null, "orbit");
  }
  // a skybridge in the nearest futuristic city
  const fut = near.find((st) => flavorOf(st).id === "futuristic");
  if (fut) {
    const c = world.cellAt(fut.x, fut.y);
    let br = null;
    for (let dj = -1; dj <= 1 && !br; dj += 1) for (let di = -1; di <= 1 && !br; di += 1) br = world.cellPlan(c.i + di, c.j + dj).skybridges?.[0] ?? null;
    if (br) {
      const r = br.rect;
      const cx = (r.x0 + r.x1) / 2;
      const cy = (r.y0 + r.y1) / 2;
      add("Skybridge (futuristic city)", br.alongX ? cx : r.x0 + 12, br.alongX ? r.y0 + 12 : cy, Math.min(br.za, br.zb) + 2, "walk");
    }
  }
  // the nearest village (its main street crossing), a farm, a harbour and housing projects
  {
    let best = null;
    const cellV = world.config.world.villageCell * 8;
    for (const v of world.fields.villagesIn({ x0: x - 4 * cellV, y0: y - 4 * cellV, x1: x + 4 * cellV, y1: y + 4 * cellV }))
      if (!v.hamlet && (!best || Math.hypot(v.x - x, v.y - y) < Math.hypot(best.x - x, best.y - y))) best = v;
    if (best) add(`Village (${(Math.hypot(best.x - x, best.y - y) / 8000).toFixed(1)} km)`, best.x + 20, best.y + 20, null, "walk");
    let farm = null;
    for (let r = 3; r < 8 && !farm; r += 1)
      for (let i = -r; i <= r && !farm; i += 1) {
        const c = world.cellAt(x + i * 4960, y + r * 4960);
        const lot = world.cellPlan(c.i, c.j).lots.find((l) => l.farmRole === "barn" && l.building);
        if (lot) farm = lot;
      }
    if (farm) add("Farm (barn and silos)", (farm.rect.x0 + farm.rect.x1) / 2, farm.rect.y1 + 40, null, "walk");
    let port = null;
    for (let b = -8; b <= 8; b += 1)
      for (let a = -8; a <= 8; a += 1) {
        const L = world.lakes?.lake(Math.floor(x / world.lakes.cell) + a, Math.floor(y / world.lakes.cell) + b);
        if (L?.port && (!port || Math.hypot(L.x - x, L.y - y) < Math.hypot(port.x - x, port.y - y))) port = L;
      }
    if (port) {
      const s = world.fields.settlement(...port.port.slice(1).split("_").map(Number));
      let quay = null;
      for (let dj = -6; dj <= 6 && !quay; dj += 1)
        for (let di = -6; di <= 6 && !quay; di += 1) {
          const c = world.cellAt(s.x + di * 4960, s.y + dj * 4960);
          quay = world.cellPlan(c.i, c.j).spaces.find((q) => q.quay);
        }
      if (quay) {
        const q = quay.quay;
        const cx = (quay.rect.x0 + quay.rect.x1) / 2;
        const cy = (quay.rect.y0 + quay.rect.y1) / 2;
        add(`Harbour (${(Math.hypot(cx - x, cy - y) / 8000).toFixed(1)} km)`, q.axis === "x" ? q.c - q.sign * 40 : cx, q.axis === "y" ? q.c - q.sign * 40 : cy, null, "walk");
      }
    }
    let proj = null;
    for (let r = 0; r < 6 && !proj; r += 1)
      for (let j = -r; j <= r && !proj; j += 1)
        for (let i = -r; i <= r && !proj; i += 1) {
          if (Math.max(Math.abs(i), Math.abs(j)) !== r) continue;
          const c = world.cellAt(x + i * 4960, y + j * 4960);
          const b = world.cellPlan(c.i, c.j).buildings.find((e) => e.district === "projects");
          if (b) proj = b;
        }
    if (proj) add("Housing projects", (proj.R.x0 + proj.R.x1) / 2, proj.R.y1 + 60, null, "walk");
  }
  if (world.fields.island) islandPois(world, add);
  // the nearest deep link tunnel between two sites
  if (world.siteLinks) {
    const R = 15000 * 8;
    let best = null;
    for (const L of world.siteLinks.near({ x0: x - R, y0: y - R, x1: x + R, y1: y + R })) {
      const g = L.segs[0];
      const t = Math.min(g.len, 400 * 8);
      const px = g.alongX ? g.p.x + g.dirSign * t : g.p.x;
      const py = g.alongX ? g.p.y : g.p.y + g.dirSign * t;
      const d = Math.hypot(px - x, py - y);
      if (!best || d < best.d) best = { d, px, py, z: L.prof[Math.round(t / (16 * 8))] };
    }
    if (best) add(`Deep link tunnel between sites (${(best.d / 8000).toFixed(1)} km)`, best.px, best.py, best.z + 1, "walk");
  }
  // special sites: the nearest of each kind, with the points each kind offers (def.pois)
  if (world.sites) {
    const seen = new Set();
    for (const n of world.sites.nearest(x, y, 30, 5)) {
      if (seen.has(n.type)) continue;
      const s = world.sites.siteAt(Math.floor(n.x / world.sites.cell), Math.floor(n.y / world.sites.cell));
      if (!s?.def.pois) continue;
      seen.add(n.type);
      const km = (n.d / 8000).toFixed(1);
      s.def.pois(world, s).forEach((p, k) => add(k === 0 ? `${p.label} (${km} km)` : p.label, p.x, p.y, p.z, p.mode ?? "walk"));
    }
  }
  return out;
}

/**
 * Island landmarks: the harbour, the smaller places, a cabin, a fjord, a
 * sea cliff, a beach, the skerries and the highest fell.
 */
function islandPois(world, add) {
  const isl = world.fields.island;
  const { towns, villages } = world.fields.islandSettlements();
  const km = (p) => (Math.hypot(p.x, p.y) / 8000).toFixed(1);
  const h = isl.harbour();
  if (h) add("Harbour front", (h.x - h.dx * 40) * 8, (h.y - h.dy * 40) * 8, null, "walk");
  townPois(world, towns[0], add);
  for (const t of towns.slice(1)) add(`Small town (${km(t)} km)`, t.x + 20, t.y + 20, null, "orbit");
  villages.forEach((v, k) => add(`${v.hamlet ? "Hamlet" : "Village"} ${k + 1} (${km(v)} km)`, v.x + 20, v.y + 20, null, "walk"));
  // a cabin: the nearest cabin lot around the town
  let cabin = null;
  const c0 = world.cellAt(0, 0);
  for (let r = 1; r < 9 && !cabin; r += 1)
    for (let dj = -r; dj <= r && !cabin; dj += 1)
      for (let di = -r; di <= r && !cabin; di += 1) {
        if (Math.max(Math.abs(di), Math.abs(dj)) !== r) continue;
        const lot = world.cellPlan(c0.i + di, c0.j + dj).lots.find((l) => l.cabin && l.building);
        if (lot) cabin = lot;
      }
  if (cabin) {
    const r = cabin.rect;
    const off = { N: [0, -40], S: [0, 40], W: [-40, 0], E: [40, 0] }[cabin.front] ?? [0, 40];
    add("Cabin in the woods", (r.x0 + r.x1) / 2 + off[0] * 2, (r.y0 + r.y1) / 2 + off[1] * 2, null, "walk");
  }
  // walk the island on a coarse grid for a summit, a fjord, a cliff, a beach and skerries
  const b = isl.bounds();
  let top = null;
  let fjord = null;
  let cliff = null;
  let beach = null;
  let skerry = null;
  const step = Math.max(160, (b.x1 - b.x0) / 120);
  for (let ym = b.y0; ym <= b.y1; ym += step)
    for (let xm = b.x0; xm <= b.x1; xm += step) {
      const c = isl.coast(xm, ym);
      if (c > 200 && isl.highland(xm, ym) > 0.4) {
        const hh = world.terrain.sample(xm * 8, ym * 8).h;
        if (!top || hh > top.h) top = { x: xm, y: ym, h: hh };
      }
      if (c > -40 && c < 0) {
        const lx = xm + isl.ox;
        const ly = ym + isl.oy;
        const raw = isl.shape(lx, ly);
        if (!fjord && raw > 900) fjord = { x: xm, y: ym };
        const k = isl.cliff(xm, ym);
        const d = Math.hypot(xm, ym);
        if (k > 0.8 && raw < 200 && (!cliff || d < cliff.d)) cliff = { x: xm, y: ym, d };
        if (k < 0.3 && (!beach || k + d / 400000 < beach.k + beach.d / 400000)) beach = { x: xm, y: ym, d, k };
      }
      if (c < -300 && !skerry && isl.skerry(xm, ym, c) > 12) skerry = { x: xm, y: ym };
    }
  if (top) add(`Fell summit (${Math.round(top.h / 8)} m)`, top.x * 8, top.y * 8, null, "walk");
  // the coast's landmarks: the lighthouse, a row of boathouses, a cairn
  if (world.landmarks) {
    const items = world.landmarks.all();
    const first = (kind) => items.find((it) => it.kind === kind);
    const side = (it, d) => [(it.foot.x0 + it.foot.x1) / 2 + d, it.foot.y1 + d];
    const lh = first("lighthouse");
    if (lh) add("Lighthouse", ...side(lh, 60), null, "walk");
    const bh = first("boathouse");
    if (bh) add("Boathouses (naust)", ...side(bh, 40), null, "walk");
    const ca = first("cairn");
    if (ca) add("Cairn on the fell", ...side(ca, 24), null, "walk");
  }
  if (fjord) add("Fjord", fjord.x * 8, fjord.y * 8, null, "orbit");
  if (cliff) add("Sea cliffs", cliff.x * 8, cliff.y * 8, null, "orbit");
  if (beach) add("Beach", beach.x * 8, beach.y * 8, null, "walk");
  if (skerry) add("Skerries", skerry.x * 8, skerry.y * 8, null, "orbit");
}

/**
 * A town's landmarks (city/townPlan.js) and old-town places for the
 * Places list: the market square, the church, the cemetery, the wharf,
 * allotments, a cobbled lane.
 */
function townPois(world, s, add) {
  if (!s) return;
  const R = s.radius * 1.3;
  const cells = world.cellsOverlapping({ x0: s.x - R, y0: s.y - R, x1: s.x + R, y1: s.y + R });
  const spaces = [];
  const buildings = [];
  let lane = null;
  for (const { i, j } of cells) {
    const p = world.cellPlan(i, j);
    spaces.push(...p.spaces);
    buildings.push(...p.buildings);
    lane ??= p.net.roads.find((r) => r.cls === "lane" && Math.hypot(r.pts[0].x - s.x, r.pts[0].y - s.y) < s.radius * 0.5);
  }
  const d = (r) => Math.hypot((r.x0 + r.x1) / 2 - s.x, (r.y0 + r.y1) / 2 - s.y);
  const nearest = (list, rect) => list.sort((a, b) => d(rect(a)) - d(rect(b)))[0];
  const sq = nearest(spaces.filter((q) => q.kind === "square"), (q) => q.rect);
  if (sq) add("Market square", (sq.rect.x0 + sq.rect.x1) / 2, (sq.rect.y0 + sq.rect.y1) / 2 + 40, null, "walk");
  const ch = nearest(buildings.filter((b) => b.archetype === "church" && !b.lot.endsWith("/l0")), (b) => b.R) ?? nearest(buildings.filter((b) => b.archetype === "church"), (b) => b.R);
  if (ch) add("Church", (ch.R.x0 + ch.R.x1) / 2, ch.R.y1 + 48, null, "walk");
  const ce = spaces.find((q) => q.kind === "cemetery");
  if (ce) add("Cemetery", (ce.rect.x0 + ce.rect.x1) / 2, (ce.rect.y0 + ce.rect.y1) / 2, null, "walk");
  const wh = nearest(buildings.filter((b) => b.archetype === "wharfhouse"), (b) => b.R);
  if (wh) add("Wharf (gabled warehouses)", (wh.R.x0 + wh.R.x1) / 2, (wh.R.y0 + wh.R.y1) / 2, null, "orbit");
  if (lane) add("Old-town lane", (lane.pts[0].x + lane.pts[1].x) / 2, (lane.pts[0].y + lane.pts[1].y) / 2, null, "walk");
  const al = spaces.find((q) => q.kind === "allotments");
  if (al) add("Allotment gardens", (al.rect.x0 + al.rect.x1) / 2, al.rect.y0 + 40, null, "walk");
  const ga = spaces.find((q) => q.kind === "garages" || q.kind === "wasteland");
  if (ga) add(ga.kind === "garages" ? "Garages by the works" : "Wasteland by the works", (ga.rect.x0 + ga.rect.x1) / 2, (ga.rect.y0 + ga.rect.y1) / 2, null, "walk");
  civicPois(buildings, s.x, s.y, add);
}

const CIVIC_LABELS = {
  townHall: "Town hall",
  museum: "Museum",
  artGallery: "Art gallery",
  concertHall: "Concert hall",
  houseOfCulture: "House of culture",
  cinema: "Cinema",
  musicClub: "Music club",
  library: "Library",
  hotel: "Hotel",
  departmentStore: "Department store",
  marketHall: "Market hall",
  supermarket: "Supermarket",
  petrolStation: "Petrol station",
  hospital: "Hospital",
  polyclinic: "Polyclinic",
  policeStation: "Police station",
  fireStation: "Fire station",
};

/** The nearest civic building of each kind (buildings/civic.js), in front of its entrance. */
function civicPois(buildings, x, y, add) {
  const best = new Map();
  for (const b of buildings) {
    if (!CIVIC_LABELS[b.archetype]) continue;
    const d = Math.hypot((b.R.x0 + b.R.x1) / 2 - x, (b.R.y0 + b.R.y1) / 2 - y);
    if (!best.has(b.archetype) || d < best.get(b.archetype).d) best.set(b.archetype, { b, d });
  }
  for (const [id, label] of Object.entries(CIVIC_LABELS)) {
    const hit = best.get(id);
    if (!hit) continue;
    const f = frameOf(hit.b);
    const [px, py] = f.toWorld(Math.floor(hit.b.U / 2), -24);
    add(label, px, py, null, "walk");
  }
}

/**
 * Coarse world overview raster for planet-scale authoring: biome colors in
 * the countryside, settlement urbanization in grey/ochre, water, snow and a
 * hillshade from the terrain. rect in voxels; returns RGBA bytes (w x h)
 * plus the legend of biomes present.
 */
export function overview(world, rect, w, h) {
  const rgba = new Uint8ClampedArray(w * h * 4);
  const sx = (rect.x1 - rect.x0) / w;
  const sy = (rect.y1 - rect.y0) / h;
  const hex = new Map(BIOMES.all().map((b) => [b.id, [parseInt(b.color.slice(1, 3), 16), parseInt(b.color.slice(3, 5), 16), parseInt(b.color.slice(5, 7), 16)]]));
  const present = new Set();
  const seaZ = world.config.world.seaLevel * 8;
  const heights = new Float32Array((w + 1) * (h + 1));
  for (let j = 0; j <= h; j += 1)
    for (let i = 0; i <= w; i += 1) heights[i + j * (w + 1)] = world.terrain.sample(rect.x0 + i * sx, rect.y0 + j * sy).h;
  for (let j = 0; j < h; j += 1) {
    for (let i = 0; i < w; i += 1) {
      const x = rect.x0 + (i + 0.5) * sx;
      const y = rect.y0 + (j + 0.5) * sy;
      const hz = heights[i + j * (w + 1)];
      const ur = world.fields.urban(x, y);
      const ri = world.rivers ? world.rivers.at(x, y) : null;
      let c;
      const lk = world.lakes ? world.lakes.at(x, y) : null;
      if (hz < seaZ || (ri && ri.d < Math.max(ri.half, (sx + sy) / 16 * 0.6)) || (lk && lk.k < 1)) c = [52, 96, 128];
      else if (ur.u > 0.2) {
        const k = Math.min(1, ur.core * 1.4);
        c = [150 + 60 * k, 140 + 30 * k, 128 - 20 * k];
      } else {
        const b = world.landCover.biomeAt(x, y, hz / 8);
        present.add(b.id);
        c = hex.get(b.id);
        const { t } = world.landCover.climate(x, y, hz / 8);
        if (t < SNOWLINE) c = [236, 240, 244];
        else if (t < TREELINE) c = [150, 146, 136];
      }
      // hillshade from the NW
      const dzx = heights[i + 1 + j * (w + 1)] - hz;
      const dzy = heights[i + (j + 1) * (w + 1)] - hz;
      const shade = Math.max(0.55, Math.min(1.25, 1 - (dzx + dzy) / (sx + sy) * 1.6));
      const o = (i + j * w) * 4;
      rgba[o] = c[0] * shade;
      rgba[o + 1] = c[1] * shade;
      rgba[o + 2] = c[2] * shade;
      rgba[o + 3] = 255;
    }
  }
  const legend = [...present].map((id) => ({ id, label: BIOMES.get(id).label, color: BIOMES.get(id).color }));
  const sites = world.sites ? world.sites.mapData(rect) : [];
  // deep link tunnels between sites (only for regional views: planning them builds the complexes)
  const links = world.siteLinks && rect.x1 - rect.x0 <= 80000 * 8 ? world.siteLinks.mapData(rect) : [];
  const highways = world.highways ? world.highways.mapData(rect) : [];
  const cities = world.fields.settlementsIn(rect).map((st) => ({ x: st.x, y: st.y, r: st.radius, flavor: flavorOf(st).id }));
  const villages = rect.x1 - rect.x0 <= 200000 * 8 ? world.fields.villagesIn(rect).map((v) => ({ x: v.x, y: v.y, r: v.radius, hamlet: !!v.hamlet })) : [];
  return { rect, w, h, rgba, legend, sites, links, highways, cities, villages };
}
