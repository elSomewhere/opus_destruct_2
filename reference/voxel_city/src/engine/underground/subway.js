import { hashFloat } from "../core/hash.js";
import { LRU } from "../core/lru.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { roadSpecs } from "../network/roadClasses.js";

/**
 * Subway: lines run under selected arterial lines of the global grid,
 * stations sit under arterial intersections. Vertical lines (axis 0) run at
 * 14 m depth, horizontal lines (axis 1) at 22 m so crossings never collide;
 * where two lines cross, the deep station connects to the shallow station's
 * mezzanine through a long transfer stair and passage.
 *
 * Station anatomy (in a line-local frame: c across, l along the line):
 *   platform hall   96 m x 16 m, island platform, two tracks, tiled walls
 *   mezzanine       20 m x 10 m hall 7 m below the street
 *   stairs          platform -> mezzanine; mezzanine -> two sidewalk entrances
 * Everything is boxes (shell, carve, detail) in world coordinates, plus a
 * per-column tunnel rasterizer between stations.
 */

const HALF_LEN = 384; // 48 m
const HALL_W = 64; // half inner width (8 m)
const TUN_W = 36; // tunnel half inner width
const HEAD = 20; // stair headroom
const TRACK_TUN = 18; // track centre offset in the running tunnel
const TRACK_ST = 46; // track centre offset along the island platform
const SPREAD = 320; // the tracks spread over the last 40 m before a station

export class Subway {
  constructor(world) {
    this.world = world;
    this.cfg = world.config.subway;
    this.stations = new LRU(64);
    this.art = roadSpecs(world.config).arterial;
  }

  lineExists(axis, i) {
    return Math.abs(i + axis * 7) % this.cfg.lineEvery === 0;
  }

  spanActive(axis, i, j) {
    if (!this.lineExists(axis, i)) return false;
    const A = this.world.arterials;
    const fixed = A.line(axis, i);
    const a = A.line(1 - axis, j);
    const b = A.line(1 - axis, j + 1);
    const m = (a + b) / 2;
    const x = axis === 0 ? fixed : m;
    const y = axis === 0 ? m : fixed;
    return this.world.fields.urban(x, y).u >= this.cfg.minUrbanization;
  }

  hasStation(axis, i, j) {
    if (!(this.spanActive(axis, i, j - 1) || this.spanActive(axis, i, j))) return false;
    return !this.riverBlocked(axis, i, j);
  }

  /**
   * No station where a river crosses its footprint (the mezzanine and the
   * street entrances sit above the river bed); trains run straight through.
   */
  riverBlocked(axis, i, j) {
    const rivers = this.world.rivers;
    if (!rivers) return false;
    const n = this.nodeXY(axis, i, j);
    const e = HALF_LEN - 160;
    return rivers.hitsRect({ x0: n.x - e, y0: n.y - e, x1: n.x + e, y1: n.y + e }, 4);
  }

  nodeXY(axis, i, j) {
    const A = this.world.arterials;
    return axis === 0 ? { x: A.line(0, i), y: A.line(1, j) } : { x: A.line(0, j), y: A.line(1, i) };
  }

  streetZ(x, y) {
    return Math.round(this.world.streetLevel(x, y));
  }

  /** Platform surface z of the station of line (axis,i) at node j. */
  platformZ(axis, i, j) {
    const n = this.nodeXY(axis, i, j);
    const zs = this.streetZ(n.x, n.y);
    return zs - (axis === 0 ? 112 : 176);
  }

  /** Station structure (world boxes) cached by key. */
  station(axis, i, j) {
    const key = `${axis},${i},${j}`;
    let st = this.stations.get(key);
    if (st !== undefined) return st;
    st = this.hasStation(axis, i, j) ? this.buildStation(axis, i, j) : null;
    this.stations.set(key, st);
    return st;
  }

  buildStation(axis, i, j) {
    const n = this.nodeXY(axis, i, j);
    const zs = this.streetZ(n.x, n.y);
    const zp = this.platformZ(axis, i, j);
    const zm = zs - 56;
    const boxes = [];
    // local (c, l) -> world rect
    // shells (tile) only replace solid ground so they never refill carved space
    const W = (c0, c1, l0, l1, z0, z1, m, mode = m === MAT.TUNNEL_TILE ? 2 : 0) => {
      const r =
        axis === 0
          ? { x0: n.x + Math.min(c0, c1), x1: n.x + Math.max(c0, c1), y0: n.y + Math.min(l0, l1), y1: n.y + Math.max(l0, l1) }
          : { x0: n.x + Math.min(l0, l1), x1: n.x + Math.max(l0, l1), y0: n.y + Math.min(c0, c1), y1: n.y + Math.max(c0, c1) };
      boxes.push({ ...r, z0, z1, m, mode });
    };
    const ends = { lo: this.spanActive(axis, i, j - 1), hi: this.spanActive(axis, i, j) };
    // hall shell + carve
    W(-HALL_W - 4, HALL_W + 4, -HALF_LEN - 4, HALF_LEN + 4, zp - 12, zp + 48, MAT.TUNNEL_TILE);
    W(-HALL_W, HALL_W, -HALF_LEN, HALF_LEN, zp - 7, zp + 43, 0);
    // tunnel mouths at the hall ends (the tunnel arrives at full platform width)
    if (ends.lo) W(-HALL_W, HALL_W, -HALF_LEN - 4, -HALF_LEN - 1, zp - 7, zp + 36, 0);
    if (ends.hi) W(-HALL_W, HALL_W, HALF_LEN + 1, HALF_LEN + 4, zp - 7, zp + 36, 0);
    // accent band + ceiling
    W(-HALL_W, -HALL_W, -HALF_LEN, HALF_LEN, zp + 10, zp + 12, MAT.TUNNEL_TILE_ACCENT);
    W(HALL_W, HALL_W, -HALF_LEN, HALF_LEN, zp + 10, zp + 12, MAT.TUNNEL_TILE_ACCENT);
    // track beds
    for (const sgn of [-1, 1]) {
      const c0 = sgn < 0 ? -HALL_W : 28;
      const c1 = sgn < 0 ? -28 : HALL_W;
      W(c0, c1, -HALF_LEN, HALF_LEN, zp - 8, zp - 8, MAT.BALLAST);
      const tc = sgn * TRACK_ST;
      // sleepers in the same world phase as the tunnels' (every 5 voxels)
      const nl = axis === 0 ? n.y : n.x;
      const l0 = -HALF_LEN + ((((HALF_LEN - nl) % 5) + 5) % 5);
      for (let l = l0; l <= HALF_LEN; l += 5) W(tc - 9, tc + 9, l, l + 1, zp - 8, zp - 8, MAT.RAIL_TIE);
      W(tc - 6, tc - 6, -HALF_LEN, HALF_LEN, zp - 7, zp - 7, MAT.RAIL_STEEL);
      W(tc + 6, tc + 6, -HALF_LEN, HALF_LEN, zp - 7, zp - 7, MAT.RAIL_STEEL);
      // lights over the tracks
      for (let l = -HALF_LEN + 8; l < HALF_LEN; l += 24) W(tc - 1, tc + 1, l, l + 6, zp + 43, zp + 43, MAT.LIGHT_STRIP);
    }
    // island platform
    W(-27, 27, -HALF_LEN, HALF_LEN, zp - 7, zp, MAT.FLOOR_TERRAZZO);
    W(-27, -26, -HALF_LEN, HALF_LEN, zp, zp, MAT.PLATFORM_EDGE);
    W(26, 27, -HALF_LEN, HALF_LEN, zp, zp, MAT.PLATFORM_EDGE);
    for (let l = -HALF_LEN + 32; l < HALF_LEN - 16; l += 64) {
      for (const c of [-15, 14]) W(c, c + 1, l, l + 1, zp + 1, zp + 43, MAT.CONCRETE_LIGHT);
      W(-4, 3, l + 20, l + 22, zp + 4, zp + 4, MAT.WOOD_MED);
      W(-4, -4, l + 20, l + 20, zp + 1, zp + 3, MAT.METAL_BLACK);
      W(3, 3, l + 20, l + 20, zp + 1, zp + 3, MAT.METAL_BLACK);
      W(-2, 1, l + 40, l + 40, zp + 26, zp + 30, MAT.SIGN_BLUE);
    }
    for (let l = -HALF_LEN + 8; l < HALF_LEN; l += 24) W(-2, 2, l, l + 6, zp + 43, zp + 43, MAT.LIGHT_STRIP);

    const crossing = axis === 1 && this.hasStation(0, j, i);
    if (!crossing) {
      // mezzanine
      W(-44, 44, -84, 84, zm - 2, zm + 42, MAT.TUNNEL_TILE);
      W(-40, 40, -80, 80, zm + 1, zm + 38, 0);
      W(-40, 40, -80, 80, zm, zm, MAT.FLOOR_TERRAZZO);
      for (let l = -72; l <= 72; l += 16) W(-2, 2, l, l + 6, zm + 38, zm + 38, MAT.LIGHT_STRIP);
      // ticket gates line
      for (let c = -30; c <= 30; c += 8) W(c, c + 1, -24, -21, zm + 1, zm + 7, MAT.METAL_CHROME);
      W(24, 36, 30, 34, zm + 1, zm + 12, MAT.METAL_PANEL);
      W(24, 36, 30, 30, zm + 6, zm + 10, MAT.SCREEN);
      // platform -> mezzanine stair (in the platform centre, rising along +l)
      this.stairStraight(W, -10, 10, zp, zm, -(zm - zp), +1);
      // two street entrances on opposite sidewalks
      const hc = this.art.hc;
      for (const sgn of [-1, 1]) {
        const ca = sgn > 0 ? hc + 10 : -hc - 30;
        const cb = sgn > 0 ? hc + 30 : -hc - 10;
        const lp0 = sgn > 0 ? 60 : -80;
        const lp1 = sgn > 0 ? 80 : -60;
        // passage from the mezzanine to under the sidewalk
        const pc0 = sgn > 0 ? 40 : cb;
        const pc1 = sgn > 0 ? cb : -40;
        W(pc0 - 2, pc1 + 2, lp0 - 2, lp1 + 2, zm - 2, zm + 26, MAT.TUNNEL_TILE);
        W(pc0, pc1, lp0, lp1, zm + 1, zm + 24, 0);
        W(pc0, pc1, lp0, lp1, zm, zm, MAT.FLOOR_TERRAZZO);
        // stair up to the sidewalk, away from the intersection; it tops out on
        // the sidewalk's own level (the street may climb along the block)
        const lStart = sgn > 0 ? lp1 : lp0;
        let zTop = zs + 1;
        for (let it = 0; it < 3; it += 1) {
          const lEnd = lStart + sgn * 2 * (zTop - zm);
          const cm = (ca + cb) / 2;
          const px = axis === 0 ? n.x + cm : n.x + lEnd;
          const py = axis === 0 ? n.y + lEnd : n.y + cm;
          zTop = this.streetZ(px, py) + 1;
        }
        this.stairStraight(W, ca, cb, zm, zTop, lStart, sgn, true);
      }
    } else {
      // transfer: deep platform -> shallow station mezzanine
      const zs0 = zs;
      const zmT = zs0 - 56;
      const R = zmT - zp;
      this.stairStraight(W, -10, 10, zp, zmT, 80, +1);
      const lTop = 80 + 2 * R;
      W(-12, 12, 38, lTop + 2, zmT - 2, zmT + 26, MAT.TUNNEL_TILE);
      W(-10, 10, 40, lTop, zmT + 1, zmT + 24, 0);
      W(-10, 10, 40, lTop, zmT, zmT, MAT.FLOOR_TERRAZZO);
    }
    let bb = null;
    for (const b of boxes) bb = bb ? { x0: Math.min(bb.x0, b.x0), y0: Math.min(bb.y0, b.y0), z0: Math.min(bb.z0, b.z0), x1: Math.max(bb.x1, b.x1), y1: Math.max(bb.y1, b.y1), z1: Math.max(bb.z1, b.z1) } : { ...b };
    return { axis, i, j, x: n.x, y: n.y, zp, zs, boxes, bb };
  }

  /**
   * Straight stair (local frame) from level zLow to zHigh (walking surface
   * voxels), starting at l = l0 and rising towards `dir`. Enclosed in a tile
   * shell; `surface` adds railings where it breaks the ground.
   */
  stairStraight(W, c0, c1, zLow, zHigh, l0, dir, surface = false) {
    const R = zHigh - zLow;
    for (let k = 1; k <= R; k += 1) {
      const la = l0 + dir * 2 * (k - 1);
      const lb = la + dir;
      const l_lo = Math.min(la, lb);
      const l_hi = Math.max(la, lb);
      W(c0 - 2, c1 + 2, l_lo, l_hi, zLow + k - 3, surface ? Math.min(zLow + k + HEAD + 2, zHigh - 1) : zLow + k + HEAD + 2, MAT.TUNNEL_TILE);
      W(c0, c1, l_lo, l_hi, zLow + k - 2, zLow + k, MAT.STAIR_CONCRETE);
      W(c0, c1, l_lo, l_hi, zLow + k + 1, zLow + k + HEAD, 0);
      W(c0, c0, l_lo, l_hi, zLow + k + 7, zLow + k + 7, MAT.RAILING);
      if (surface && zLow + k + HEAD > zHigh - 1) {
        // where the stair breaks the sidewalk: green posts + top rail
        W(c0 - 2, c1 + 2, l_lo, l_hi, zHigh + 1, zHigh + HEAD + 4, 0);
        for (const cc of [c0 - 1, c1 + 1]) {
          W(cc, cc, l_lo, l_hi, zHigh, zHigh, MAT.GRANITE);
          W(cc, cc, l_lo, l_hi, zHigh + 8, zHigh + 8, MAT.POLE_GREEN);
          if ((l_lo & 3) === 0) W(cc, cc, l_lo, l_lo, zHigh + 1, zHigh + 7, MAT.POLE_GREEN);
        }
      }
    }
    if (surface) {
      const lEnd = l0 + dir * 2 * R;
      const le = Math.min(lEnd, lEnd + dir * 3);
      const lf = Math.max(lEnd, lEnd + dir * 3);
      W(c0, c1, le, lf, zHigh - 1, zHigh, MAT.SIDEWALK);
      // entrance sign
      W(c1 + 3, c1 + 3, lEnd, lEnd, zHigh + 1, zHigh + 22, MAT.POLE_METAL);
      W(c1 + 1, c1 + 5, lEnd, lEnd, zHigh + 18, zHigh + 22, MAT.SIGNAGE_BLUE);
      // back railing at the low end of the opening
      const lb = l0 + dir * 2 * Math.max(0, R - HEAD - 1);
      W(c0 - 1, c1 + 1, lb, lb, zHigh + 8, zHigh + 8, MAT.POLE_GREEN);
      for (let c = c0; c <= c1; c += 4) W(c, c, lb, lb, zHigh + 1, zHigh + 7, MAT.POLE_GREEN);
      // globe lamps on the entrance posts
      W(c0 - 1, c0 - 1, lEnd, lEnd, zHigh + 9, zHigh + 16, MAT.POLE_GREEN);
      W(c0 - 2, c0, lEnd - 1, lEnd + 1, zHigh + 17, zHigh + 19, MAT.LAMP_LIGHT);
    }
  }

  /** Stations whose bounds overlap a rect. */
  stationsNear(rect) {
    const A = this.world.arterials;
    const out = [];
    const pad = 420;
    const ix0 = A.indexAt(0, rect.x0 - pad);
    const ix1 = A.indexAt(0, rect.x1 + pad) + 1;
    const iy0 = A.indexAt(1, rect.y0 - pad);
    const iy1 = A.indexAt(1, rect.y1 + pad) + 1;
    for (let i = ix0; i <= ix1; i += 1) {
      if (!this.lineExists(0, i)) continue;
      for (let j = iy0; j <= iy1; j += 1) {
        const s = this.station(0, i, j);
        if (s && overlap(s.bb, rect)) out.push(s);
      }
    }
    for (let i = iy0; i <= iy1; i += 1) {
      if (!this.lineExists(1, i)) continue;
      for (let j = ix0; j <= ix1; j += 1) {
        const s = this.station(1, i, j);
        if (s && overlap(s.bb, rect)) out.push(s);
      }
    }
    return out;
  }

  /** Active tunnel spans near a rect: {axis, i, j, fixed, l0, l1, z0, z1} (track bed z). */
  tunnelsNear(rect) {
    const A = this.world.arterials;
    const out = [];
    for (const axis of [0, 1]) {
      const lo = axis === 0 ? rect.x0 : rect.y0;
      const hi = axis === 0 ? rect.x1 : rect.y1;
      const i0 = A.indexAt(axis, lo - 100);
      const i1 = A.indexAt(axis, hi + 100) + 1;
      const alo = axis === 0 ? rect.y0 : rect.x0;
      const ahi = axis === 0 ? rect.y1 : rect.x1;
      const j0 = A.indexAt(1 - axis, alo) - 1;
      const j1 = A.indexAt(1 - axis, ahi) + 1;
      for (let i = i0; i <= i1; i += 1) {
        if (!this.lineExists(axis, i)) continue;
        const fixed = A.line(axis, i);
        if (fixed + HALL_W + 4 < lo || fixed - HALL_W - 4 > hi) continue;
        for (let j = j0; j <= j1; j += 1) {
          if (!this.spanActive(axis, i, j)) continue;
          // tunnels start at the hall ends, or run on through a node without a station
          const s0 = !this.riverBlocked(axis, i, j);
          const s1 = !this.riverBlocked(axis, i, j + 1);
          const a = A.line(1 - axis, j) + (s0 ? HALF_LEN + 4 : 0);
          const b = A.line(1 - axis, j + 1) - (s1 ? HALF_LEN + 4 : 0);
          if (b < alo || a > ahi) continue;
          out.push({ axis, fixed, l0: a, l1: b, s0, s1, z0: this.platformZ(axis, i, j) - 8, z1: this.platformZ(axis, i, j + 1) - 8 });
        }
      }
    }
    return out;
  }

  /** Is (x,y) above an entrance / station opening (street props should avoid it)? */
  blocksSurface(x, y) {
    for (const s of this.stationsNear({ x0: x - 4, y0: y - 4, x1: x + 4, y1: y + 4 })) {
      const dx = x - s.x;
      const dy = y - s.y;
      const c = s.axis === 0 ? dx : dy;
      const l = s.axis === 0 ? dy : dx;
      if (Math.abs(c) > this.art.hc + 2 && Math.abs(c) < this.art.hc + 36 && Math.abs(l) > 50 && Math.abs(l) < 220) return true;
    }
    return false;
  }

  mapData(rect) {
    const A = this.world.arterials;
    const lines = [];
    for (const t of this.tunnelsNear(rect)) {
      lines.push({ pts: t.axis === 0 ? [[t.fixed, t.l0 - HALF_LEN], [t.fixed, t.l1 + HALF_LEN]] : [[t.l0 - HALF_LEN, t.fixed], [t.l1 + HALF_LEN, t.fixed]] });
    }
    const stations = this.stationsNear(rect).map((s) => ({ x: s.x, y: s.y, axis: s.axis }));
    void A;
    return { lines, stations };
  }
}

function overlap(a, b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

export const subwaySource = {
  id: "subway",
  order: 3,
  maxLod: 2,
  zRange(world, rect) {
    const sw = world.subway;
    let lo = Infinity;
    let hi = -Infinity;
    for (const s of sw.stationsNear(rect)) {
      lo = Math.min(lo, s.bb.z0);
      hi = Math.max(hi, s.bb.z1);
    }
    for (const t of sw.tunnelsNear(rect)) {
      lo = Math.min(lo, t.z0 - 6, t.z1 - 6);
      hi = Math.max(hi, t.z0 + 50, t.z1 + 50);
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk) {
    const sw = world.subway;
    const box = chunk.worldBox;
    for (const s of sw.stationsNear(box)) {
      if (s.bb.z1 < box.z0 || s.bb.z0 > box.z1) continue;
      for (const q of s.boxes) {
        if (q.x1 < box.x0 || q.x0 > box.x1 || q.y1 < box.y0 || q.y0 > box.y1 || q.z1 < box.z0 || q.z0 > box.z1) continue;
        chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode ?? 0);
      }
    }
    for (const t of sw.tunnelsNear(box)) rasterizeTunnel(chunk, t, box);
  },
};

/**
 * Running tunnel between two stations (or through a node without one): two
 * tracks at ±TRACK_TUN that spread to the platform tracks (±TRACK_ST) over
 * the last SPREAD voxels before a station, the tunnel widening with them,
 * so rails, sleepers and walls meet the station hall exactly.
 */
function rasterizeTunnel(chunk, t, box) {
  const d = chunk.data;
  const len = t.l1 - t.l0;
  const ease = (u) => (u <= 0 ? 0 : u >= 1 ? 1 : u * u * (3 - 2 * u));
  for (let j = 0; j < P; j += 1) {
    const y = chunk.wy(j);
    for (let i = 0; i < P; i += 1) {
      const x = chunk.wx(i);
      const c = (t.axis === 0 ? x : y) - t.fixed;
      const l = t.axis === 0 ? y : x;
      if (Math.abs(c) > HALL_W + 4 || l < t.l0 || l > t.l1) continue;
      let spread = 0;
      if (t.s0) spread = Math.max(spread, ease(1 - (l - t.l0) / SPREAD));
      if (t.s1) spread = Math.max(spread, ease(1 - (t.l1 - l) / SPREAD));
      const off = Math.round(TRACK_TUN + (TRACK_ST - TRACK_TUN) * spread);
      const tw = Math.max(TUN_W, off + 18);
      const ac = Math.abs(c);
      if (ac > tw + 4) continue;
      const zb = Math.round(t.z0 + ((t.z1 - t.z0) * (l - t.l0)) / (len || 1));
      const [k0, k1] = chunk.rangeZ(zb - 4, zb + 48);
      for (let k = k0; k <= k1; k += 1) {
        const z = chunk.wz(k);
        let m;
        if (ac > tw || z < zb || z > zb + 44) m = MAT.TUNNEL_WALL;
        else if (z === zb) {
          const tc = Math.abs(ac - off);
          m = tc <= 9 && ((l % 5) + 5) % 5 < 2 ? MAT.RAIL_TIE : MAT.BALLAST;
        } else if (z === zb + 1 && (ac === off - 6 || ac === off + 6)) m = MAT.RAIL_STEEL;
        else if (z === zb + 44 - 1 && ac < 2 && ((l % 80) + 80) % 80 < 8) m = MAT.LIGHT_STRIP;
        else if (ac === tw && z === zb + 12) m = MAT.PIPE;
        else m = 0;
        d[i + j * P + k * P2] = m;
      }
    }
  }
  void box;
}

export { hashFloat };
