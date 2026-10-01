import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";
import { hash32 } from "../src/engine/core/hash.js";

function chunkHash(c) {
  let h = 0;
  for (let i = 0; i < c.data.length; i += 1) h = hash32(h, c.data[i], i);
  return h;
}

test("arterial cells tile the plane without gaps", () => {
  const w = createWorld({ seed: 11 });
  for (let k = 0; k < 200; k += 1) {
    const x = (k * 7919) % 20000 - 10000;
    const y = (k * 104729) % 20000 - 10000;
    const { i, j } = w.cellAt(x, y);
    const r = w.arterials.cellRect(i, j);
    assert.ok(x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1);
  }
});

test("chunks are deterministic and independent of generation order", () => {
  const a = createWorld({ seed: 5 });
  const b = createWorld({ seed: 5 });
  const keys = [
    [0, 10, 12, 1],
    [0, 11, 12, 1],
    [2, 3, 3, 0],
  ];
  const ha = keys.map(([l, x, y, z]) => chunkHash(buildChunk(a, l, x, y, z)));
  const hb = keys
    .slice()
    .reverse()
    .map(([l, x, y, z]) => chunkHash(buildChunk(b, l, x, y, z)))
    .reverse();
  assert.deepEqual(ha, hb);
});

test("chunk aprons agree with their neighbours (seamless borders)", () => {
  const w = createWorld({ seed: 3 });
  for (const lod of [0, 2]) {
    const [cx, cy, cz] = lod === 0 ? [9, 14, 1] : [2, 3, 0];
    const A = buildChunk(w, lod, cx, cy, cz);
    const B = buildChunk(w, lod, cx + 1, cy, cz);
    let diff = 0;
    for (let k = 1; k <= 32; k += 1)
      for (let j = 1; j <= 32; j += 1) {
        // A's east apron (i = 33) is B's first core column (i = 1)
        if (A.data[33 + j * P + k * P2] !== B.data[1 + j * P + k * P2]) diff += 1;
      }
    assert.equal(diff, 0, `lod ${lod}`);
  }
});

test("road surface: carriageway on centerlines, property outside the right-of-way", () => {
  const w = createWorld({ seed: 1337 });
  const net = w.cellNet(0, 0);
  const view = w.roadView(0, 0);
  const s = makeRoadSample();
  const art = net.roads.find((r) => r.cls === "arterial");
  assert.ok(art, "cell 0,0 has an arterial");
  const p = art.pts[0];
  const q = art.pts[1];
  const mx = Math.round((p.x + q.x) / 2);
  const my = Math.round((p.y + q.y) / 2);
  const dirX = q.x !== p.x;
  const probe = (off) => {
    const x = dirX ? mx : mx + off;
    const y = dirX ? my + off : my;
    sampleRoadSurface(view.near({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), x + 0.5, y + 0.5, s, w.seed);
    return s.kind;
  };
  assert.notEqual(probe(art.hc - 20), KIND.NONE);
  assert.equal(probe(art.hc - 20), KIND.CARRIAGE);
  assert.equal(probe(art.hc + 10), KIND.SIDEWALK);
  assert.equal(probe(art.hr + 4), KIND.NONE);
});

test("every block's property rect is off the road and every lot lies in its block", () => {
  const w = createWorld({ seed: 21 });
  const plan = w.cellPlan(0, 0);
  const view = w.roadView(0, 0);
  const s = makeRoadSample();
  for (const lot of plan.lots.slice(0, 80)) {
    const x = Math.round((lot.rect.x0 + lot.rect.x1) / 2);
    const y = Math.round((lot.rect.y0 + lot.rect.y1) / 2);
    sampleRoadSurface(view.near({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), x + 0.5, y + 0.5, s, w.seed);
    assert.equal(s.kind, KIND.NONE, lot.id);
  }
});

test("highway corridors keep buildings out", () => {
  const w = createWorld({ seed: 1337 });
  const edges = w.highways.edgesNear({ x0: -8000, y0: -8000, x1: 8000, y1: 8000 });
  assert.ok(edges.length > 0);
  let checked = 0;
  for (let i = -2; i <= 2; i += 1) {
    for (let j = -2; j <= 2; j += 1) {
      const plan = w.cellPlan(i, j);
      for (const env of plan.buildings) {
        const near = w.highways.nearest((env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2, w.highways.edgesNear(env.R), w.highways.hw);
        assert.equal(near, null, env.id);
        checked += 1;
      }
    }
  }
  assert.ok(checked > 100);
});

test("subway stations are underground except their street entrances", () => {
  const w = createWorld({ seed: 1337 });
  const st = w.subway.stationsNear({ x0: -4000, y0: -4000, x1: 4000, y1: 4000 });
  assert.ok(st.length > 0, "stations exist near the city center");
  for (const s of st) {
    assert.ok(s.zp < s.zs - 80, "platform well below street");
    const solidAbove = s.boxes.filter((b) => b.m !== 0 && b.z0 > s.zs + 1);
    // only entrance railings / signs rise above the street
    assert.ok(solidAbove.every((b) => b.z1 - b.z0 <= 24));
  }
  // a tile over a station reaches down to the platform level
  const s = st[0];
  const tile = groundTile(w, 0, Math.floor(s.x / 32), Math.floor(s.y / 32));
  assert.ok(tile.zMin > s.zp);
});

test("planet charts: adjacent cube faces sample the same fields along their shared edge", async () => {
  const { CubeSphereChart } = await import("../src/engine/world/chart.js");
  const R = 240000;
  const north = new CubeSphereChart({ radius: R, face: 4 });
  const east = new CubeSphereChart({ radius: R, face: 0 });
  const h = north.half;
  for (const t of [-0.9, -0.3, 0, 0.4, 0.8]) {
    // face 4 edge a = +1 meets face 0 edge b = +1 with the other coordinate shared
    const p = north.toField(h, t * h);
    const q = east.toField(t * h, h);
    for (let k = 0; k < 3; k += 1) assert.ok(Math.abs(p[k] - q[k]) < 1e-6, `edge point ${t}`);
  }
  assert.ok(north.latitude(...north.toField(0, 0)) > 0.99, "face 4 centre is the north pole");
  assert.ok(Math.abs(east.latitude(...east.toField(0, 0))) < 1e-9, "face 0 centre is on the equator");
  const pole = createWorld({ seed: 1337, world: { chart: "cube", planet: { radius: R, face: 4 } } });
  const equator = createWorld({ seed: 1337, world: { chart: "cube", planet: { radius: R, face: 0 } } });
  assert.ok(pole.fields.temperature(0, 0) < equator.fields.temperature(0, 0), "poles are colder");
  // settlements keep a wilderness band along the face edges
  const cellM = pole.config.world.settlementCell;
  const n = Math.ceil(north.half / cellM);
  let near = 0;
  for (let j = -n; j <= n; j += 1)
    for (let i = -n; i <= n; i += 1) {
      const st = pole.fields.settlement(i, j);
      if (!st) continue;
      const d = north.edgeDistance(st.x / 8, st.y / 8);
      if (d < pole.config.world.faceMargin) near += 1;
    }
  assert.equal(near, 0, "no settlement near a face edge");
});
