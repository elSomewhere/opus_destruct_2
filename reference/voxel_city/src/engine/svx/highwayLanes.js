import { hash32 } from "../core/hash.js";
import { VOXEL_SIZE, vx } from "../core/units.js";
import { pointAt, offsetAt, rampZ } from "../network/highways.js";
import { idOf } from "./ids.js";

/**
 * The highways (network/highways.js) as lanes of structvox's RoadNetwork,
 * beside the streets of svx/roads.js and in its conventions (metres, z the
 * surface, right-hand traffic in structvox's frame, lane k counting from
 * the median outwards):
 *
 *   - deck lanes along each carriageway, in pieces straight within
 *     LATERAL_SLACK of the curve and PROFILE_SLACK of the deck's level;
 *     each carriageway cut into links where a ramp leaves or joins it;
 *   - a ramp is one lane, one way: an off-ramp from its deck end down to
 *     its landing on the arterial, an on-ramp from its landing up; an
 *     off-ramp leaves from the kerb lane (a right turn), an on-ramp joins
 *     the kerb lane;
 *   - at a lattice node where two highways meet the lanes run on; at a
 *     junction of three or four, lanes stop short of its plateau and turn
 *     across it (right from the kerb lane, left from the inner lane),
 *     signalled one phase per heading as the streets' junctions are; at a
 *     terminus (a route's last node) they turn back.
 *
 * `city` links the ramps to the streets (svx/roads.js): `out(rampKey)`
 * gives the lanes leaving an off-ramp's landing on its arterial (with
 * their headings). Every record a pure function of the world.
 */

const H = VOXEL_SIZE;
const m = (q) => H * (q - 0.5);
/** Speed limits (m/s): the deck, a ramp. */
const SPEED = 27.8;
const RAMP_SPEED = 13.9;
/** Most a lane piece's chord strays from the deck's curve (voxels) before it is cut in two, and from its level. */
const LATERAL_SLACK = 2;
const PROFILE_SLACK = 1;
/** The median's half-width and gap (voxels): lane 0's inner edge. */
const INNER = 3;
/** Signal phases (as the streets'): 17 s each, 14 green. */
const PHASE = 17;
const GREEN = 14;
const ONE_PHASE = 0.924;
const STRAIGHT = Math.cos(Math.PI / 6);

export function highwayLanes(world, city) {
  const hw = world.highways;
  const cfg = world.config.highways;
  const n = cfg.lanesPerSide;
  const laneW = vx(cfg.laneWidth);
  const byEdge = new Map();
  const index = new Map();

  /** The edge object by id (H<axis>_<a>_<b>). */
  const edgeById = (id) => {
    const [, axis, a, b] = /^H(\d)_(-?\d+)_(-?\d+)$/.exec(id).map(Number);
    return hw.edge(axis, a, b);
  };

  /** The pieces [s, s'] of arc interval [s0, s1] along a curve (offset `off`, level z(s)): straight within the slack. */
  const pieces = (e, s0, s1, off, z) => {
    const out = [];
    const refine = (a, b, depth) => {
      const pa = offsetAt(e, a, off);
      const pb = offsetAt(e, b, off);
      const za = z(a);
      const zb = z(b);
      let worst = 0;
      let worstZ = 0;
      for (let k = 1; k < 8; k += 1) {
        const t = k / 8;
        const p = offsetAt(e, a + (b - a) * t, off);
        const cx = pa.x + (pb.x - pa.x) * t;
        const cy = pa.y + (pb.y - pa.y) * t;
        worst = Math.max(worst, Math.hypot(p.x - cx, p.y - cy));
        worstZ = Math.max(worstZ, Math.abs(z(a + (b - a) * t) - (za + (zb - za) * t)));
      }
      if ((worst > LATERAL_SLACK || worstZ > PROFILE_SLACK) && depth < 8 && Math.abs(b - a) > 32) {
        refine(a, (a + b) / 2, depth + 1);
        refine((a + b) / 2, b, depth + 1);
      } else out.push([a, b]);
    };
    if (Math.abs(s1 - s0) >= 8) refine(s0, s1, 0);
    return out;
  };

  /** Lane end (metres) at arc s, offset off, level z. */
  const end = (e, s, off, z) => {
    const p = offsetAt(e, s, off);
    return [m(p.x), m(p.y), H * (Math.round(z) + 0.5)];
  };

  /**
   * An edge's lanes: per direction (0 along the deck's arc, traffic on its
   * right, side -1; 1 against it, side +1) links between the cuts (the
   * junction trims at its ends, the deck ends of the ramps on that side),
   * pieces, lanes; and its ramps' lanes.
   */
  const edgeLanes = (e) => {
    let rec = byEdge.get(e.id);
    if (rec) return rec;
    const deg = [e.nodes[0][2], e.nodes[1][2]];
    // (a junction of three or four: the lanes stop short of its plateau; elsewhere they run to the node)
    const trim = (d) => (d >= 3 ? Math.round(hw.hw * 1.4) : 0);
    const s0 = trim(deg[0]);
    const s1 = e.total - trim(deg[1]);
    const deckZ = (s) => pointAt(e, Math.max(0, Math.min(e.total, s))).z;
    const ramps = hw.ramps(e);
    const links = [[], []];
    const lanes = [];
    for (const dir of [0, 1]) {
      const side = dir === 0 ? -1 : 1;
      const cuts = [...new Set(ramps.filter((r) => r.side === side && r.sDeck > s0 + 8 && r.sDeck < s1 - 8).map((r) => r.sDeck))].sort((p, q) => p - q);
      const bounds = [s0, ...cuts, s1];
      const asc = [];
      for (let k = 0; k + 1 < bounds.length; k += 1) asc.push([bounds[k], bounds[k + 1]]);
      // (in its traffic's order)
      const order = dir === 0 ? asc : asc.slice().reverse();
      order.forEach(([a, b], li) => {
        const [from, to] = dir === 0 ? [a, b] : [b, a];
        links[dir].push({ li, from, to });
        for (let k = 0; k < n; k += 1) {
          const off = side * (INNER + (k + 0.5) * laneW);
          const ps = pieces(e, a, b, off, deckZ);
          const seq = dir === 0 ? ps : ps.slice().reverse().map(([p, q]) => [q, p]);
          seq.forEach(([sa, sb], pi) => {
            lanes.push({
              id: idOf("hw", e.id, dir, li, pi, k),
              a: end(e, sa, off, deckZ(sa)),
              b: end(e, sb, off, deckZ(sb)),
              width: laneW * H,
              speed: SPEED,
              key: { hw: e.id, dir, link: li, piece: pi, k, last: pi === seq.length - 1, sa, sb },
            });
          });
        }
      });
    }
    // ramps: one lane each, the way its traffic goes
    ramps.forEach((r, ri) => {
      const off = r.side * hw.rampMid;
      const offRamp = r.side < 0 ? r.sDeck < r.sGround : r.sDeck > r.sGround;
      const [from, to] = offRamp ? [r.sDeck, r.sGround] : [r.sGround, r.sDeck];
      const z = (s) => rampZ(e, r, s);
      const ps = pieces(e, Math.min(from, to), Math.max(from, to), off, z);
      const seq = from < to ? ps : ps.slice().reverse().map(([p, q]) => [q, p]);
      seq.forEach(([sa, sb], pi) => {
        lanes.push({
          id: idOf("hwramp", e.id, ri, pi),
          a: end(e, sa, off, z(sa)),
          b: end(e, sb, off, z(sb)),
          width: vx(3.75) * H,
          speed: RAMP_SPEED,
          key: { hw: e.id, ramp: ri, off: offRamp, piece: pi, last: pi === seq.length - 1, sDeck: r.sDeck, side: r.side, arterial: r.arterial },
        });
      });
    });
    for (const l of lanes) index.set(l.id, { lane: l, e });
    rec = { e, links, lanes, ramps };
    byEdge.set(e.id, rec);
    return rec;
  };

  const heading = (l) => {
    const dx = l.b[0] - l.a[0];
    const dy = l.b[1] - l.a[1];
    const d = Math.hypot(dx, dy) || 1;
    return [dx / d, dy / d];
  };
  const turnOf = (from, to) => {
    const [hx, hy] = heading(from);
    const [ox, oy] = heading(to);
    const dot = hx * ox + hy * oy;
    return dot > STRAIGHT ? 0 : hx * oy - hy * ox < 0 ? 1 : -1;
  };

  /** The lanes leaving lattice node (a, b) on its edges (first pieces), with their edge. */
  const leavingNode = (a, b) => {
    const out = [];
    for (const [ax, p, q] of hw.edgesAt(a, b)) {
      const e = hw.edge(ax, p, q);
      const rec = edgeLanes(e);
      const atStart = p === a && q === b;
      // (leaving the node: along the arc from its start, against it from its end)
      const dir = atStart ? 0 : 1;
      for (const l of rec.lanes) if (l.key.dir === dir && l.key.link === 0 && l.key.piece === 0 && l.key.ramp === undefined) out.push({ l, e });
    }
    return out;
  };

  /** The node (a, b) a deck lane's last link runs into, or null mid-edge. */
  const nodeAhead = (rec, l) => {
    const lastLink = rec.links[l.key.dir].length - 1;
    if (l.key.link !== lastLink) return null;
    return l.key.dir === 0 ? rec.e.nodes[1] : rec.e.nodes[0];
  };

  /** A junction's phases (edges by heading, as the streets'): { phase(edgeId), phases, offset }. */
  const junctionPhases = (a, b) => {
    const node = hw.node(a, b);
    const heads = [];
    const phase = new Map();
    const arms = hw
      .edgesAt(a, b)
      .map(([ax, p, q]) => hw.edge(ax, p, q))
      .sort((p, q) => (p.id < q.id ? -1 : 1));
    for (const e of arms) {
      const atStart = e.nodes[0][0] === a && e.nodes[0][1] === b;
      const p = pointAt(e, atStart ? 0 : e.total);
      let i = heads.findIndex((h) => Math.abs(h[0] * p.tx + h[1] * p.ty) >= ONE_PHASE);
      if (i < 0) i = heads.push([p.tx, p.ty]) - 1;
      phase.set(e.id, i);
    }
    return { phase, phases: heads.length, offset: (hash32(world.seed, Math.round(node.x), Math.round(node.y), 0x517) / 4294967296) * PHASE * heads.length };
  };

  return {
    /** Highway and ramp lanes through a box (city voxels), unsorted. */
    lanesIn(box, inBox) {
      if (!hw) return [];
      const out = [];
      for (const e of hw.edgesNear(box)) for (const l of edgeLanes(e).lanes) if (inBox(l)) out.push(l);
      return out;
    },

    has(id) {
      return index.has(id);
    },

    lane(id) {
      return index.get(id)?.lane ?? null;
    },

    /** The lanes of a ramp by (edge id, ramp index): its first and last pieces. */
    rampLanes(edgeId, ri) {
      const rec = edgeLanes(edgeById(edgeId));
      const all = rec.lanes.filter((l) => l.key.ramp === ri);
      return { first: all.find((l) => l.key.piece === 0), last: all.find((l) => l.key.last), off: all[0]?.key.off };
    },

    next(id) {
      const hit = index.get(id);
      if (!hit) return [];
      const { lane: l, e } = hit;
      const rec = edgeLanes(e);
      const k = l.key;
      if (k.ramp !== undefined) {
        const along = rec.lanes.filter((q) => q.key.ramp === k.ramp);
        if (!k.last) return [[along.find((q) => q.key.piece === k.piece + 1).id, 0]];
        if (k.off) {
          // down on the arterial: its lanes leaving the landing, either way
          return city
            .out(e.id, k.ramp)
            .map((q) => [q.id, turnOf(l, q)])
            .sort((p, q) => p[0] - q[0]);
        }
        // up on the deck: the kerb lane of the link that starts where it joins
        const dir = k.side < 0 ? 0 : 1;
        const link = rec.links[dir].find((q) => Math.abs(q.from - k.sDeck) < 1);
        const kerb = link && rec.lanes.find((q) => q.key.ramp === undefined && q.key.dir === dir && q.key.link === link.li && q.key.piece === 0 && q.key.k === n - 1);
        return kerb ? [[kerb.id, 0]] : [];
      }
      const lanesOf = (li) => rec.lanes.filter((q) => q.key.ramp === undefined && q.key.dir === k.dir && q.key.link === li);
      if (!k.last) return [[lanesOf(k.link).find((q) => q.key.piece === k.piece + 1 && q.key.k === k.k).id, 0]];
      const out = [];
      const node = nodeAhead(rec, l);
      if (!node) {
        // a cut: on along the next link, and (from the kerb lane) the off-ramp leaving here
        const nextLink = lanesOf(k.link + 1).find((q) => q.key.piece === 0 && q.key.k === k.k);
        if (nextLink) out.push([nextLink.id, 0]);
        const side = k.dir === 0 ? -1 : 1;
        if (k.k === n - 1)
          rec.ramps.forEach((r, ri) => {
            const offRamp = r.side < 0 ? r.sDeck < r.sGround : r.sDeck > r.sGround;
            if (r.side === side && offRamp && Math.abs(r.sDeck - k.sb) < 1) out.push([rec.lanes.find((q) => q.key.ramp === ri && q.key.piece === 0).id, 1]);
          });
        return out.sort((p, q) => p[0] - q[0]);
      }
      const [a, b, deg] = node;
      if (deg <= 1) {
        // a terminus: back the way it came, from the inner lane
        const back = rec.lanes.find((q) => q.key.ramp === undefined && q.key.dir !== k.dir && q.key.link === 0 && q.key.piece === 0 && q.key.k === 0);
        return back ? [[back.id, -1]] : [];
      }
      const ways = new Map();
      for (const { l: q, e: qe } of leavingNode(a, b)) {
        if (qe.id === e.id) continue;
        if (!ways.has(qe.id)) ways.set(qe.id, []);
        ways.get(qe.id).push(q);
      }
      if (deg === 2) for (const list of ways.values()) for (const q of list) if (q.key.k === k.k) out.push([q.id, 0]);
      if (deg >= 3) {
        const straight = [...ways.values()].some((list) => turnOf(l, list[0]) === 0);
        for (const list of ways.values()) {
          const turn = turnOf(l, list[0]);
          const byK = (kk) => list.find((q) => q.key.k === kk);
          if (turn === 0) out.push([byK(k.k).id, 0]);
          else if (turn === 1 && (k.k === n - 1 || !straight)) out.push([byK(n - 1).id, 1]);
          else if (turn === -1 && (k.k === 0 || !straight)) out.push([byK(0).id, -1]);
        }
      }
      return out.sort((p, q) => p[0] - q[0]);
    },

    /** The signal a deck lane meets at a junction of three or four highways, or null. */
    signal(id) {
      const hit = index.get(id);
      if (!hit || hit.lane.key.ramp !== undefined || !hit.lane.key.last) return null;
      const node = nodeAhead(edgeLanes(hit.e), hit.lane);
      if (!node || node[2] < 3) return null;
      const j = junctionPhases(node[0], node[1]);
      if (j.phases < 2) return null;
      const i = j.phase.get(hit.e.id);
      return { cycle: PHASE * j.phases, offset: j.offset, from: PHASE * i, to: PHASE * i + GREEN };
    },
  };
}
