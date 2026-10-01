import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { roadNetwork } from "../src/engine/svx/roads.js";
import { handle } from "../src/engine/svx/worker.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { VOXEL_SIZE } from "../src/engine/core/units.js";

/**
 * The streets as structvox's RoadNetwork (svx/roads.js): lanes one way on
 * the carriageway, right of the centre line in structvox's frame, stopping
 * at the kerb of the road met; turns labelled as structvox counts them
 * (right: clockwise), a way on from every lane, back only at a dead end,
 * straight on over a junction onto the road beyond (a road cut at a cell's
 * border); signals never letting crossing traffic go at once; walkways on
 * the sidewalks and down the alleys meeting at their corners; parking in
 * the parking strips; stable ids and answers, whatever the query.
 */

const H = VOXEL_SIZE;
const LO = [-250, -250];
const HI = [250, 250];
const worlds = new Map();
function world(preset) {
  if (!worlds.has(preset)) worlds.set(preset, createWorld(presetConfig(preset)));
  return worlds.get(preset);
}

/** The voxel material at export metres (x, y) at height z (m), or `dz` voxels under it. */
function matAt(w, [x, y, z], dz = 0) {
  const vx = Math.floor(x / H + 0.5);
  const vy = Math.floor(y / H + 0.5);
  const vz = Math.floor(z / H + 0.5) - 1 - dz;
  const c = buildChunk(w, 0, Math.floor(vx / 32), Math.floor(vy / 32), Math.floor(vz / 32));
  return c.data[vx - c.cx * 32 + 1 + (vy - c.cy * 32 + 1) * P + (vz - c.cz * 32 + 1) * P2];
}

test("svx roads: lanes one way on the carriageway, right of their road in structvox's frame, every one with a way on", () => {
  for (const preset of ["cities", "angledOldHarbourTown"]) {
    const w = world(preset);
    const net = roadNetwork(w);
    const lanes = net.lanesIn(LO, HI);
    assert.ok(lanes.length > 300, `${preset}: ${lanes.length} lanes`);
    let onRoad = 0;
    let checked = 0;
    let onto = 0;
    for (const l of lanes) {
      // (a way on: the next piece, a turn, or at a dead end back the way it came)
      const next = net.next(l.id);
      assert.ok(next.length > 0, `${preset}: lane ${l.id} leads nowhere`);
      const dx = l.b[0] - l.a[0];
      const dy = l.b[1] - l.a[1];
      const len = Math.hypot(dx, dy);
      for (const [nid, turn] of next) {
        const n = net.lane(nid);
        assert.ok(n, `next ${nid} known`);
        const ex = n.b[0] - n.a[0];
        const ey = n.b[1] - n.a[1];
        const el = Math.hypot(ex, ey);
        const cross = (dx * ey - dy * ex) / (len * el);
        const dot = (dx * ex + dy * ey) / (len * el);
        // right: clockwise (cross < 0), left: counter-clockwise, straight on: within 30°
        if (turn === 0) assert.ok(dot > 0.85, `straight on: ${dot}`);
        else if (dot > -0.95) assert.equal(Math.sign(cross), turn === 1 ? -1 : 1, `turn ${turn}: cross ${cross}`);
        if (turn === 0 && n.key.road !== l.key.road) onto += 1;
      }
      // (back the way it came only at a dead end: no other road's lane leaves near its end)
      if (next.length === 1 && net.lane(next[0][0]).key.road === l.key.road && net.lane(next[0][0]).key.dir !== l.key.dir)
        for (const o of lanes) if (o.key.road !== l.key.road) assert.ok(Math.hypot(o.a[0] - l.b[0], o.a[1] - l.b[1]) > 5, `${preset}: lane ${l.id} turns back beside ${o.key.road}`);
      // on the carriageway (sampled), its surface right under the lane's middle
      if (checked < 60 && len > 10) {
        checked += 1;
        const mid = [(l.a[0] + l.b[0]) / 2, (l.a[1] + l.b[1]) / 2, (l.a[2] + l.b[2]) / 2];
        // (the road's surface within a voxel under it: a steep street's steps fall between its profile's levels)
        if ([0, 1].some((dz) => matAt(w, mid, dz) !== 0)) onRoad += 1;
      }
    }
    assert.ok(onRoad >= checked * 0.9, `${preset}: ${onRoad} of ${checked} lanes on the road`);
    // (straight on over junctions onto another road: the arterials, cut at every cell's border, go on)
    assert.ok(onto > 20, `${preset}: ${onto} straight on onto another road`);
    // two-way streets: the two directions side by side, each on its right (structvox: heading (dx, dy), right is (dy, -dx))
    // (the inner lanes of one piece of a link, one each way)
    const pairs = new Map();
    for (const l of lanes) {
      if (l.key.k !== 0) continue;
      const k = `${l.key.road}/${l.key.link}`;
      pairs.set(k, [...(pairs.get(k) ?? []), l]);
    }
    let sides = 0;
    for (const list of pairs.values()) {
      const p = list.find((l) => l.key.dir === 0 && l.key.piece === 0);
      const q = list.find((l) => l.key.dir === 1 && l.key.last);
      if (!p || !q) continue;
      const hx = p.b[0] - p.a[0];
      const hy = p.b[1] - p.a[1];
      if (hx * (q.b[0] - q.a[0]) + hy * (q.b[1] - q.a[1]) >= 0) continue;
      // (p's right is towards its kerb: away from q, which drives the other way on the other side)
      const toQ = [(q.a[0] + q.b[0]) / 2 - (p.a[0] + p.b[0]) / 2, (q.a[1] + q.b[1]) / 2 - (p.a[1] + p.b[1]) / 2];
      assert.ok(toQ[0] * hy - toQ[1] * hx < 0, "keeps to the right");
      sides += 1;
    }
    assert.ok(sides > 20, `${preset}: ${sides} two-way pairs`);
  }
});

/** Do segments p-q and r-s cross? */
function crosses(p, q, r, s) {
  const o = (a, b, c) => Math.sign((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
  return o(p, q, r) * o(p, q, s) < 0 && o(r, s, p) * o(r, s, q) < 0;
}

test("svx roads: signals never let crossing traffic go at once, every road of a junction on its cycle", () => {
  for (const preset of ["cities", "angledCities"]) {
    const net = roadNetwork(world(preset));
    // the ways straight on through the junctions (a lane's end to the next lane's start), with their signals
    const moves = [];
    for (const l of net.lanesIn([-500, -500], [500, 500])) {
      if (!l.key.last) continue;
      for (const [n, turn] of net.next(l.id)) if (turn === 0) moves.push({ l, p: l.b, q: net.lane(n).a, sig: net.signal(l.id) });
    }
    let signalled = 0;
    let crossing = 0;
    for (let i = 0; i < moves.length; i += 1)
      for (let k = i + 1; k < moves.length; k += 1) {
        const a = moves[i];
        const b = moves[k];
        if (Math.hypot(a.p[0] - b.p[0], a.p[1] - b.p[1]) > 60 || !crosses(a.p, a.q, b.p, b.q)) continue;
        crossing += 1;
        // (a signalled way crosses only signalled ones, of the same cycle, never green together)
        assert.equal(!a.sig, !b.sig, `${preset}: a signalled way crosses an unsignalled one`);
        if (!a.sig) continue;
        signalled += 1;
        assert.equal(a.sig.cycle, b.sig.cycle);
        assert.equal(a.sig.offset, b.sig.offset, `${preset}: one junction, two offsets`);
        for (let t = 0; t < a.sig.cycle; t += 0.25) assert.ok(!(net.green(a.l.id, t) && net.green(b.l.id, t)), `${preset}: ${a.l.key.road} and ${b.l.key.road} green together at ${t}`);
      }
    assert.ok(crossing > 100 && signalled > 20, `${preset}: ${crossing} crossing ways, ${signalled} signalled`);
  }
});

test("svx roads: walkways meet at their corners, down the alleys too; parking in the strips", () => {
  const w = world("cities");
  const net = roadNetwork(w);
  // walkways: sidewalks on the sidewalk, a crossing now and then, the walks at an end know each other
  const walks = net.walksIn(LO, HI);
  const crossings = walks.filter((q) => q.crossing);
  assert.ok(walks.length > 300 && crossings.length > 30, `${walks.length} walks, ${crossings.length} crossings`);
  let paved = 0;
  let sampled = 0;
  let joined = 0;
  for (const wk of walks) {
    for (const end of [0, 1])
      for (const [oid, oend] of net.walkNext(wk.id, end)) {
        // (mutual: the other walk's end lists this one's)
        assert.ok(net.walkNext(oid, oend).some((q) => q[0] === wk.id && q[1] === end), "walk_next both ways");
        // (and the two ends are the same corner)
        const o = net.walk(oid);
        const p = end === 0 ? wk.a : wk.b;
        const q = oend === 0 ? o.a : o.b;
        assert.ok(Math.hypot(p[0] - q[0], p[1] - q[1]) < 0.5, "at one corner");
      }
    if (net.walkNext(wk.id, 0).length && net.walkNext(wk.id, 1).length) joined += 1;
    if (!wk.crossing && sampled < 60) {
      sampled += 1;
      const mid = [(wk.a[0] + wk.b[0]) / 2, (wk.a[1] + wk.b[1]) / 2, (wk.a[2] + wk.b[2]) / 2];
      if (matAt(w, mid) !== 0) paved += 1;
    }
  }
  assert.ok(joined >= walks.length * 0.95, `${joined} of ${walks.length} walks joined at both ends`);
  assert.ok(paved >= sampled * 0.9, `${paved} of ${sampled} sidewalks on the ground`);
  // parking: in the strips, heading with the traffic on its side (a unit vector)
  const park = net.parkingIn(LO, HI);
  assert.ok(park.length > 50);
  for (const p of park) assert.ok(Math.abs(Math.hypot(...p.heading) - 1) < 1e-9);
  // an old town's alleys and lanes: a walk down the middle, joined to the streets' sidewalks at their mouths
  const old = roadNetwork(world("angledOldHarbourTown"));
  const all = old.walksIn(LO, HI);
  const middle = all.filter((q) => q.kind === "middle");
  assert.ok(middle.length > 20, `${middle.length} walks down alleys and lanes`);
  const both = all.filter((q) => old.walkNext(q.id, 0).length && old.walkNext(q.id, 1).length).length;
  assert.ok(both >= all.length * 0.9, `${both} of ${all.length} old town walks joined at both ends`);
  assert.ok(middle.some((q) => old.walkNext(q.id, 0).some(([o]) => old.walk(o).kind === "sidewalk")), "an alley's walk meets a sidewalk");
});

test("svx roads: stable ids and records, whatever the query and its order; the worker gives a region whole", () => {
  const a = roadNetwork(world("angledCities"));
  const b = roadNetwork(createWorld(presetConfig("angledCities")));
  const la = a.lanesIn(LO, HI);
  // (another network over a fresh world, asked in pieces from the other corner first)
  b.lanesIn([0, 0], HI);
  b.walksIn([0, 0], HI);
  const lb = b.lanesIn(LO, HI);
  assert.deepEqual(la.map((l) => l.id), lb.map((l) => l.id));
  assert.deepEqual(la.slice(0, 50).map((l) => [l.a, l.b, a.next(l.id)]), lb.slice(0, 50).map((l) => [l.a, l.b, b.next(l.id)]));
  const wa = a.walksIn(LO, HI);
  assert.deepEqual(wa.map((q) => q.id), b.walksIn(LO, HI).map((q) => q.id));
  // (the walks at a walk's ends and the signals: the same answers, whatever was asked before)
  for (const q of wa) for (const end of [0, 1]) assert.deepEqual(a.walkNext(q.id, end), b.walkNext(q.id, end));
  for (const l of la) assert.deepEqual(a.signal(l.id), b.signal(l.id));
  assert.deepEqual(a.parkingIn(LO, HI), b.parkingIn(LO, HI));
  for (const l of la) assert.ok(Number.isSafeInteger(l.id) && l.id > 0);
  // the worker protocol: init, then a region's plain data
  const state = {};
  handle(state, { type: "init", config: presetConfig("angledCities") });
  const { reply } = handle(state, { type: "roads", lo: LO, hi: HI });
  assert.equal(reply.lanes.length, la.length);
  assert.ok(reply.lanes.every((l) => Array.isArray(l.next) && l.next.length > 0));
  assert.ok(reply.walks.length > 0 && reply.walks.every((q) => Array.isArray(q.nextA) && Array.isArray(q.nextB)));
  assert.ok(JSON.parse(JSON.stringify(reply)).lanes.length === la.length, "plain data");
});

test("svx roads: highways — lanes on the deck, ramps off it onto their arterial and up from the street, junctions taking turns", () => {
  const w = createWorld({ seed: 1337, world: { mode: "infiniteCity" } });
  const net = roadNetwork(w);
  const lanes = net.lanesIn([-2500, -2500], [2500, 2500]);
  const hwl = lanes.filter((l) => l.key.hw);
  const ramps = hwl.filter((l) => l.key.ramp !== undefined);
  assert.ok(hwl.length > 500 && ramps.length > 20, `${hwl.length} highway lanes, ${ramps.length} on ramps`);
  let onDeck = 0;
  let sampled = 0;
  for (const l of hwl) {
    assert.ok(net.next(l.id).length > 0, `highway lane ${l.id} leads nowhere`);
    if (sampled < 120 && l.key.ramp === undefined) {
      sampled += 1;
      const mid = [(l.a[0] + l.b[0]) / 2, (l.a[1] + l.b[1]) / 2, (l.a[2] + l.b[2]) / 2];
      if ([0, 1].some((dz) => matAt(w, mid, dz) !== 0)) onDeck += 1;
    }
    // (an off-ramp ends on its arterial: every way on is a street's lane)
    if (l.key.ramp !== undefined && l.key.off && l.key.last) for (const [id] of net.next(l.id)) assert.equal(net.lane(id).key.road, l.key.arterial);
  }
  assert.ok(onDeck >= sampled * 0.95, `${onDeck} of ${sampled} deck lanes on the deck`);
  // every on-ramp is reached from its arterial, every off-ramp from the deck's kerb lane
  const reached = new Set();
  for (const l of lanes) for (const [id] of net.next(l.id)) if (net.lane(id)?.key.ramp !== undefined && net.lane(id).key.piece === 0) reached.add(`${net.lane(id).key.hw}/${net.lane(id).key.ramp}/${l.key.hw ? "deck" : "street"}`);
  // (of the ramps whose ends lie well inside the box: what leads to them was asked for too)
  const inside = (p) => Math.abs(p[0]) < 2300 && Math.abs(p[1]) < 2300;
  const whole = ramps.filter((q) => q.key.piece === 0 && inside(q.a) && ramps.some((z) => z.key.hw === q.key.hw && z.key.ramp === q.key.ramp && z.key.last && inside(z.b)));
  assert.ok(whole.length > 5, `${whole.length} ramps inside`);
  for (const l of whole) assert.ok(reached.has(`${l.key.hw}/${l.key.ramp}/${l.key.off ? "deck" : "street"}`), `ramp ${l.key.hw}/${l.key.ramp} unreachable`);
  // junctions of highways: straight ways across one never green together
  const moves = [];
  for (const l of hwl) if (l.key.last && l.key.ramp === undefined) for (const [n, turn] of net.next(l.id)) if (turn === 0 && net.signal(l.id)) moves.push({ l, p: l.b, q: net.lane(n).a, sig: net.signal(l.id) });
  for (let i = 0; i < moves.length; i += 1)
    for (let k = i + 1; k < moves.length; k += 1) {
      const a = moves[i];
      const b = moves[k];
      if (!crosses(a.p, a.q, b.p, b.q)) continue;
      for (let t = 0; t < a.sig.cycle; t += 0.25) assert.ok(!(net.green(a.l.id, t) && net.green(b.l.id, t)), "crossing highway traffic green together");
    }
});
