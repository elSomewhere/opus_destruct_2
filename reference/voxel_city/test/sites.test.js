import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { floodRegion } from "../src/engine/validate/walkability.js";
import { shapeRects } from "../src/engine/sites/complex.js";

function nearestBase(w, type = "militaryBase", radius = 4) {
  const n = w.sites.nearest(0, 0, 400, radius).find((q) => q.type === type);
  assert.ok(n, `a ${type} exists within reach of the spawn city`);
  const s = w.sites.siteAt(Math.floor(n.x / w.sites.cell), Math.floor(n.y / w.sites.cell));
  s.def.structure(w, s);
  return s;
}

test("military base: surface buildings get full interiors via the archetype system", () => {
  const w = createWorld({ seed: 1337 });
  const s = nearestBase(w);
  const plan = w.cellPlan(s.cell.i, s.cell.j);
  const envs = plan.buildings.filter((b) => b.district === "military");
  assert.ok(envs.length >= 3);
  for (const env of envs) {
    const bp = w.buildingPlan(env);
    assert.ok(bp);
    assert.deepEqual(bp.issues, []);
  }
});

test("military base: bunker stairs reach the complex and the shaft links every level", () => {
  for (const seed of [1337, 42]) {
    const w = createWorld({ seed });
    const s = nearestBase(w);
    const u = s.plan.under;
    const bk = s.plan.bunker;
    const sh0 = u.shafts[0];
    const reg = {
      x0: Math.min(bk.x0, sh0.rect.x0) - 60,
      y0: Math.min(bk.y0, sh0.rect.y0) - 80,
      x1: Math.max(bk.x1, sh0.rect.x1) + 60,
      y1: Math.max(bk.y1, sh0.rect.y1) + 60,
      z0: sh0.zLow - 6,
      z1: s.padZ + 40,
    };
    const f = floodRegion(w, reg, [[Math.round((bk.x0 + bk.x1) / 2), bk.y1 + 12, s.padZ + 1]]);
    assert.ok(f.reached(sh0.rect.x0 + 8, sh0.rect.y1 + 20, sh0.zLow + 1), `seed ${seed}: entrance shaft`);
    for (const sh of u.shafts.slice(1)) {
      const r = { x0: sh.rect.x0 - 40, y0: sh.rect.y0 - 80, x1: sh.rect.x1 + 40, y1: sh.rect.y1 + 40, z0: sh.zLow - 6, z1: sh.zHigh + 40 };
      const g = floodRegion(w, r, [[sh.rect.x0 + 8, sh.rect.y0 - 24, sh.zHigh + 1]]);
      for (const z of sh.levels) assert.ok(g.reached(sh.rect.x0 + 8, sh.rect.y0 - 24, z + 1), `seed ${seed}: level at ${z}`);
    }
  }
});

test("military base: deepest level rooms, reactor catwalk and ladder shortcuts are reachable", () => {
  const w = createWorld({ seed: 42 });
  const s = nearestBase(w);
  const u = s.plan.under;
  const B = u.bounds;
  const lv = u.levels[u.levels.length - 1];
  const ante = lv.rooms[0];
  const reg = { x0: B.x0 - 20, y0: B.y0 - 20, x1: B.x1 + 20, y1: B.y1 + 20, z0: lv.zf - 6, z1: lv.zf + 70 };
  const f = floodRegion(w, reg, [[Math.round((ante.x0 + ante.x1) / 2), Math.round((ante.y0 + ante.y1) / 2), lv.zf + 1]]);
  for (const q of lv.rooms) {
    const hit = f.reached(Math.round((q.x0 + q.x1) / 2), Math.round((q.y0 + q.y1) / 2), lv.zf + 1, 3) || f.reached(q.x0 + 10, q.y0 + 10, lv.zf + 1, 3) || f.reached(q.x1 - 6, q.y1 - 6, lv.zf + 1, 3);
    assert.ok(hit, `${q.type} room reachable`);
  }
  const reactor = lv.rooms.find((q) => q.type === "reactor");
  if (reactor) assert.ok(f.reached(reactor.x0 + 5, Math.round((reactor.y0 + reactor.y1) / 2), lv.zf + 29, 3), "catwalk ring");
  assert.ok(u.ladders.length > 0, "ladder shortcuts");
  for (const L of u.ladders) {
    const r = L.rect;
    const lr = { x0: r.x0 - 40, y0: r.y0 - 40, x1: r.x1 + 40, y1: r.y1 + 40, z0: L.zBot - 6, z1: L.zTop + 30 };
    const top = [r.x1 + 12, r.y1 + 12, L.zTop + 1];
    const bot = [r.x1 + 12, r.y1 + 12, L.zBot + 1];
    assert.ok(floodRegion(w, lr, [bot], { climb: true }).reached(...top), "ladder up");
    assert.ok(floodRegion(w, lr, [top], { climb: true }).reached(...bot), "ladder down");
  }
});

test("research complex: campus interiors, portal stairs and every room of a sector reachable", () => {
  const w = createWorld({ seed: 1337 });
  const s = nearestBase(w, "researchComplex", 8);
  const plan = w.cellPlan(s.cell.i, s.cell.j);
  const envs = plan.buildings.filter((b) => b.district === "research");
  assert.ok(envs.length >= 4, "campus buildings");
  for (const env of envs) assert.deepEqual(w.buildingPlan(env).issues, []);
  const u = s.plan.under;
  assert.equal(u.sectors.length, 4);
  // every sector shaft continues down to the tram station
  for (const sec of u.sectors) {
    const sh = u.shafts.find((q) => q.rect.x0 === sec.shaftRect.x0 && q.rect.y0 === sec.shaftRect.y0);
    assert.ok(sh && sh.levels.includes(u.tram.z), `sector ${sec.id} shaft reaches the tram`);
  }
  // the tram loop links the stations, entering and leaving each one along its platform
  assert.ok(u.tram.segs.length >= 4);
  for (const pts of u.tram.routes) {
    assert.equal(pts[0].y, pts[1].y, "leaves along the platform");
    assert.equal(pts[pts.length - 1].y, pts[pts.length - 2].y, "arrives along the platform");
  }
  // portal stairs: from the campus lawn down to the first level
  const sh0 = u.shafts[0];
  const portal = s.plan.portals.find((q) => sh0.rect.x0 >= q.x0 && sh0.rect.x1 <= q.x1 && sh0.rect.y0 >= q.y0 && sh0.rect.y0 <= q.y1);
  assert.ok(portal, "the first shaft starts in a portal block");
  const reg = { x0: portal.x0 - 60, y0: portal.y0 - 60, x1: portal.x1 + 60, y1: sh0.rect.y1 + 80, z0: sh0.zLow - 6, z1: s.padZ + 40 };
  const f0 = floodRegion(w, reg, [[Math.round((portal.x0 + portal.x1) / 2), portal.y1 + 12, s.padZ + 1]]);
  assert.ok(f0.reached(sh0.rect.x0 + 8, sh0.rect.y1 + 20, sh0.zLow + 1, 3), "portal stairs reach level 0");
  // every room on every level of the sector holding the atrium
  const sector = u.levels.find((l) => l.rooms.some((q) => q.type === "atriumTop"))?.sector ?? 0;
  for (const lv of u.levels.filter((l) => l.sector === sector)) {
    const b = u.sectors[sector].bounds;
    const ante = lv.rooms[0];
    const f = floodRegion(w, { x0: b.x0 - 60, y0: b.y0 - 60, x1: b.x1 + 60, y1: b.y1 + 60, z0: lv.zf - 8, z1: lv.zf + 60 }, [[Math.round((ante.x0 + ante.x1) / 2), Math.round((ante.y0 + ante.y1) / 2), lv.zf + 1]]);
    for (const q of lv.rooms) assert.ok(shapeRects(q).some((r) => f.reachedRect(r, lv.zf + 1)), `level ${lv.k}: ${q.type} reachable`);
  }
});

test("mountain stronghold: apron -> tunnel -> cavern town -> stairs down to the complex", () => {
  const w = createWorld({ seed: 1337 });
  const s = nearestBase(w, "mountainBase", 6);
  const p = s.plan;
  const z = s.padZ;
  // the cavern keeps its rock cover and the buildings on its floor get full interiors
  const plan = w.cellPlan(s.cell.i, s.cell.j);
  const envs = plan.buildings.filter((b) => b.district === "stronghold");
  assert.ok(envs.length >= 2, "cavern buildings");
  for (const env of envs) assert.deepEqual(w.buildingPlan(env).issues, []);
  // drive in: from the apron through the blast doors and the tunnel onto the cavern road
  const [ax, ay] = p.frame.toWorld(p.dist + 60, 0);
  const [cx, cy] = p.frame.toWorld(s.placed.L * 0.3, 0);
  const r = { x0: Math.min(ax, cx) - 80, y0: Math.min(ay, cy) - 80, x1: Math.max(ax, cx) + 80, y1: Math.max(ay, cy) + 80, z0: z - 4, z1: z + 90 };
  const f = floodRegion(w, r, [[ax, ay, z + 1]]);
  assert.ok(f.reached(cx, cy, z + 1, 2), "cavern road reached from the apron");
  // the yard's portal block leads down to the first level
  const u = p.under;
  const sh0 = u.shafts[0];
  const q = p.portal;
  const reg = { x0: q.x0 - 60, y0: q.y0 - 60, x1: q.x1 + 60, y1: sh0.rect.y1 + 80, z0: sh0.zLow - 6, z1: z + 40 };
  const g = floodRegion(w, reg, [[Math.round((q.x0 + q.x1) / 2), q.y1 + 12, z + 1]]);
  assert.ok(g.reached(sh0.rect.x0 + 8, sh0.rect.y1 + 20, sh0.zLow + 1, 3), "portal stairs reach level 0");
  assert.ok(u.tram && u.tram.stations.length === 2, "the two sectors share a tram");
  // a service road leaves the apron gate down the flank, drivable (walkable) from the apron
  const road = s.placed.road[0];
  assert.ok(road && road.path.length >= 12, "access road");
  for (let k = 1; k < road.path.length; k += 1) {
    const a = road.path[k - 1];
    const b = road.path[k];
    assert.ok(Math.abs(a.z - b.z) <= 0.1 * Math.hypot(b.x - a.x, b.y - a.y) + 1, "road grade within 10%");
  }
  const q10 = road.path[10];
  const rr = { x0: Math.min(ax, q10.x) - 120, y0: Math.min(ay, q10.y) - 120, x1: Math.max(ax, q10.x) + 120, y1: Math.max(ay, q10.y) + 120, z0: q10.z - 20, z1: z + 40 };
  const fr = floodRegion(w, rr, [[ax, ay, z + 1]]);
  assert.ok(fr.reached(q10.x, q10.y, q10.z + 1, 4), "road reached from the apron");
});

test("site links: deep tunnels keep their cover and grade, and are walkable out of the port", () => {
  const w = createWorld({ seed: 1337 });
  const R = 15000 * 8;
  const links = w.siteLinks.near({ x0: -R, y0: -R, x1: R, y1: R });
  assert.ok(links.length > 0, "links near the spawn");
  for (const L of links) {
    for (let i = 1; i < L.prof.length; i += 1) assert.ok(Math.abs(L.prof[i] - L.prof[i - 1]) <= 0.065 * 128 + 1, `${L.id}: grade`);
    for (let i = 3; i < L.prof.length - 3; i += 5) {
      const s = i * 128;
      let g = L.segs[0];
      for (const q of L.segs) if (s >= q.s0) g = q;
      const t = Math.min(g.len, s - g.s0);
      const x = g.alongX ? g.p.x + g.dirSign * t : g.p.x;
      const y = g.alongX ? g.p.y : g.p.y + g.dirSign * t;
      assert.ok(w.terrain.sample(x, y).h - L.prof[i] >= 8 * 25, `${L.id}: cover at ${i}`);
    }
  }
  // walk 150 m along the first link from its port
  const L = links[0];
  const g = L.segs[0];
  const a = { x: g.p.x + (g.alongX ? g.dirSign * 8 * 30 : 0), y: g.p.y + (g.alongX ? 0 : g.dirSign * 8 * 30) };
  const b = { x: g.p.x + (g.alongX ? g.dirSign * 8 * 180 : 0), y: g.p.y + (g.alongX ? 0 : g.dirSign * 8 * 180) };
  const z0 = Math.min(L.prof[0], L.prof[12]);
  const z1 = Math.max(L.prof[0], L.prof[12]);
  const reg = { x0: Math.min(a.x, b.x) - 40, y0: Math.min(a.y, b.y) - 40, x1: Math.max(a.x, b.x) + 40, y1: Math.max(a.y, b.y) + 40, z0: z0 - 8, z1: z1 + 70 };
  const zA = L.prof[Math.round(30 / 16)];
  const f = floodRegion(w, reg, [[a.x, a.y, zA + 1]]);
  assert.ok(f.reached(b.x, b.y, L.prof[Math.round(180 / 16)] + 1, 6), "the tunnel runs through");
});
