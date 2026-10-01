import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { floodRegion } from "../src/engine/validate/walkability.js";

test("sewers: cells agree on every shared node", () => {
  const w = createWorld({ seed: 1337 });
  const seen = new Map();
  let shared = 0;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1)
      for (const n of w.sewers.cellPlan(i, j).nodes) {
        const sig = [n.z, n.zr, n.shaft, n.open, n.qx, n.qy, n.hall].join();
        const prev = seen.get(n.key);
        if (prev !== undefined) {
          shared += 1;
          assert.equal(sig, prev, `node ${n.key}`);
        } else seen.set(n.key, sig);
      }
  assert.ok(shared > 0, "neighbouring cells share junction nodes");
});

test("sewers: runs and chambers keep clear of subway stations", () => {
  const w = createWorld({ seed: 1337 });
  const hit = (a, b) => a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1 && a.z0 <= b.z1 && b.z0 <= a.z1;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const p = w.sewers.cellPlan(i, j);
      if (!p.bb) continue;
      const stations = w.subway.stationsNear(p.bb);
      for (const r of p.runs) {
        const b =
          r.axis === 0
            ? { x0: r.fixed - 16, x1: r.fixed + 16, y0: r.l0, y1: r.l1 }
            : { x0: r.l0, x1: r.l1, y0: r.fixed - 16, y1: r.fixed + 16 };
        Object.assign(b, { z0: Math.min(r.z0, r.z1) - 5, z1: Math.max(r.z0, r.z1) + 25 });
        for (const s of stations) for (const q of s.boxes) if (q.m === 0) assert.ok(!hit(b, q), `run at ${r.fixed} crosses station ${s.x},${s.y}`);
      }
    }
});

test("sewers: the hall stair leads from the sidewalk to the ledge, basin and next chamber", () => {
  for (const seed of [1337, 42]) {
    const w = createWorld({ seed });
    const hall = w.sewers.nearestHall(0, 0);
    assert.ok(hall?.stairTop, `seed ${seed}: a hall near the spawn city`);
    const c = w.cellAt(hall.x, hall.y);
    const tgt = w.sewers
      .cellPlan(c.i, c.j)
      .nodes.filter((n) => !n.hall)
      .sort((a, b) => Math.hypot(a.x - hall.x, a.y - hall.y) - Math.hypot(b.x - hall.x, b.y - hall.y))[0];
    const reg = {
      x0: Math.min(hall.x, tgt.x) - 140,
      y0: Math.min(hall.y, tgt.y) - 140,
      x1: Math.max(hall.x, tgt.x) + 140,
      y1: Math.max(hall.y, tgt.y) + 140,
      z0: hall.z - 34,
      z1: hall.zr + 30,
    };
    const t = hall.stairTop;
    const f = floodRegion(w, reg, [[t.x, t.y, t.z]]);
    assert.ok(f.reached(hall.x + 58, hall.y - 30, hall.z + 1) || f.reached(hall.x - 58, hall.y - 30, hall.z + 1), `seed ${seed}: ledge`);
    assert.ok(f.reached(hall.x - 30, hall.y + 30, hall.z - 23), `seed ${seed}: basin`);
    assert.ok(f.reached(tgt.x - tgt.qx * 10, tgt.y - tgt.qy * 10, tgt.z + 1), `seed ${seed}: next chamber`);
  }
});

test("sewers: open manholes are climbable both ways (and only by ladder)", () => {
  for (const seed of [1337, 42]) {
    const w = createWorld({ seed });
    const c = w.cellAt(0, 0);
    const n = w.sewers.cellPlan(c.i, c.j).nodes.find((q) => q.open);
    assert.ok(n, `seed ${seed}: an open manhole`);
    const sx = n.x + n.qx * 12;
    const sy = n.y + n.qy * 12;
    const street = [sx - n.qx * 12, sy + n.qy * 10, n.zr + 1];
    const floor = [n.x - n.qx * 10, n.y - n.qy * 10, n.z + 1];
    const reg = { x0: n.x - 40, y0: n.y - 40, x1: n.x + 40, y1: n.y + 40, z0: n.z - 6, z1: n.zr + 24 };
    assert.ok(floodRegion(w, reg, [street], { climb: true }).reached(...floor), `seed ${seed}: down the ladder`);
    assert.ok(floodRegion(w, reg, [floor], { climb: true }).reached(...street), `seed ${seed}: up the ladder`);
    assert.ok(!floodRegion(w, reg, [floor]).reached(...street), `seed ${seed}: no way up without the ladder`);
  }
});
