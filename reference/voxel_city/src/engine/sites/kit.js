import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { SpatialGrid } from "../core/geom2d.js";
import { edgeInfo } from "../city/cellNetwork.js";

/**
 * Site kit: surface structures shared by military / research sites. All
 * write boxes { x0..z1, m, mode } into lists (details unless noted):
 * fences with a gate, guard booth + barrier, watchtowers, radar masts,
 * radomes, dish arrays, fuel tanks, trucks, portal blocks (a concrete
 * bunker over a stair shaft) and mountain blast-door portals.
 */

export function box(list, x0, y0, z0, x1, y1, z1, m, mode = 0) {
  list.push({ x0: Math.min(x0, x1), y0: Math.min(y0, y1), z0, x1: Math.max(x0, x1), y1: Math.max(y0, y1), z1, m, mode });
}

/** Gate side facing the nearest road of the site's arterial cell, the gate gap and a driveway. */
export function planGate(world, site, driveHalf = vx(3.5)) {
  const r = site.rect;
  const cr = site.cellRect;
  const { i, j } = site.cell;
  const sides = [
    { side: "N", e: edgeInfo(world, 1, j, i), d: r.y0 - cr.y0 },
    { side: "S", e: edgeInfo(world, 1, j + 1, i), d: cr.y1 - r.y1 },
    { side: "W", e: edgeInfo(world, 0, i, j), d: r.x0 - cr.x0 },
    { side: "E", e: edgeInfo(world, 0, i + 1, j), d: cr.x1 - r.x1 },
  ].filter((s) => s.e.cls);
  sides.sort((a, b) => a.d - b.d);
  const gateSide = sides.length ? sides[0].side : "S";
  const gw = vx(8);
  const mid = gateSide === "N" || gateSide === "S" ? Math.round((r.x0 + r.x1) / 2) : Math.round((r.y0 + r.y1) / 2);
  let gate;
  let drive;
  if (gateSide === "N") {
    gate = { x0: mid - gw / 2, x1: mid + gw / 2, y0: r.y0, y1: r.y0 };
    drive = { x0: mid - driveHalf, x1: mid + driveHalf, y0: cr.y0, y1: r.y0 + vx(12) };
  } else if (gateSide === "S") {
    gate = { x0: mid - gw / 2, x1: mid + gw / 2, y0: r.y1, y1: r.y1 };
    drive = { x0: mid - driveHalf, x1: mid + driveHalf, y0: r.y1 - vx(12), y1: cr.y1 };
  } else if (gateSide === "W") {
    gate = { y0: mid - gw / 2, y1: mid + gw / 2, x0: r.x0, x1: r.x0 };
    drive = { y0: mid - driveHalf, y1: mid + driveHalf, x0: cr.x0, x1: r.x0 + vx(12) };
  } else {
    gate = { y0: mid - gw / 2, y1: mid + gw / 2, x0: r.x1, x1: r.x1 };
    drive = { y0: mid - driveHalf, y1: mid + driveHalf, x0: r.x1 - vx(12), x1: cr.x1 };
  }
  return { gateSide, gate, drive };
}

/** Perimeter fence (chain link, barbed top, posts) with a gap at the gate; `skip` sides ({ N: true }) stay open. */
export function fence(out, r, z, gateSide, gate, h = 22, skip = {}) {
  const run = (x0, y0, x1, y1) => {
    box(out, x0, y0, z + 1, x1, y1, z + h, MAT.CHAINLINK);
    box(out, x0, y0, z + h + 1, x1, y1, z + h + 2, MAT.STEEL_BEAM);
  };
  const splitAt = (a0, a1, g0, g1, mk) => {
    if (g0 > a0) mk(a0, g0 - 1);
    if (g1 < a1) mk(g1 + 1, a1);
  };
  if (gateSide === "N") splitAt(r.x0, r.x1, gate.x0, gate.x1, (a, b) => run(a, r.y0, b, r.y0));
  else if (!skip.N) run(r.x0, r.y0, r.x1, r.y0);
  if (gateSide === "S") splitAt(r.x0, r.x1, gate.x0, gate.x1, (a, b) => run(a, r.y1, b, r.y1));
  else if (!skip.S) run(r.x0, r.y1, r.x1, r.y1);
  if (gateSide === "W") splitAt(r.y0, r.y1, gate.y0, gate.y1, (a, b) => run(r.x0, a, r.x0, b));
  else if (!skip.W) run(r.x0, r.y0, r.x0, r.y1);
  if (gateSide === "E") splitAt(r.y0, r.y1, gate.y0, gate.y1, (a, b) => run(r.x1, a, r.x1, b));
  else if (!skip.E) run(r.x1, r.y0, r.x1, r.y1);
  const post = (x, y) => box(out, x, y, z + 1, x, y, z + h + 3, MAT.STEEL_BEAM);
  for (let x = r.x0; x <= r.x1; x += 24) {
    if (!skip.N) post(x, r.y0);
    if (!skip.S) post(x, r.y1);
  }
  for (let y = r.y0; y <= r.y1; y += 24) {
    if (!skip.W) post(r.x0, y);
    if (!skip.E) post(r.x1, y);
  }
}

/** Guard booth + barrier arm just inside the gate. */
export function gateBooth(out, gate, gateSide, z) {
  const gc = { x: Math.round((gate.x0 + gate.x1) / 2), y: Math.round((gate.y0 + gate.y1) / 2) };
  const inward = { N: [0, 1], S: [0, -1], W: [1, 0], E: [-1, 0] }[gateSide];
  const perp = [inward[1], inward[0]];
  const bx = gc.x + inward[0] * vx(6) + perp[0] * vx(7);
  const by = gc.y + inward[1] * vx(6) + perp[1] * vx(7);
  box(out, bx - 14, by - 14, z + 1, bx + 14, by + 14, z + 22, MAT.CONCRETE_LIGHT);
  box(out, bx - 12, by - 12, z + 2, bx + 12, by + 12, z + 20, 0);
  box(out, bx - 14, by - 14, z + 9, bx + 14, by + 14, z + 18, MAT.GLASS_TINT);
  box(out, bx - 12, by - 12, z + 9, bx + 12, by + 12, z + 18, 0);
  box(out, bx - 16, by - 16, z + 23, bx + 16, by + 16, z + 24, MAT.METAL_PANEL_DARK);
  box(out, bx - 4 * perp[0] - 3 * Math.abs(inward[0]), by - 4 * perp[1] - 3 * Math.abs(inward[1]), z + 2, bx - 4 * perp[0], by - 4 * perp[1], z + 17, 0);
  const ax0 = gc.x + inward[0] * vx(3) - perp[0] * vx(4);
  const ay0 = gc.y + inward[1] * vx(3) - perp[1] * vx(4);
  box(out, ax0, ay0, z + 1, ax0, ay0, z + 8, MAT.HAZARD_BLACK);
  box(out, ax0, ay0, z + 8, ax0 + perp[0] * vx(7), ay0 + perp[1] * vx(7), z + 8, MAT.HAZARD_YELLOW);
}

/** Watchtowers at the four corners of a rect. */
export function watchtowers(out, r, z) {
  for (const [cx, cy] of [
    [r.x0 + 6, r.y0 + 6],
    [r.x1 - 6, r.y0 + 6],
    [r.x0 + 6, r.y1 - 6],
    [r.x1 - 6, r.y1 - 6],
  ]) {
    for (const [dx, dy] of [[-8, -8], [8, -8], [-8, 8], [8, 8]]) box(out, cx + dx, cy + dy, z + 1, cx + dx, cy + dy, z + 64, MAT.STEEL_BEAM);
    box(out, cx - 12, cy - 12, z + 64, cx + 12, cy + 12, z + 65, MAT.WOOD_DARK);
    box(out, cx - 12, cy - 12, z + 66, cx + 12, cy + 12, z + 74, MAT.WOOD_DARK);
    box(out, cx - 11, cy - 11, z + 66, cx + 11, cy + 11, z + 74, 0);
    box(out, cx - 12, cy - 12, z + 70, cx + 12, cy + 12, z + 73, 0);
    box(out, cx - 14, cy - 14, z + 82, cx + 14, cy + 14, z + 83, MAT.CORRUGATED);
    for (const [dx, dy] of [[-12, -12], [12, -12], [-12, 12], [12, 12]]) box(out, cx + dx, cy + dy, z + 66, cx + dx, cy + dy, z + 81, MAT.STEEL_BEAM);
    for (let k = z + 2; k < z + 64; k += 3) box(out, cx - 2, cy - 9, k, cx + 2, cy - 9, k, MAT.STEEL_BEAM);
    box(out, cx - 2, cy - 9, z + 64, cx + 2, cy - 9, z + 65, 0);
  }
}

export function radarMast(out, x, y, z) {
  box(out, x - 1, y - 1, z + 1, x + 1, y + 1, z + 120, MAT.STEEL_BEAM);
  for (let k = 0; k < 12; k += 1) box(out, x - 14 + k, y - 14 + k * 2, z + 120 + k, x + 14 - k, y + 14 - k, z + 120 + k, MAT.METAL_PANEL);
  box(out, x, y, z + 132, x, y, z + 136, MAT.SIGNAL_RED);
}

/** A white radome (sphere on a drum). */
export function radome(out, x, y, z, R = 48) {
  box(out, x - R + 4, y - R + 4, z + 1, x + R - 4, y + R - 4, z + 24, MAT.CONCRETE_LIGHT);
  for (let dz = 0; dz <= R; dz += 1) {
    const rr = Math.round(Math.sqrt(Math.max(0, R * R - dz * dz)));
    box(out, x - rr, y - Math.round(rr * 0.4), z + 24 + dz, x + rr, y + Math.round(rr * 0.4), z + 24 + dz, MAT.PANEL_WHITE);
    box(out, x - Math.round(rr * 0.4), y - rr, z + 24 + dz, x + Math.round(rr * 0.4), y + rr, z + 24 + dz, MAT.PANEL_WHITE);
    box(out, x - Math.round(rr * 0.72), y - Math.round(rr * 0.72), z + 24 + dz, x + Math.round(rr * 0.72), y + Math.round(rr * 0.72), z + 24 + dz, MAT.PANEL_WHITE);
  }
  box(out, x, y, z + 24 + R + 1, x, y, z + 24 + R + 6, MAT.SIGNAL_RED);
}

/** A row of tilted satellite dishes on pedestals. */
export function dishArray(out, x0, y, z, n = 3, gap = 64) {
  for (let k = 0; k < n; k += 1) {
    const x = x0 + k * gap;
    box(out, x - 2, y - 2, z + 1, x + 2, y + 2, z + 30, MAT.CONCRETE_LIGHT);
    for (let t = 0; t < 10; t += 1) box(out, x - 22 + t * 2, y - 4 + t, z + 30 + t * 2, x + 22 - t * 2, y - 2 + t, z + 31 + t * 2, MAT.PANEL_WHITE);
    box(out, x, y + 6, z + 38, x, y + 8, z + 50, MAT.STEEL_BEAM);
  }
}

/** Three fuel tanks in a row starting at (x0, y). */
export function fuelTanks(out, x0, y, z, n = 3) {
  for (let k = 0; k < n; k += 1) {
    const tx = x0 + k * vx(9);
    for (let dz = 1; dz <= 40; dz += 1) {
      box(out, tx - 22, y - 10, z + dz, tx + 22, y + 10, z + dz, MAT.TANK_WHITE);
      box(out, tx - 10, y - 22, z + dz, tx + 10, y + 22, z + dz, MAT.TANK_WHITE);
      box(out, tx - 18, y - 18, z + dz, tx + 18, y + 18, z + dz, MAT.TANK_WHITE);
    }
    box(out, tx - 18, y - 3, z + 12, tx + 18, y + 3, z + 14, MAT.HAZARD_YELLOW);
  }
}

export function truck(out, x, y, z, rng) {
  const c = rng.pick([MAT.CONTAINER_GREEN, MAT.CAR_GREEN, MAT.CONTAINER_BLUE]);
  const b = (x0, y0, z0, x1, y1, z1, m) => out.push({ x0, y0, z0, x1, y1, z1, m, mode: 0 });
  b(x, y, z + 3, x + 18, y + 54, z + 8, c);
  b(x + 1, y, z + 9, x + 17, y + 14, z + 18, c);
  b(x + 2, y, z + 13, x + 16, y, z + 16, MAT.CAR_GLASS);
  b(x + 1, y + 16, z + 9, x + 17, y + 54, z + 22, MAT.FABRIC_GREEN);
  for (const yy of [y + 6, y + 36, y + 46]) {
    b(x - 1, yy, z, x + 1, yy + 5, z + 5, MAT.TIRE);
    b(x + 17, yy, z, x + 19, yy + 5, z + 5, MAT.TIRE);
  }
}

/**
 * Portal block: a thick concrete bunker over a stair shaft, with a blast
 * door opening on its south side (the floor is laid in the shells pass so
 * the shaft can open through it).
 */
export function portalBlock(lists, bk, z, accent = MAT.HAZARD_YELLOW) {
  const { shells, carves, details } = lists;
  box(shells, bk.x0, bk.y0, z + 1, bk.x1, bk.y1, z + 40, MAT.CONCRETE_DARK);
  box(carves, bk.x0 + 4, bk.y0 + 4, z + 2, bk.x1 - 4, bk.y1 - 4, z + 36, 0);
  for (let k = 0; k < 4; k += 1) box(details, bk.x0 + k * 2, bk.y0 + k * 2, z + 41 + k, bk.x1 - k * 2, bk.y1 - k * 2, z + 41 + k, MAT.CONCRETE);
  box(shells, bk.x0 + 4, bk.y0 + 4, z + 1, bk.x1 - 4, bk.y1 - 4, z + 1, MAT.FLOOR_EPOXY);
  const dmx = Math.round((bk.x0 + bk.x1) / 2);
  box(carves, dmx - 10, bk.y1 - 4, z + 2, dmx + 10, bk.y1, z + 26, 0);
  box(details, dmx - 12, bk.y1, z + 2, dmx - 11, bk.y1, z + 27, accent);
  box(details, dmx + 11, bk.y1, z + 2, dmx + 12, bk.y1, z + 27, accent);
  box(details, dmx - 12, bk.y1, z + 27, dmx + 12, bk.y1, z + 28, MAT.HAZARD_BLACK);
  box(details, dmx - 22, bk.y1 - 3, z + 2, dmx - 13, bk.y1 - 1, z + 26, MAT.METAL_PANEL_DARK);
  box(details, bk.x0 + 6, bk.y1, z + 30, bk.x0 + 30, bk.y1, z + 34, MAT.SIGNAGE_BLUE);
}

/**
 * Spatial grid + bounds of a site's boxes (shells, then carves, then
 * details; each box keeps its index so rasterization preserves that order)
 * and of its custom volumes ({ bb, rasterize(chunk) }, e.g. a rock cavern),
 * which rasterize before the boxes.
 */
export function finishStructure(lists) {
  const all = [...lists.shells, ...lists.carves, ...lists.details];
  const custom = lists.custom ?? [];
  const grid = new SpatialGrid(128);
  let bb = null;
  const grow = (q) => {
    bb = bb ? { x0: Math.min(bb.x0, q.x0), y0: Math.min(bb.y0, q.y0), z0: Math.min(bb.z0, q.z0), x1: Math.max(bb.x1, q.x1), y1: Math.max(bb.y1, q.y1), z1: Math.max(bb.z1, q.z1) } : { x0: q.x0, y0: q.y0, z0: q.z0, x1: q.x1, y1: q.y1, z1: q.z1 };
  };
  all.forEach((q, i) => {
    q.i = i;
    grid.insert(q, q);
    grow(q);
  });
  for (const c of custom) grow(c.bb);
  return { boxes: all, grid, bb, custom };
}
