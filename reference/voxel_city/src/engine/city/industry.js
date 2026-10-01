import { Rng, hash32 } from "../core/hash.js";
import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";

/**
 * Heavy industry block programs: tank farms (storage tanks in bunded pads,
 * pipe racks, a process unit with distillation columns and a flare stack)
 * and container yards (stacked shipping containers in blocks with gantry
 * cranes and trucks). One deterministic layout per space is shared by the
 * ground surface (landscape) and the props (dressing).
 */

const cache = new WeakMap();

function layoutOf(space) {
  let L = cache.get(space);
  if (!L) {
    L = space.kind === "tankFarm" ? tankFarm(space) : space.kind === "containerYard" ? containerYard(space) : null;
    cache.set(space, L);
  }
  return L;
}

function tankFarm(space) {
  const rng = Rng.from(0x7a4f, space.id);
  const r = space.rect;
  const m = vx(6);
  const R = Math.round(vx(rng.float(8, 13)));
  const cell = 2 * R + vx(10);
  const nx = Math.max(1, Math.floor((r.x1 - r.x0 - 2 * m) / cell));
  const ny = Math.max(1, Math.floor((r.y1 - r.y0 - 2 * m) / cell));
  const x0 = Math.round((r.x0 + r.x1 - nx * cell) / 2);
  const y0 = Math.round((r.y0 + r.y1 - ny * cell) / 2);
  // the last column (if there are several) is the process unit
  const process = nx >= 3 ? { x0: x0 + (nx - 1) * cell, y0, x1: x0 + nx * cell - 1, y1: y0 + ny * cell - 1 } : null;
  const tanks = [];
  for (let j = 0; j < ny; j += 1)
    for (let i = 0; i < (process ? nx - 1 : nx); i += 1) {
      if (rng.chance(0.08)) continue;
      const cx = x0 + i * cell + Math.round(cell / 2);
      const cy = y0 + j * cell + Math.round(cell / 2);
      tanks.push({ x: cx, y: cy, r: R - (rng.chance(0.3) ? vx(2) : 0), h: Math.round(vx(rng.float(9, 15))), pad: Math.round(cell / 2) - vx(3) });
    }
  // pipe racks along the lanes between tank rows
  const racks = [];
  for (let j = 1; j < ny; j += 1) racks.push({ x0: x0 + vx(2), x1: x0 + nx * cell - vx(2), y: y0 + j * cell - vx(2) });
  const columns = [];
  let flare = null;
  let racksV = null;
  if (process) {
    const px = Math.round((process.x0 + process.x1) / 2);
    // distillation columns spread along the unit, in pairs across a central rack
    const len = process.y1 - process.y0;
    const n = Math.max(2, Math.min(8, Math.floor(len / vx(16))));
    const gap = (len - vx(20)) / Math.max(1, n - 1);
    for (let k = 0; k < n; k += 1) columns.push({ x: px + (k & 1 ? vx(6) : -vx(6)), y: Math.round(process.y0 + vx(8) + k * gap), r: Math.round(vx(rng.float(1.6, 2.8))), h: Math.round(vx(rng.float(22, 44))) });
    racksV = { x: px, y0: process.y0 + vx(4), y1: process.y1 - vx(14) };
    flare = { x: process.x1 - vx(4), y: process.y1 - vx(6), h: Math.round(vx(rng.float(40, 55))) };
  }
  return { kind: "tankFarm", cell, x0, y0, nx, ny, tanks, racks, racksV, process, columns, flare };
}

const CL = 98; // container length (12.2 m)
const CW = 20; // width (2.44 m)
const CH = 21; // height (2.6 m)

function containerYard(space) {
  const rng = Rng.from(0x5c0f, space.id);
  const r = space.rect;
  const m = vx(5);
  const lane = vx(10);
  const rows = 6;
  const blockW = rows * (CW + 2);
  const blocks = [];
  for (let y = r.y0 + m; y + blockW <= r.y1 - m; y += blockW + lane) {
    const stacks = [];
    for (let x = r.x0 + m; x + CL <= r.x1 - m; x += CL + 6)
      for (let k = 0; k < rows; k += 1) {
        const n = rng.chance(0.12) ? 0 : rng.int(1, 4);
        if (n) stacks.push({ x, y: y + k * (CW + 2), n, seed: rng.int(0, 1 << 30) });
      }
    const span = blockW + vx(4);
    const cranes = [];
    const len = r.x1 - m - (r.x0 + m);
    if (len > vx(40)) cranes.push({ x: r.x0 + m + Math.round(rng.float(0.2, 0.8) * (len - vx(8))), y: y - vx(2), span });
    blocks.push({ y0: y, y1: y + blockW - 1, stacks, cranes });
  }
  return { kind: "containerYard", blocks, lane };
}

/** Ground surface of a heavy-industry space at world column (x, y). Returns true when handled. */
export function industrySurface(space, x, y, out) {
  const L = layoutOf(space);
  if (!L) return false;
  const r = space.rect;
  const edge = Math.min(x - r.x0, r.x1 - x, y - r.y0, r.y1 - y);
  if (edge < vx(2)) {
    out.mat = MAT.GRAVEL;
    return true;
  }
  if (L.kind === "tankFarm") {
    for (const t of L.tanks) {
      const d = Math.hypot(x - t.x, y - t.y);
      if (d < t.r + vx(1)) {
        out.mat = MAT.CONCRETE;
        return true;
      }
      if (Math.abs(x - t.x) < t.pad && Math.abs(y - t.y) < t.pad) {
        out.mat = MAT.GRAVEL;
        return true;
      }
    }
    if (L.process && x >= L.process.x0 && x <= L.process.x1 && y >= L.process.y0 && y <= L.process.y1) {
      out.mat = ((x >> 4) + (y >> 4)) & 1 ? MAT.CONCRETE : MAT.CONCRETE_DARK;
      return true;
    }
    out.mat = MAT.ASPHALT_WORN;
    return true;
  }
  // container yard: concrete stacking blocks, asphalt lanes with markings
  for (const b of L.blocks) {
    if (y >= b.y0 - 2 && y <= b.y1 + 2) {
      out.mat = (y - b.y0) % (CW + 2) >= CW ? MAT.LINE_YELLOW : MAT.CONCRETE;
      return true;
    }
  }
  out.mat = ((x >> 3) & 7) === 0 && Math.abs(((y - r.y0) % (L.lane + 6 * (CW + 2))) - 3 * (CW + 2)) > 0 ? MAT.ASPHALT : MAT.ASPHALT_WORN;
  return true;
}

/**
 * Straight quay of a port yard that a big lake reaches into: the water side
 * (the axis direction towards the lake) beyond the line where half the yard's
 * cross-section is lake becomes a dredged basin; the land side stays level.
 * Returns { axis, sign, c, level } or null.
 */
export function portQuay(world, rect) {
  if (!world.shoreNear) return null;
  const cx = (rect.x0 + rect.x1) / 2;
  const cy = (rect.y0 + rect.y1) / 2;
  // a big lake's shore, or the sea at an island town's harbour
  const sh = world.shoreNear(cx, cy, vx(500));
  if (!sh) return null;
  const axis = Math.abs(sh.nx) > Math.abs(sh.ny) ? "x" : "y";
  const sign = -Math.sign(axis === "x" ? sh.nx : sh.ny) || 1;
  const a0 = axis === "x" ? rect.x0 : rect.y0;
  const a1 = axis === "x" ? rect.x1 : rect.y1;
  const b0 = axis === "x" ? rect.y0 : rect.x0;
  const b1 = axis === "x" ? rect.y1 : rect.x1;
  const step = vx(6);
  let c = null;
  // walk from the land side towards the water; the quay goes where the lake takes over
  for (let t = 0; t <= a1 - a0; t += step) {
    const a = sign > 0 ? a0 + t : a1 - t;
    let wet = 0;
    let n = 0;
    for (let b = b0; b <= b1; b += step) {
      n += 1;
      if (axis === "x" ? world.openWaterAt(a, b) : world.openWaterAt(b, a)) wet += 1;
    }
    if (wet * 2 >= n) {
      c = a;
      break;
    }
  }
  if (c === null) return null;
  // keep a working apron of at least 40 m on land; where the lake covers
  // the land side as well, the yard is reclaimed (filled) out to 60 m
  if ((sign > 0 ? c - a0 : a1 - c) < vx(40)) {
    if (a1 - a0 < vx(90)) return null;
    c = sign > 0 ? a0 + vx(60) : a1 - vx(60);
  }
  return { axis, sign, c: Math.round(c), level: sh.level };
}

/** Is (x, y) on the water side of a quay line? */
export function quaySide(q, x, y) {
  return q.sign * ((q.axis === "x" ? x : y) - q.c);
}

/**
 * Harbour front: ship-to-shore cranes along a port yard's quay with their
 * booms over the basin, and a cargo ship moored alongside each.
 */
export function dressPort(world, space, z, addProp) {
  const q = space.quay;
  if (!q) return;
  const r = space.rect;
  const [bx, by] = q.axis === "x" ? [q.sign, 0] : [0, q.sign];
  const ax = -by;
  const ay = bx;
  const b0 = q.axis === "x" ? r.y0 : r.x0;
  const b1 = q.axis === "x" ? r.y1 : r.x1;
  for (let b = b0 + vx(40); b <= b1 - vx(40); b += vx(95)) {
    const [x, y] = q.axis === "x" ? [q.c, b] : [b, q.c];
    addProp("stsCrane", x - bx * vx(2), y - by * vx(2), z, ax, ay, bx, by);
    addProp("cargoShip", x + bx * vx(14), y + by * vx(14), q.level + 1, ax, ay, bx, by, { seed: hash32(x, y, 5) });
  }
}

/** Props of a heavy-industry space via dressing's addProp(kind, x, y, z, ax, ay, bx, by, extra). */
export function dressIndustry(world, space, z, addProp) {
  const L = layoutOf(space);
  if (!L) return;
  // nothing stands in the water where a lake or river cuts into the yard
  const dry = (x, y, m = 4) => !(world.isWet && world.isWet(x, y, m)) && !(space.quay && quaySide(space.quay, x, y) > -vx(16));
  const add = addProp;
  addProp = (kind, x, y, ...rest) => (dry(x, y) ? add(kind, x, y, ...rest) : undefined);
  if (L.kind === "tankFarm") {
    for (const t of L.tanks) {
      addProp("storageTank", t.x, t.y, z, 1, 0, 0, 1, { r: t.r, h: t.h });
      addProp("bund", t.x, t.y, z, 1, 0, 0, 1, { hw: t.pad });
    }
    for (const k of L.racks) addProp("pipeRack", k.x0, k.y, z, 1, 0, 0, 1, { len: k.x1 - k.x0 });
    // the unit's rack runs along its length (a = +y, b = -x)
    if (L.racksV) addProp("pipeRack", L.racksV.x, L.racksV.y0, z, 0, 1, -1, 0, { len: L.racksV.y1 - L.racksV.y0 });
    for (const c of L.columns) addProp("processColumn", c.x, c.y, z, 1, 0, 0, 1, { r: c.r, h: c.h });
    if (L.flare) addProp("flareStack", L.flare.x, L.flare.y, z, 1, 0, 0, 1, { h: L.flare.h });
    return;
  }
  for (const b of L.blocks) {
    for (const s of b.stacks) addProp("containerStack", s.x, s.y, z, 1, 0, 0, 1, { n: s.n, seed: s.seed });
    for (const c of b.cranes) addProp("gantryCrane", c.x, c.y, z, 1, 0, 0, 1, { span: c.span });
  }
}

// ------------------------------------------------------------ prefabs

const B = (a0, a1, b0, b1, z0, z1, m) => ({ a0, a1, b0, b1, z0, z1, m });

/** A cylinder of radius r (voxels) as full-height slabs, one per row. */
function cylinder(out, r, z0, z1, m, ca = 0, cb = 0) {
  for (let b = -r; b <= r; b += 1) {
    const w = Math.floor(Math.sqrt(Math.max(0, r * r - b * b)));
    out.push(B(ca - w, ca + w, cb + b, cb + b, z0, z1, m));
  }
}

export const INDUSTRY_PROPS = {
  silo(rng, o) {
    const out = [];
    const r = o.r;
    const h = o.h;
    cylinder(out, r, 0, h, rng.chance(0.3) ? MAT.CONCRETE_LIGHT : MAT.SILO_STEEL);
    for (let k = 1; k <= Math.ceil(r * 0.6); k += 1) cylinder(out, Math.round(Math.sqrt(Math.max(0, r * r - (k * 1.6) ** 2))), h + k, h + k, MAT.SILO_STEEL);
    out.push(B(-1, 1, -r - 1, -r - 1, 0, h, MAT.LADDER));
    for (const z of [Math.round(h * 0.3), Math.round(h * 0.7)]) cylinder(out, r + 1, z, z, MAT.METAL_PANEL_DARK);
    return out;
  },
  stsCrane() {
    // ship-to-shore gantry: legs on the quay (b <= 0), boom far out over the water (+b)
    const out = [];
    const H = vx(32);
    const hw = vx(7);
    for (const a of [-hw, hw]) {
      out.push(B(a - 2, a + 2, -vx(16), -vx(16) + 3, 0, H, MAT.SIGNAL_RED));
      out.push(B(a - 2, a + 2, -3, 0, 0, H, MAT.SIGNAL_RED));
      out.push(B(a - 2, a + 2, -vx(16), 0, H - 3, H, MAT.SIGNAL_RED));
      out.push(B(a - 2, a + 2, -vx(16), 0, vx(10), vx(10) + 2, MAT.SIGNAL_RED));
    }
    out.push(B(-hw, hw, -vx(16), -vx(16) + 3, H - 3, H, MAT.SIGNAL_RED));
    out.push(B(-hw, hw, -3, 0, H - 3, H, MAT.SIGNAL_RED));
    // boom and back-reach, machinery house, trolley and spreader
    out.push(B(-vx(2), vx(2), -vx(30), vx(38), H + 1, H + 5, MAT.PANEL_WHITE));
    out.push(B(-vx(3), vx(3), -vx(24), -vx(14), H + 6, H + vx(3.5), MAT.PANEL_WHITE));
    out.push(B(-vx(1), vx(1), -vx(6), -vx(4), H + 6, H + vx(9), MAT.SIGNAL_RED));
    out.push(B(-vx(2), vx(2), vx(14), vx(17), H - 4, H, MAT.METAL_PANEL_DARK));
    out.push(B(-1, 0, vx(15), vx(15) + 1, vx(14), H - 5, MAT.METAL_BLACK));
    out.push(B(-vx(3), vx(3), vx(14), vx(17), vx(13), vx(14) - 1, MAT.HAZARD_YELLOW));
    out.push(B(-1, 1, -1, 1, H + vx(9), H + vx(9) + 2, MAT.SIGNAL_AMBER));
    return out;
  },
  cargoShip(rng, o) {
    // a container feeder alongside the quay: hull, stacked containers, bridge aft
    const r = new Rng(o.seed ?? 1);
    const len = vx(r.float(70, 110));
    const beam = vx(r.float(13, 17));
    const hl = len / 2;
    const hb = beam / 2;
    const hull = r.pick([MAT.CONTAINER_BLUE, MAT.PAINT_DARK, MAT.CONTAINER_RED, MAT.CONTAINER_GREEN]);
    const out = [];
    // bow tapers over the front 12 m
    for (let a = -hl; a <= hl; a += 4) {
      const t = Math.max(0, (a - (hl - vx(12))) / vx(12));
      const w = Math.round(hb * (1 - 0.8 * t * t));
      out.push(B(a, Math.min(hl, a + 3), -w, w, 0, vx(3.5), hull));
      out.push(B(a, Math.min(hl, a + 3), -w, w, vx(3.5) + 1, vx(3.5) + 2, MAT.PAINT_GRAY));
    }
    const colors = [MAT.CONTAINER_RED, MAT.CONTAINER_BLUE, MAT.CONTAINER_GREEN, MAT.CONTAINER_ORANGE, MAT.CORRUGATED_RUST];
    for (let a = -hl + vx(18); a + 98 < hl - vx(14); a += 102)
      for (let b = -hb + 4; b + 20 < hb - 3; b += 22) {
        const n = r.int(1, 4);
        for (let k = 0; k < n; k += 1) out.push(B(a, a + 97, b, b + 19, vx(3.5) + 3 + k * 21, vx(3.5) + 22 + k * 21, r.pick(colors)));
      }
    // bridge house at the stern
    out.push(B(-hl + vx(3), -hl + vx(12), -hb + 4, hb - 4, vx(3.5) + 3, vx(14), MAT.PANEL_WHITE));
    out.push(B(-hl + vx(3), -hl + vx(12), -hb + 4, hb - 4, vx(12), vx(13), MAT.GLASS_TINT));
    out.push(B(-hl + vx(6), -hl + vx(8), -2, 2, vx(14) + 1, vx(18), MAT.PAINT_DARK));
    return out;
  },
  garageRow(rng, o) {
    // lock-up garages: corrugated boxes side by side, doors facing +b
    const out = [];
    for (let k = 0; k < o.n; k += 1) {
      const a = k * 24;
      const door = rng.pick([MAT.DOOR_METAL, MAT.CORRUGATED_BLUE, MAT.CORRUGATED_RUST, MAT.DOOR_GREEN]);
      out.push(B(a + 3, a + 20, 47, 47, 1, 16, door));
      out.push(B(a, a + 23, 0, 47, 20, 20, MAT.ROOF_MEMBRANE));
      out.push(B(a, a + 23, 0, 47, 0, 19, rng.pick([MAT.CORRUGATED, MAT.CORRUGATED_RUST, MAT.CINDERBLOCK, MAT.CORRUGATED])));
    }
    return out;
  },
  roundBale() {
    // a round hay bale lying on its side (a cylinder along a)
    const out = [];
    const R = 5;
    for (let z = 0; z <= 2 * R; z += 1) {
      const w = Math.round(Math.sqrt(Math.max(0, R * R - (z - R) ** 2)));
      out.push(B(-5, 5, -w, w, z, z, MAT.HAY));
    }
    return out;
  },
  storageTank(rng, o) {
    const out = [];
    const r = o.r;
    const h = o.h;
    cylinder(out, r, 0, h, MAT.TANK_WHITE);
    // shallow cone roof, a band and a caged ladder with a top railing
    for (let k = 1; k <= 4; k += 1) cylinder(out, Math.round(r * (1 - k * 0.2)), h + k, h + k, MAT.METAL_PANEL);
    cylinder(out, r + 1, Math.round(h * 0.55), Math.round(h * 0.55) + 2, rng.chance(0.5) ? MAT.HAZARD_YELLOW : MAT.SIGN_BLUE);
    out.push(B(-2, 2, -r - 2, -r - 1, 0, h, MAT.LADDER));
    out.push(B(-3, 3, -r - 3, -r - 3, 6, h + 6, MAT.RAILING));
    return out;
  },
  bund(rng, o) {
    const w = o.hw;
    return [B(-w, w, -w, -w + 1, 0, 7, MAT.CONCRETE), B(-w, w, w - 1, w, 0, 7, MAT.CONCRETE), B(-w, -w + 1, -w, w, 0, 7, MAT.CONCRETE), B(w - 1, w, -w, w, 0, 7, MAT.CONCRETE)];
  },
  pipeRack(rng, o) {
    const out = [];
    const len = o.len;
    for (let a = 0; a <= len; a += vx(6)) {
      out.push(B(a, a + 1, -8, -7, 0, 52, MAT.STEEL_BEAM), B(a, a + 1, 7, 8, 0, 52, MAT.STEEL_BEAM));
      out.push(B(a, a + 1, -8, 8, 52, 53, MAT.STEEL_BEAM), B(a, a + 1, -8, 8, 36, 37, MAT.STEEL_BEAM));
    }
    for (const b of [-6, -2, 2, 6]) out.push(B(0, len, b - 1, b, 54, 55, b > 0 ? MAT.PIPE : MAT.STEEL_RUST));
    for (const b of [-5, 0, 5]) out.push(B(0, len, b - 1, b, 38, 39, MAT.PIPE));
    return out;
  },
  processColumn(rng, o) {
    const out = [];
    cylinder(out, o.r, 0, o.h, MAT.METAL_CHROME);
    for (let z = vx(6); z < o.h - 4; z += vx(6)) cylinder(out, o.r + 5, z, z, MAT.GRATE_STEEL);
    out.push(B(-1, 1, -o.r - 3, -o.r - 2, 0, o.h, MAT.LADDER));
    out.push(B(-1, 1, -1, 1, o.h + 1, o.h + 6, MAT.PIPE), B(-1, 1, -1, 1, o.h + 7, o.h + 7, MAT.SIGNAL_RED));
    return out;
  },
  flareStack(rng, o) {
    const h = o.h;
    const out = [B(-3, 3, -3, 3, 0, 4, MAT.CONCRETE), B(-1, 1, -1, 1, 5, h, MAT.STEEL_RUST)];
    for (const [da, db] of [[-10, -10], [10, -10], [-10, 10], [10, 10]]) for (let k = 0; k < 10; k += 1) out.push(B(Math.round(da * (1 - k / 10)), Math.round(da * (1 - k / 10)), Math.round(db * (1 - k / 10)), Math.round(db * (1 - k / 10)), Math.round((k * h) / 14), Math.round(((k + 1) * h) / 14), MAT.STEEL_BEAM));
    out.push(B(-2, 2, -2, 2, h + 1, h + 2, MAT.METAL_BLACK), B(-1, 1, -1, 1, h + 3, h + 7, MAT.LAMP_LIGHT), B(0, 0, 0, 0, h + 8, h + 10, MAT.SIGNAL_AMBER));
    return out;
  },
  containerStack(rng, o) {
    const r = new Rng(o.seed);
    const colors = [MAT.CONTAINER_RED, MAT.CONTAINER_BLUE, MAT.CONTAINER_GREEN, MAT.CONTAINER_ORANGE, MAT.CORRUGATED_RUST, MAT.CORRUGATED_BLUE];
    const out = [];
    for (let k = 0; k < o.n; k += 1) {
      const z = k * CH;
      const c = r.pick(colors);
      // props fill only air: details first, then the body
      out.push(B(0, 0, 0, CW - 1, z, z + CH - 1, MAT.METAL_BLACK), B(CL - 1, CL - 1, 0, CW - 1, z, z + CH - 1, MAT.METAL_BLACK));
      out.push(B(1, CL - 2, 0, CW - 1, z + CH - 1, z + CH - 1, MAT.METAL_PANEL));
      out.push(B(0, CL - 1, 0, CW - 1, z, z + CH - 1, c));
    }
    return out;
  },
  gantryCrane(rng, o) {
    const s = o.span;
    const h = vx(18);
    const out = [];
    for (const b of [0, s]) {
      out.push(B(0, 3, b, b + 3, 0, h, MAT.HAZARD_YELLOW), B(vx(8), vx(8) + 3, b, b + 3, 0, h, MAT.HAZARD_YELLOW));
      out.push(B(0, vx(8) + 3, b, b + 3, 0, 3, MAT.HAZARD_YELLOW), B(-2, 5, b - 1, b + 4, 0, 1, MAT.TIRE), B(vx(8) - 2, vx(8) + 5, b - 1, b + 4, 0, 1, MAT.TIRE));
    }
    out.push(B(0, 3, 0, s + 3, h, h + 5, MAT.HAZARD_YELLOW), B(vx(8), vx(8) + 3, 0, s + 3, h, h + 5, MAT.HAZARD_YELLOW));
    const tb = Math.round(s * 0.4);
    out.push(B(0, vx(8) + 3, tb, tb + 16, h - 6, h - 1, MAT.METAL_PANEL_DARK), B(2, 8, tb + 2, tb + 10, h - 12, h - 7, MAT.CAR_GLASS));
    out.push(B(vx(4), vx(4) + 1, tb + 6, tb + 7, h - 40, h - 7, MAT.METAL_BLACK), B(vx(4) - 8, vx(4) + 9, tb, tb + 16, h - 42, h - 41, MAT.HAZARD_BLACK));
    return out;
  },
};
