import { vx } from "../core/units.js";
import { rw, rh, OPPOSITE } from "../core/rect.js";

/**
 * Block -> lots. All lots are axis-aligned inclusive voxel rects inside the
 * block's property rect. Each lot records which sides face streets (and of
 * what class), which side faces an alley, and its main frontage.
 */

const isStreet = (s) => s && s.cls && s.cls !== "alley";

function frontagesOf(block, rect) {
  const p = block.prop;
  const out = [];
  if (rect.y0 === p.y0 && isStreet(block.sides.N)) out.push({ side: "N", cls: block.sides.N.cls });
  if (rect.y1 === p.y1 && isStreet(block.sides.S)) out.push({ side: "S", cls: block.sides.S.cls });
  if (rect.x0 === p.x0 && isStreet(block.sides.W)) out.push({ side: "W", cls: block.sides.W.cls });
  if (rect.x1 === p.x1 && isStreet(block.sides.E)) out.push({ side: "E", cls: block.sides.E.cls });
  return out;
}

function alleyOf(block, rect) {
  const p = block.prop;
  for (const side of ["N", "S", "W", "E"]) {
    const s = block.sides[side];
    if (!s || s.cls !== "alley") continue;
    if (side === "N" && rect.y0 === p.y0) return side;
    if (side === "S" && rect.y1 === p.y1) return side;
    if (side === "W" && rect.x0 === p.x0) return side;
    if (side === "E" && rect.x1 === p.x1) return side;
  }
  return null;
}

/**
 * Split [a0, a1] (inclusive) into pieces drawn from [wMin, wMax] voxels.
 * The first and last pieces (corner lots) are widened by `cornerBoost`.
 */
function splitWidths(a0, a1, wMin, wMax, rng, cornerBoost = 1.35) {
  const len = a1 - a0 + 1;
  if (len < wMin * 2) return [[a0, a1]];
  const out = [];
  let a = a0;
  let rem = len;
  let k = 0;
  while (rem > 0) {
    let w = Math.round((rng.float(wMin, wMax) * (k === 0 ? cornerBoost : 1)) / 4) * 4;
    if (rem - w < wMin * cornerBoost) {
      w = rem <= wMax * cornerBoost * 1.2 ? rem : Math.round(rem / 2 / 4) * 4;
    }
    out.push([a, a + w - 1]);
    a += w;
    rem -= w;
    k += 1;
  }
  return out;
}

/** Main frontage of a lot: the highest-class street, the longer side preferred (null: none). */
export function mainFrontage(rect, frontages) {
  const rank = { arterial: 5, collector: 4, local: 3, village: 3, pedestrian: 2, rural: 1, lane: 0.5 };
  let front = null;
  let bestScore = -1;
  for (const f of frontages) {
    const len = f.side === "N" || f.side === "S" ? rw(rect) : rh(rect);
    const score = len * (1 + 0.15 * (rank[f.cls] ?? 0));
    if (score > bestScore) {
      bestScore = score;
      front = f.side;
    }
  }
  return front;
}

function makeLot(block, rect, rng, extra = {}) {
  const frontages = frontagesOf(block, rect);
  const alley = alleyOf(block, rect);
  const front = mainFrontage(rect, frontages);
  return {
    rect,
    block: block.id,
    cell: block.cell,
    district: block.district,
    frontages,
    front: front ?? (alley ? OPPOSITE[alley] : "S"),
    alley,
    corner: frontages.length >= 2,
    ...extra,
  };
}

/** Two back-to-back rows (or one row) of lots along the long axis. */
function rowLots(block, rng, wRange, opts = {}) {
  const p = block.prop;
  const W = rw(p);
  const H = rh(p);
  const longX = W >= H;
  const shortLen = longX ? H : W;
  const alleySide = ["N", "S", "W", "E"].find((s) => block.sides[s]?.cls === "alley");
  const lots = [];
  const [wMin, wMax] = [vx(wRange[0]), vx(wRange[1])];
  let rows;
  const twoRows = !alleySide && shortLen >= vx(opts.twoRowMin ?? 44);
  if (twoRows) {
    const mid = longX ? Math.round((p.y0 + p.y1) / 2) : Math.round((p.x0 + p.x1) / 2);
    rows = longX
      ? [{ ...p, y1: mid }, { ...p, y0: mid + 1 }]
      : [{ ...p, x1: mid }, { ...p, x0: mid + 1 }];
  } else {
    rows = [p];
  }
  for (const row of rows) {
    const pieces = longX
      ? splitWidths(row.x0, row.x1, wMin, wMax, rng)
      : splitWidths(row.y0, row.y1, wMin, wMax, rng);
    for (const [a0, a1] of pieces) {
      const rect = longX ? { x0: a0, x1: a1, y0: row.y0, y1: row.y1 } : { x0: row.x0, x1: row.x1, y0: a0, y1: a1 };
      lots.push(makeLot(block, rect, rng));
    }
  }
  return lots;
}

/** Row lots of a given frontage width range (m) on any block (wharf warehouses...). */
export function rowLotsOf(block, rng, width) {
  return rowLots(block, rng, width, { twoRowMin: 40 }).map((lot, k) => ({ ...lot, id: `${block.id}/l${k}` }));
}

function downtownLots(block, rng) {
  const p = block.prop;
  const W = rw(p);
  const H = rh(p);
  const longX = W >= H;
  const r = rng.next();
  if (r < 0.34 || Math.max(W, H) < vx(60)) return [makeLot(block, p, rng, { whole: true })];
  if (r < 0.78) {
    // halves across the long axis
    const mid = longX ? Math.round((p.x0 + p.x1) / 2 + rng.float(-0.12, 0.12) * W) : Math.round((p.y0 + p.y1) / 2 + rng.float(-0.12, 0.12) * H);
    const a = longX ? { ...p, x1: mid } : { ...p, y1: mid };
    const b = longX ? { ...p, x0: mid + 1 } : { ...p, y0: mid + 1 };
    return [makeLot(block, a, rng), makeLot(block, b, rng)];
  }
  const mx = Math.round((p.x0 + p.x1) / 2);
  const my = Math.round((p.y0 + p.y1) / 2);
  return [
    makeLot(block, { x0: p.x0, y0: p.y0, x1: mx, y1: my }, rng),
    makeLot(block, { x0: mx + 1, y0: p.y0, x1: p.x1, y1: my }, rng),
    makeLot(block, { x0: p.x0, y0: my + 1, x1: mx, y1: p.y1 }, rng),
    makeLot(block, { x0: mx + 1, y0: my + 1, x1: p.x1, y1: p.y1 }, rng),
  ];
}

function industrialLots(block, rng, wRange) {
  const p = block.prop;
  const W = rw(p);
  const H = rh(p);
  const longX = W >= H;
  const pieces = longX
    ? splitWidths(p.x0, p.x1, vx(wRange[0]), vx(wRange[1]), rng, 1)
    : splitWidths(p.y0, p.y1, vx(wRange[0]), vx(wRange[1]), rng, 1);
  const shortLen = longX ? H : W;
  const lots = [];
  for (const [a0, a1] of pieces) {
    if (shortLen > vx(150)) {
      const mid = longX ? Math.round((p.y0 + p.y1) / 2) : Math.round((p.x0 + p.x1) / 2);
      const halves = longX
        ? [{ x0: a0, x1: a1, y0: p.y0, y1: mid }, { x0: a0, x1: a1, y0: mid + 1, y1: p.y1 }]
        : [{ x0: p.x0, x1: mid, y0: a0, y1: a1 }, { x0: mid + 1, x1: p.x1, y0: a0, y1: a1 }];
      for (const r of halves) lots.push(makeLot(block, r, rng));
    } else {
      const rect = longX ? { x0: a0, x1: a1, y0: p.y0, y1: p.y1 } : { x0: p.x0, x1: p.x1, y0: a0, y1: a1 };
      lots.push(makeLot(block, rect, rng));
    }
  }
  return lots;
}

/**
 * Countryside: farms set back behind the (wobbly) rural road on the block's
 * road sides: a farmhouse plot with a barn plot beside it (the dressing adds
 * silos, bales and fences); the rest of the block stays fields and nature.
 */
function farmsteadLots(block, rng) {
  const p = block.prop;
  const out = [];
  const setback = vx(30);
  const margin = vx(60);
  const clash = (r) => out.some((l) => l.rect.x0 <= r.x1 + 16 && r.x0 <= l.rect.x1 + 16 && l.rect.y0 <= r.y1 + 16 && r.y0 <= l.rect.y1 + 16);
  const inside = (r) => r.x0 >= p.x0 && r.y0 >= p.y0 && r.x1 <= p.x1 && r.y1 <= p.y1;
  for (const side of ["N", "S", "W", "E"]) {
    if (!block.sides[side]?.cls) continue;
    const alongX = side === "N" || side === "S";
    const a0 = (alongX ? p.x0 : p.y0) + margin;
    const a1 = (alongX ? p.x1 : p.y1) - margin;
    for (let a = a0 + vx(rng.float(0, 160)); a < a1; a += vx(rng.float(260, 560))) {
      if (!rng.chance(0.62)) continue;
      const depth = vx(rng.float(34, 44));
      const hw = vx(rng.float(24, 30));
      const bw = vx(rng.float(24, 32));
      if (a + hw + vx(4) + bw > a1) break;
      const rectAt = (s0, w) => {
        if (side === "N") return { x0: s0, x1: s0 + w - 1, y0: p.y0 + setback, y1: p.y0 + setback + depth - 1 };
        if (side === "S") return { x0: s0, x1: s0 + w - 1, y0: p.y1 - setback - depth + 1, y1: p.y1 - setback };
        if (side === "W") return { x0: p.x0 + setback, x1: p.x0 + setback + depth - 1, y0: s0, y1: s0 + w - 1 };
        return { x0: p.x1 - setback - depth + 1, x1: p.x1 - setback, y0: s0, y1: s0 + w - 1 };
      };
      const house = rectAt(a, hw);
      const barn = rectAt(a + hw + vx(4), bw);
      if (!inside(house) || !inside(barn) || clash(house) || clash(barn)) continue;
      const farm = `f${out.length}`;
      out.push(makeLot(block, house, rng, { front: side, farmstead: true, farmRole: "house", farm }));
      out.push(makeLot(block, barn, rng, { front: side, farmstead: true, farmRole: "barn", farm }));
    }
  }
  return out;
}

/**
 * Village: house plots strung along the block's road sides, set back
 * enough to clear the gently bending main street; cellPlan thins them out
 * towards the village edge. The block's interior stays fields and meadow.
 */
function villageLots(block, rng) {
  const p = block.prop;
  const out = [];
  for (const side of ["N", "S", "W", "E"]) {
    if (!block.sides[side]?.cls) continue;
    // the village's own streets are straight; the country roads bend a little
    const setback = block.sides[side].id ? vx(1) : vx(7);
    const alongX = side === "N" || side === "S";
    const a0 = (alongX ? p.x0 : p.y0) + vx(8);
    const a1 = (alongX ? p.x1 : p.y1) - vx(8);
    for (let a = a0 + vx(rng.float(0, 6)); a < a1; ) {
      const width = vx(rng.float(14, 24));
      const depth = vx(rng.float(28, 40));
      if (a + width > a1) break;
      if (rng.chance(0.93)) {
        let rect;
        if (side === "N") rect = { x0: a, x1: a + width - 1, y0: p.y0 + setback, y1: p.y0 + setback + depth - 1 };
        else if (side === "S") rect = { x0: a, x1: a + width - 1, y0: p.y1 - setback - depth + 1, y1: p.y1 - setback };
        else if (side === "W") rect = { x0: p.x0 + setback, x1: p.x0 + setback + depth - 1, y0: a, y1: a + width - 1 };
        else rect = { x0: p.x1 - setback - depth + 1, x1: p.x1 - setback, y0: a, y1: a + width - 1 };
        const clash = out.some((l) => l.rect.x0 <= rect.x1 + 8 && rect.x0 <= l.rect.x1 + 8 && l.rect.y0 <= rect.y1 + 8 && rect.y0 <= l.rect.y1 + 8);
        if (!clash && rect.x0 >= p.x0 && rect.y0 >= p.y0 && rect.x1 <= p.x1 && rect.y1 <= p.y1) out.push(makeLot(block, rect, rng, { front: side, village: true }));
      }
      a += width + vx(rng.float(1, 5));
    }
  }
  return out;
}

/**
 * Superblock of freestanding slabs (microdistrict, projects): rows of long
 * panel slabs parallel to the block's long side with generous courtyards
 * between them, now and then a point tower instead of a slab; entrances of
 * neighbouring rows face each other across the courtyard. Each lot is the
 * building's footprint (plus a thin apron); the rest of the block is the
 * shared courtyard (cellPlan adds it as an open space).
 */
export function microLots(block, rng) {
  const p = block.prop;
  const m = vx(9);
  const inner = { x0: p.x0 + m, y0: p.y0 + m, x1: p.x1 - m, y1: p.y1 - m };
  const W = inner.x1 - inner.x0;
  const H = inner.y1 - inner.y0;
  if (W < vx(40) || H < vx(30)) return [];
  const alongX = W >= H;
  const L = alongX ? W : H;
  const D = alongX ? H : W;
  const slabD = vx(15);
  const out = [];
  let row = 0;
  for (let b = rng.int(0, vx(6)); b + slabD <= D; b += slabD + vx(rng.float(24, 36))) {
    let a = rng.int(0, vx(10));
    while (a < L) {
      const tower = rng.chance(0.2) && D - b >= vx(26);
      const len = tower ? vx(26) : vx(rng.float(48, 110));
      const dep = tower ? vx(26) : slabD;
      if (a + len > L) {
        if (L - a >= vx(40) && !tower) {
          const r2 = alongX ? { x0: inner.x0 + a, x1: inner.x1, y0: inner.y0 + b, y1: inner.y0 + b + dep - 1 } : { x0: inner.x0 + b, x1: inner.x0 + b + dep - 1, y0: inner.y0 + a, y1: inner.y1 };
          out.push(makeLot(block, r2, rng, { front: alongX ? (row & 1 ? "N" : "S") : row & 1 ? "W" : "E", micro: true, arch: "panelSlab" }));
        }
        break;
      }
      const rect = alongX
        ? { x0: inner.x0 + a, x1: inner.x0 + a + len - 1, y0: inner.y0 + b, y1: inner.y0 + b + dep - 1 }
        : { x0: inner.x0 + b, x1: inner.x0 + b + dep - 1, y0: inner.y0 + a, y1: inner.y0 + a + len - 1 };
      if (rect.x1 <= inner.x1 && rect.y1 <= inner.y1) {
        const front = alongX ? (row & 1 ? "N" : "S") : row & 1 ? "W" : "E";
        out.push(makeLot(block, rect, rng, { front, micro: true, arch: tower ? "panelTower" : "panelSlab" }));
      }
      a += len + vx(rng.float(14, 26));
    }
    row += 1;
  }
  return out;
}

/**
 * A freestanding lot anywhere inside a block (a cabin in the woods), facing
 * `front`; no street frontage.
 */
export function freeLot(block, rect, front, rng, extra = {}) {
  return { ...makeLot(block, rect, rng, extra), front };
}

/** The whole block as one lot (schools and other civic buildings). */
export function wholeBlockLot(block, rng) {
  return { ...makeLot(block, block.prop, rng, { whole: true }), id: `${block.id}/l0` };
}

export function planBlockLots(block, district, rng) {
  const mode = district.lots.mode;
  let lots;
  switch (mode) {
    case "downtown":
      lots = downtownLots(block, rng);
      break;
    case "perimeter":
      lots = rowLots(block, rng, district.lots.width, { twoRowMin: 40 });
      break;
    case "suburban":
      lots = rowLots(block, rng, district.lots.width, { twoRowMin: 50 });
      break;
    case "industrial":
      lots = industrialLots(block, rng, district.lots.width);
      break;
    case "rural":
      lots = farmsteadLots(block, rng);
      break;
    case "village":
      lots = villageLots(block, rng);
      break;
    default:
      lots = [];
  }
  return lots.map((lot, k) => ({ ...lot, id: `${block.id}/l${k}` }));
}
