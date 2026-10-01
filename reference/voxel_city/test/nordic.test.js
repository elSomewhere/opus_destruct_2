import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { stageArchetype } from "../src/engine/buildings/sample.js";
import { planBuildingEnvelopeAs } from "../src/engine/buildings/archetypes.js";
import { planBuilding } from "../src/engine/buildings/interior/plan.js";
import { voxelizeBuilding } from "../src/engine/buildings/interior/voxelize.js";
import { voxelizeMassing } from "../src/engine/buildings/massing.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { ChunkBuffer } from "../src/engine/voxel/chunk.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { STYLES } from "../src/engine/world/registry.js";
import { Rng } from "../src/engine/core/hash.js";
import { vx } from "../src/engine/core/units.js";

/**
 * Nordic architecture kit: wooden town houses, forest cabins, churches and
 * roof snow.
 * The archetypes are staged on real graded lots (buildings/sample.js) so the
 * voxel walker can check them before any district places them.
 */

const OLDTOWN = { id: "oldtown", floors: [2, 3], archetypes: [], styles: [] };
const FOREST = { id: "forest", floors: [1, 1], archetypes: [], styles: [] };

/** A cabin-sized square plot (14-22 m) at the front of a larger lot. */
const cabinPlot = (lot, f) => {
  const side = vx(14 + ((lot.rect.x0 * 7 + lot.rect.y0 * 3) & 0xffff) % 9);
  return side <= f.U && side <= f.V ? { x0: 0, y0: 0, x1: side - 1, y1: side - 1 } : null;
};

function frontEntrance(plan) {
  return plan.floorByIndex.get(0).grid.doors.find((d) => d.b === -1 && (d.kind === "entrance" || d.kind === "shopfront") && d.v0 <= 2);
}

test("nordic styles and materials are registered", () => {
  for (const id of ["nordicWood", "nordicPlaster", "cabin", "nordicChurch"]) assert.ok(STYLES.get(id), id);
  for (const m of ["CLAD_FALU", "CLAD_OCHRE", "CLAD_WHITE", "CLAD_GREYBLUE", "CLAD_GREEN", "LOG_TARRED", "ROOF_BLACK_METAL", "ROOF_SLATE", "RENDER_YELLOW"]) assert.ok(MAT[m] > 0, m);
});

test("town houses: valid plans on several seeds, shops in the old town, walkable to every room", () => {
  const programs = new Set();
  let gableFront = 0;
  for (const seed of [1337, 7, 4242]) {
    const w = createWorld({ seed });
    const envs = [
      ...stageArchetype(w, "townhouse", "nordicWood", { n: 5, from: ["walkup", "rowhouse"], district: OLDTOWN }),
      ...stageArchetype(w, "townhouse", "nordicPlaster", { n: 2, from: ["rowhouse", "walkup"], district: { ...OLDTOWN, id: "mixed" } }),
    ];
    assert.ok(envs.length >= 5, `seed ${seed}: staged ${envs.length}`);
    for (const env of envs) {
      assert.ok(env.floors >= 2 && env.floors <= 3, `${env.id} floors`);
      assert.equal(env.roof.type, "gable");
      const plan = w.buildingPlan(env);
      assert.deepEqual(plan.issues, [], `${env.id} ${env.program.ground}`);
      assert.ok(frontEntrance(plan), `${env.id}: entrance on the front facade`);
      const r = walkabilityReport(w, env, {});
      assert.ok(r.ok, `${env.id} ${env.program.ground} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
      programs.add(env.program.ground);
      if (env.roof.ridge === "v") gableFront += 1;
    }
  }
  for (const p of ["retail", "house", "apartments"]) assert.ok(programs.has(p), `program ${p}`);
  assert.ok(gableFront > 0, "some gables face the street");
});

test("cabins on 14-22 m forest plots: synthetic lots on every side plan a complete small home", () => {
  const w = createWorld({ seed: 99 });
  const rng = new Rng(5);
  let n = 0;
  for (let k = 0; k < 40; k += 1) {
    const side = vx(rng.float(14, 22));
    const deep = vx(rng.float(14, 22));
    const front = ["N", "S", "E", "W"][k % 4];
    const x0 = 4000 + k * 400;
    const rect = front === "N" || front === "S" ? { x0, y0: 0, x1: x0 + side - 1, y1: deep - 1 } : { x0, y0: 0, x1: x0 + deep - 1, y1: side - 1 };
    const lot = { id: `T/b0/l${k}`, rect, front, district: "forest", u: 0, core: 0 };
    const env = planBuildingEnvelopeAs(lot, "cabin", "cabin", FOREST, rng.fork(`c${k}`), { u: 0, core: 0, groundZ: 80, config: w.config });
    assert.ok(env, `cabin on ${side / 8}x${deep / 8} m`);
    assert.equal(env.floors, 1);
    assert.ok(env.porch && env.plinth);
    const plan = planBuilding(w, env);
    assert.deepEqual(plan.issues, [], env.id);
    const types = plan.floors[0].grid.rooms.map((r) => r.type);
    for (const t of ["living", "kitchen", "bedroom", "bath", "foyer"]) assert.ok(types.includes(t), `${env.id} has a ${t}`);
    assert.ok(frontEntrance(plan), `${env.id}: front door`);
    const door = frontEntrance(plan);
    assert.ok(Math.abs((door.u0 + door.u1) / 2 - env.entranceU) <= 4, "envelope knows the door");
    n += 1;
  }
  assert.equal(n, 40);
});

test("cabins are walkable at voxel level, porch and steps included", () => {
  for (const seed of [1337, 2024]) {
    const w = createWorld({ seed });
    const envs = stageArchetype(w, "cabin", "cabin", { n: 4, from: ["house"], district: FOREST, radius: 5, lotRect: cabinPlot });
    assert.ok(envs.length >= 2, `seed ${seed}: staged ${envs.length}`);
    for (const env of envs) {
      assert.deepEqual(w.buildingPlan(env).issues, [], env.id);
      const r = walkabilityReport(w, env, {});
      assert.ok(r.ok, `${env.id} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
    }
  }
});

test("wooden churches: a vestibule in the tower and one open nave, steeple above the ridge, walkable", () => {
  for (const seed of [1337, 7]) {
    const w = createWorld({ seed });
    const envs = stageArchetype(w, "church", "nordicChurch", { n: 3, from: ["walkup", "midrise", "house"], district: OLDTOWN });
    assert.ok(envs.length >= 2, `seed ${seed}: staged ${envs.length}`);
    for (const env of envs) {
      assert.ok(env.steeple && env.roof.ridge === "v");
      const plan = w.buildingPlan(env);
      assert.deepEqual(plan.issues, [], env.id);
      assert.deepEqual(plan.floors[0].grid.rooms.map((r) => r.type).sort(), ["foyer", "nave"]);
      assert.ok(frontEntrance(plan), `${env.id}: door in the tower front`);
      // the spire tops the roof and stays inside the envelope's height
      const ridge = Math.floor((env.U / 2 + 3) * env.roof.slope) + 1;
      assert.ok(env.steeple.shaft > ridge && env.topZ >= env.baseZ + env.storyH[0] + env.steeple.shaft + env.steeple.spire + 16);
      const r = walkabilityReport(w, env, {});
      assert.ok(r.ok, `${env.id} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
    }
  }
});

/** Count snow voxels a building's own voxelizer writes (LOD0 interiors or coarse massing). */
function roofSnow(world, env, lod) {
  const s = 32 << lod;
  let snow = 0;
  let roof = 0;
  const b = env.bounds;
  for (let cz = Math.floor(env.baseZ / s); cz <= Math.floor((env.topZ + 4) / s); cz += 1)
    for (let cy = Math.floor(b.y0 / s); cy <= Math.floor(b.y1 / s); cy += 1)
      for (let cx = Math.floor(b.x0 / s); cx <= Math.floor(b.x1 / s); cx += 1) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        if (lod === 0) voxelizeBuilding(world, env, ch);
        else voxelizeMassing(world, env, ch);
        for (const m of ch.data) {
          if (m === MAT.SNOW) snow += 1;
          else if (m === MAT.ROOF_MEMBRANE || m === MAT.ROOF_GRAVEL || m === MAT.ROOF_BLACK_METAL || m === MAT.ROOF_BLACK_TILE || m === MAT.ROOF_TILE_DARK || m === MAT.ROOF_GREEN_DARK || m === MAT.ROOF_SLATE || m === MAT.ROOF_SOD) roof += 1;
        }
      }
  return { snow, roof };
}

function roofSamples(world) {
  const town = stageArchetype(world, "townhouse", "nordicWood", { n: 1, from: ["walkup", "rowhouse"], district: OLDTOWN });
  const cabin = stageArchetype(world, "cabin", "cabin", { n: 1, from: ["house"], district: FOREST, radius: 5, lotRect: cabinPlot });
  const ids = new Set([...town, ...cabin].map((e) => e.id));
  const flat = world.cellPlan(0, 0).buildings.find((e) => e.roof.type === "flat" && e.floors <= 6 && !ids.has(e.id));
  return [...town, ...cabin, flat];
}

test("roof snow only when config.world.climate.snowCover is set, at every LOD", () => {
  const bare = createWorld({ seed: 1337 });
  for (const env of roofSamples(bare)) {
    for (const lod of [0, 1]) assert.equal(roofSnow(bare, env, lod).snow, 0, `${env.archetype} lod ${lod}`);
  }
  const winter = createWorld({ seed: 1337, world: { climate: { snowCover: 1 } } });
  const winterSamples = roofSamples(winter);
  assert.equal(winterSamples.length, 3);
  for (const env of winterSamples) {
    for (const lod of [0, 1]) {
      const { snow, roof } = roofSnow(winter, env, lod);
      assert.ok(snow > 0, `${env.archetype} ${env.roof.type} lod ${lod}: snow`);
      // the eaves / parapets stay bare, but the roof surface is white
      assert.ok(snow > roof * 0.15, `${env.archetype} lod ${lod}: ${snow} snow vs ${roof} roofing`);
    }
  }
  const patchy = createWorld({ seed: 1337, world: { climate: { snowCover: 0.5 } } });
  const [town] = roofSamples(patchy);
  const full = roofSnow(winter, winterSamples[0], 0).snow;
  const half = roofSnow(patchy, town, 0).snow;
  assert.ok(half > 0 && half < full, `partial cover ${half} of ${full}`);
});
