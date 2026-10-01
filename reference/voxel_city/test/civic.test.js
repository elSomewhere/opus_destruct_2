import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { stageArchetype } from "../src/engine/buildings/sample.js";
import { CIVIC, civicStyle } from "../src/engine/buildings/civic.js";
import { voxelizeBuilding } from "../src/engine/buildings/interior/voxelize.js";
import { voxelizeMassing } from "../src/engine/buildings/massing.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { ChunkBuffer } from "../src/engine/voxel/chunk.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { Rng } from "../src/engine/core/hash.js";
import { rOverlaps } from "../src/engine/core/rect.js";

/**
 * Civic buildings, venues and big shops (buildings/civic.js): every
 * archetype plans a valid, walkable interior with its own rooms; towns
 * place them (town hall by the square, hospitals, police and fire
 * stations, supermarkets and petrol stations further out; houses of
 * culture and polyclinics in Russian towns, market halls by a harbour);
 * warehouse variants; onion-dome churches.
 */

const worlds = new Map();
function world(preset = null) {
  if (!worlds.has(preset)) worlds.set(preset, createWorld(preset ? presetConfig(preset, { seed: 1337 }) : { seed: 1337 }));
  return worlds.get(preset);
}

/** Rooms that make each civic building what it is. */
const ROOMS = {
  supermarket: ["sales", "storage"],
  departmentStore: ["departmentFloor"],
  petrolStation: ["grocery"],
  marketHall: ["marketHall"],
  concertHall: ["auditorium", "foyerBar", "dressing", "void"],
  houseOfCulture: ["auditorium", "danceHall", "clubroom"],
  musicClub: ["venueFloor", "dressing"],
  cinema: ["cinema", "kiosk"],
  hospital: ["emergency", "ward", "operating", "radiology"],
  polyclinic: ["exam", "waiting", "radiology"],
  policeStation: ["policeDesk", "cell", "interview", "garageBay"],
  fireStation: ["garageBay", "dorm", "lockerRoom"],
  museum: ["exhibit", "museumShop"],
  artGallery: ["gallery"],
  library: ["library"],
  townHall: ["council", "registry"],
  hotel: ["reception", "hotelRoom"],
};

const FROM = ["warehouse", "factory", "office", "tower", "midrise", "walkup", "house", "school", "rowhouse", "panelSlab"];

test("civic buildings: every archetype plans a valid, walkable interior with its own rooms", () => {
  const w = world();
  assert.deepEqual(Object.keys(ROOMS).sort(), Object.keys(CIVIC).sort());
  for (const id of Object.keys(CIVIC)) {
    const envs = stageArchetype(w, id, civicStyle(id, "modern", Rng.from(1, id)), { n: 1, radius: 5, from: FROM });
    assert.equal(envs.length, 1, `${id} staged`);
    const env = envs[0];
    assert.equal(env.civic, id);
    const plan = w.buildingPlan(env);
    assert.deepEqual(plan.issues, [], id);
    const types = new Set(plan.floors.flatMap((f) => f.grid.rooms.map((r) => r.type)));
    for (const t of ROOMS[id]) assert.ok(types.has(t), `${id}: ${t} in ${[...types]}`);
    const r = walkabilityReport(w, env, { maxFloors: 2 });
    assert.ok(r.ok, `${id} ${env.id} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
  }
});

/** Buildings of the cells round the spawn. */
function spawnBuildings(w, r = 3) {
  const out = [];
  for (let j = -r; j <= r; j += 1) for (let i = -r; i <= r; i += 1) out.push(...w.cellPlan(i, j).buildings.map((b) => ({ b, i, j })));
  return out;
}

test("towns place civic buildings on lots of their own", () => {
  const w = world();
  const all = spawnBuildings(w);
  const civic = all.filter(({ b }) => CIVIC[b.archetype]);
  const kinds = new Set(civic.map(({ b }) => b.archetype));
  for (const k of ["townHall", "hospital", "policeStation", "fireStation", "supermarket", "petrolStation", "museum", "concertHall", "cinema", "library", "hotel", "musicClub"]) assert.ok(kinds.has(k), `${k} in ${[...kinds]}`);
  // no other building stands on a civic lot
  for (const { b, i, j } of civic) {
    const lot = w.cellPlan(i, j).lots.find((l) => l.id === b.lot);
    assert.ok(lot, b.id);
    for (const o of w.cellPlan(i, j).buildings) if (o.id !== b.id) assert.ok(!rOverlaps(o.R, lot.rect), `${o.id} on ${b.lot}`);
  }
  // a petrol station has its canopy and pumps, a supermarket its car park, a museum or town hall its portico
  const petrol = civic.find(({ b }) => b.archetype === "petrolStation").b;
  assert.ok(petrol.annexes.some((a) => a.kind === "canopy") && petrol.storefront);
  assert.ok(civic.find(({ b }) => b.archetype === "supermarket").b.parking);
  assert.ok(civic.some(({ b }) => b.portico));
});

test("a civic building placed in a town (portico, signs) is walkable from the street", () => {
  const w = world();
  const withPortico = spawnBuildings(w).filter(({ b }) => b.portico).slice(0, 2);
  assert.ok(withPortico.length >= 1);
  for (const { b } of withPortico) {
    assert.deepEqual(w.buildingPlan(b).issues, [], b.id);
    const r = walkabilityReport(w, b, { maxFloors: 1 });
    assert.ok(r.ok, `${b.archetype} ${b.id} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
  }
});

test("Russian and Nordic towns: houses of culture, polyclinics, market halls", () => {
  const civicIn = (preset) => {
    const w = world(preset);
    const s = w.fields.islandSettlements().towns[0];
    const r = Math.round(s.radius * 1.3);
    const cells = new Map();
    for (let y = -r; y <= r; y += 1000) for (let x = -r; x <= r; x += 1000) {
      const c = w.cellAt(s.x + x, s.y + y);
      cells.set(`${c.i},${c.j}`, c);
    }
    const kinds = new Set();
    for (const c of cells.values()) for (const b of w.cellPlan(c.i, c.j).buildings) if (CIVIC[b.archetype]) kinds.add(b.archetype);
    return kinds;
  };
  const white = civicIn("whiteSeaTown");
  for (const k of ["houseOfCulture", "polyclinic", "townHall", "supermarket"]) assert.ok(white.has(k), `White Sea town: ${k} in ${[...white]}`);
  assert.ok(!white.has("concertHall"), "a house of culture instead of a concert hall");
  const nordic = civicIn("oldHarbourTown");
  for (const k of ["marketHall", "townHall", "museum"]) assert.ok(nordic.has(k), `harbour town: ${k} in ${[...nordic]}`);
});

test("warehouse variants: distribution centres, self-storage, cold stores, timber merchants", () => {
  const w = world();
  const kinds = new Map();
  for (const { b } of spawnBuildings(w, 5)) {
    if (b.archetype !== "warehouse") continue;
    if (!kinds.has(b.program.ground)) kinds.set(b.program.ground, b);
  }
  assert.ok(kinds.size >= 4, [...kinds.keys()].join());
  for (const [k, env] of kinds) {
    const plan = w.buildingPlan(env);
    assert.deepEqual(plan.issues, [], env.id);
    assert.ok(plan.floors[0].grid.rooms.some((r) => r.type === k), `${k} hall`);
  }
});

/** Materials a building's own voxelizer writes (LOD0 interiors or coarse massing). */
function materials(w, env, lod) {
  const s = 32 << lod;
  const seen = new Set();
  const b = env.bounds;
  for (let cz = Math.floor(env.baseZ / s); cz <= Math.floor((env.topZ + 4) / s); cz += 1)
    for (let cy = Math.floor(b.y0 / s); cy <= Math.floor(b.y1 / s); cy += 1)
      for (let cx = Math.floor(b.x0 / s); cx <= Math.floor(b.x1 / s); cx += 1) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        if (lod === 0) voxelizeBuilding(w, env, ch);
        else voxelizeMassing(w, env, ch);
        for (const m of ch.data) seen.add(m);
      }
  return seen;
}

test("Orthodox churches in Russian and Karelian towns: onion domes and an icon screen", () => {
  const churches = (w) => {
    const out = [];
    for (let j = -2; j <= 2; j += 1) for (let i = -2; i <= 2; i += 1) for (const b of w.cellPlan(i, j).buildings) if (b.archetype === "church") out.push(b);
    return out;
  };
  const w = world("whiteSeaTown");
  const found = churches(w);
  // (the town's church has domes along its nave too; a cemetery chapel only the tower's)
  assert.ok(found.length >= 1 && found.every((c) => c.steeple.dome), "onion domes on the bell towers");
  assert.ok(found.some((c) => c.domes && c.domes.length >= 1), "domes on the nave");
  for (const church of found) {
    const plan = w.buildingPlan(church);
    assert.deepEqual(plan.issues, []);
    assert.ok(plan.floors[0].grid.rooms.some((r) => r.type === "orthodoxNave"));
    for (const lod of [0, 1]) {
      const mats = materials(w, church, lod);
      assert.ok(mats.has(church.steeple.dome) && mats.has(MAT.GOLD), `${church.id}: dome and gilded cross at lod ${lod}`);
    }
    assert.ok(walkabilityReport(w, church, {}).ok);
  }
  // a western town's church keeps its spire
  const plain = churches(world("oldHarbourTown"));
  assert.ok(plain.length && plain.every((c) => !c.steeple.dome && !c.domes));
});
