import { MAT } from "../voxel/materials.js";
import { sminCircular } from "../core/math.js";
import { hash32 } from "../core/hash.js";

/**
 * Per-column road surface classification.
 *
 * Roads are signed distance fields around their centerlines: the carriageway
 * union uses a circular smooth-min between different roads, which produces
 * exact curb fillets (corner radii) at every junction, T or X, for straight or
 * curved roads alike. The right-of-way union uses a sharp min so property
 * lines stay square. Markings, crosswalks, stop lines, medians, parking lanes
 * and sidewalk patterns are then resolved in the frame of the dominant road.
 *
 * Output (written into `out`): kind, mat, dz (height above road grade),
 * plus the dominant segment and local coords for prop placement.
 */

export const KIND = {
  NONE: 0,
  CARRIAGE: 1,
  CURB: 2,
  SIDEWALK: 3,
  MEDIAN: 4,
  PLAZA: 5,
  SHOULDER: 6,
  STRIP: 7,
};

const perRoadD = new Float64Array(64);
const perRoadR = new Float64Array(64);
const perRoadCorner = new Float64Array(64);
const perRoadRef = new Array(64);

export function makeRoadSample(period = 0) {
  // period: a wrapping world's size in voxels (texture cells repeat with it), 0 = unbounded
  return { kind: 0, mat: 0, dz: 0, seg: null, along: 0, side: 0, q: 0, sdfC: 0, sdfR: 0, period };
}

/**
 * @param cands segments near this column (RoadView.near)
 * @param px,py column center (voxel coords, x+0.5)
 */
export function sampleRoadSurface(cands, px, py, out, seed = 0, reach = 0) {
  out.kind = KIND.NONE;
  out.mat = 0;
  out.dz = 0;
  out.seg = null;
  out.sdfR = Infinity;
  out.sdfC = Infinity;
  if (!cands.length) return out;

  let nRoads = 0;
  let best = null;
  let bestD = Infinity;
  let bestAlong = 0;
  let bestSide = 0;
  let pedestrian = false;

  for (let k = 0; k < cands.length; k += 1) {
    const s = cands[k];
    const vx = px - s.ax;
    const vy = py - s.ay;
    const t = vx * s.dx + vy * s.dy;
    const side = s.dx * vy - s.dy * vx;
    const tc = t < 0 ? 0 : t > s.len ? s.len : t;
    const ex = px - (s.ax + s.dx * tc);
    const ey = py - (s.ay + s.dy * tc);
    const dist = Math.sqrt(ex * ex + ey * ey);
    if (dist > s.hr + Math.max(s.road.corner, reach) + 1) continue;
    const dC = s.hc > 0 ? dist - s.hc : Infinity;
    const dR = dist - s.hr;
    // aggregate per road with plain min
    let slot = -1;
    for (let r = 0; r < nRoads; r += 1) {
      if (perRoadRef[r] === s.road) {
        slot = r;
        break;
      }
    }
    if (slot < 0) {
      slot = nRoads++;
      perRoadRef[slot] = s.road;
      perRoadD[slot] = dC;
      perRoadR[slot] = dR;
      perRoadCorner[slot] = s.road.corner;
    } else {
      if (dC < perRoadD[slot]) perRoadD[slot] = dC;
      if (dR < perRoadR[slot]) perRoadR[slot] = dR;
    }
    const score = s.hc > 0 ? dC : dR + 1000;
    if (score < bestD) {
      bestD = score;
      best = s;
      bestAlong = t;
      bestSide = side;
    }
    if (s.cls === "pedestrian" && dR < 0) pedestrian = true;
  }
  if (!nRoads) return out;

  let sdfC = Infinity;
  let sdfR = Infinity;
  // (the curb fillets fold road by road, each with its own corner radius, so
  // the fold's order matters: the angled world's roads, which carry the
  // cell that owns them, fold in id order, the same for any candidate list,
  // so neighbouring tiles agree on every column)
  // (and a fillet takes the smaller corner of the roads it joins: a lane
  // without a sidewalk never gets a big street's fillet bulging into a lot)
  const order = nRoads > 1 && perRoadRef[0].home ? sortedSlots(nRoads) : null;
  let kMin = Infinity;
  for (let q = 0; q < nRoads; q += 1) {
    const r = order ? order[q] : q;
    const d = perRoadD[r];
    if (d !== Infinity) {
      if (sdfC === Infinity) sdfC = d;
      else sdfC = sminCircular(sdfC, d, Math.min(order ? Math.min(kMin, perRoadCorner[r]) : perRoadCorner[r], 64));
      kMin = Math.min(kMin, perRoadCorner[r]);
    }
    if (perRoadR[r] < sdfR) sdfR = perRoadR[r];
  }
  for (let r = 0; r < nRoads; r += 1) perRoadRef[r] = null;
  out.sdfC = sdfC;
  out.sdfR = sdfR;
  out.seg = best;
  out.along = bestAlong;
  out.side = bestSide;

  if (sdfC < 0) {
    classifyCarriage(best, bestAlong, bestSide, px, py, out, seed);
    return out;
  }
  if (sdfR >= 0) return out;
  if (pedestrian && best.cls === "pedestrian") {
    out.kind = KIND.PLAZA;
    out.dz = 1;
    const a = Math.floor(best.s0 + bestAlong);
    const l = Math.floor(bestSide + 4096);
    if (best.road.paving === "cobble") out.mat = cobbleAt(px, py, out.period);
    else out.mat = (a & 15) === 0 || (l & 15) === 0 ? MAT.PLAZA_STONE_DARK : MAT.PLAZA_STONE;
    return out;
  }
  if (sdfC < 1 && best.sidewalk > 0) {
    out.kind = KIND.CURB;
    out.mat = MAT.CURB;
    out.dz = 1;
    // lowered curb with tactile paving where a crosswalk lands
    const { j, rel } = nearestJunction(best, bestAlong);
    if (j && j.cw && Math.abs(rel) >= j.hc + 6 && Math.abs(rel) < j.hc + 30) {
      out.mat = MAT.TACTILE;
      out.dz = 0;
    }
    return out;
  }
  if (best.sidewalk <= 0 && best.hc > 0 && sdfC < best.hr - best.hc + 1) {
    // alley / rural apron
    return out;
  }
  classifySidewalk(best, bestAlong, sdfC, out);
  return out;
}

const slotOrder = new Array(64);

/** Slots 0..n-1 by the id of their road. */
function sortedSlots(n) {
  for (let r = 0; r < n; r += 1) slotOrder[r] = r;
  const byId = (a, b) => (perRoadRef[a].id < perRoadRef[b].id ? -1 : perRoadRef[a].id > perRoadRef[b].id ? 1 : 0);
  // (insertion sort: a handful of roads)
  for (let i = 1; i < n; i += 1) {
    const v = slotOrder[i];
    let j = i - 1;
    while (j >= 0 && byId(slotOrder[j], v) > 0) {
      slotOrder[j + 1] = slotOrder[j];
      j -= 1;
    }
    slotOrder[j + 1] = v;
  }
  return slotOrder;
}

function nearestJunction(s, along) {
  let bestJ = null;
  let bestRel = Infinity;
  for (const j of s.jn) {
    const rel = along - j.s;
    if (Math.abs(rel) < Math.abs(bestRel)) {
      bestRel = rel;
      bestJ = j;
    }
  }
  return { j: bestJ, rel: bestRel };
}

/** Cobbles (setts of ~25 cm) in three shades; wrapping worlds hash canonical positions. */
function cobbleAt(px, py, period) {
  const hx = period ? (((Math.floor(px) % period) + period) % period) >> 1 : Math.floor(px) >> 1;
  const hy = period ? (((Math.floor(py) % period) + period) % period) >> 1 : Math.floor(py) >> 1;
  const k = hash32(hx, hy, 0xc0b) & 15;
  return k < 3 ? MAT.COBBLE_DARK : k < 5 ? MAT.COBBLE_LIGHT : MAT.COBBLE;
}

function classifyCarriage(s, along, side, px, py, out, seed) {
  out.kind = KIND.CARRIAGE;
  out.dz = 0;
  const d = Math.abs(side);
  // old-town streets and lanes: cobbles with a gutter of dark setts, no markings
  if (s.road.paving === "cobble") {
    out.mat = d > s.hc - 2 && s.hc > 12 ? MAT.COBBLE_DARK : cobbleAt(px, py, out.period);
    return;
  }
  const a = s.s0 + along;
  const { j, rel } = nearestJunction(s, along);
  const arel = Math.abs(rel);
  const inJunction = j && arel < j.hc + 2;
  let mat = MAT.ASPHALT;

  // wear / patches for texture variety (coarse hashed cells)
  const hx = Math.floor(px / 24);
  const hy = Math.floor(py / 24);
  const hn = out.period / 24;
  const hcell = hn ? hash32(((hx % hn) + hn) % hn, ((hy % hn) + hn) % hn, seed, 77) : hash32(hx, hy, seed, 77);
  if ((hcell & 31) === 0) mat = MAT.ASPHALT_PATCH;
  else if ((hcell & 31) < 4) mat = MAT.ASPHALT_WORN;

  if (s.cls === "alley") {
    out.mat = (hcell & 7) === 0 ? MAT.ASPHALT_WORN : MAT.CONCRETE_DARK;
    return;
  }

  // crosswalks & stop lines
  if (j && j.cw && !inJunction) {
    const c0 = j.hc + 6;
    const c1 = j.hc + 30;
    if (arel >= c0 && arel < c1) {
      const stripe = Math.floor(side + 2048) & 7;
      out.mat = stripe < 4 ? MAT.LINE_WHITE : mat;
      return;
    }
    if (arel >= c1 + 4 && arel < c1 + 7 && d < s.hc - s.parking && (rel < 0 ? side < 0 : side > 0) && s.lanes > 1) {
      out.mat = MAT.LINE_WHITE;
      return;
    }
    // lane arrows pointing at the junction, in each approach lane
    const t = arel - (c1 + 16);
    if (t >= 0 && t < 22 && (s.cls === "arterial" || s.cls === "collector") && (rel < 0 ? side < 0 : side > 0)) {
      const half = s.median / 2;
      const perSide = Math.max(1, s.lanes / 2);
      for (let k = 0; k < perSide; k += 1) {
        const lat = Math.abs(d - (half + s.lane * k + s.lane / 2));
        if (t < 7 ? lat <= t * 0.7 : lat < 1) {
          out.mat = MAT.LINE_WHITE;
          return;
        }
      }
    }
  }

  // raised median (arterials); painted (hatched) median near junctions
  if (s.median > 0 && d < s.median / 2) {
    const clear = j ? arel < j.hr + 10 : false;
    if (!clear) {
      out.kind = KIND.MEDIAN;
      out.dz = 1;
      out.mat = d >= s.median / 2 - 1 ? MAT.CURB : MAT.GRASS_LAWN;
      return;
    }
    if (!inJunction) {
      const edge = d >= s.median / 2 - 1;
      const hatch = ((Math.floor(a + side * (side < 0 ? -1 : 1)) % 12) + 12) % 12 < 2;
      out.mat = edge || hatch ? MAT.LINE_YELLOW : mat;
      return;
    }
  }

  if (inJunction) {
    out.mat = mat;
    return;
  }

  if (s.cls === "rural") {
    if (d >= s.hc - s.shoulder) {
      out.kind = KIND.SHOULDER;
      out.mat = MAT.GRAVEL;
      return;
    }
    if (d >= s.hc - s.shoulder - 1) {
      out.mat = MAT.LINE_WHITE;
      return;
    }
    if (d < 1 && ((Math.floor(a) % 96) + 96) % 96 < 32) {
      out.mat = MAT.LINE_YELLOW;
      return;
    }
    out.mat = mat;
    return;
  }

  const inner = s.hc - s.parking;
  // parking lane boundary + stall ticks
  if (s.parking > 0 && d >= inner - 1) {
    if (d < inner) {
      out.mat = MAT.LINE_WHITE;
      return;
    }
    const pa = ((Math.floor(a) % 48) + 48) % 48;
    if (pa === 0 && d < s.hc - 2) {
      out.mat = MAT.LINE_WHITE;
      return;
    }
    // occasional manhole / drain grate near curb
    if (d >= s.hc - 3 && ((Math.floor(a) % 160) + 160) % 160 < 4) {
      out.mat = MAT.DRAIN_GRATE;
      return;
    }
    out.mat = mat;
    return;
  }

  if (s.cls === "arterial") {
    const half = s.median / 2;
    if (half > 0 && d >= half && d < half + 1) {
      out.mat = MAT.LINE_YELLOW;
      return;
    }
    if (d >= s.hc - 2 && d < s.hc - 1) {
      out.mat = MAT.LINE_WHITE;
      return;
    }
    const perSide = Math.max(1, s.lanes / 2);
    for (let k = 1; k < perSide; k += 1) {
      const lx = half + s.lane * k;
      if (d >= lx - 0.5 && d < lx + 0.5 && ((Math.floor(a) % 96) + 96) % 96 < 24) {
        out.mat = MAT.LINE_WHITE;
        return;
      }
    }
  } else if (s.cls === "collector") {
    if (d >= 0.5 && d < 1.5) {
      out.mat = MAT.LINE_YELLOW;
      return;
    }
  } else if (s.cls === "local" || s.cls === "village") {
    if (side >= 0 && side < 1 && ((Math.floor(a) % 72) + 72) % 72 < 24) {
      out.mat = MAT.LINE_YELLOW;
      return;
    }
  }

  // manholes in lane centers
  const ma = ((Math.floor(a) % 240) + 240) % 240;
  const laneCenter = s.median / 2 + s.lane / 2;
  if (ma < 6 && Math.abs(d - laneCenter) < 3) {
    out.mat = MAT.MANHOLE;
    return;
  }
  out.mat = mat;
}

function classifySidewalk(s, along, sdfC, out) {
  out.kind = KIND.SIDEWALK;
  out.dz = 1;
  const q = sdfC; // distance from curb face
  out.q = q;
  const a = s.s0 + along;
  if (s.road.paving === "cobble") {
    // flagstone pavement of an old town
    const pa = ((Math.floor(a) % 8) + 8) % 8;
    out.mat = pa === 0 || Math.floor(q) % 7 === 0 ? MAT.SIDEWALK_JOINT : MAT.FLAGSTONE;
    return;
  }
  const strip = s.road.strip ?? "pits";
  const { j, rel } = nearestJunction(s, along);
  const nearCorner = j && Math.abs(rel) < j.hr + 4;
  if (s.sidewalk >= 24 && q >= 1 && q < 11 && !nearCorner) {
    if (strip === "grass") {
      out.kind = KIND.STRIP;
      out.mat = MAT.GRASS_LAWN;
      return;
    }
    if (strip === "pits") {
      const pa = ((Math.floor(a) % 72) + 72) % 72;
      if (pa >= 30 && pa < 40) {
        out.kind = KIND.STRIP;
        out.mat = MAT.TREE_PIT;
        return;
      }
    }
  }
  const qa = Math.floor(q);
  const pa = ((Math.floor(a) % 12) + 12) % 12;
  out.mat = pa === 0 || qa % 12 === 0 ? MAT.SIDEWALK_JOINT : MAT.SIDEWALK;
}
