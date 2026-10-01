import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { groundTile } from "../src/engine/voxel/compose.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";
import { P } from "../src/engine/voxel/chunk.js";
import { roadLevelAt } from "../src/engine/network/roadLevel.js";
import { presetConfig } from "../src/engine/config/presets.js";

const w = createWorld({ seed: 1337 });

/** The steepest built-up ground (natural grade over ±40 m) among the towns around the spawn. */
function hilliestTownSpot() {
  let best = null;
  for (let j = -3; j <= 3; j += 1)
    for (let i = -3; i <= 3; i += 1) {
      const s = w.fields.settlement(i, j);
      if (!s) continue;
      for (let a = 0; a < 24; a += 1)
        for (const f of [0.4, 0.6, 0.8]) {
          const x = Math.round(s.x + Math.cos((a / 24) * Math.PI * 2) * s.radius * f);
          const y = Math.round(s.y + Math.sin((a / 24) * Math.PI * 2) * s.radius * f);
          if (w.fields.urban(x, y).u < 0.45) continue;
          const h = (dx, dy) => w.terrain.sample(x + dx, y + dy).h;
          const g = Math.hypot(h(320, 0) - h(-320, 0), h(0, 320) - h(0, -320)) / 640;
          if (!best || g > best.g) best = { g, x, y };
        }
    }
  // onto the nearest street's centre line (the search point may lie inside a block)
  const c = w.cellAt(best.x, best.y);
  const r = roadLevelAt(w, w.roadView(c.i, c.j), best.x, best.y, 400);
  if (r) {
    best.x = Math.round(r.seg.ax + r.seg.dx * r.along);
    best.y = Math.round(r.seg.ay + r.seg.dy * r.along);
  }
  return best;
}

// the spawn centre and the hilliest town ground nearby (found, not hard-coded,
// so terrain tuning cannot quietly move the test onto flat land)
const HILL = hilliestTownSpot();
const SPOTS = [
  [0, 0],
  [HILL.x, HILL.y],
];

test("roads: the hilly test spot really is hilly", () => {
  assert.ok(HILL.g > 0.05, `natural grade ${HILL.g.toFixed(3)}`);
});

/** Carriageway columns of the LOD0 ground tiles around a point: { z, seg, along, side, x, y }. */
function roadColumns(px, py, n = 4) {
  const out = [];
  const rs = makeRoadSample();
  const c0x = Math.floor(px / 32);
  const c0y = Math.floor(py / 32);
  for (let cy = c0y - n; cy <= c0y + n; cy += 1)
    for (let cx = c0x - n; cx <= c0x + n; cx += 1) {
      const tile = groundTile(w, 0, cx, cy);
      for (let j = 1; j < P - 1; j += 1)
        for (let i = 1; i < P - 1; i += 1) {
          const x = cx * 32 - 1 + i;
          const y = cy * 32 - 1 + j;
          const c = w.cellAt(x, y);
          const view = w.roadView(c.i, c.j);
          sampleRoadSurface(view.near({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), x + 0.5, y + 0.5, rs, w.seed);
          if (rs.kind !== KIND.CARRIAGE || tile.deck?.[i + j * P]) continue;
          out.push({ z: tile.z[i + j * P], seg: rs.seg, along: Math.round(rs.along), side: rs.side, x, y });
        }
    }
  return out;
}

test("roads: level across the carriageway (no sideways tilt), graded along it", () => {
  for (const [px, py] of SPOTS) {
    const cols = roadColumns(px, py);
    assert.ok(cols.length > 500, "road columns");
    // across: same segment, same position along -> one level
    const byCut = new Map();
    for (const c of cols) {
      const k = `${c.seg.road.id}|${c.seg.idx}|${c.along}`;
      const e = byCut.get(k) ?? { min: Infinity, max: -Infinity, n: 0 };
      e.min = Math.min(e.min, c.z);
      e.max = Math.max(e.max, c.z);
      e.n += 1;
      byCut.set(k, e);
    }
    let cuts = 0;
    for (const e of byCut.values()) {
      if (e.n < 8) continue;
      cuts += 1;
      assert.ok(e.max - e.min <= 1, `cross-slope ${e.max - e.min} voxels`);
    }
    assert.ok(cuts > 50);
    // along: neighbouring columns of a road differ by a gentle grade (junction blends included)
    const at = new Map(cols.map((c) => [`${c.x},${c.y}`, c]));
    let worst = 0;
    for (const c of cols) {
      for (const [dx, dy] of [[8, 0], [0, 8]]) {
        const o = at.get(`${c.x + dx},${c.y + dy}`);
        if (o && o.seg.road === c.seg.road) worst = Math.max(worst, Math.abs(o.z - c.z) / 8);
      }
    }
    assert.ok(worst <= 0.2, `grade ${worst.toFixed(2)}`);
  }
});

test("roads: lots sit flush with their sidewalk", () => {
  for (const [px, py] of SPOTS) {
    const c = w.cellAt(px, py);
    // (the spot's cell and its neighbours: an estate of superblocks has few street lots)
    const lots = [];
    for (let dj = -1; dj <= 1; dj += 1) for (let di = -1; di <= 1; di += 1) lots.push(...w.cellPlan(c.i + di, c.j + dj).lots);
    let n = 0;
    for (const lot of lots) {
      if (!lot.building || lot.farmstead || lot.underground || lot.micro) continue;
      const fx = lot.front === "W" ? lot.rect.x0 - 6 : lot.front === "E" ? lot.rect.x1 + 6 : (lot.rect.x0 + lot.rect.x1) / 2;
      const fy = lot.front === "N" ? lot.rect.y0 - 6 : lot.front === "S" ? lot.rect.y1 + 6 : (lot.rect.y0 + lot.rect.y1) / 2;
      const walk = w.streetLevel(fx, fy) + 1;
      assert.ok(Math.abs(lot.groundZ - walk) <= 2, `lot ${lot.id} ${lot.groundZ} vs sidewalk ${walk.toFixed(1)}`);
      n += 1;
    }
    assert.ok(n > 10);
  }
});

test("roads: a road ending in another's carriageway meets it there (a junction, no ledge in the street)", () => {
  // (a side road winding into a winding village street may end off its centre line, short of it or past
  // it: where it meets no other road, that end is its junction, so the two meet at one level;
  // network/roadView.js endIn)
  const hilly = createWorld(presetConfig("oldHarbourTown"));
  let ends = 0;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const v = hilly.roadView(i, j);
      for (const e of v.segs)
        for (const t of [...(e.first ? [0] : []), ...(e.last ? [e.len] : [])]) {
          const px = e.ax + e.dx * t;
          const py = e.ay + e.dy * t;
          // (the roads this end meets)
          const at = e.s0 + t;
          const met = new Set(v.segs.filter((x) => x.road === e.road).flatMap((x) => x.jn.filter((q) => Math.abs(x.s0 + q.s - at) <= e.hr + 8).map((q) => q.other.road)));
          for (const c of v.near({ x0: px - 1, y0: py - 1, x1: px + 1, y1: py + 1 })) {
            if (c.road === e.road || Math.abs(e.dx * c.dy - e.dy * c.dx) < 0.5) continue;
            const tc = (px - c.ax) * c.dx + (py - c.ay) * c.dy;
            if (tc < -3 || tc > c.len + 3 || Math.abs((px - c.ax) * c.dy - (py - c.ay) * c.dx) > c.hc) continue;
            // (it meets that road, or another one at this end, whose junction gives the end its level)
            const joined = v.segs.some((x) => x.road === e.road && x.jn.some((q) => q.other.road === c.road));
            assert.ok(joined || met.size > 0, `${e.road.id} ends in ${c.road.id}'s carriageway without a junction`);
            if (joined) ends += 1;
          }
        }
    }
  assert.ok(ends > 50, `${ends} road ends in a carriageway`);
});
