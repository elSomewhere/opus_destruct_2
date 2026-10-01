import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";

/**
 * Default city layouts: varied collector layouts (jogs, T-junctions),
 * jogged grids, a historic core of cobbled irregular blocks, a hierarchy
 * of avenues and narrower main streets. Blocks and lots never overlap.
 */

const w = createWorld({ seed: 1337 });

test("city blocks never overlap and stay inside their cell (jogged grids, T and jogged collectors)", () => {
  for (let j = -3; j <= 3; j += 1)
    for (let i = -3; i <= 3; i += 1) {
      const net = w.cellNet(i, j);
      const B = net.blocks;
      for (const b of B) {
        assert.ok(b.rect.x0 >= net.rect.x0 && b.rect.x1 <= net.rect.x1 && b.rect.y0 >= net.rect.y0 && b.rect.y1 <= net.rect.y1, `${b.id} inside ${net.id}`);
        assert.ok(b.prop.x1 > b.prop.x0 && b.prop.y1 > b.prop.y0, `${b.id} has a property rect`);
      }
      for (let a = 0; a < B.length; a += 1)
        for (let c = a + 1; c < B.length; c += 1) {
          const p = B[a].prop;
          const q = B[c].prop;
          const overlap = p.x0 < q.x1 && q.x0 < p.x1 && p.y0 < q.y1 && q.y0 < p.y1;
          assert.ok(!overlap, `${B[a].id} overlaps ${B[c].id}`);
        }
    }
});

test("collectors: not every town cell is four equal squares", () => {
  let cross = 0;
  let other = 0;
  for (let j = -4; j <= 4; j += 1)
    for (let i = -4; i <= 4; i += 1) {
      const net = w.cellNet(i, j);
      const cols = net.roads.filter((r) => r.cls === "collector" && !r.sub && !r.arterialEdge);
      if (!cols.length) continue;
      const full = cols.filter((r) => {
        const [p, q] = r.pts;
        return p.x === q.x ? Math.min(p.y, q.y) === net.rect.y0 && Math.max(p.y, q.y) === net.rect.y1 : Math.min(p.x, q.x) === net.rect.x0 && Math.max(p.x, q.x) === net.rect.x1;
      });
      if (cols.length === 2 && full.length === 2) cross += 1;
      else other += 1;
    }
  assert.ok(cross > 3 && other > 3, `${cross} plain crosses, ${other} jogs / T / single`);
});

test("the spawn city has a historic core of small cobbled blocks round its centre", () => {
  const core = [];
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) core.push(...w.cellNet(i, j).blocks.filter((b) => b.district === "oldcore"));
  assert.ok(core.length > 15, `${core.length} old-core blocks`);
  for (const b of core) assert.ok(Math.hypot((b.rect.x0 + b.rect.x1) / 2, (b.rect.y0 + b.rect.y1) / 2) < 700 * 8, `${b.id} near the centre`);
  const cobbled = [];
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) cobbled.push(...w.cellNet(i, j).roads.filter((r) => r.paving === "cobble"));
  assert.ok(cobbled.length > 10, `${cobbled.length} cobbled streets`);
  // pitched roofs in the core
  let pitched = 0;
  let n = 0;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1)
      for (const e of w.cellPlan(i, j).buildings)
        if (e.district === "oldcore" && (e.archetype === "walkup" || e.archetype === "rowhouse")) {
          n += 1;
          if (e.roof.type !== "flat") pitched += 1;
        }
  assert.ok(n > 20 && pitched > n * 0.5, `${pitched}/${n} pitched`);
});

test("a hierarchy of main roads: avenues on the subway lines and in the centre, collectors between", () => {
  const cls = new Map();
  for (let j = -4; j <= 4; j += 1)
    for (let i = -4; i <= 4; i += 1)
      for (const r of w.cellNet(i, j).roads) if (r.arterialEdge) cls.set(r.cls, (cls.get(r.cls) ?? 0) + 1);
  assert.ok((cls.get("arterial") ?? 0) > 20 && (cls.get("collector") ?? 0) > 10, JSON.stringify([...cls]));
});
