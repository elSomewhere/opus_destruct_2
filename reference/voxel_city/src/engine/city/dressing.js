import { Rng, hash32, hashString } from "../core/hash.js";
import { SpatialGrid } from "../core/geom2d.js";
import { vx } from "../core/units.js";
import { PROPS } from "./propPrefabs.js";
import { spaceSurface, ALLOT } from "./landscape.js";
import { parkLayout, pathDist, pondDist, groveAt } from "./parks.js";
import { MAT } from "../voxel/materials.js";
import { Frame, lotFrameOf } from "../buildings/frame.js";
import { treeBounds } from "../nature/trees.js";
import { dressIndustry, dressPort } from "./industry.js";
import { wrapOf } from "../world/wrap.js";
import { roadLevelAt } from "../network/roadLevel.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";
import { seasonOf } from "../world/season.js";
import { LAPSE } from "../nature/landcover.js";
import { spaceGroundAt, naturalGroundAt } from "../voxel/compose.js";
import { insideCuts } from "./blockPoly.js";

/**
 * Stage 3 of an arterial cell: street dressing. Props are placed along the
 * road segments the cell OWNS (so neighbours never duplicate them), in the
 * cell's parks, plazas and yards. Output: props (box prefabs with a local
 * street frame) and trees, each indexed in a spatial grid.
 */

/** Street tree species, one per street (lime / plane = "street"). */
const STREET_TREES = ["street", "street", "street", "maple", "oak", "blossom", "birch", "maple"];

export function planDressing(world, i, j) {
  const plan = world.cellPlan(i, j);
  const view = world.roadView(i, j);
  const seed = world.seed;
  // position hashes use canonical coordinates (a wrapping world repeats its props)
  const W = wrapOf(world.config);
  const props = [];
  const trees = [];
  const angledProps = !!(world.config.world.angles?.enabled && world.config.world.angles.features?.roads !== false);
  // street props stand on the road's level (network/roadLevel.js) within its
  // right-of-way, elsewhere on the ground as the tiles shape it (embankments, block surfaces)
  const ground = (x, y) => {
    const r = roadLevelAt(world, view, x, y, 16);
    return r && r.dist <= r.seg.hr ? Math.round(r.z) : naturalGroundAt(world, Math.round(x), Math.round(y));
  };

  const overRiver = (x, y) => (world.isWet ? world.isWet(x, y, 2) : false);
  const blocked = (x, y) =>
    overRiver(x, y) ||
    (world.blocksSurface && world.blocksSurface(x, y)) ||
    // (under a highway's deck or ramp, or within a crown's reach of one)
    (world.highways && world.highways.covers(x, y, 28));
  const addProp = (kind, x, y, z, ax, ay, bx, by, extra = {}) => {
    if (kind !== "car" && kind !== "hedge" && kind !== "fence" && world.blocksSurface && world.blocksSurface(x, y)) return;
    if ((kind === "car" || kind === "busShelter" || kind === "bench" || kind === "bin") && overRiver(x, y)) return;
    const rng = Rng.from(seed, kind, W.vi(x), W.vi(y));
    const boxes = PROPS[kind](rng, extra);
    const world0 = [];
    for (const q of boxes) {
      const xa = x + q.a0 * ax + q.b0 * bx;
      const ya = y + q.a0 * ay + q.b0 * by;
      const xb = x + q.a1 * ax + q.b1 * bx;
      const yb = y + q.a1 * ay + q.b1 * by;
      world0.push({
        x0: Math.round(Math.min(xa, xb)),
        y0: Math.round(Math.min(ya, yb)),
        x1: Math.round(Math.max(xa, xb)),
        y1: Math.round(Math.max(ya, yb)),
        z0: z + q.z0,
        z1: z + q.z1,
        m: q.m,
      });
    }
    let bb = null;
    for (const q of world0) {
      bb = bb
        ? { x0: Math.min(bb.x0, q.x0), y0: Math.min(bb.y0, q.y0), z0: Math.min(bb.z0, q.z0), x1: Math.max(bb.x1, q.x1), y1: Math.max(bb.y1, q.y1), z1: Math.max(bb.z1, q.z1) }
        : { ...q };
    }
    // (nothing reaches up into a highway's deck or ramp: a car fits under a deck, a crane does not, nothing under a ramp near the ground)
    if (world.highways && world.highways.edgesNear(bb).length) {
      const hw = world.highways;
      for (const q of world0)
        for (const [px, py] of [[q.x0, q.y0], [q.x1, q.y0], [q.x0, q.y1], [q.x1, q.y1], [(q.x0 + q.x1) >> 1, (q.y0 + q.y1) >> 1]]) {
          const under = hw.underside(px, py);
          if (under !== null && q.z1 >= under) return;
        }
    }
    props.push({ kind, boxes: world0, bb, big: bb.z1 - bb.z0 > vx(8) });
  };
  // planted trees follow the regional climate: palms in hot towns, conifers up north
  const cx = (plan.rect.x0 + plan.rect.x1) / 2;
  const cy = (plan.rect.y0 + plan.rect.y1) / 2;
  const climT = world.fields.temperature(cx, cy);
  const climM = world.fields.moisture(cx, cy);
  // (palms only where the land itself is tropical, savanna or desert)
  const hot = ["tropical", "savanna", "desert"].includes(world.landCover.biomeAt(cx, cy, world.terrain.sample(cx, cy).h / 8).id);
  const regional = (kind, x, y) => {
    const h = hash32(seed, W.vi(x), W.vi(y), 92);
    if (climT > 0.66 && hot) {
      if (kind === "street") return "palm";
      return (h & 3) === 0 ? (climM < 0.4 ? "acacia" : "jungle") : "palm";
    }
    if (climT < 0.3) {
      if (kind === "street") return (h & 1) === 0 ? "birch" : "pine";
      return (h & 3) === 0 ? "birch" : (h & 4) === 0 ? "spruce" : "pine";
    }
    // northern towns: birches, rowans and a few spruces instead of planes and blossom
    if (climT < 0.4) {
      if (kind === "street" || kind === "blossom") return (h & 3) === 0 ? "rowan" : (h & 3) === 1 ? "street" : "birch";
      if (kind === "oak" || kind === "autumn") return (h & 7) === 0 ? "spruce" : (h & 7) < 3 ? "rowan" : (h & 7) < 6 ? "birch" : "oak";
    }
    return kind;
  };
  const season = seasonOf(world.config);
  const addTree = (x, y, z, h, r, kind0) => {
    if (blocked(x, y)) return;
    const kind = regional(kind0, x, y);
    const t = { x: Math.round(x), y: Math.round(y), z, h: Math.round(h), r, kind, seed: hash32(seed, W.vi(x), W.vi(y), 91) };
    t.look = season.treeLook(kind, t.seed, climT - Math.max(0, z / 8) / LAPSE);
    t.bb = treeBounds(t);
    // (the angled world's wings, S5: a tree keeps clear of them)
    if (plan.wings && plan.wings.some((w) => t.bb.x0 <= w.bounds.x1 && w.bounds.x0 <= t.bb.x1 && t.bb.y0 <= w.bounds.y1 && w.bounds.y0 <= t.bb.y1 && t.bb.z1 >= w.z0 && t.bb.z0 <= w.z1)) return;
    trees.push(t);
  };

  // ---------------- delineator posts along country roads (any direction)
  for (const s of view.segs) {
    if (s.road.cell !== plan.id || s.cls !== "rural") continue;
    const nx = -s.dy;
    const ny = s.dx;
    const phase = ((hash32(seed, W.vi(s.ax), W.vi(s.ay), 41) % 400) + 400) % 400;
    for (let t = phase; t < s.len; t += vx(50)) {
      if (s.jn.some((jn) => Math.abs(t - jn.s) < jn.hr + vx(8))) continue;
      for (const side of [-1, 1]) {
        const x = s.ax + s.dx * t + nx * side * (s.hc + vx(1.5));
        const y = s.ay + s.dy * t + ny * side * (s.hc + vx(1.5));
        addProp("delineator", x, y, ground(x, y) + 1, s.dx, s.dy, -nx * side, -ny * side);
      }
    }
  }

  // ---------------- avenues: now and then a stretch of country road near a
  // town, a village or a farm is lined with trees on both sides (birch
  // alleys in the north, oaks and limes further south, poplars where it is hot)
  const avenueRs = makeRoadSample();
  const STRETCH = vx(120);
  for (const s of view.segs) {
    if (s.road.cell !== plan.id || (s.cls !== "rural" && s.cls !== "village")) continue;
    const nx = -s.dy;
    const ny = s.dx;
    const off = s.hr + vx(2.2);
    for (let a0 = 0; a0 < s.len; a0 += STRETCH) {
      const am = Math.min(s.len, a0 + STRETCH / 2);
      const sx = s.ax + s.dx * am;
      const sy = s.ay + s.dy * am;
      const k = hash32(seed, W.vi(Math.floor(sx / STRETCH)), W.vi(Math.floor(sy / STRETCH)), 57) / 4294967296;
      if (k > 0.3) continue;
      const u = world.fields.urban(sx, sy).u;
      if (u > 0.35 || u < 0.004) continue;
      const hM = world.terrain.sample(sx, sy).h / 8;
      if (world.landCover.forestDensity(sx, sy, u, null, hM) > 0.4) continue;
      const tl = world.landCover.climate(sx, sy, hM).t;
      const kind = tl < 0.42 ? "birch" : tl > 0.72 ? "poplar" : k < 0.12 ? "oak" : "street";
      const step = vx(9 + 3 * (k / 0.3));
      for (let t = a0 + step / 2; t < Math.min(s.len, a0 + STRETCH); t += step) {
        if (s.jn.some((jn) => Math.abs(t - jn.s) < jn.hr + vx(10))) continue;
        for (const side of [-1, 1]) {
          const x = Math.round(s.ax + s.dx * t + nx * side * off);
          const y = Math.round(s.ay + s.dy * t + ny * side * off);
          if (overRiver(x, y) || (world.isWet && world.isWet(x, y, 3)) || plan.lotAt(x, y) || plan.spaceAt(x, y)) continue;
          sampleRoadSurface(view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x + 0.5, y + 0.5, avenueRs, seed);
          if (avenueRs.kind !== KIND.NONE) continue;
          const g = hash32(seed, W.vi(x), W.vi(y), 58) / 4294967296;
          const [h, r] = kind === "poplar" ? [16 + 6 * g, 1.8 + 0.6 * g] : kind === "birch" ? [11 + 5 * g, 2 + 0.8 * g] : [10 + 5 * g, 3 + 1.2 * g];
          addTree(x, y, naturalGroundAt(world, x, y) + 1, vx(h), vx(r), kind);
        }
      }
    }
  }

  // ---------------- street props along owned segments
  for (const s of view.segs) {
    if (s.road.cell !== plan.id) continue;
    const axisAligned = Math.abs(s.dx) < 1e-6 || Math.abs(s.dy) < 1e-6;
    // (the angled world: props line angled streets too, square to the
    // nearest axis: props stay in the world grid, ANGLED_WORLD_PLAN.md §4.6)
    if ((!axisAligned && !angledProps) || s.sidewalk <= 0 || s.cls === "alley") continue;
    const [ox, oy] = axisAligned ? [s.dx, s.dy] : Math.abs(s.dx) >= Math.abs(s.dy) ? [Math.sign(s.dx), 0] : [0, Math.sign(s.dy)];
    const cls = s.cls;
    const lampSpacing = cls === "arterial" ? vx(26) : cls === "collector" ? vx(28) : vx(32);
    const nx = -s.dy;
    const ny = s.dx;
    const nearJunction = (t, pad) => s.jn.some((jn) => Math.abs(t - jn.s) < jn.hr + pad);
    const strip = s.road.strip ?? "pits";
    const pedestrian = cls === "pedestrian";
    for (const side of [-1, 1]) {
      const bx = -nx * side; // towards the road
      const by = -ny * side;
      // (square to the prop's own axis, still towards the road)
      const flip = axisAligned || -oy * bx + ox * by > 0 ? 1 : -1;
      const obx = axisAligned ? bx : -oy * flip;
      const oby = axisAligned ? by : ox * flip;
      const px = (t, q) => s.ax + s.dx * t + nx * side * q;
      const py = (t, q) => s.ay + s.dy * t + ny * side * q;
      const curbQ = s.hc;
      // lamps (iron lanterns along the cobbled streets of an old town, one side only)
      const cobbled = s.road.paving === "cobble";
      if (cobbled ? side === -1 : pedestrian || side === -1 || cls !== "local") {
        const phase = side < 0 ? 0 : lampSpacing / 2;
        const spacing = cobbled ? vx(22) : lampSpacing;
        for (let t = phase + vx(6); t < s.len - vx(4); t += spacing) {
          if (nearJunction(t, vx(3))) continue;
          const q = pedestrian ? s.hr - 6 : curbQ + 5;
          const x = px(t, q);
          const y = py(t, q);
          if (cobbled) addProp("oldLamp", x, y, ground(x, y) + 2, ox, oy, obx, oby);
          else addProp("streetlight", x, y, ground(x, y) + 2, ox, oy, obx, oby, { reach: pedestrian ? 3 : Math.min(18, Math.max(8, Math.round(s.hc * 0.35))) });
        }
      }
      // street trees: one species per street (limes, planes, maples, oaks,
      // cherries, birches), a street now and then without any, the size of
      // a street's trees in one age class
      const rh = hashString(s.road.id);
      const avenue = STREET_TREES[rh % STREET_TREES.length];
      const age = 0.6 + 0.5 * (((rh >>> 8) & 255) / 255);
      if (!pedestrian && s.sidewalk >= 24 && ((rh >>> 16) & 7) !== 0) {
        if (strip === "pits") {
          const a0 = ((35 - Math.floor(s.s0)) % 72 + 72) % 72;
          for (let t = a0; t < s.len; t += 72) {
            if (nearJunction(t, vx(5))) continue;
            if (Math.abs(((t - (side < 0 ? 0 : lampSpacing / 2) - vx(6)) % lampSpacing)) < vx(3)) continue;
            const x = px(t, curbQ + 6);
            const y = py(t, curbQ + 6);
            addTree(x, y, ground(x, y) + 2, vx(Rngf(W.v(x), W.v(y), seed, 7, 10) * age), vx(Rngf(W.v(y), W.v(x), seed, 2.2, 2.9) * age), avenue);
          }
        } else if (strip === "grass") {
          for (let t = vx(5); t < s.len; t += vx(11)) {
            if (nearJunction(t, vx(5))) continue;
            const x = px(t, curbQ + 6);
            const y = py(t, curbQ + 6);
            if ((hash32(seed, W.vi(x), W.vi(y), 5) & 3) === 0) continue;
            addTree(x, y, ground(x, y) + 2, vx(Rngf(W.v(x), W.v(y), seed, 6.5, 10) * age), vx(Rngf(W.v(y), W.v(x), seed, 2.2, 3.4) * age), (hash32(seed, W.v(x), W.v(y), 8) & 15) === 0 ? "blossom" : avenue === "street" ? "oak" : avenue);
          }
        }
      }
      if (pedestrian) {
        for (let t = vx(10); t < s.len - vx(10); t += vx(16)) {
          if (nearJunction(t, vx(6))) continue;
          const x = px(t, s.hr - 16);
          const y = py(t, s.hr - 16);
          addProp("planter", x, y, ground(x, y) + 2, ox, oy, obx, oby);
          addProp("bench", px(t + vx(4), s.hr - 10), py(t + vx(4), s.hr - 10), ground(x, y) + 2, ox, oy, -obx, -oby);
        }
        continue;
      }
      // hydrants / bins / benches
      const hyd = (hash32(seed, s.road.id.length, W.vi(s.ax), side) % 200) + vx(20);
      for (let t = hyd; t < s.len - vx(8); t += vx(85)) {
        if (nearJunction(t, vx(4))) continue;
        const x = px(t, curbQ + 3);
        const y = py(t, curbQ + 3);
        addProp("hydrant", x, y, ground(x, y) + 2, ox, oy, obx, oby);
      }
      if (cls === "arterial" || cls === "collector") {
        for (const jn of s.jn) {
          for (const dir of [-1, 1]) {
            const t = jn.s + dir * (jn.hr + vx(4));
            if (t < vx(2) || t > s.len - vx(2)) continue;
            if (nearJunction(t, 0)) continue;
            const q = s.hr - 5;
            const x = px(t, q);
            const y = py(t, q);
            const z = ground(x, y) + 2;
            if ((hash32(seed, W.vi(x), W.vi(y), 13) & 1) === 0) addProp("bin", x, y, z, ox, oy, obx, oby);
            else addProp("bench", x - s.dx * 6, y - s.dy * 6, z, ox, oy, obx, oby);
            // traffic signals on the approach side (right-hand traffic)
            if (jn.signal && side === -dir) {
              const tq = jn.s + dir * (jn.hc + vx(1.5));
              const sx = px(tq, curbQ + 3);
              const sy = py(tq, curbQ + 3);
              addProp("signal", sx, sy, ground(sx, sy) + 2, ox, oy, obx, oby, { reach: Math.max(10, s.hc - 8) });
            }
          }
        }
      }
      if (cls === "arterial") {
        const start = (hash32(seed, W.vi(s.ax), W.vi(s.ay), side) % vx(200)) + vx(60);
        for (let t = start; t < s.len - vx(40); t += vx(380)) {
          if (nearJunction(t, vx(20))) continue;
          const x = px(t, s.hr - 2);
          const y = py(t, s.hr - 2);
          addProp("busShelter", x, y, ground(x, y) + 2, ox, oy, obx, oby);
        }
      }
      // parked cars in the parking lane
      if (s.parking > 0 && world.config.vehicles?.parked) {
        const q = s.hc - s.parking / 2;
        const first = ((24 - Math.floor(s.s0)) % 48 + 48) % 48;
        for (let t = first; t < s.len; t += 48) {
          if (nearJunction(t, vx(9))) continue;
          if ((hash32(seed, W.vi(s.ax + t), W.vi(s.ay + t), side) % 100) > 58) continue;
          const x = px(t - 16, q + 7);
          const y = py(t - 16, q + 7);
          // car local a = length along the street, b = width towards the curb
          addProp("car", x, y, ground(x, y) + 1, ox, oy, -obx, -oby);
        }
      }
    }
  }

  // ---------------- the angled world: trees on the lawns between a slanted
  // street and its block's lots (compose.js), a few metres off the street
  // and the lots, now and then (a polygon block with nothing on it is a
  // small tree-lined green)
  if (angledProps) {
    for (const b of plan.net.blocks) {
      if (!b.cuts || !b.levelAt) continue;
      const r = b.prop;
      const kind = STREET_TREES[hashString(b.id) % STREET_TREES.length];
      for (let y = r.y0 + 24; y <= r.y1 - 24; y += 48)
        for (let x = r.x0 + 24; x <= r.x1 - 24; x += 48) {
          const h = hash32(seed, W.vi(x), W.vi(y), 0x6e7e);
          if (h % 100 >= 55) continue;
          const tx = x + ((h >>> 8) & 15) - 8;
          const ty = y + ((h >>> 12) & 15) - 8;
          if (b.cuts.some((k) => k.nx * tx + k.ny * ty < k.c + 20) || !insideCuts(b.cuts, tx + 0.5, ty + 0.5)) continue;
          if (plan.blockAt(tx, ty) !== b || plan.spaceAt(tx, ty)) continue;
          if ([[0, 0], [-24, 0], [24, 0], [0, -24], [0, 24]].some(([dx, dy]) => plan.lotAt(tx + dx, ty + dy))) continue;
          const age = 0.7 + 0.4 * (((h >>> 16) & 255) / 255);
          addTree(tx, ty, naturalGroundAt(world, tx, ty) + 1, vx(Rngf(W.v(tx), W.v(ty), seed, 7, 11) * age), vx(Rngf(W.v(ty), W.v(tx), seed, 2.4, 3.4) * age), kind);
        }
    }
  }

  // ---------------- parks, plazas, sports
  const ss = { mat: 0, dz: 0, water: false };
  for (const sp of plan.spaces) {
    const r = sp.rect;
    const rng = Rng.from(seed, sp.id, "dress");
    const z = sp.groundZ + 1;
    // props stand on the space's graded surface at their own spot
    const lift = (x, y, zz) => (sp.levelAt ? spaceGroundAt(world, plan, sp, x, y) + 1 + (zz - z) : zz);
    const treeS = (x, y, zz, ...rest) => addTree(x, y, lift(x, y, zz), ...rest);
    const propS0 = (kind, x, y, zz, ...rest) => addProp(kind, x, y, lift(x, y, zz), ...rest);
    // a row of lock-up garages steps down a slope in pairs, each pair level
    // with the lowest ground under it (dug into the slope on its uphill side)
    const propS = (kind, x, y, zz, ax, ay, bx, by, extra = {}) => {
      if (kind !== "garageRow") return propS0(kind, x, y, zz, ax, ay, bx, by, extra);
      for (let k = 0; k < extra.n; k += 2) {
        const m = Math.min(2, extra.n - k);
        const px = x + ax * k * 24;
        const py = y + ay * k * 24;
        let zMin = Infinity;
        for (const [da, db] of [[0, 0], [m * 24 - 1, 0], [0, 47], [m * 24 - 1, 47]]) zMin = Math.min(zMin, lift(px + ax * da + bx * db, py + ay * da + by * db, zz));
        addProp(kind, px, py, zMin, ax, ay, bx, by, { ...extra, n: m });
      }
    };
    if (sp.kind === "tankFarm" || sp.kind === "containerYard") {
      dressIndustry(world, sp, z, propS);
      if (sp.quay) dressPort(world, sp, z, addProp);
    } else if (sp.quay) {
      // a quay car park: cranes and a moored ship along the edge
      dressPort(world, sp, z, addProp);
    } else if (sp.kind === "courtyard") {
      // birches and poplars on the lawns, playgrounds, benches, a row of lock-up garages
      const free = (x, y, pad) => !plan.buildingsIn({ x0: x - pad, y0: y - pad, x1: x + pad, y1: y + pad }).length;
      const step = vx(11);
      for (let y = r.y0 + vx(12); y < r.y1 - vx(12); y += step)
        for (let x = r.x0 + vx(12); x < r.x1 - vx(12); x += step) {
          const jx = Math.round(x + rng.float(-0.4, 0.4) * step);
          const jy = Math.round(y + rng.float(-0.4, 0.4) * step);
          spaceSurface(sp, jx, jy, ss, W.v(jx), W.v(jy));
          if (ss.mat !== MAT.GRASS_LAWN && ss.mat !== MAT.GRASS_DRY) continue;
          if (!rng.chance(sp.district === "projects" ? 0.25 : 0.45) || !free(jx, jy, vx(6))) continue;
          const kind = rng.weighted([["birch", 4], ["poplar", 2], ["oak", 1]]);
          treeS(jx, jy, z, vx(rng.float(8, 15)), vx(rng.float(2, 3.2)), kind === "poplar" ? "pine" : kind);
        }
      for (let k = 0; k < 2; k += 1) {
        const x = Math.round(r.x0 + (r.x1 - r.x0) * rng.float(0.25, 0.75));
        const y = Math.round(r.y0 + (r.y1 - r.y0) * rng.float(0.25, 0.75));
        if (free(x + vx(3), y + vx(3), vx(9))) propS("playground", x, y, z, 1, 0, 0, 1);
        if (free(x - vx(4), y, vx(3))) propS("bench", x - vx(4), y, z, 1, 0, 0, 1);
      }
      // garages: along the inside of the parking band on one side
      const gx = r.x0 + vx(9);
      const gy = rng.chance(0.5) ? r.y0 + vx(9) : r.y1 - vx(9) - vx(6);
      const n = Math.min(14, Math.floor((r.x1 - r.x0 - vx(30)) / vx(3)));
      if (n >= 4 && free(gx + (n * vx(3)) / 2, gy + vx(3), (n * vx(3)) / 2)) propS("garageRow", gx, gy, z, 1, 0, 0, 1, { n });
    } else if (sp.kind === "park") {
      dressPark(sp, rng, z, propS, treeS);
    } else if (sp.kind === "square") {
      dressSquare(sp, rng, z, propS, treeS, seasonOf(world.config).id === "winter");
    } else if (sp.kind === "garden") {
      // fruit trees in the lawns, benches round the middle bed
      const cx = (r.x0 + r.x1) / 2;
      const cy = (r.y0 + r.y1) / 2;
      for (const [fx, fy] of [[0.27, 0.27], [0.73, 0.27], [0.27, 0.73], [0.73, 0.73]]) {
        if (!rng.chance(0.8)) continue;
        treeS(r.x0 + (r.x1 - r.x0) * fx, r.y0 + (r.y1 - r.y0) * fy, z, vx(rng.float(4.5, 7)), vx(rng.float(1.8, 2.6)), rng.pick(["blossom", "rowan", "oak"]));
      }
      for (const [dx, dy, ax, ay] of [[0, -1, 1, 0], [0, 1, 1, 0], [-1, 0, 0, 1], [1, 0, 0, 1]]) propS("bench", cx + dx * vx(4.2) - ax * 6, cy + dy * vx(4.2) - ay * 6, z, ax, ay, -dx, -dy);
    } else if (sp.kind === "cemetery") {
      dressCemetery(world, sp, rng, z, lift, addProp, addTree, props);
    } else if (sp.kind === "allotments") {
      dressAllotments(sp, rng, z, lift, addProp, addTree, props);
    } else if (sp.kind === "garages") {
      // rows of lock-up garages back to back, lanes between them
      const alongX = r.x1 - r.x0 >= r.y1 - r.y0;
      const L = alongX ? r.x1 - r.x0 : r.y1 - r.y0;
      const D = alongX ? r.y1 - r.y0 : r.x1 - r.x0;
      const n = Math.min(24, Math.floor((L - vx(8)) / 24));
      for (let b = vx(4); b + 96 <= D - vx(4) && n >= 3; b += 96 + vx(9)) {
        for (const [bb, dir] of [[b, -1], [b + 48, 1]]) {
          // (a row faces away from its back-to-back partner)
          const x = alongX ? r.x0 + vx(4) : r.x0 + bb + (dir < 0 ? 47 : 0);
          const y = alongX ? r.y0 + bb + (dir < 0 ? 47 : 0) : r.y0 + vx(4);
          if (alongX) propS("garageRow", x, y, z, 1, 0, 0, dir, { n });
          else propS("garageRow", x, y, z, 0, 1, dir, 0, { n });
        }
      }
    } else if (sp.kind === "wasteland") {
      // rubble, scrub and birch saplings; the remains of a fence
      const step = vx(9);
      for (let y = r.y0 + vx(4); y < r.y1 - vx(4); y += step)
        for (let x = r.x0 + vx(4); x < r.x1 - vx(4); x += step) {
          const q = rng.next();
          const jx = Math.round(x + rng.float(-0.4, 0.4) * step);
          const jy = Math.round(y + rng.float(-0.4, 0.4) * step);
          if (q < 0.12) propS("rubble", jx, jy, z, 1, 0, 0, 1);
          else if (q < 0.3) treeS(jx, jy, z, vx(rng.float(0.8, 1.6)), vx(rng.float(0.8, 1.4)), "shrub");
          else if (q < 0.4) treeS(jx, jy, z, vx(rng.float(3, 7)), vx(rng.float(1, 1.8)), "birch");
        }
      const lf = new Frame(r, sp.front ?? "S");
      props.push(...runPropZ(lf, 0, 0, Math.round(lf.U * rng.float(0.3, 0.7)), 0, lift, z, "chainlink"));
    } else if (sp.kind === "riverside") {
      // trees in the lawn squares of the promenade grid, lamps and benches on the paths
      for (let y = r.y0; y < r.y1; y += 96) {
        for (let x = r.x0; x < r.x1; x += 96) {
          const cx = x + 54;
          const cy = y + 54;
          if (cx > r.x1 - 16 || cy > r.y1 - 16) continue;
          const ri = world.rivers ? world.rivers.at(cx, cy) : null;
          if (ri && ri.d < ri.half + 6) continue;
          if (rng.chance(0.7)) treeS(cx, cy, z, vx(rng.float(7, 11)), vx(rng.float(2.4, 3.4)), rng.pick(["oak", "street", "blossom"]));
          const ri2 = world.rivers ? world.rivers.at(x + 6, cy) : null;
          if (!(ri2 && ri2.d < ri2.half + 3)) {
            const pick = rng.next();
            if (pick < 0.5) propS("parkLamp", x + 6, cy, z, 1, 0, 0, 1);
            else if (pick < 0.75) propS("bench", x + 4, cy - 6, z, 0, 1, 1, 0);
          }
        }
      }
    } else if (sp.kind === "plaza") {
      // trees in the beds along the long sides, benches between them, lamps
      const W2 = r.x1 - r.x0;
      const H2 = r.y1 - r.y0;
      const long = W2 >= H2;
      const L2 = long ? W2 : H2;
      const kind = rng.pick(["street", "street", "maple", "oak"]);
      // (the beds of landscape.js: every 8 m along, 4 m in from the side)
      for (let a = vx(8); a < L2 - vx(4); a += vx(8)) {
        for (const side of [0, 1]) {
          const q = side ? (long ? H2 : W2) - vx(4) : vx(4);
          const [x, y] = long ? [r.x0 + a + vx(0.9), r.y0 + q] : [r.x0 + q, r.y0 + a + vx(0.9)];
          treeS(x, y, z, vx(rng.float(7, 11)), vx(rng.float(2.2, 3.2)), kind);
          const [bx, by] = long ? [x + vx(4), y] : [x, y + vx(4)];
          if (rng.chance(0.5)) propS("bench", bx - (long ? 6 : 0), by - (long ? 0 : 6), z, long ? 1 : 0, long ? 0 : 1, 0, side ? -1 : 1);
          else if (rng.chance(0.4)) propS("parkLamp", bx, by, z, 1, 0, 0, 1);
        }
      }
    }
  }

  const addTree0 = addTree;
  const addProp0 = addProp;
  // ---------------- yards: trees, hedges, fences
  for (const lot of plan.lots) {
    if (!lot.building) continue;
    const env = plan.buildingById.get(lot.building);
    if (!env) continue;
    const rng = Rng.from(seed, lot.id, "yard");
    const z = lot.groundZ + 1;
    // everything in the yard stands on the graded ground at its own spot (city/grading.js)
    const onGround = (x, y) => plan.grading.at(x, y, lot.levelAt ? lot.levelAt(x, y) : world.terrain.sample(x, y).h, lot.block, lot) + 1;
    const lift = (x, y, zz) => onGround(x, y) + (zz - z);
    const addTree = (x, y, zz, ...rest) => addTree0(x, y, lift(x, y, zz), ...rest);
    const addProp = (kind, x, y, zz, ...rest) => addProp0(kind, x, y, lift(x, y, zz), ...rest);
    const runProp = (lf2, u0, v0, u1, v1, zz, kind) => runPropZ(lf2, u0, v0, u1, v1, lift, zz, kind);
    const lf = lotFrameOf(lot);
    // the building in the lot frame (a turned lot: its canonical offset, both share one placement)
    const toLot = (r) => (lf.turned ? { x0: r.x0 + env.turn.ou - lot.turn.ou, y0: r.y0 + env.turn.ov - lot.turn.ov, x1: r.x1 + env.turn.ou - lot.turn.ou, y1: r.y1 + env.turn.ov - lot.turn.ov } : null);
    const bLot = lf.turned ? toLot({ x0: 0, y0: 0, x1: env.U - 1, y1: env.V - 1 }) : lf.rectFromWorld(env.R);
    if (lot.churchyard) {
      // rows of graves round the church, a stone wall and a few birches and spruces
      const clear = (u, v) => u >= bLot.x0 - vx(2) && u <= bLot.x1 + vx(2) && v >= bLot.y0 - vx(3) && v <= bLot.y1 + vx(2);
      // the lot frame's axes in world steps: headstones face the street
      const [ox, oy] = lf.toWorld(0, 0);
      const [ux, uy] = lf.toWorld(1, 0);
      const [wx, wy] = lf.toWorld(0, 1);
      for (let v = vx(3); v < lf.V - vx(2.5); v += vx(2.5))
        for (let u = vx(2.5); u < lf.U - vx(2.5); u += vx(1.75)) {
          if (clear(u, v) || Math.abs(u - (bLot.x0 + bLot.x1) / 2) < vx(1.5) || !rng.chance(0.62)) continue;
          const [x, y] = lf.toWorld(u, v);
          addProp("gravestone", x, y, z, ux - ox, uy - oy, ox - wx, oy - wy);
        }
      for (let k = 0; k < rng.int(3, 6); k += 1) {
        const u = rng.chance(0.5) ? rng.int(vx(1.5), vx(3)) : rng.int(lf.U - vx(3), lf.U - vx(1.5));
        const [x, y] = lf.toWorld(u, rng.int(vx(4), lf.V - vx(3)));
        addTree(x, y, z, vx(rng.float(8, 14)), vx(rng.float(1.8, 3)), rng.chance(0.5) ? "birch" : "pine");
      }
      props.push(...runProp(lf, 0, 0, lf.U - 1, 0, z, "wall"), ...runProp(lf, 0, lf.V - 1, lf.U - 1, lf.V - 1, z, "wall"));
      props.push(...runProp(lf, 0, 0, 0, lf.V - 1, z, "wall"), ...runProp(lf, lf.U - 1, 0, lf.U - 1, lf.V - 1, z, "wall"));
    } else if (env.archetype === "barn") {
      // silos beside the barn, round bales in the yard, a board fence round the plot
      const side = rng.chance(0.5) ? 1 : -1;
      const su = side > 0 ? Math.min(lf.U - vx(3), bLot.x1 + vx(3.5)) : Math.max(vx(3), bLot.x0 - vx(3.5));
      const n = rng.int(1, 2);
      for (let k = 0; k < n; k += 1) {
        const [x, y] = lf.toWorld(su, bLot.y0 + vx(4) + k * vx(7));
        addProp("silo", x, y, z, 1, 0, 0, 1, { r: Math.round(vx(rng.float(2.2, 3))), h: Math.round(vx(rng.float(9, 15))) });
      }
      for (let k = 0; k < rng.int(3, 7); k += 1) {
        const [x, y] = lf.toWorld(rng.int(vx(3), lf.U - vx(3)), rng.int(Math.min(lf.V - vx(3), bLot.y1 + vx(3)), lf.V - vx(2)));
        addProp("roundBale", x, y, z, 1, 0, 0, 1);
      }
      props.push(...runProp(lf, 0, lf.V - 1, lf.U - 1, lf.V - 1, z, "fence"));
      props.push(...runProp(lf, 0, vx(6), 0, lf.V - 1, z, "fence"));
      props.push(...runProp(lf, lf.U - 1, vx(6), lf.U - 1, lf.V - 1, z, "fence"));
    } else if (env.archetype === "house" || env.archetype === "rowhouse") {
      // back-yard trees
      const backDepth = lf.V - 1 - bLot.y1;
      if (backDepth > vx(5)) {
        const n = env.archetype === "house" ? rng.int(1, 3) : rng.int(0, 1);
        for (let k = 0; k < n; k += 1) {
          const u = rng.int(vx(2), lf.U - vx(2));
          const v = rng.int(bLot.y1 + vx(3), lf.V - vx(2));
          const [x, y] = lf.toWorld(u, v);
          const kind = rng.weighted([["oak", 4], ["birch", 2], ["blossom", 1], ["pine", 1]]);
          addTree(x, y, z, vx(rng.float(5, 10)), vx(rng.float(1.8, 3.2)), kind);
        }
      }
      if (env.archetype === "house") {
        // hedge along the front lot line except the path/driveway, fences on the sides
        const entU = bLot.x0 + (env.entranceU ?? env.U / 2);
        const garage = env.annexes[0] ? (lf.turned ? toLot(env.annexes[0].canon) : lf.rectFromWorld(env.annexes[0].world)) : null;
        const gaps = [[entU - vx(1.2), entU + vx(1.2)]];
        if (garage) gaps.push([garage.x0 - 1, garage.x1 + 1]);
        gaps.sort((a, b) => a[0] - b[0]);
        let u0 = 0;
        for (const [g0, g1] of gaps) {
          if (g0 - 1 >= u0) props.push(...runProp(lf, u0, 0, g0 - 1, 0, z, "hedge"));
          u0 = Math.max(u0, g1 + 1);
        }
        if (u0 <= lf.U - 1) props.push(...runProp(lf, u0, 0, lf.U - 1, 0, z, "hedge"));
        props.push(...runProp(lf, 0, vx(3), 0, lf.V - 1, z, "fence"));
        props.push(...runProp(lf, lf.U - 1, vx(3), lf.U - 1, lf.V - 1, z, "fence"));
      }
    } else if ((env.archetype === "townhouse" || env.archetype === "wharfhouse") && lf.V - 1 - bLot.y1 > vx(4)) {
      // an old-town back yard: a board fence round it, a shed or a woodpile
      // against the back fence, a rowan or a birch, currant bushes
      const back = lf.V - 1;
      props.push(...runPropZ(lf, 0, back, lf.U - 1, back, lift, z, "fence"));
      if (!lot.corner) {
        props.push(...runPropZ(lf, 0, bLot.y1 + 2, 0, back, lift, z, "fence"));
        props.push(...runPropZ(lf, lf.U - 1, bLot.y1 + 2, lf.U - 1, back, lift, z, "fence"));
      }
      const [ax, ay, fx, fy] = frameAxes(lf);
      const depth = back - bLot.y1;
      if (depth > vx(5) && lf.U > vx(7)) {
        const su = rng.int(vx(1), Math.max(vx(1), lf.U - vx(4)));
        const [sx, sy] = lf.toWorld(su, back - vx(1.5));
        addProp(rng.chance(0.55) ? "allotmentHut" : "woodpile", sx, sy, z, ax, ay, fx, fy);
      }
      if (depth > vx(6) && rng.chance(0.6)) {
        const [tx, ty] = lf.toWorld(rng.int(vx(2), lf.U - vx(2)), bLot.y1 + Math.round(depth * rng.float(0.35, 0.6)));
        addTree(tx, ty, z, vx(rng.float(5, 9)), vx(rng.float(1.8, 2.8)), rng.pick(["rowan", "birch", "blossom"]));
      }
      for (let k = 0; k < rng.int(0, 2); k += 1) {
        const [bx, by] = lf.toWorld(rng.int(vx(1), lf.U - vx(1)), back - vx(0.8));
        addTree(bx, by, z, vx(rng.float(0.7, 1.1)), vx(rng.float(0.5, 0.9)), "shrub");
      }
    } else if ((env.archetype === "walkup" || env.archetype === "midrise") && lf.V - 1 - bLot.y1 > vx(7)) {
      const n = Math.round(((lf.V - bLot.y1) * lf.U) / (vx(9) * vx(9)));
      for (let k = 0; k < n; k += 1) {
        const u = rng.int(vx(2), lf.U - vx(2));
        const v = rng.int(bLot.y1 + vx(3), lf.V - vx(3));
        if (!rng.chance(0.5)) continue;
        const [x, y] = lf.toWorld(u, v);
        addTree(x, y, z, vx(rng.float(6, 11)), vx(rng.float(2, 3.4)), rng.pick(["oak", "birch", "autumn"]));
      }
    }
  }

  const propGrid = new SpatialGrid(128);
  for (const p of props) propGrid.insert(p, p.bb);
  const treeGrid = new SpatialGrid(128);
  for (const t of trees) treeGrid.insert(t, t.bb);
  return { props, trees, propGrid, treeGrid };
}

const RUNS = {
  hedge: [MAT.HEDGE, 7],
  wall: [MAT.GRANITE, 6],
  fence: [MAT.FENCE_WOOD, 9],
  picket: [MAT.FENCE_WHITE, 6],
  chainlink: [MAT.CHAINLINK, 14],
};

/**
 * A lot frame's axes in world steps [ux, uy, fx, fy]: along its front, and
 * towards its street. Square to the grid on a turned lot (props stay in the
 * world grid: the axes of the turn, snapped).
 */
function frameAxes(lf) {
  if (lf.turned) {
    const [a, b] = lf.dirToWorld(1, 0);
    const [ux, uy] = Math.abs(a) >= Math.abs(b) ? [Math.sign(a), 0] : [0, Math.sign(b)];
    return [ux, uy, uy, -ux];
  }
  const [ox, oy] = lf.toWorld(0, 0);
  const [ux, uy] = lf.toWorld(1, 0);
  const [wx, wy] = lf.toWorld(0, 1);
  return [ux - ox, uy - oy, ox - wx, oy - wy];
}

function runProp(lf, u0, v0, u1, v1, z, kind) {
  if (lf.turned) return turnedRun(lf, u0, v0, u1, v1, z, kind);
  const r = lf.rectToWorld({ x0: u0, y0: v0, x1: u1, y1: v1 });
  const [m, h] = RUNS[kind] ?? RUNS.fence;
  const box = { ...r, z0: z, z1: z + h - 1, m };
  return { kind, boxes: [box], bb: { ...box } };
}

/**
 * A run along a turned lot's side: the world columns whose cells lie on it
 * (the frame's exact map), merged along x, so a fence or a hedge steps
 * along the turned line in the world grid.
 */
function turnedRun(lf, u0, v0, u1, v1, z, kind) {
  const [m, h] = RUNS[kind] ?? RUNS.fence;
  const r = lf.rectToWorld({ x0: u0, y0: v0, x1: u1, y1: v1 });
  const boxes = [];
  for (let y = r.y0; y <= r.y1; y += 1) {
    let a = null;
    for (let x = r.x0; x <= r.x1 + 1; x += 1) {
      let inside = false;
      if (x <= r.x1) {
        const [u, v] = lf.fromWorld(x, y);
        inside = u >= u0 && u <= u1 && v >= v0 && v <= v1;
      }
      if (inside && a === null) a = x;
      else if (!inside && a !== null) {
        boxes.push({ x0: a, y0: y, z0: z, x1: x - 1, y1: y, z1: z + h - 1, m });
        a = null;
      }
    }
  }
  return { kind, boxes, bb: { x0: r.x0, y0: r.y0, z0: z, x1: r.x1, y1: r.y1, z1: z + h - 1 } };
}

/** A wall / fence run in 4 m pieces, each on the ground at its own spot (lift(x, y, z)). */
function runPropZ(lf, u0, v0, u1, v1, lift, z, kind) {
  const out = [];
  const alongU = u1 - u0 >= v1 - v0;
  const a0 = alongU ? u0 : v0;
  const a1 = alongU ? u1 : v1;
  for (let a = a0; a <= a1; a += 32) {
    const b = Math.min(a1, a + 31);
    const q = alongU ? { x0: a, y0: v0, x1: b, y1: v1 } : { x0: u0, y0: a, x1: u1, y1: b };
    const [cx, cy] = lf.toWorld((q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2);
    out.push(runProp(lf, q.x0, q.y0, q.x1, q.y1, lift(cx, cy, z), kind));
  }
  return out;
}

/**
 * Landscape park (city/parks.js): trees in groves, each grove mostly one
 * species, big solitary trees on the open lawn, willows and alders by the
 * pond, a fountain on the hub, benches and lamps along the walks, a
 * playground near an entrance.
 */
function dressPark(sp, rng, z, propS, treeS) {
  const r = sp.rect;
  const L = parkLayout(sp);
  const GROVE = [
    ["oak", 5],
    ["maple", 4],
    ["street", 2],
    ["birch", 3],
    ["pine", 1.5],
    ["spruce", 1],
    ["blossom", 1],
  ];
  const clearOf = (u, v, pad) => {
    if (u < pad || v < pad || u > L.W - pad || v > L.H - pad) return false;
    if (pathDist(L, u, v) < L.pathW + pad) return false;
    if (Math.hypot(u - L.hub.u, v - L.hub.v) < L.hub.r + pad) return false;
    if (L.pond && pondDist(L.pond, u, v) < pad) return false;
    return true;
  };
  const step = vx(3.5);
  for (let v = step / 2; v < L.H; v += step)
    for (let u = step / 2; u < L.W; u += step) {
      const ju = u + rng.float(-0.45, 0.45) * step;
      const jv = v + rng.float(-0.45, 0.45) * step;
      const g = groveAt(L, ju, jv);
      const nearPond = L.pond ? pondDist(L.pond, ju, jv) : Infinity;
      // groves where the grove noise is high; solitary trees on the lawns; willows by the water
      let p = g > 0.58 ? 0.55 * Math.min(1, (g - 0.58) * 6) : 0.006;
      if (nearPond < vx(6) && nearPond > vx(1.5)) p = Math.max(p, 0.12);
      if (!rng.chance(p)) continue;
      const solitary = g <= 0.58 && nearPond >= vx(6);
      if (!clearOf(ju, jv, solitary ? vx(4) : vx(1.8))) continue;
      // one species per grove (the grove cell's hash), a few others mixed in
      const gcell = hash32(L.seed, Math.floor(ju / vx(22)), Math.floor(jv / vx(22)), 0x61);
      let kind = rng.chance(0.8) ? pickWeightedBy(GROVE, (gcell & 0xffff) / 0x10000) : rng.weighted(GROVE);
      if (nearPond < vx(6)) kind = rng.chance(0.7) ? "willow" : "birch";
      if (solitary) kind = rng.pick(["oak", "maple", "willow", "street"]);
      const big = solitary ? 1.2 : 1;
      const [hh, rr] = kind === "pine" || kind === "spruce" ? [rng.float(10, 18), rng.float(2, 3)] : kind === "birch" ? [rng.float(9, 15), rng.float(1.8, 2.6)] : [rng.float(8, 14) * big, rng.float(2.6, 4.2) * big];
      treeS(r.x0 + ju, r.y0 + jv, z, vx(hh), vx(rr), kind);
    }
  // shrubs along the edges of the groves
  for (let k = 0; k < Math.round((L.W * L.H) / (vx(14) * vx(14))); k += 1) {
    const u = rng.float(vx(3), L.W - vx(3));
    const v = rng.float(vx(3), L.H - vx(3));
    const g = groveAt(L, u, v);
    if (g < 0.5 || g > 0.62 || !clearOf(u, v, vx(1.5))) continue;
    treeS(r.x0 + u, r.y0 + v, z, vx(rng.float(1, 2.2)), vx(rng.float(1, 1.8)), "shrub");
  }
  // a fountain on the hub; benches and lamps along the walks
  if (L.hub.r > vx(4)) propS("fountain", r.x0 + L.hub.u, r.y0 + L.hub.v, z, 1, 0, 0, 1);
  let acc = 0;
  for (const s of L.segs) {
    const len = Math.hypot(s[2] - s[0], s[3] - s[1]);
    acc += len;
    if (acc < vx(24)) continue;
    acc = 0;
    const dx = (s[2] - s[0]) / (len || 1);
    const dy = (s[3] - s[1]) / (len || 1);
    const side = rng.chance(0.5) ? 1 : -1;
    const off = L.pathW + 3;
    const px = s[0] - dy * side * off;
    const py = s[1] + dx * side * off;
    if (px < 8 || py < 8 || px > L.W - 8 || py > L.H - 8) continue;
    if (rng.chance(0.5)) propS("parkLamp", r.x0 + px, r.y0 + py, z, dx, dy, dy * side, -dx * side);
    else propS("bench", r.x0 + px, r.y0 + py, z, dx, dy, dy * side, -dx * side);
  }
  if (L.W > vx(60) && L.H > vx(50) && rng.chance(0.6)) {
    const e = L.ents[0];
    const pu = Math.min(L.W - vx(10), Math.max(vx(4), e.u - vx(3)));
    const pv = Math.min(L.H - vx(9), Math.max(vx(4), e.v - vx(3)));
    if (clearOf(pu + vx(3), pv + vx(2.5), vx(3.5))) propS("playground", r.x0 + pu, r.y0 + pv, z, 1, 0, 0, 1);
  }
}

function pickWeightedBy(list, t) {
  let total = 0;
  for (const [, w] of list) total += w;
  let acc = t * total;
  for (const [k, w] of list) {
    acc -= w;
    if (acc < 0) return k;
  }
  return list[list.length - 1][0];
}

/**
 * Market square: a monument in the middle, market stalls in a ring round
 * it (packed away in winter), lanterns along the edge, benches facing the
 * monument, a tree at each corner.
 */
function dressSquare(sp, rng, z, propS, treeS, winter) {
  const r = sp.rect;
  const cx = Math.round((r.x0 + r.x1) / 2);
  const cy = Math.round((r.y0 + r.y1) / 2);
  const W = r.x1 - r.x0;
  const H = r.y1 - r.y0;
  propS("monument", cx, cy, z, 1, 0, 0, 1);
  if (!winter) {
    const R = Math.min(W, H) * 0.3;
    const n = Math.max(3, Math.min(10, Math.round((2 * Math.PI * R) / vx(7))));
    for (let k = 0; k < n; k += 1) {
      if (!rng.chance(0.75)) continue;
      const a = (k / n) * Math.PI * 2 + rng.float(-0.1, 0.1);
      const x = cx + Math.cos(a) * R;
      const y = cy + Math.sin(a) * R;
      // stalls face the monument
      const ox = Math.abs(Math.cos(a)) > 0.7;
      propS("marketStall", x - (ox ? 8 : 12), y - (ox ? 12 : 8), z, ox ? 0 : 1, ox ? 1 : 0, ox ? 1 : 0, ox ? 0 : 1);
    }
  }
  const inset = vx(2.2);
  for (let x = r.x0 + inset; x <= r.x1 - inset; x += vx(12)) {
    propS("oldLamp", x, r.y0 + inset, z, 1, 0, 0, 1);
    propS("oldLamp", x, r.y1 - inset, z, 1, 0, 0, -1);
  }
  for (let y = r.y0 + inset + vx(12); y <= r.y1 - inset - vx(6); y += vx(12)) {
    propS("oldLamp", r.x0 + inset, y, z, 0, 1, 1, 0);
    propS("oldLamp", r.x1 - inset, y, z, 0, 1, -1, 0);
  }
  for (const [fx, fy] of [[0.1, 0.1], [0.9, 0.1], [0.1, 0.9], [0.9, 0.9]]) {
    if (W < vx(30) || H < vx(30)) break;
    treeS(r.x0 + W * fx, r.y0 + H * fy, z, vx(rng.float(8, 12)), vx(rng.float(2.4, 3.4)), "street");
  }
  for (const [dx, dy] of [[0, -1], [0, 1], [-1, 0], [1, 0]]) propS("bench", cx + dx * vx(4.5) - (dy ? 6 : 0), cy + dy * vx(4.5) - (dx ? 6 : 0), z, dy ? 1 : 0, dx ? 1 : 0, -dx, -dy);
}

/**
 * Cemetery: a granite wall round the block with a gate on the street,
 * rows of headstones and grave crosses facing the main path, birches,
 * spruces and limes along it, benches by the chapel.
 */
function dressCemetery(world, sp, rng, z, lift, addProp, addTree, props) {
  const lf = new Frame(sp.rect, sp.front ?? "S");
  const U = lf.U;
  const V = lf.V;
  const [ox, oy] = lf.toWorld(0, 0);
  const [ux, uy] = lf.toWorld(1, 0);
  const [wx, wy] = lf.toWorld(0, 1);
  const au = [ux - ox, uy - oy];
  const av = [wx - ox, wy - oy];
  const chapel = sp.chapel ? lf.rectFromWorld(sp.chapel) : null;
  const mid = (U - 1) / 2;
  const inChapel = (u, v) => chapel && u >= chapel.x0 - vx(2) && u <= chapel.x1 + vx(2) && v >= chapel.y0 - vx(3) && v <= chapel.y1 + vx(1);
  // rows of graves between the paths, their faces to the main path's side
  for (let v = vx(3.5); v < V - vx(2.5); v += vx(2.4)) {
    if (((v - vx(4)) % vx(16)) < vx(1.6)) continue;
    for (let u = vx(2.8); u < U - vx(2.8); u += vx(1.8)) {
      if (Math.abs(u - mid) < vx(2.4) || inChapel(u, v) || !rng.chance(0.7)) continue;
      const [x, y] = lf.toWorld(u, v);
      addProp("gravestone", x, y, lift(x, y, z), au[0], au[1], -av[0], -av[1]);
    }
  }
  // an avenue of trees along the main path
  for (let v = vx(6); v < V - vx(6); v += vx(9)) {
    if (inChapel(mid, v)) break;
    const kind = rng.pick(["birch", "spruce", "street"]);
    for (const du of [-vx(3), vx(3)]) {
      const [x, y] = lf.toWorld(mid + du, v);
      addTree(x, y, lift(x, y, z), vx(kind === "spruce" ? rng.float(10, 16) : rng.float(8, 13)), vx(rng.float(1.8, 2.8)), kind);
    }
  }
  // the wall, with a gate where the main path meets the street
  const g0 = Math.round(mid - vx(1.6));
  const g1 = Math.round(mid + vx(1.6));
  props.push(...runPropZ(lf, 0, 0, g0 - 1, 0, lift, z, "wall"), ...runPropZ(lf, g1 + 1, 0, U - 1, 0, lift, z, "wall"));
  props.push(...runPropZ(lf, 0, V - 1, U - 1, V - 1, lift, z, "wall"));
  props.push(...runPropZ(lf, 0, 0, 0, V - 1, lift, z, "wall"), ...runPropZ(lf, U - 1, 0, U - 1, V - 1, lift, z, "wall"));
  if (chapel) {
    for (const du of [-vx(4), vx(4)]) {
      const [x, y] = lf.toWorld(mid + du, chapel.y0 - vx(3));
      addProp("bench", x, y, lift(x, y, z), au[0], au[1], av[0], av[1]);
    }
  }
}

/**
 * Allotment gardens: a hut, a berry bush or two and a fruit tree on every
 * plot, low fences along the paths (see landscape.allotmentSurface).
 */
function dressAllotments(sp, rng, z, lift, addProp, addTree, props) {
  const lf = new Frame(sp.rect, sp.front ?? "S");
  const PW = ALLOT.w + ALLOT.path;
  const PD = ALLOT.d + ALLOT.path;
  const [ox, oy] = lf.toWorld(0, 0);
  const [ux, uy] = lf.toWorld(1, 0);
  const [wx, wy] = lf.toWorld(0, 1);
  for (let pv = 0; pv + PD <= lf.V; pv += PD)
    for (let pu = 0; pu + PW <= lf.U; pu += PW) {
      const u0 = pu + ALLOT.path;
      const v0 = pv + ALLOT.path;
      if (rng.chance(0.08)) continue;
      // the hut in a front corner, its door to the plot
      const hu = rng.chance(0.5) ? u0 + 4 : u0 + ALLOT.w - 28;
      const [hx, hy] = lf.toWorld(hu, v0 + 3);
      if (rng.chance(0.85)) addProp("allotmentHut", hx, hy, lift(hx, hy, z), ux - ox, uy - oy, wx - ox, wy - oy);
      // a fruit tree and berry bushes at the back
      const [tx, ty] = lf.toWorld(u0 + rng.int(8, ALLOT.w - 8), v0 + ALLOT.d - vx(2));
      if (rng.chance(0.7)) addTree(tx, ty, lift(tx, ty, z), vx(rng.float(3.5, 6)), vx(rng.float(1.4, 2.2)), rng.pick(["blossom", "rowan", "birch"]));
      for (let k = 0; k < rng.int(1, 3); k += 1) {
        const [bx, by] = lf.toWorld(u0 + rng.int(6, ALLOT.w - 6), v0 + rng.int(Math.round(ALLOT.d * 0.5), ALLOT.d - 6));
        addTree(bx, by, lift(bx, by, z), vx(rng.float(0.7, 1.2)), vx(rng.float(0.6, 1)), "shrub");
      }
      // a low fence along the path in front of the plot
      props.push(...runPropZ(lf, u0, v0, u0 + ALLOT.w - 1, v0, lift, z, rng.chance(0.5) ? "picket" : "fence"));
    }
}

function Rngf(a, b, seed, lo, hi) {
  return lo + (hash32(seed, Math.round(a), Math.round(b), 77) / 4294967296) * (hi - lo);
}
