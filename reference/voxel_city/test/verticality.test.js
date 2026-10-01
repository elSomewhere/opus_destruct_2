import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { floodRegion, walkabilityReport } from "../src/engine/validate/walkability.js";
import { flavorOf } from "../src/engine/city/flavors.js";

test("parking garages: every deck is reached by ramp and stair, and is walkable", () => {
  const w = createWorld({ seed: 1337 });
  const garages = [];
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) garages.push(...w.cellPlan(i, j).buildings.filter((b) => b.archetype === "garage"));
  assert.ok(garages.length > 0, "garages exist downtown");
  const env = garages[0];
  const plan = w.buildingPlan(env);
  assert.equal(plan.ramps.length, env.floors - 1, "one ramp per deck above the ground");
  assert.equal(plan.links.length, env.floors - 1);
  assert.deepEqual(plan.issues, []);
  const rep = walkabilityReport(w, env, { maxFloors: env.floors });
  assert.ok(rep.ok, `unreachable: ${JSON.stringify(rep.missing)}`);
});

test("skybridges: futuristic cities link facing offices with walkable bridges", () => {
  const w = createWorld({ seed: 1337 });
  let city = null;
  for (let a = -6; a <= 6 && !city; a += 1) for (let b = -6; b <= 6 && !city; b += 1) {
    const s = w.fields.settlement(a, b);
    if (s && flavorOf(s).id === "futuristic") city = s;
  }
  assert.ok(city, "a futuristic city nearby");
  const c = w.cellAt(city.x, city.y);
  const bridges = [];
  for (let j = c.j - 1; j <= c.j + 1; j += 1) for (let i = c.i - 1; i <= c.i + 1; i += 1) bridges.push(...w.cellPlan(i, j).skybridges);
  assert.ok(bridges.length > 0, "bridges were planned");
  for (const br of bridges.slice(0, 2)) {
    for (const id of [br.a, br.b]) {
      const plan = w.buildingPlan(w.envelope(id));
      const doors = plan.floors.flatMap((f) => f.grid.doors.filter((d) => d.kind === "sky"));
      assert.ok(doors.length >= 1 && doors.every((d) => !d.misplaced), `sky door in ${id}`);
      assert.deepEqual(plan.issues, []);
    }
    const r = br.rect;
    const cx = Math.round((r.x0 + r.x1) / 2);
    const cy = Math.round((r.y0 + r.y1) / 2);
    const inA = br.alongX ? [cx, r.y0 - 24] : [r.x0 - 24, cy];
    const inB = br.alongX ? [cx, r.y1 + 24] : [r.x1 + 24, cy];
    const reg = { x0: Math.min(inA[0], inB[0]) - 40, x1: Math.max(inA[0], inB[0]) + 40, y0: Math.min(inA[1], inB[1]) - 40, y1: Math.max(inA[1], inB[1]) + 40, z0: br.za - 6, z1: br.za + 40 };
    const f = floodRegion(w, reg, [[inA[0], inA[1], br.za + 2]]);
    assert.ok(f.reached(inB[0], inB[1], br.zb + 2), `${br.id}: walk across`);
  }
});

test("highways: grades stay within 6% and tunnels run through from portal to portal", () => {
  const w = createWorld({ seed: 1337 });
  const hw = w.highways;
  const R = 30000 * 8;
  const edges = hw.edgesNear({ x0: -R, y0: -R, x1: R, y1: R });
  let best = null;
  for (const e of edges) {
    let start = -1;
    for (let k = 1; k < e.pts.length; k += 1) {
      const p = e.pts[k];
      const q = e.pts[k - 1];
      assert.ok(Math.abs(p.z - q.z) / Math.hypot(p.x - q.x, p.y - q.y) < 0.065, `grade on ${e.id}`);
      const cover = w.terrain.sample(p.x, p.y).h - p.z;
      if (cover > 64 && start < 0) start = k - 1;
      if (cover < 40 && start >= 0) {
        const a = e.pts[Math.max(0, start - 2)];
        const b = e.pts[Math.min(e.pts.length - 1, k + 2)];
        const len = Math.hypot(b.x - a.x, b.y - a.y);
        if (len < 1100 && (!best || Math.hypot(a.x, a.y) < Math.hypot(best.a.x, best.a.y))) best = { a, b };
        start = -1;
      }
    }
  }
  assert.ok(best, "a tunnel near the spawn");
  const { a, b } = best;
  const L = Math.hypot(b.x - a.x, b.y - a.y);
  const nx = (-(b.y - a.y) / L) * 20;
  const ny = ((b.x - a.x) / L) * 20;
  const A = [Math.round(a.x + nx), Math.round(a.y + ny), Math.round(a.z) + 1];
  const B = [Math.round(b.x + nx), Math.round(b.y + ny), Math.round(b.z) + 1];
  const reg = { x0: Math.min(A[0], B[0]) - 60, y0: Math.min(A[1], B[1]) - 60, x1: Math.max(A[0], B[0]) + 60, y1: Math.max(A[1], B[1]) + 60, z0: Math.round(Math.min(a.z, b.z)) - 20, z1: Math.round(Math.max(a.z, b.z)) + 70 };
  assert.ok(floodRegion(w, reg, [A]).reached(B[0], B[1], B[2], 8), "through the tunnel");
});
