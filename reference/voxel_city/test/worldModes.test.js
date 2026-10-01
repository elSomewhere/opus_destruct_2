import test from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { PRESETS, presetConfig } from "../src/engine/config/presets.js";
import { groundTile } from "../src/engine/voxel/compose.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { edgeInfo } from "../src/engine/city/cellNetwork.js";

test("presets: every preset (and size) builds a world with ground at the spawn", () => {
  for (const p of PRESETS.all()) {
    for (const size of p.sizes ?? [null]) {
      const w = createWorld(presetConfig(p.id, { size: size?.id ?? null, seed: 7 }));
      const h = w.terrain.sample(0, 0).h;
      assert.ok(Number.isFinite(h), `${p.id}/${size?.id}: terrain at the spawn`);
      assert.ok(h > 0, `${p.id}/${size?.id}: the spawn is on land`);
    }
  }
});

test("wrapping world: fields, lattices, towns, cell plans and ground repeat every lap", () => {
  const w = createWorld(presetConfig("wrapWorld", { size: "small", seed: 11 }));
  const S = w.config.world.size * 8;
  assert.equal(S % (240 * 8), 0);
  // continuous fields
  for (let k = 0; k < 200; k += 1) {
    const x = Math.round(Math.sin(k * 12.9898) * S);
    const y = Math.round(Math.cos(k * 78.233) * S);
    const a = w.terrain.sample(x, y);
    const b = w.terrain.sample(x + S, y - 2 * S);
    assert.ok(Math.abs(a.h - b.h) < 1e-6, "terrain repeats");
    assert.ok(Math.abs(a.u - b.u) < 1e-9, "urbanization repeats");
    assert.equal(w.landCover.biomeAt(x, y, a.h / 8).id, w.landCover.biomeAt(x + S, y - 2 * S, b.h / 8).id);
  }
  // lattices
  const n = w.arterials.n;
  assert.ok(n > 0 && n % 2 === 0);
  for (const i of [-5, 0, 3, n - 1]) assert.equal(w.arterials.line(1, i + n) - w.arterials.line(1, i), S);
  const t0 = w.fields.settlement(0, 0);
  const tn = w.fields.settlement(w.fields.nTown, -w.fields.nTown);
  assert.equal(tn.id, t0.id);
  assert.equal(tn.x - t0.x, S);
  assert.equal(tn.y - t0.y, -S);
  // the town's cells plan the same streets and buildings one lap away
  const c = w.cellAt(t0.x, t0.y);
  for (const [di, dj] of [[0, 0], [1, 1], [-1, 2]]) {
    const p = w.cellPlan(c.i + di, c.j + dj);
    const q = w.cellPlan(c.i + di - n, c.j + dj + n);
    assert.equal(q.net.roads.length, p.net.roads.length);
    assert.equal(q.buildings.length, p.buildings.length);
    p.buildings.forEach((b, k) => {
      const o = q.buildings[k];
      assert.equal(o.archetype, b.archetype);
      assert.equal(o.floors, b.floors);
      assert.equal(o.R.x0 - b.R.x0, -S);
      assert.equal(o.R.y0 - b.R.y0, S);
    });
  }
  // voxel ground, up close and far away
  for (const lod of [0, 3]) {
    const cs = 32 << lod;
    const cx = Math.floor(t0.x / cs) + 2;
    const cy = Math.floor(t0.y / cs) - 1;
    const A = groundTile(w, lod, cx, cy);
    const B = groundTile(w, lod, cx + S / cs, cy + S / cs);
    assert.deepEqual([...B.z], [...A.z], `LOD${lod} ground heights repeat`);
    assert.deepEqual([...B.top], [...A.top], `LOD${lod} ground materials repeat`);
  }
});

test("wrapping world: climate runs from an equator to a pole around the world", () => {
  const w = createWorld(presetConfig("wrapWorld", { size: "small", seed: 3 }));
  const S = w.config.world.size * 8;
  let hot = -Infinity;
  let cold = Infinity;
  for (let k = 0; k < 24; k += 1) {
    const t = w.fields.temperature(0, (k / 24) * S);
    hot = Math.max(hot, t);
    cold = Math.min(cold, t);
  }
  assert.ok(hot > 0.75, `a warm belt (${hot.toFixed(2)})`);
  assert.ok(cold < 0.25, `a polar belt (${cold.toFixed(2)})`);
});

const nordic = createWorld(presetConfig("nordicIsland", { seed: 1337 }));

test("island: one island in the sea, its town at the origin, places on land", () => {
  const w = nordic;
  const isl = w.fields.island;
  assert.ok(isl.coast(0, 0) > 0, "the main town stands on land");
  // the open sea all around, far out
  const b = isl.bounds();
  for (let k = 0; k < 16; k += 1) {
    const a = (k / 16) * Math.PI * 2;
    const r = (b.x1 - b.x0) * 0.75;
    const x = (-isl.ox + Math.cos(a) * r) * 8;
    const y = (-isl.oy + Math.sin(a) * r) * 8;
    assert.ok(w.terrain.sample(x, y).h < -8 * 20, "deep sea beyond the island");
  }
  const { towns, villages } = w.fields.islandSettlements();
  const cfg = w.config.world.island;
  assert.equal(towns[0].id, "S0_0");
  assert.ok(towns.length <= 1 + cfg.towns && villages.length <= cfg.villages + cfg.hamlets);
  assert.ok(towns.length + villages.length >= 3, "a town and some smaller places");
  for (const s of [...towns, ...villages]) assert.ok(w.fields.coastDistance(s.x, s.y) > 100, `${s.id} is inland`);
  // the smaller places are joined to the town by country roads
  const trunk = isl.trunkEdges(w);
  assert.ok(trunk.size > 10);
  for (const key of trunk) {
    const [axis, line, span] = key.split(":").map(Number);
    assert.ok(edgeInfo(w, axis, line, span).cls, `trunk edge ${key} carries a road`);
  }
});

test("island: nothing is built over the sea; the harbour has a quay", () => {
  const w = nordic;
  const c0 = w.cellAt(0, 0);
  let quay = null;
  for (let dj = -3; dj <= 3; dj += 1)
    for (let di = -3; di <= 3; di += 1) {
      const plan = w.cellPlan(c0.i + di, c0.j + dj);
      for (const r of plan.net.roads) {
        const a = r.pts[0];
        const z = r.pts[r.pts.length - 1];
        assert.ok(w.fields.coastDistance((a.x + z.x) / 2, (a.y + z.y) / 2) > -2, `road ${r.id} is on land`);
      }
      for (const lot of plan.lots) {
        if (!lot.building) continue;
        const cx = (lot.rect.x0 + lot.rect.x1) / 2;
        const cy = (lot.rect.y0 + lot.rect.y1) / 2;
        assert.ok(w.fields.coastDistance(cx, cy) > 0, `lot ${lot.id} is on land`);
      }
      quay ??= plan.spaces.find((s) => s.quay);
    }
  assert.ok(quay, "a quay at the harbour");
  assert.equal(quay.quay.level, 0, "the quay faces the sea");
});

test("island coasts: cliffs rise straight from the water, beaches slope gently", () => {
  const w = nordic;
  const isl = w.fields.island;
  const b = isl.bounds();
  let cliffs = 0;
  let beaches = 0;
  const step = 100;
  for (let ym = b.y0; ym <= b.y1 && (cliffs < 6 || beaches < 6); ym += step)
    for (let xm = b.x0; xm <= b.x1; xm += step) {
      const c = isl.coast(xm, ym);
      if (c < 4 || c > 20) continue;
      if (w.fields.urban(xm * 8, ym * 8).u > 0.05) continue;
      // walk inland along the coast gradient
      const gx = isl.coast(xm + 5, ym) - c;
      const gy = isl.coast(xm, ym + 5) - c;
      const g = Math.hypot(gx, gy) || 1;
      const hAt = (d) => w.terrain.sample((xm + (gx / g) * d) * 8, (ym + (gy / g) * d) * 8).h / 8;
      const k = isl.cliff(xm, ym);
      if (k > 0.95 && cliffs < 6) {
        cliffs += 1;
        assert.ok(hAt(40) > 8, `a cliff at ${xm.toFixed(0)},${ym.toFixed(0)} (${hAt(40).toFixed(1)} m 40 m inland)`);
      } else if (k < 0.02 && beaches < 6 && isl.highland(xm, ym) < 0.05) {
        beaches += 1;
        assert.ok(hAt(30) < 6, `a beach at ${xm.toFixed(0)},${ym.toFixed(0)} (${hAt(30).toFixed(1)} m 30 m inland)`);
      }
    }
  assert.ok(cliffs > 0 && beaches > 0, `found ${cliffs} cliffs and ${beaches} beaches`);
});

test("winter: snow on open ground, frozen lakes, an open sea", () => {
  const w = nordic;
  const isl = w.fields.island;
  const cs = 32 << 2;
  const count = (t, fn) => {
    let n = 0;
    for (let i = 0; i < t.top.length; i += 1) if (fn(i)) n += 1;
    return n;
  };
  // countryside around the town
  let snow = 0;
  let green = 0;
  for (let k = 0; k < 12; k += 1) {
    const a = (k / 12) * Math.PI * 2;
    const x = Math.cos(a) * 2600 * 8;
    const y = Math.sin(a) * 2600 * 8;
    if (isl.coast(x / 8, y / 8) < 20) continue;
    const t = groundTile(w, 2, Math.floor(x / cs), Math.floor(y / cs));
    snow += count(t, (i) => t.top[i] === MAT.SNOW || t.top[i] === MAT.SNOW_WIND);
    green += count(t, (i) => t.top[i] === MAT.GRASS || t.top[i] === MAT.GRASS_LAWN || t.top[i] === MAT.GRASS_DRY);
  }
  assert.ok(snow > 1000, `snow cover (${snow} columns)`);
  assert.ok(snow > green * 4, `mostly white (${snow} snow, ${green} grass)`);
  // the sea off the harbour stays open
  const h = isl.harbour();
  const sx = (h.x + h.dx * 250) * 8;
  const sy = (h.y + h.dy * 250) * 8;
  const st = groundTile(w, 2, Math.floor(sx / cs), Math.floor(sy / cs));
  const sea = count(st, (i) => st.water[i] > st.z[i]);
  assert.ok(sea > 0, "sea water off the harbour");
  assert.ok(!st.ice || count(st, (i) => st.ice[i]) === 0, "the sea is not frozen");
  // lakes freeze
  let lake = null;
  for (let b = -4; b <= 4 && !lake; b += 1) for (let a = -4; a <= 4 && !lake; a += 1) lake = w.lakes.lake(a, b);
  assert.ok(lake, "a lake on the island");
  const lt = groundTile(w, 2, Math.floor(lake.x / cs), Math.floor(lake.y / cs));
  const wet = count(lt, (i) => lt.water[i] > lt.z[i]);
  assert.ok(wet > 0 && count(lt, (i) => lt.water[i] > lt.z[i] && lt.ice?.[i]) === wet, "the lake is frozen");
});

test("nordic town: a wooden old town, panel estates, a harbour, a church and cabins in the woods", () => {
  const w = nordic;
  const c0 = w.cellAt(0, 0);
  const districts = new Set();
  const arch = {};
  let church = false;
  for (let dj = -2; dj <= 2; dj += 1)
    for (let di = -2; di <= 2; di += 1) {
      const plan = w.cellPlan(c0.i + di, c0.j + dj);
      for (const s of plan.net.subcells) districts.add(s.district);
      for (const b of plan.buildings) {
        if (b.district === "oldtown") arch[b.archetype] = (arch[b.archetype] ?? 0) + 1;
        if (b.archetype === "church") church = true;
      }
    }
  for (const d of ["oldtown", "mixed", "microdistrict", "harbour"]) assert.ok(districts.has(d), `${d} in ${[...districts]}`);
  assert.ok(arch.townhouse > (arch.walkup ?? 0), `town houses dominate the old town ${JSON.stringify(arch)}`);
  assert.ok(church, "a church in the old town");
  // cabins: freestanding in the woods or on the shore, each with a cabin on it
  let cabins = 0;
  for (let dj = -8; dj <= 8; dj += 2)
    for (let di = -8; di <= 8; di += 2)
      for (const lot of w.cellPlan(c0.i + di, c0.j + dj).lots)
        if (lot.cabin && lot.building) {
          cabins += 1;
          assert.equal(w.envelope(lot.building).archetype, "cabin");
          assert.ok(w.fields.coastDistance((lot.rect.x0 + lot.rect.x1) / 2, (lot.rect.y0 + lot.rect.y1) / 2) > 20);
        }
  assert.ok(cabins >= 5, `${cabins} cabins`);
});
