/**
 * Road levels: streets are built, not draped over the terrain.
 *
 * Every road gets a vertical profile along its centreline (samples every
 * 8 m of the natural ground, lightly smoothed), fitted to the ground within
 * a grade limit and pinned at its end points to the ground there, so all
 * roads ending at a node agree. The fit balances cut and fill: it is the
 * mean of the highest grade-limited profile that nowhere rises above the
 * ground (all cut) and the lowest that nowhere dips below it (all fill),
 * so a road over a bump or up a scarp runs half in a cutting, half on an
 * embankment instead of on a long ramp of fill. The grade limit is the
 * class's comfortable grade (GRADE), relaxed where the land itself is
 * steeper up to the class's steepest (STEEP): hill towns have steep streets.
 * A road is level across its whole width, sidewalks included. At a
 * junction the more important road keeps its own profile and the other one
 * meets it: both flatten to the dominant road's level over the junction
 * box and blend back to their own profiles over JUNCTION_BLEND.
 *
 * Everything that sits on a street asks here: the ground tile (road
 * columns, embankments), lots and block surfaces (cellPlan), street props
 * (dressing), subway entrances, sewer manholes and highway ramps. Profiles
 * are cached on the road objects (pure functions of the road and terrain).
 */

import { PITCHES } from "../core/placement.js";

const STEP = 64; // 8 m
const JUNCTION_BLEND = 96; // 12 m
const GRADE = { arterial: 0.08, collector: 0.1, local: 0.12, village: 0.13, alley: 0.14, lane: 0.16, pedestrian: 0.12, rural: 0.12, path: 0.15 };
const STEEP = { arterial: 0.1, collector: 0.13, local: 0.16, village: 0.16, alley: 0.17, lane: 0.18, pedestrian: 0.17, rural: 0.14, path: 0.18 };

/**
 * The angled world (ANGLED_WORLD_PLAN.md §4.1, S1): the same limits as
 * exact pitches of core/placement.js (8.01%, 10.03%, 12.55%, 14.36%,
 * 16.78%, 20.20%), so a road at its limit climbs at a table pitch: an
 * inclined road part (S4) of that grade is exact. Indices into PITCHES.
 */
const PITCH_GRADE = { arterial: 1, collector: 2, local: 3, village: 4, alley: 4, lane: 5, pedestrian: 3, rural: 3, path: 4 };
const PITCH_STEEP = { arterial: 2, collector: 3, local: 5, village: 5, alley: 5, lane: 6, pedestrian: 5, rural: 4, path: 5 };
const pitchGrade = (i) => PITCHES[i].s / PITCHES[i].c;

/** Comfortable and steepest grade of a road class: [g0, g1]. */
function gradeLimits(world, cls) {
  const a = world.config.world.angles;
  if (a?.enabled && a.features?.roads !== false) {
    const g0 = pitchGrade(PITCH_GRADE[cls] ?? 3);
    return [g0, Math.max(g0, pitchGrade(PITCH_STEEP[cls] ?? 5))];
  }
  const g0 = GRADE[cls] ?? 0.12;
  return [g0, Math.max(g0, STEEP[cls] ?? 0.16)];
}

/**
 * Profile of a road: { z (voxels per sample), L (arc length), n }. An end
 * that meets a through road (a T) is pinned to that road's own level there
 * (its profile fitted between plain node levels: no chains, no cycles), so
 * the junction blend only takes up what is left; other ends are pinned to
 * the node level.
 */
export function roadProfile(world, road) {
  if (road.prof) return road.prof;
  const pts = road.pts;
  const endLevel = (p) => {
    const t = throughAt(world, road, p);
    return t ? profileAt(fitProfile(world, t.road, null, null), t.s) : nodeLevel(world, p.x, p.y);
  };
  road.prof = fitProfile(world, road, endLevel(pts[0]), endLevel(pts[pts.length - 1]));
  return road.prof;
}

/** The road a node of `road` lies on without ending there (a T), with the arc position: { road, s } or null. */
function throughAt(world, road, p) {
  const c = world.cellAt(p.x, p.y);
  const view = world.roadView(c.i, c.j);
  let best = null;
  for (const s of view.near({ x0: p.x - 8, y0: p.y - 8, x1: p.x + 8, y1: p.y + 8 })) {
    if (s.road === road) continue;
    const t = Math.max(0, Math.min(s.len, (p.x - s.ax) * s.dx + (p.y - s.ay) * s.dy));
    const d = Math.hypot(p.x - (s.ax + s.dx * t), p.y - (s.ay + s.dy * t));
    if (d > 4) continue;
    const q = s.road.pts;
    const atEnd = Math.hypot(p.x - q[0].x, p.y - q[0].y) < 6 || Math.hypot(p.x - q[q.length - 1].x, p.y - q[q.length - 1].y) < 6;
    if (atEnd) return null; // a corner or a crossing of road ends: the node level
    if (!best || s.rank > best.rank || (s.rank === best.rank && String(s.road.id) < String(best.road.id))) best = { road: s.road, s: s.s0 + t, rank: s.rank };
  }
  return best;
}

/** A road's profile between end levels z0 and z1 (null: the node levels), cached per road for the plain case. */
function fitProfile(world, road, z0, z1) {
  const plain = z0 === null && z1 === null;
  if (plain && road.prof0) return road.prof0;
  const pts = road.pts;
  const acc = [0];
  for (let k = 1; k < pts.length; k += 1) acc.push(acc[k - 1] + Math.hypot(pts[k].x - pts[k - 1].x, pts[k].y - pts[k - 1].y));
  const L = acc[acc.length - 1];
  const n = Math.max(2, Math.ceil(L / STEP) + 1);
  const at = new Float64Array(n);
  const raw = new Float64Array(n);
  let k = 0;
  for (let i = 0; i < n; i += 1) {
    const s = i === n - 1 ? L : Math.min(L, i * STEP);
    at[i] = s;
    while (k < pts.length - 2 && acc[k + 1] < s) k += 1;
    const segL = acc[k + 1] - acc[k] || 1;
    const t = Math.max(0, Math.min(1, (s - acc[k]) / segL));
    const x = pts[k].x + (pts[k + 1].x - pts[k].x) * t;
    const y = pts[k].y + (pts[k + 1].y - pts[k].y) * t;
    raw[i] = world.terrain.sample(x, y).h;
  }
  // smooth out the small bumps (a ±16 m window), keep the node heights
  const T = new Float64Array(n);
  for (let i = 0; i < n; i += 1) {
    let sum = 0;
    let cnt = 0;
    for (let q = Math.max(0, i - 2); q <= Math.min(n - 1, i + 2); q += 1) {
      sum += raw[q];
      cnt += 1;
    }
    T[i] = sum / cnt;
  }
  // the end nodes: the ground round the node (the same for every road meeting there), or the through road's level
  T[0] = z0 ?? nodeLevel(world, pts[0].x, pts[0].y);
  T[n - 1] = z1 ?? nodeLevel(world, pts[pts.length - 1].x, pts[pts.length - 1].y);
  // the grade allowed on each step (g[i]: from sample i-1 to i): the
  // comfortable grade, up to the steepest where the ground is that steep
  const [g0, g1] = gradeLimits(world, road.cls);
  const g = new Float64Array(n);
  const slope = (i) => {
    const a = Math.max(0, i - 2);
    const b = Math.min(n - 1, i + 2);
    return b > a ? Math.abs(T[b] - T[a]) / (at[b] - at[a] || 1) : 0;
  };
  let reach = 0;
  for (let i = 1; i < n; i += 1) {
    g[i] = Math.max(g0, Math.min(g1, Math.max(slope(i - 1), slope(i))));
    reach += g[i] * (at[i] - at[i - 1]);
  }
  // the pinned ends must be reachable: a short road between very different nodes grades steeper
  const need = Math.abs(T[n - 1] - T[0]) * 1.02;
  if (need > reach && reach > 0) for (let i = 1; i < n; i += 1) g[i] *= need / reach;
  const d = (i) => g[i] * (at[i] - at[i - 1]);
  // all-cut (upper) and all-fill (lower) envelopes, then their mean
  const up = Float64Array.from(T);
  const lo = Float64Array.from(T);
  for (let i = 1; i < n; i += 1) {
    up[i] = Math.min(up[i], up[i - 1] + d(i));
    lo[i] = Math.max(lo[i], lo[i - 1] - d(i));
  }
  for (let i = n - 2; i >= 0; i -= 1) {
    up[i] = Math.min(up[i], up[i + 1] + d(i + 1));
    lo[i] = Math.max(lo[i], lo[i + 1] - d(i + 1));
  }
  const z = new Float64Array(n);
  for (let i = 0; i < n; i += 1) z[i] = (up[i] + lo[i]) / 2;
  // pin the ends: within the cones of reach from both end nodes
  const from0 = new Float64Array(n);
  for (let i = 1; i < n; i += 1) from0[i] = from0[i - 1] + d(i);
  const total = from0[n - 1];
  for (let i = 0; i < n; i += 1) {
    const a = from0[i];
    const b = total - a;
    const hi = Math.min(T[0] + a, T[n - 1] + b);
    const lw = Math.max(T[0] - a, T[n - 1] - b);
    z[i] = Math.max(lw, Math.min(hi, z[i]));
  }
  const prof = { z: Float32Array.from(z), L, n };
  if (tableProfiles(world)) quantizeProfile(prof, at);
  if (plain) road.prof0 = prof;
  return prof;
}

/** Steps steeper than this (grade) are the steep stretches a table grade takes over (tableProfiles). */
const STEEP_Q = 0.06;

/** Do road profiles keep to the pitch table (the angled world's pitched roads, S4)? */
function tableProfiles(world) {
  const a = world.config.world.angles;
  return !!(a?.enabled && a.features?.roads !== false && a.features?.ramps !== false);
}

/**
 * Pitch a profile to the grade table (ANGLED_WORLD_PLAN.md §4.1, S4): every
 * steep stretch (steps of one sign steeper than STEEP_Q) keeps its two end
 * levels and becomes one run at the gentlest table grade that climbs it
 * (8.01% ... 20.20%), with level landings of equal length either side, so a
 * pitched road part lies on it whole; gentle stretches stay as fitted. The
 * profile gets knots of its own (`ks`, `kz`).
 */
function quantizeProfile(prof, at) {
  const z = prof.z;
  const n = prof.n;
  const ks = [at[0]];
  const kz = [z[0]];
  const grade = (i) => (z[i] - z[i - 1]) / (at[i] - at[i - 1] || 1);
  let i = 1;
  while (i < n) {
    const g = grade(i);
    if (Math.abs(g) < STEEP_Q) {
      ks.push(at[i]);
      kz.push(z[i]);
      i += 1;
      continue;
    }
    let i1 = i;
    while (i1 + 1 < n && Math.abs(grade(i1 + 1)) >= STEEP_Q && Math.sign(grade(i1 + 1)) === Math.sign(g)) i1 += 1;
    const s0 = at[i - 1];
    const s1 = at[i1];
    const dz = z[i1] - z[i - 1];
    const need = Math.abs(dz) / (s1 - s0);
    let G = 0;
    for (let k = 1; k < PITCHES.length && !G; k += 1) if (PITCHES[k].s / PITCHES[k].c >= need) G = PITCHES[k].s / PITCHES[k].c;
    if (G) {
      const land = (s1 - s0 - Math.abs(dz) / G) / 2;
      ks.push(s0 + land, s1 - land);
      kz.push(z[i - 1], z[i1]);
    }
    ks.push(s1);
    kz.push(z[i1]);
    i = i1 + 1;
  }
  prof.ks = Float64Array.from(ks);
  prof.kz = Float64Array.from(kz);
}

/**
 * Level of a road node: the ground averaged over a disc of NODE_R round it
 * (a pure function of the node's position, so every road meeting there
 * agrees), so a junction on the lip of a scarp does not drag all its
 * roads up or down to the lip.
 */
const NODE_R = 160; // 20 m
function nodeLevel(world, x, y) {
  const t = world.terrain;
  let sum = t.sample(x, y).h * 2;
  let w = 2;
  for (let k = 0; k < 8; k += 1) {
    const a = (k / 8) * Math.PI * 2;
    for (const r of [NODE_R / 2, NODE_R]) {
      sum += t.sample(x + Math.cos(a) * r, y + Math.sin(a) * r).h;
      w += 1;
    }
  }
  return sum / w;
}

function profileAt(prof, s) {
  if (prof.ks) return knotAt(prof.ks, prof.kz, s);
  const f = Math.max(0, Math.min(prof.n - 1.000001, s / STEP));
  const i = Math.floor(f);
  const t = f - i;
  return prof.z[i] * (1 - t) + prof.z[Math.min(i + 1, prof.n - 1)] * t;
}

/** Level at arc s of a profile with knots of its own (ks increasing), held flat past its ends. */
function knotAt(ks, kz, s) {
  const n = ks.length;
  if (s <= ks[0]) return kz[0];
  if (s >= ks[n - 1]) return kz[n - 1];
  let lo = 0;
  let hi = n - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (ks[mid] <= s) lo = mid;
    else hi = mid;
  }
  const span = ks[hi] - ks[lo];
  return span > 0 ? kz[lo] + ((kz[hi] - kz[lo]) * (s - ks[lo])) / span : kz[hi];
}

/**
 * Level of a junction: the dominant road's profile where the two meet
 * (cached on the annotation). At a T the road that goes on through keeps
 * its level and the one ending there meets it (two streets ending on the
 * same street a few metres apart then meet it at almost the same level);
 * at a crossing or a corner the more important road does.
 */
function junctionZ(world, seg, j) {
  if (j.z !== undefined) return j.z;
  const o = j.other;
  const selfDominant = !o || (j.sEnds !== j.cEnds ? j.cEnds : seg.rank > o.rank || (seg.rank === o.rank && String(seg.road.id) <= String(o.road.id)));
  j.dom = selfDominant;
  j.z = selfDominant ? profileAt(roadProfile(world, seg.road), seg.s0 + j.s) : profileAt(roadProfile(world, o.road), o.s0 + j.tOther);
  return j.z;
}

/**
 * Half-length of a junction's level box along a road: the crossing road's
 * right-of-way for the road that meets it, only its carriageway for the
 * dominant road (so a steep street with side streets a few metres apart
 * still has room to climb between them).
 */
function boxHalf(world, e) {
  const j = e.j;
  if (j.dom === undefined) junctionZ(world, e.seg, j);
  return j.dom ? Math.max(8, j.hc) : j.hr;
}

/** Blend length of a junction (voxels): it lengthens with the offset the road needs there, so the extra grade stays small on hilly ground. */
function blendOf(world, e, prof) {
  const j = e.j;
  if (j.blend === undefined) {
    const jz = junctionZ(world, e.seg, j);
    const h = boxHalf(world, e);
    const d = Math.max(Math.abs(jz - profileAt(prof, e.at - h)), Math.abs(jz - profileAt(prof, e.at + h)));
    j.blend = Math.min(JUNCTION_BLEND * 8, Math.max(JUNCTION_BLEND, (d * 1.5) / 0.035));
  }
  return j.blend;
}

/** A road's segment as its owner cell's road view has it (cached on the asking segment). */
function ownSeg(world, seg) {
  if (seg._own) return seg._own;
  const [i, j] = seg.road.home;
  seg._own = world.roadView(i, j).segs.find((s) => s.road === seg.road && s.idx === seg.idx) ?? seg;
  return seg._own;
}

function angledLevels(world) {
  const a = world.config.world.angles;
  return !!(a?.enabled && a.features?.roads !== false);
}

/**
 * Road level near its junctions in the angled world, one stretch between
 * two neighbouring junctions at a time (so it is continuous however their
 * boxes overlap: lanes end on a steep street from both sides a few metres
 * apart, the dominant road's box its carriageway, the other's its
 * right-of-way). Between junctions p and q the road lies at p's level over
 * p's box and at q's over q's, and blends to its own profile between them
 * as elsewhere; where the gap between the boxes is too short for their
 * level difference, it eases from one level to the other instead, over
 * the length the class's steepest grade needs (a smoothstep, at most 1.5 x
 * its mean slope) or the boxes' overlap, whichever is longer, centred on
 * the gap and never past the junctions' centres. `rj` sorted by arc
 * (cached on the view's junction list of the road); `a` the arc along the
 * road, `base` the profile there.
 */
function junctionLevel(world, seg, rj, prof, a, base) {
  const list = rj._sorted ?? (rj._sorted = rj.slice().sort((p, q) => p.at - q.at));
  // (p the last junction at or before a, q the next)
  let lo = 0;
  let hi = list.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (list[mid].at <= a) lo = mid + 1;
    else hi = mid;
  }
  const p = lo > 0 ? list[lo - 1] : null;
  const q = lo < list.length ? list[lo] : null;
  const hp = p ? boxHalf(world, p) : 0;
  const hq = q ? boxHalf(world, q) : 0;
  const zp = p ? junctionZ(world, p.seg, p.j) : 0;
  const zq = q ? junctionZ(world, q.seg, q.j) : 0;
  const e0 = p ? p.at + hp : -Infinity;
  const e1 = q ? q.at - hq : Infinity;
  if (p && q) {
    const need = (1.5 * Math.abs(zq - zp)) / gradeLimits(world, seg.road.cls)[1];
    if (e1 - e0 < need) {
      const len = Math.max(need, e0 - e1);
      const c = Math.max(p.at, Math.min(q.at, (e0 + e1) / 2));
      const l0 = Math.max(p.at, c - len / 2);
      const l1 = Math.min(q.at, c + len / 2);
      if (a <= l0 || l1 <= l0) return zp;
      if (a >= l1) return zq;
      const u = (a - l0) / (l1 - l0);
      return zp + (zq - zp) * u * u * (3 - 2 * u);
    }
  }
  if (a <= e0) return zp;
  if (a >= e1) return zq;
  let bL = p ? blendOf(world, p, prof) : 0;
  let bR = q ? blendOf(world, q, prof) : 0;
  if (p && q && e1 - e0 < bL + bR) {
    const k = (e1 - e0) / (bL + bR);
    bL *= k;
    bR *= k;
  }
  let off = 0;
  if (p && bL > 0) {
    const u = (a - e0) / bL;
    if (u < 1) off += (zp - profileAt(prof, e0)) * (1 - u * u * (3 - 2 * u));
  }
  if (q && bR > 0) {
    const u = (e1 - a) / bR;
    if (u < 1) off += (zq - profileAt(prof, e1)) * (1 - u * u * (3 - 2 * u));
  }
  return base + off;
}

/**
 * Road surface level (voxels, carriageway top) at `along` on a segment.
 *
 * Over a junction box the road lies at the junction's level. Between
 * junctions the nearest one on either side pulls the road's own profile
 * towards its level, the pull fading out over its blend; where two blends
 * would overlap both shrink to share the gap, so the level is continuous
 * and independent of the order the junctions are found in. Junctions are
 * taken along the whole road (seg.rj, road view), not just this straight
 * piece, so a wobbly street's vertices do not cut a blend short. The
 * angled world takes them one stretch between neighbours at a time
 * (junctionLevel), continuous where junction boxes overlap.
 */
export function segmentLevel(world, seg, along) {
  // (the angled world: the road's own segment in its owner's view, which
  // sees every junction along it, so no view gives it another level)
  if (seg.road.home) seg = ownSeg(world, seg);
  const prof = roadProfile(world, seg.road);
  const a = seg.s0 + along;
  const base = profileAt(prof, a);
  const rj = seg.rj ?? seg.jn.map((j) => ({ j, seg, at: seg.s0 + j.s }));
  if (!rj.length) return base;
  // (the angled world: junction by junction, continuous however their boxes overlap)
  if (angledLevels(world)) return junctionLevel(world, seg, rj, prof, a, base);
  let inBox = null;
  let inBox2 = null;
  let left = null;
  let right = null;
  for (const e of rj) {
    const hr = boxHalf(world, e);
    const d = a - e.at;
    if (Math.abs(d) <= hr) {
      if (!inBox || Math.abs(d) < Math.abs(a - inBox.at)) [inBox, inBox2] = [e, inBox];
      else if (!inBox2 || Math.abs(d) < Math.abs(a - inBox2.at)) inBox2 = e;
    } else if (d > 0) {
      if (!left || e.at + hr > left.at + boxHalf(world, left)) left = e;
    } else if (!right || e.at - hr < right.at - boxHalf(world, right)) right = e;
  }
  if (inBox && inBox2 && Math.abs(inBox.at - inBox2.at) > 0.5) {
    // two junction boxes overlap (streets meeting a few metres apart): the
    // level eases from one junction's to the other's across the overlap
    const [p, q] = inBox.at < inBox2.at ? [inBox, inBox2] : [inBox2, inBox];
    const lo = q.at - boxHalf(world, q);
    const hi = p.at + boxHalf(world, p);
    const u = hi > lo ? Math.max(0, Math.min(1, (a - lo) / (hi - lo))) : 0.5;
    const zp = junctionZ(world, p.seg, p.j);
    return zp + (junctionZ(world, q.seg, q.j) - zp) * u * u * (3 - 2 * u);
  }
  if (inBox) return junctionZ(world, inBox.seg, inBox.j);
  let bL = left ? blendOf(world, left, prof) : 0;
  let bR = right ? blendOf(world, right, prof) : 0;
  const hL = left ? boxHalf(world, left) : 0;
  const hR = right ? boxHalf(world, right) : 0;
  if (left && right) {
    const gap = right.at - hR - (left.at + hL);
    if (gap < bL + bR) {
      const k = gap / (bL + bR);
      bL *= k;
      bR *= k;
    }
  }
  let off = 0;
  if (left && bL > 0) {
    const edge = left.at + hL;
    const u = (a - edge) / bL;
    if (u < 1) off += (junctionZ(world, left.seg, left.j) - profileAt(prof, edge)) * (1 - u * u * (3 - 2 * u));
  }
  if (right && bR > 0) {
    const edge = right.at - hR;
    const u = (edge - a) / bR;
    if (u < 1) off += (junctionZ(world, right.seg, right.j) - profileAt(prof, edge)) * (1 - u * u * (3 - 2 * u));
  }
  return base + off;
}

/**
 * Level of the nearest road at a point (voxels, carriageway top) and how the
 * point relates to it: { z, seg, along, dist (from the centre line), sidewalk }
 * or null when no road is within `reach` of its right-of-way.
 */
export function roadLevelAt(world, view, x, y, reach = 8) {
  const cands = view.near({ x0: x - reach, y0: y - reach, x1: x + reach, y1: y + reach });
  let best = null;
  let bestScore = Infinity;
  for (const s of cands) {
    const vx = x - s.ax;
    const vy = y - s.ay;
    const t = Math.max(0, Math.min(s.len, vx * s.dx + vy * s.dy));
    const d = Math.hypot(x - (s.ax + s.dx * t), y - (s.ay + s.dy * t));
    if (d > s.hr + reach) continue;
    const score = d - s.hc;
    if (score < bestScore) {
      bestScore = score;
      best = { seg: s, along: t, dist: d };
    }
  }
  if (!best) return null;
  best.z = segmentLevel(world, best.seg, best.along);
  best.sidewalk = best.seg.sidewalk > 0;
  return best;
}
