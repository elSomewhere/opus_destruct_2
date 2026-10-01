import { test } from "node:test";
import assert from "node:assert/strict";
import { createSvxSource, gridFrame } from "../src/engine/svx/source.js";
import { CLASSIFY, CLASSES, CLASS, LOOKS, FLORA, CITY_BASE, vox, svxMaterials } from "../src/engine/svx/materials.js";
import { MATERIALS, MAT } from "../src/engine/voxel/materials.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { Placement, YAWS, PITCHES, yawIndex } from "../src/engine/core/placement.js";
import { partChunk } from "../src/engine/world/partRaster.js";
import { hash32 } from "../src/engine/core/hash.js";
import { planDressing } from "../src/engine/city/dressing.js";

/**
 * The structvox export (src/engine/svx): the city as opus_destruct_2's
 * ChunkSource sees it (core/include/svx/world/source.hpp): voxel bytes of
 * physics classes with their looks in a layer, the ground anchored, props
 * and furniture isolated, the parts as oriented grids placed exactly where
 * the city draws them, all pure functions of the chunk or the grid id.
 */

const H = 0.125;
const sources = new Map();
function source(preset, size = null) {
  const key = `${preset}/${size}`;
  if (!sources.has(key)) sources.set(key, createSvxSource(presetConfig(preset, { size })));
  return sources.get(key);
}
const svxIndex = (x, y, z) => (x * 32 + y) * 32 + z;

test("svx materials: every city material has a physics class; ids fit structvox; looks give the material back", () => {
  assert.equal(CLASSIFY.length, MATERIALS.length);
  const ids = new Set();
  for (const c of CLASSES) {
    assert.ok(Number.isInteger(c.id) && c.id >= 0 && c.id < 127, `${c.name}: id ${c.id}`);
    assert.ok(!ids.has(c.id), `${c.name}: id unique`);
    ids.add(c.id);
    // the city's own right after the core's (12) and the game's (9)
    if (c.own) assert.ok(c.id >= CITY_BASE && c.id < CITY_BASE + 16);
    else assert.ok(c.id < CITY_BASE);
  }
  for (const m of MATERIALS) {
    const c = CLASSIFY[m.id];
    if (m.id === 0) assert.ok(c.air);
    else if (c.liquid) assert.ok(["WATER", "SEWER_WATER", "SLUDGE", "NUKAGE"].includes(m.name));
    else {
      assert.equal(LOOKS.get(c.s)[c.sLook], m.id, `${m.name}: its look as structure`);
      assert.equal(LOOKS.get(c.g)[c.gLook], m.id, `${m.name}: its look as ground`);
      if (c.flora) assert.equal(FLORA[c.floraIdx], m.id);
    }
  }
  for (const list of LOOKS.values()) assert.ok(list.length <= 256);
  // what the classes are made of, spot checks
  assert.equal(CLASSIFY[MAT.ASPHALT].g, CLASS.asphalt.id);
  assert.equal(CLASSIFY[MAT.BRICK_RED].s, CLASS.masonry.id);
  assert.equal(CLASSIFY[MAT.GLASS].s, CLASS.glass.id);
  assert.ok(CLASSIFY[MAT.LEAVES].flora && CLASSIFY[MAT.WATER].liquid);
  assert.equal(CLASSIFY[MAT.BEDROCK].g, CLASS.bedrock.id);
  // the voxel byte: 1 + id, bit 7 anchored
  assert.equal(vox(CLASS.rc.id, false), 1);
  assert.equal(vox(CLASS.soil.id, true), 0x80 | 5);
  // what a host registers: complete Material records at their ids
  const reg = svxMaterials().register;
  assert.deepEqual(
    reg.map((r) => r.id),
    CLASSES.filter((c) => c.own).map((c) => c.id),
  );
  for (const r of reg) for (const f of ["E", "G", "rho", "ft", "fb", "fc", "cohesion", "friction", "Gf", "frag", "frag_noise"]) assert.ok(r[f] !== undefined, `${r.name}.${f}`);
});

test("svx conventions: the float quaternion a grid is placed with maps every voxel as the exact integer matrix does", () => {
  // (a primitive triple's legs have opposite parity, so a voxel centre never lands on a cell face: no ties to round)
  let mism = 0;
  let n = 0;
  const rolls = [0, 5, 16, 40];
  // (and the composed yaws of bays on turned buildings: a turned yaw times a canted bay's)
  const bays = [{ c: 24, s: 7, r: 25 }, { c: 24, s: -7, r: 25 }, { c: 40, s: 9, r: 41 }, { c: 40, s: -9, r: 41 }].map(yawIndex);
  const cases = [];
  for (let yaw = 0; yaw < YAWS.length; yaw += 1) {
    for (let pitch = -(PITCHES.length - 1); pitch < PITCHES.length; pitch += 1) for (const roll of pitch === 0 ? rolls : [0]) cases.push({ yaw, pitch, roll });
    for (const yaw2 of bays) cases.push({ yaw, yaw2 });
  }
  for (const c of cases) {
    const p = new Placement({ origin: { x: 1237, y: -811, z: 302 }, ...c });
    const f = gridFrame(p, H);
    const { x: qx, y: qy, z: qz, w: qw } = f.rot;
    for (let w = -4; w <= 4; w += 2)
      for (let v = -9; v <= 9; v += 2)
        for (let u = -9; u <= 9; u += 2) {
          // local cell -> world (the host's R(rot) (h p) + origin), back in city voxel units
          const hx = H * u;
          const hy = H * v;
          const hz = H * w;
          const tx = 2 * (qy * hz - qz * hy);
          const ty = 2 * (qz * hx - qx * hz);
          const tz = 2 * (qx * hy - qy * hx);
          const X = f.origin[0] + hx + qw * tx + (qy * tz - qz * ty);
          const Y = f.origin[1] + hy + qw * ty + (qz * tx - qx * tz);
          const Z = f.origin[2] + hz + qw * tz + (qx * ty - qy * tx);
          const got = [X, Y, Z].map((q) => Math.floor(q / H + 0.5));
          const want = p.toWorld(u, v, w);
          n += 1;
          if (got[0] !== want[0] || got[1] !== want[1] || got[2] !== want[2]) mism += 1;
        }
  }
  assert.ok(n > 1e6);
  assert.equal(mism, 0);
});

test("svx generate: the parts-mode world grid, byte for byte: physics classes, looks, the ground anchored, props and furniture isolated", () => {
  const src = source("angledCities");
  const w = src.world;
  // chunks through a building's ground floor and upper floors (furniture), and round a street prop
  const env = w.cellPlan(0, 0).buildings.find((b) => !b.turn && b.floors >= 3 && b.archetype === "walkup");
  const prop = planDressing(w, 0, 0).props.find((q) => q.kind === "streetlight" || q.kind === "bench");
  const at = (x, y, z) => [Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32)];
  const mid = (r) => [Math.floor((r.x0 + r.x1) / 2), Math.floor((r.y0 + r.y1) / 2)];
  let checked = 0;
  let anchored = 0;
  let isolated = 0;
  let free = 0;
  for (const [cx, cy, cz] of [at(...mid(env.R), env.baseZ + 2), at(...mid(env.R), env.baseZ + env.storyH[0] + 4), at(...mid(prop.bb), prop.bb.z0)]) {
    const c = buildChunk(w, 0, cx, cy, cz);
    const g = src.generate(cx, cy, cz);
    const look = src.generateLayer(cx, cy, cz, "look");
    const iso = new Map();
    const list = src.isolated(cx, cy, cz) ?? [];
    for (let k = 0; k < list.length; k += 3) iso.set(list[k], [list[k + 1], list[k + 2]]);
    isolated += iso.size;
    for (let z = 0; z < 32; z += 1)
      for (let y = 0; y < 32; y += 1)
        for (let x = 0; x < 32; x += 1) {
          const m = c.data[x + 1 + (y + 1) * P + (z + 1) * P2];
          const s = svxIndex(x, y, z);
          const v = g.vox[s];
          const cl = CLASSIFY[m];
          if (!m || cl.liquid || cl.flora) {
            assert.equal(v, 0);
            continue;
          }
          checked += 1;
          if (iso.has(s)) {
            assert.equal(v, 0, "an isolated voxel is not in the world grid");
            assert.equal(LOOKS.get((iso.get(s)[0] & 0x7f) - 1)[iso.get(s)[1]], m);
            continue;
          }
          assert.ok(v !== 0, `(${x}, ${y}, ${z}): ${MATERIALS[m].name} lost`);
          const id = (v & 0x7f) - 1;
          assert.equal(LOOKS.get(id)[look[s]], m, `(${x}, ${y}, ${z}): its look`);
          if (v & 0x80) anchored += 1;
          else free += 1;
        }
  }
  assert.ok(checked > 20000 && anchored > 5000 && free > 1000 && isolated > 0, `${checked} ${anchored} ${free} ${isolated}`);
});

test("svx generate: the ground anchored, a building on it free, water and plants in layers", () => {
  const src = source("angledNordicTown", "fjord");
  const w = src.world;
  // a column of open ground at the origin: anchored up to the surface, air above
  const g = src.generate(0, 0, Math.floor(src.spawn().pos[2] / H / 32));
  let anchoredTop = -1;
  for (let z = 0; z < 32; z += 1) if (g.vox[svxIndex(0, 0, z)] & 0x80) anchoredTop = z;
  assert.ok(anchoredTop >= 0);
  // water: air in the grid, 255 in its layer
  let water = 0;
  let flora = 0;
  for (let cy = -40; cy <= 40 && (!water || !flora); cy += 8)
    for (let cx = -40; cx <= 40 && (!water || !flora); cx += 8)
      for (let cz = -1; cz <= 3; cz += 1) {
        const wl = src.generateLayer(cx, cy, cz, "water");
        const fl = src.generateLayer(cx, cy, cz, "flora");
        const v = src.generate(cx, cy, cz).vox;
        if (wl) for (let s = 0; s < wl.length; s += 1) if (wl[s]) water += v[s] === 0 && wl[s] === 255 ? 1 : -1e9;
        if (fl) for (let s = 0; s < fl.length; s += 1) if (fl[s]) flora += v[s] === 0 && FLORA[fl[s] - 1] !== undefined ? 1 : -1e9;
      }
  assert.ok(water > 0 && flora > 0, `water ${water}, flora ${flora}`);
  // a building's walls are structure, not ground
  const env = w.cellPlan(0, 0).buildings.find((b) => !b.turn && b.floors >= 2);
  const r = env.R;
  const z = env.baseZ + env.storyH[0] + 8;
  const cx = Math.floor(r.x0 / 32);
  const cy = Math.floor(r.y0 / 32);
  const cz = Math.floor(z / 32);
  const v = src.generate(cx, cy, cz).vox;
  let wall = 0;
  for (let y = 0; y < 32; y += 1) for (let x = 0; x < 32; x += 1) if (v[svxIndex(x, y, z - cz * 32)]) wall += v[svxIndex(x, y, z - cz * 32)] & 0x80 ? -1e9 : 1;
  assert.ok(wall > 0, `a storey up: ${wall}`);
});

test("svx grids: the parts at home in a chunk, stable ids, priorities as structvox reads them, found again by id alone", () => {
  for (const [preset, size] of [["angledOldHarbourTown", null], ["angledCities", null]]) {
    const src = source(preset, size);
    const w = src.world;
    const parts = [];
    for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...w.cellPlan(i, j).parts);
    const kinds = new Set();
    for (const p of parts.filter((q, k) => k % 7 === 0)) {
      const gs = src.grids(p.home.cx, p.home.cy, p.home.cz);
      const g = gs.find((q) => q.id === p.id);
      assert.ok(g, `${p.key}: listed at its home chunk`);
      kinds.add(p.kind);
      assert.ok(g.id > 0 && g.id < 2 ** 32 && Number.isInteger(g.priority) && Math.abs(g.priority) < 2 ** 31);
      assert.equal(g.voxelSize, H);
      // (turned buildings, road slabs and chamfers displace the world grid where they meet it; a wing cast into a building it holds yields)
      const castIn = p.kind === "wing" && !p.key.endsWith("/chamfer") && !src.world.cellPlan(...p.cell).buildingById.get(p.env).turn;
      if (castIn) assert.ok(g.priority < 0, `${p.key}: yields to the world grid`);
      else assert.ok(g.priority > 0, `${p.key}: owns what it shares with the world grid`);
      assert.equal(g.anchored, p.kind === "road");
      const q = Math.hypot(g.rot.x, g.rot.y, g.rot.z, g.rot.w);
      assert.ok(Math.abs(q - 1) < 1e-12);
    }
    assert.ok(kinds.has("building") && kinds.has("wing"), [...kinds].join(","));
    // a fresh source finds any of them by id alone (structvox's generate_grid gets only the id)
    const fresh = createSvxSource(presetConfig(preset, { size }));
    for (const p of parts.filter((q, k) => k % 23 === 0)) assert.equal(fresh.grid(p.id)?.key, p.key);
  }
});

test("svx generateGrid: a part's voxels in its own lattice, a road slab's anchored", () => {
  const src = source("angledOldHarbourTown");
  const w = src.world;
  const parts = [];
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...w.cellPlan(i, j).parts);
  for (const kind of ["road", "wing", "building"]) {
    const p = parts.find((q) => q.kind === kind);
    const g = src.generateGrid(p.id);
    assert.ok(g && g.h === H && g.chunks.length > 0, `${p.key}: voxels`);
    let n = 0;
    for (const c of g.chunks) {
      const pc = partChunk(w, p, 0, c.cx, c.cy, c.cz);
      for (let z = 0; z < 32; z += 1)
        for (let y = 0; y < 32; y += 1)
          for (let x = 0; x < 32; x += 1) {
            const m = pc.data[x + 1 + (y + 1) * P + (z + 1) * P2];
            const v = c.vox[svxIndex(x, y, z)];
            const cl = CLASSIFY[m];
            if (!m || cl.flora || cl.liquid) continue;
            n += 1;
            assert.equal((v & 0x7f) - 1, cl.s, `${p.key}: class`);
            assert.equal(!!(v & 0x80), kind === "road", `${p.key}: anchored as it is`);
            assert.equal(LOOKS.get(cl.s)[c.look[svxIndex(x, y, z)]], m);
          }
    }
    assert.ok(n > 500, `${p.key}: ${n}`);
  }
});

test("svx: pure (any source, any order) and regions keep a building whole", () => {
  const a = source("angledOldHarbourTown");
  const b = createSvxSource(presetConfig("angledOldHarbourTown"));
  const hash = (arr) => {
    let h = 0;
    for (let k = 0; k < arr.length; k += 1) h = hash32(h, arr[k], k);
    return h;
  };
  const keys = [];
  for (let cy = -2; cy <= 1; cy += 1) for (let cx = -2; cx <= 1; cx += 1) keys.push([cx, cy, 3]);
  const ha = keys.map(([x, y, z]) => hash(a.generate(x, y, z).vox));
  const hb = keys
    .slice()
    .reverse()
    .map(([x, y, z]) => hash(b.generate(x, y, z).vox))
    .reverse();
  assert.deepEqual(ha, hb);
  // every chunk column a building's footprint centre area covers comes back with the same region
  const w = a.world;
  let whole = 0;
  let all = 0;
  for (const env of w.cellPlan(0, 0).buildings.slice(0, 60)) {
    const r = env.R;
    const keysOf = new Set();
    for (let y = r.y0 + 4; y <= r.y1 - 4; y += 8) for (let x = r.x0 + 4; x <= r.x1 - 4; x += 8) keysOf.add(a.region(Math.floor(x / 32), Math.floor(y / 32)));
    all += 1;
    if (keysOf.size <= 1) whole += 1;
  }
  assert.ok(whole / all > 0.9, `${whole} of ${all} buildings in one region`);
});

test("svx: the far tier, spawn and extent", () => {
  const src = source("angledCities");
  const sp = src.spawn();
  assert.equal(sp.pos.length, 3);
  const lo = src.chunkLo();
  const hi = src.chunkHi();
  for (let a = 0; a < 3; a += 1) assert.ok(lo[a] < hi[a] && Math.abs(lo[a]) * 32 < (1 << 20) && Math.abs(hi[a]) * 32 < (1 << 20));
  // the far tier: solid under the ground, air high above it
  const z0 = Math.floor(sp.pos[2] / H) - 64;
  const c = src.coarse([0, 0, z0], [4, 4, 4], 8);
  assert.equal(c.length, 64);
  assert.ok(c.every((v) => v !== 0));
  assert.ok(src.coarse([0, 0, z0 + 4000], [4, 4, 4], 8).every((v) => v === 0));
});

test("svx worker protocol: init, generate, grids, grid, region, coarse", async () => {
  const { handle } = await import("../src/engine/svx/worker.js");
  const state = {};
  const init = handle(state, { type: "init", config: presetConfig("angledOldHarbourTown") }).reply;
  assert.ok(init.extent[0].length === 3 && init.materials.register.length > 0);
  const gz = Math.floor(init.spawn.pos[2] / H / 32);
  const g = handle(state, { type: "generate", cx: 0, cy: 0, cz: gz });
  assert.ok(g.reply.vox.length === 32768 && g.transfer.length >= 1);
  const w = state.src.world;
  const p = w.cellPlan(0, 0).parts[0] ?? w.cellPlan(-1, 0).parts[0];
  const gr = handle(state, { type: "grids", cx: p.home.cx, cy: p.home.cy, cz: p.home.cz }).reply.grids;
  assert.ok(gr.some((q) => q.id === p.id));
  const one = handle(state, { type: "grid", gid: p.id }).reply;
  assert.equal(one.grid.id, p.id);
  assert.ok(one.voxels.chunks.length > 0);
  assert.ok(Number.isSafeInteger(handle(state, { type: "region", cx: 0, cy: 0 }).reply.region));
  assert.equal(handle(state, { type: "coarse", lo: [0, 0, 0], n: [2, 2, 2], factor: 8 }).reply.vox.length, 8);
  assert.throws(() => handle({}, { type: "generate", cx: 0, cy: 0, cz: 0 }));
});
