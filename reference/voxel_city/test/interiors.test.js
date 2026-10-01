import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { planBuilding } from "../src/engine/buildings/interior/plan.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { frameOf } from "../src/engine/buildings/frame.js";
import { floorZ } from "../src/engine/buildings/archetypes.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { IS_SOLID } from "../src/engine/voxel/materials.js";

function sampleBuildings(world, perArchetype, radius = 4) {
  const byArch = new Map();
  for (let r = 0; r <= radius; r += 1)
    for (let i = -r; i <= r; i += 1)
      for (let j = -r; j <= r; j += 1) {
        if (Math.max(Math.abs(i), Math.abs(j)) !== r) continue;
        for (const env of world.cellPlan(i, j).buildings) {
          const list = byArch.get(env.archetype) ?? [];
          if (list.length < perArchetype) list.push(env);
          byArch.set(env.archetype, list);
        }
      }
  return byArch;
}

test("every room of every building is reachable through doors and stairs", () => {
  for (const seed of [1337, 99]) {
    const w = createWorld({ seed });
    let n = 0;
    for (let i = -1; i <= 1; i += 1)
      for (let j = -1; j <= 1; j += 1)
        for (const env of w.cellPlan(i, j).buildings) {
          const plan = planBuilding(w, env);
          assert.ok(plan, env.id);
          assert.deepEqual(plan.issues, [], `${env.id} ${env.archetype}`);
          n += 1;
        }
    assert.ok(n > 200);
  }
});

test("all archetypes appear across the city", () => {
  const w = createWorld({ seed: 1337 });
  const byArch = sampleBuildings(w, 1, 5);
  for (const a of ["house", "rowhouse", "walkup", "midrise", "office", "tower", "warehouse"]) assert.ok(byArch.has(a), a);
});

test("interiors are walkable at voxel level (0.5 m walker, 2-voxel steps)", () => {
  const w = createWorld({ seed: 2024 });
  const byArch = sampleBuildings(w, 2, 4);
  for (const [arch, list] of byArch) {
    for (const env of list) {
      if (arch === "warehouse" || arch === "factory") {
        if (env !== list[0]) continue;
      }
      const r = walkabilityReport(w, env, { maxFloors: 2 });
      assert.ok(r.ok, `${arch} ${env.id} missing ${JSON.stringify(r.missing.slice(0, 5))}`);
    }
  }
});

test("high-rises are modelled on every floor: the tallest downtown tower is walkable top to bottom", () => {
  const w = createWorld({ seed: 1337 });
  let best = null;
  for (let i = -1; i <= 1; i += 1)
    for (let j = -1; j <= 1; j += 1) for (const e of w.cellPlan(i, j).buildings) if (e.archetype === "tower" && (!best || e.floors > best.floors)) best = e;
  assert.ok(best && best.floors >= 20, "a tall tower downtown");
  const plan = planBuilding(w, best);
  assert.equal(plan.floors.filter((f) => f.index >= 0).length, best.floors);
  const r = walkabilityReport(w, best, {});
  assert.ok(r.ok, `missing ${JSON.stringify(r.missing.slice(0, 5))}`);
});

test("houses and row houses are entered through a front door on the street side", () => {
  const w = createWorld({ seed: 1337 });
  const byArch = sampleBuildings(w, 40, 6);
  for (const arch of ["house", "rowhouse"]) {
    const envs = byArch.get(arch) ?? [];
    assert.ok(envs.length >= 10, `${arch}: sample of ${envs.length}`);
    for (const env of envs) {
      const plan = w.buildingPlan(env);
      const front = plan.floorByIndex.get(0).grid.doors.find((d) => d.b === -1 && d.kind === "entrance" && d.v0 <= 2);
      assert.ok(front, `${env.id}: a front door`);
      assert.ok(Math.abs((front.u0 + front.u1) / 2 - env.entranceU) <= 6, `${env.id}: the front door meets the front path`);
    }
  }
});

test("pitched roofs stay inside their envelope (coarse LODs never clip a ridge)", () => {
  const w = createWorld({ seed: 1337 });
  let checked = 0;
  for (let j = -4; j <= 4; j += 1)
    for (let i = -4; i <= 4; i += 1)
      for (const env of w.cellPlan(i, j).buildings) {
        if (env.roof.type === "flat" || env.steeple) continue;
        // the roof as massing.pitchedRoof draws it: slope x distance from the eaves
        const roof = env.roof;
        const main = env.tiers[env.tiers.length - 1].rects[0];
        const o = roof.overhang ?? 3;
        const oh = typeof o === "number" ? { F: o, B: o, L: o, R: o } : { F: o.F ?? 3, B: o.B ?? 3, L: o.L ?? 3, R: o.R ?? 3 };
        const r = { x0: main.x0 - oh.L, y0: main.y0 - oh.F, x1: main.x1 + oh.R, y1: main.y1 + oh.B };
        let h = 0;
        for (let v = r.y0; v <= r.y1; v += 1)
          for (let u = r.x0; u <= r.x1; u += 1) {
            const dv = Math.min(v - r.y0, r.y1 - v);
            const du = Math.min(u - r.x0, r.x1 - u);
            const d = roof.type === "hip" ? Math.min(du, dv) : roof.type === "sawtooth" ? ((v - r.y0) % 48) / 3 : roof.ridge === "v" ? du : dv;
            h = Math.max(h, Math.floor(d * (roof.slope ?? 0.7)) + 1);
          }
        let zTop = env.baseZ;
        for (const s of env.storyH) zTop += s;
        assert.ok(zTop + h <= env.topZ, `${env.id} (${env.archetype}, ${roof.type}): ridge ${zTop + h} above topZ ${env.topZ}`);
        checked += 1;
      }
  assert.ok(checked > 500, `${checked} pitched roofs`);
});

test("on a slope a street door is raised to its street or stepped up from it: every entrance of a long walkup walkable", () => {
  // (the old harbour town: a walkup whose two entrances front a street climbing 7 voxels along it)
  const w = createWorld(presetConfig("oldHarbourTown"));
  const env = w.cellPlan(0, -1).buildingById.get("C0_-1/b15/l2/B");
  const plan = w.buildingPlan(env);
  const doors = plan.floorByIndex.get(0).grid.doors.filter((d) => d.b === -1 && d.kind === "entrance");
  assert.equal(doors.length, 2);
  // the uphill one raised to the street (its sill that far over the floor), steps down inside
  const up = doors.find((d) => d.sill);
  assert.ok(up && up.street > env.groundZ + 3, JSON.stringify(up));
  assert.equal(up.sill, up.street - floorZ(env, 0) - 1);
  const F = frameOf(env);
  const [x, y] = F.toWorld(Math.floor((up.u0 + up.u1) / 2), up.v0);
  const z0 = floorZ(env, 0);
  const col = [];
  for (let z = z0 + 2; z < z0 + 2 + up.sill + 16; z += 1) {
    const c = buildChunk(w, 0, Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32));
    col.push(c.data[x - Math.floor(x / 32) * 32 + 1 + (y - Math.floor(y / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2]);
  }
  // (the wall under its sill, the opening over it)
  assert.ok(col.slice(0, up.sill).every((m) => IS_SOLID[m]) && col.slice(up.sill, up.sill + 14).every((m) => !IS_SOLID[m]), JSON.stringify(col));
  const r = walkabilityReport(w, env, { maxFloors: 2 });
  assert.ok(r.ok, JSON.stringify(r.missing.slice(0, 6)));
});
