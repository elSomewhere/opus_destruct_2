import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { flavorOf } from "../src/engine/city/flavors.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";

const w = createWorld({ seed: 1337 });

test("villages: small places between the towns, houses along their own main street", () => {
  const vils = [];
  for (let j = -6; j <= 6; j += 1) for (let i = -6; i <= 6; i += 1) if (w.fields.village(i, j)) vils.push(w.fields.village(i, j));
  assert.ok(vils.length > 20, "villages and hamlets");
  const v = vils.filter((q) => !q.hamlet).sort((a, b) => Math.hypot(a.x, a.y) - Math.hypot(b.x, b.y))[0];
  const c = w.cellAt(v.x, v.y);
  const net = w.cellNet(c.i, c.j);
  assert.ok(net.roads.some((r) => r.cls === "village"), "village streets");
  const plan = w.cellPlan(c.i, c.j);
  const houses = plan.buildings.filter((b) => b.district === "village");
  assert.ok(houses.length >= 8, `village houses (${houses.length})`);
  for (const env of houses.slice(0, 6)) assert.deepEqual(w.buildingPlan(env).issues, []);
});

test("farms: a barn beside each farmhouse, with a valid interior", () => {
  let barns = 0;
  for (let i = -4; i <= 4 && barns < 3; i += 1) {
    const plan = w.cellPlan(i, 5);
    for (const l of plan.lots) {
      if (l.farmRole !== "barn" || !l.building) continue;
      const env = plan.buildingById.get(l.building);
      assert.equal(env.archetype, "barn");
      assert.deepEqual(w.buildingPlan(env).issues, []);
      assert.ok(plan.lots.some((h) => h.farm === l.farm && h.farmRole === "house"), "farmhouse");
      barns += 1;
    }
  }
  assert.ok(barns > 0);
});

test("soviet towns: microdistricts of panel slabs and towers, walkable on every floor", () => {
  let town = null;
  for (let j = -3; j <= 3 && !town; j += 1) for (let i = -3; i <= 3 && !town; i += 1) {
    const s = w.fields.settlement(i, j);
    if (s && flavorOf(s).id === "soviet") town = s;
  }
  assert.ok(town, "a soviet town nearby");
  let slab = null;
  for (let dj = -2; dj <= 2 && !slab; dj += 1)
    for (let di = -2; di <= 2 && !slab; di += 1) {
      const c = w.cellAt(town.x + di * 4960, town.y + dj * 4960);
      slab = w.cellPlan(c.i, c.j).buildings.find((b) => b.archetype === "panelSlab" && b.district === "microdistrict");
    }
  assert.ok(slab, "a panel slab");
  assert.equal(slab.style === "panel" || slab.style === "projects", true);
  assert.deepEqual(w.buildingPlan(slab).issues, []);
  const r = walkabilityReport(w, slab, {});
  assert.ok(r.ok, `missing ${JSON.stringify(r.missing.slice(0, 3))}`);
});

test("ports: big lakes against towns, straight quays a few metres above the water", () => {
  let port = null;
  for (let b = -8; b <= 8 && !port; b += 1) for (let a = -8; a <= 8 && !port; a += 1) {
    const L = w.lakes.lake(a, b);
    if (L && L.port) port = L;
  }
  assert.ok(port, "a harbour lake");
  const s = w.fields.settlement(...port.port.slice(1).split("_").map(Number));
  let quay = null;
  for (let dj = -6; dj <= 6 && !quay; dj += 1)
    for (let di = -6; di <= 6 && !quay; di += 1) {
      const c = w.cellAt(s.x + di * 4960, s.y + dj * 4960);
      quay = w.cellPlan(c.i, c.j).spaces.find((q) => q.quay);
    }
  assert.ok(quay, "a quay");
  const height = (quay.groundZ - quay.quay.level) / 8;
  assert.ok(height > 1 && height < 12, `quay ${height.toFixed(1)} m above the water`);
});

test("parked cars are optional and off by default", () => {
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) assert.ok(!w.dressing(i, j).props.some((p) => p.kind === "car"));
  const wc = createWorld({ seed: 1337, vehicles: { parked: true } });
  let cars = 0;
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) cars += wc.dressing(i, j).props.filter((p) => p.kind === "car").length;
  assert.ok(cars > 20);
});

test("subway: the tunnel tracks meet the platform tracks at the station mouth", () => {
  const st = w.subway.stationsNear({ x0: -3000, y0: -3000, x1: 3000, y1: 3000 })[0];
  assert.ok(st);
  const zr = st.zp - 7; // rail level
  const at = (x, y, z) => {
    const cx = Math.floor(x / 32);
    const cy = Math.floor(y / 32);
    const ch = buildChunk(w, 0, cx, cy, Math.floor(z / 32), groundTile(w, 0, cx, cy));
    return ch.data[x - cx * 32 + 1 + (y - cy * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2];
  };
  for (const l of [384 - 6, 384 + 6]) {
    for (const c of [-52, -40, 40, 52]) {
      const x = st.axis === 0 ? st.x + c : st.x + l;
      const y = st.axis === 0 ? st.y + l : st.y + c;
      assert.equal(at(x, y, zr), MAT.RAIL_STEEL, `rail at c=${c} l=${l}`);
    }
  }
});
