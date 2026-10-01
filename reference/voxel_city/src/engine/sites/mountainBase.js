import { SITES } from "../world/sites.js";
import { DISTRICTS } from "../world/registry.js";
import { Rng, hashFloat } from "../core/hash.js";
import { SimplexNoise } from "../core/noise.js";
import { vx } from "../core/units.js";
import { MAT, IS_SOLID } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { planBuildingEnvelopeAs } from "../buildings/archetypes.js";
import { stairDims } from "../buildings/interior/stairs.js";
import { planComplex, emitComplex } from "./complex.js";
import { box, fence, gateBooth, fuelTanks, truck, portalBlock, finishStructure } from "./kit.js";

/**
 * Mountain stronghold: a mountain hollowed out into a base.
 *
 * On a mountain flank a concrete portal with blast doors stands on an apron
 * cut into the slope (fenced, with a helipad). A vehicle tunnel runs level
 * into the mountain to a vast vaulted cavern (shotcrete walls, steel arch
 * ribs, hanging floodlights) holding a small town of real buildings (office
 * blocks and a hangar with full interiors via the archetype system), trucks,
 * fuel tanks and a portal block over the stairs down to a two-sector complex
 * (military / lab / power themes) whose sectors a tram joins.
 *
 * Placement is custom (`place`): the site needs high mountain ground, a
 * flank that drops off within ~600 m in one of the four axis directions,
 * and enough rock above the cavern and along the tunnel.
 */

DISTRICTS.register({
  id: "stronghold",
  label: "Mountain stronghold",
  color: "#7a6f64",
  streets: { pattern: "none", block: [[9999, 9999], [9999, 9999]], pedestrianChance: 0, mergeChance: 0, localClass: "local" },
  blockUse: [["rural", 1]],
  lots: { mode: "none", width: [0, 0], alleyChance: 0 },
  archetypes: [],
  floors: [3, 5],
  styles: [["concrete", 2], ["industrial", 1]],
});

const TUNNEL_W = vx(9);
const TUNNEL_H = vx(7.5);
const APRON_D = vx(44);
const APRON_W = vx(56);
const COVER = vx(40);

// ------------------------------------------------------------ frame

const DIRS = {
  // u: unit vector from the cavern centre toward the portal; v: lateral
  E: { u: [1, 0], v: [0, 1] },
  W: { u: [-1, 0], v: [0, 1] },
  S: { u: [0, 1], v: [1, 0] },
  N: { u: [0, -1], v: [1, 0] },
};

/** Local (u, v) frame of a site: centre + portal side. */
function frame(cx, cy, side) {
  const d = DIRS[side];
  const toWorld = (u, v) => [Math.round(cx + d.u[0] * u + d.v[0] * v), Math.round(cy + d.u[1] * u + d.v[1] * v)];
  const rect = (u0, v0, u1, v1) => {
    const [ax, ay] = toWorld(u0, v0);
    const [bx, by] = toWorld(u1, v1);
    return { x0: Math.min(ax, bx), y0: Math.min(ay, by), x1: Math.max(ax, bx), y1: Math.max(ay, by) };
  };
  const sideOf = (dx, dy) => (dx > 0 ? "E" : dx < 0 ? "W" : dy > 0 ? "S" : "N");
  // world side a lot faces when its front looks toward -v / +v (sign -1 / +1), or toward +u (the portal)
  const facing = (sign) => sideOf(d.v[0] * sign, d.v[1] * sign);
  const facingU = (sign) => sideOf(d.u[0] * sign, d.u[1] * sign);
  return { toWorld, rect, facing, facingU, side };
}

// ------------------------------------------------------------ cavern shape

function makeCavern(seed, cx, cy, A, B, H, zf) {
  return { cx, cy, A, B, H, Hw: vx(12), zf, noise: new SimplexNoise(seed ^ 0x6a7) };
}

/** Normalized radius of a column (< 1 inside the cavern), with rough walls. */
function cavernR(cav, x, y) {
  const nx = (x - cav.cx) / cav.A;
  const ny = (y - cav.cy) / cav.B;
  const r = Math.sqrt(Math.sqrt(nx * nx * nx * nx + ny * ny * ny * ny));
  return r * (1 + 0.05 * cav.noise.n2(x / 90, y / 90) + 0.02 * cav.noise.n2(x / 23, y / 23));
}

/** Ceiling (voxel z of the last air voxel) of a column at normalized radius r. */
function cavernCeil(cav, x, y, r) {
  const t = Math.max(0, (r - 0.3) / 0.7);
  const vault = Math.sqrt(Math.max(0, 1 - t * t));
  const rough = vx(1.5) * cav.noise.n2(x / 40 + 7.1, y / 40 - 3.3);
  return Math.round(cav.zf + cav.Hw + (cav.H - cav.Hw) * vault + rough);
}

/** Custom volume: carve the vault, lay the floor, line the walls, hang ribs and lights. */
function cavernVolume(cav, side) {
  const pad = vx(4);
  const bb = { x0: cav.cx - cav.A * 1.1 - pad, y0: cav.cy - cav.B * 1.1 - pad, z0: cav.zf - 1, x1: cav.cx + cav.A * 1.1 + pad, y1: cav.cy + cav.B * 1.1 + pad, z1: cav.zf + cav.H + vx(3) };
  const along = side === "E" || side === "W" ? 0 : 1;
  return {
    bb,
    rasterize(chunk) {
      if (chunk.lod > 2) return;
      const data = chunk.data;
      for (let j = 0; j < P; j += 1)
        for (let i = 0; i < P; i += 1) {
          const x = chunk.wx(i);
          const y = chunk.wy(j);
          const r = cavernR(cav, x, y);
          if (r > 1.04) continue;
          const col = i + j * P;
          if (r >= 1) {
            // shotcrete skirt on the lower walls
            for (let k = 0; k < P; k += 1) {
              const z = chunk.wz(k);
              if (z < cav.zf || z > cav.zf + vx(5)) continue;
              if (IS_SOLID[data[col + k * P2]]) data[col + k * P2] = MAT.CONCRETE;
            }
            continue;
          }
          const ceil = cavernCeil(cav, x, y, r);
          const a = along === 0 ? x - cav.cx : y - cav.cy;
          const b = along === 0 ? y - cav.cy : x - cav.cx;
          const rib = (((a % 96) + 96) % 96) < 3;
          const la = (((a + 48) % 96) + 96) % 96;
          const lb = ((b % 72) + 72) % 72;
          const light = r < 0.85 && la >= 44 && la < 52 && lb < 3;
          const hanger = light && la === 48 && lb === 1;
          for (let k = 0; k < P; k += 1) {
            const z = chunk.wz(k);
            const idx = col + k * P2;
            if (z === cav.zf || (z < cav.zf && z > cav.zf - chunk.s)) {
              data[idx] = MAT.FLOOR_CONCRETE;
              continue;
            }
            if (z < cav.zf || z > ceil + chunk.s) continue;
            if (z > ceil) {
              // the rock right above the vault
              if (IS_SOLID[data[idx]]) data[idx] = rib ? MAT.STEEL_BEAM : MAT.ROCK;
              continue;
            }
            let m = 0;
            if (rib && (z >= ceil - 2 || r > 0.975)) m = MAT.STEEL_BEAM;
            else if (light && z >= ceil - 8 && z <= ceil - 7) m = MAT.LIGHT_STRIP;
            else if (hanger && z > ceil - 7) m = MAT.STEEL_BEAM;
            data[idx] = m;
          }
        }
    },
  };
}

// ------------------------------------------------------------ placement

/**
 * Find a flank for the portal: walk out from the centre along the four
 * axis directions until the ground is low enough that a cavern floored at
 * that level keeps COVER of rock over its vault and the tunnel stays under
 * rock; prefer the shortest tunnel.
 */
function place(world, cand) {
  const { rect, seed } = cand;
  const cx = Math.round((rect.x0 + rect.x1) / 2);
  const cy = Math.round((rect.y0 + rect.y1) / 2);
  const T = world.terrain;
  const tc = T.sample(cx, cy);
  if (tc.mountain < 0.35 || tc.u > 0.01) return null;
  const L = vx(70 + 20 * hashFloat(seed, 1));
  const Wd = vx(55 + 15 * hashFloat(seed, 2));
  const H = vx(48 + 20 * hashFloat(seed, 3));
  const sea = world.config.world.seaLevel * 8;
  const cands = [];
  for (const side of ["N", "E", "S", "W"]) {
    const d = DIRS[side];
    const A = d.u[0] ? L : Wd;
    const B = d.u[0] ? Wd : L;
    for (let dist = L + vx(60); dist <= L + vx(620); dist += vx(20)) {
      const px = cx + d.u[0] * dist;
      const py = cy + d.u[1] * dist;
      // the apron is levelled at the ground height of its centre: cut into
      // the slope behind (the portal stands in the cut face), fill in front
      const hp = T.sample(px + (d.u[0] * APRON_D) / 2, py + (d.u[1] * APRON_D) / 2).h;
      if (hp < sea + vx(10)) break;
      const zf = Math.round(hp);
      // rock over the vault
      let ok = true;
      for (const [fx, fy] of [[0, 0], [0.5, 0], [-0.5, 0], [0, 0.5], [0, -0.5], [0.8, 0.8], [-0.8, 0.8], [0.8, -0.8], [-0.8, -0.8], [0.95, 0], [-0.95, 0], [0, 0.95], [0, -0.95]]) {
        const rr = Math.max(Math.abs(fx), Math.abs(fy));
        const need = rr < 0.3 ? H : vx(12) + (H - vx(12)) * Math.sqrt(Math.max(0, 1 - ((rr - 0.3) / 0.7) ** 2));
        if (T.sample(cx + fx * A, cy + fy * B).h < zf + need + COVER) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      // rock over the tunnel (the last stretch before the portal is cut and cover)
      for (let t = L; t < dist - vx(45) && ok; t += vx(25)) if (T.sample(cx + d.u[0] * t, cy + d.u[1] * t).h < zf + TUNNEL_H + vx(10)) ok = false;
      if (!ok) continue;
      // the apron: ground falls away (or stays level) beyond the portal
      const out = T.sample(px + d.u[0] * APRON_D, py + d.u[1] * APRON_D).h;
      if (out > zf + vx(6)) continue;
      cands.push({ side, dist, zf, A, B });
      break;
    }
  }
  // the shortest tunnel whose service road makes it down the flank (a
  // stronghold nobody can drive to is no stronghold)
  cands.sort((p, q) => p.dist - q.dist);
  let best = null;
  let f = null;
  let apron = null;
  let gate = null;
  let road = null;
  for (const c of cands) {
    f = frame(cx, cy, c.side);
    apron = f.rect(c.dist, -APRON_W / 2, c.dist + APRON_D, APRON_W / 2);
    gate = pickGate(world, f, c.dist, c.zf);
    road = planRoad(world, apron, gate, gate.out, c.zf);
    if (road.length) {
      best = c;
      break;
    }
  }
  if (!best) return null;
  const cavRect = { x0: cx - best.A, y0: cy - best.B, x1: cx + best.A, y1: cy + best.B };
  const tunnel = f.rect(L * 0.8, -TUNNEL_W / 2, best.dist, TUNNEL_W / 2);
  const fp = {
    x0: Math.min(cavRect.x0, apron.x0, tunnel.x0),
    y0: Math.min(cavRect.y0, apron.y0, tunnel.y0),
    x1: Math.max(cavRect.x1, apron.x1, tunnel.x1),
    y1: Math.max(cavRect.y1, apron.y1, tunnel.y1),
  };
  return {
    padZ: best.zf,
    pads: [{ rect: apron, z: best.zf, margin: vx(22), round: true }, ...road],
    road,
    gate,
    footprint: fp,
    side: best.side,
    dist: best.dist,
    L,
    Wd,
    H,
    A: best.A,
    B: best.B,
    apron,
    tunnel,
    cavRect,
  };
}

/**
 * Service road down the flank: a path pad (a ribbon cut and filled into the
 * slope) that leaves the apron from the gate and descends at ROAD_GRADE,
 * each 8 m step turning (up to 90°) toward the ground that matches its
 * level, so it traverses the flank along the contours and folds into
 * hairpins where it must, never closer than 16 m to itself. It ends in the
 * valley, where the ground stops falling away, after at most ~5 km.
 */
const ROAD_HALF = vx(3.5);
const ROAD_GRADE = 0.09;
const ROAD_STEP = vx(8);

function planRoad(world, apron, start, heading, z0) {
  const T = world.terrain;
  const nat = (x, y) => T.sample(x, y).h;
  const keepOut = { x0: apron.x0 - vx(4), y0: apron.y0 - vx(4), x1: apron.x1 + vx(4), y1: apron.y1 + vx(4) };
  const pts = [{ x: start.x, y: start.y, z: z0 }];
  let [hx, hy] = heading;
  let x = start.x;
  let y = start.y;
  let z = z0;
  for (let k = 0; k < 600; k += 1) {
    let best = null;
    for (let a = -90; a <= 90; a += 10) {
      const r = (a * Math.PI) / 180;
      const dx = hx * Math.cos(r) - hy * Math.sin(r);
      const dy = hx * Math.sin(r) + hy * Math.cos(r);
      const qx = x + dx * ROAD_STEP;
      const qy = y + dy * ROAD_STEP;
      if (qx >= keepOut.x0 && qx <= keepOut.x1 && qy >= keepOut.y0 && qy <= keepOut.y1) continue;
      let clash = false;
      for (let m = 0; m < pts.length - 4 && !clash; m += 1) if (Math.hypot(pts[m].x - qx, pts[m].y - qy) < vx(16)) clash = true;
      if (clash) continue;
      const zt = z - ROAD_GRADE * ROAD_STEP;
      const err = Math.abs(nat(qx, qy) - zt);
      const cost = err + Math.abs(a) * 0.15;
      if (!best || cost < best.cost) best = { qx, qy, dx, dy, zt, err, cost };
    }
    // leaving the apron the road may ride a short fill; later it hugs the ground
    if (!best || best.err > (k < 5 ? vx(12) : vx(4))) break;
    if (world.isWet && world.isWet(best.qx, best.qy, 4)) break;
    x = best.qx;
    y = best.qy;
    z = best.zt;
    hx = best.dx;
    hy = best.dy;
    pts.push({ x: Math.round(x), y: Math.round(y), z: Math.round(z) });
  }
  if (pts.length < 12) return [];
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const q of pts) {
    x0 = Math.min(x0, q.x);
    y0 = Math.min(y0, q.y);
    x1 = Math.max(x1, q.x);
    y1 = Math.max(y1, q.y);
  }
  const rect = { x0: x0 - ROAD_HALF, y0: y0 - ROAD_HALF, x1: x1 + ROAD_HALF, y1: y1 + ROAD_HALF };
  return [{ rect, z: z0, path: pts, half: ROAD_HALF, margin: vx(9), round: true, road: true }];
}

/** Where the road leaves the apron: the edge point (outer or lateral) closest to the apron level. */
function pickGate(world, f, dist, zf) {
  const opts = [
    { u: dist + APRON_D, v: 0, side: f.side, out: DIRS[f.side].u },
    { u: dist + APRON_D * 0.62, v: APRON_W / 2, side: f.facing(1), out: DIRS[f.side].v },
    { u: dist + APRON_D * 0.62, v: -APRON_W / 2, side: f.facing(-1), out: DIRS[f.side].v.map((c) => -c) },
  ];
  let best = null;
  for (const o of opts) {
    const [x, y] = f.toWorld(o.u, o.v);
    const [ox, oy] = f.toWorld(o.u + (o.out === DIRS[f.side].u ? vx(10) : 0), o.v + (o.out === DIRS[f.side].u ? 0 : Math.sign(o.v) * vx(10)));
    const err = Math.abs(world.terrain.sample(ox, oy).h - zf);
    if (!best || err < best.err) best = { ...o, x, y, err };
  }
  return best;
}

// ------------------------------------------------------------ plan

const THEME_SETS = {
  stronghold: { themes: ["military", "power"], accent: MAT.HAZARD_YELLOW, kinds: ["office", "warehouse", "office"] },
  research: { themes: ["lab", "containment"], accent: MAT.SIGN_BLUE, kinds: ["office", "warehouse", "office"] },
};

function plan(world, site) {
  const rng = new Rng(site.seed);
  const pl = site.placed;
  const cx = site.center.x;
  const cy = site.center.y;
  const f = frame(cx, cy, pl.side);
  const { L, Wd } = pl;
  const flavor = rng.chance(0.5) ? "stronghold" : "research";
  const set = THEME_SETS[flavor];
  const zf = site.padZ;
  const cavern = makeCavern(site.seed, cx, cy, pl.A, pl.B, pl.H, zf);
  const inner = 0.74;
  const road = f.rect(-inner * L, -vx(5), L * 1.02, vx(5));
  // lots on both sides of the road; the portal yard near the tunnel mouth
  const lots = [
    { kind: set.kinds[0], rect: f.rect(-inner * L, vx(10), -vx(5), inner * Wd), front: f.facing(-1), style: "concrete" },
    // the hangar opens toward the tunnel mouth
    { kind: set.kinds[1], rect: f.rect(vx(6), vx(10), inner * L * 0.92, inner * Wd), front: f.facingU(1), style: "industrial" },
    { kind: set.kinds[2], rect: f.rect(-inner * L, -inner * Wd, -vx(5), -vx(10)), front: f.facing(1), style: rng.pick(["concrete", "futurist"]) },
  ];
  const yard = f.rect(vx(6), -inner * Wd, inner * L * 0.92, -vx(10));
  // portal block over the stairs down (door on its south side, into the yard)
  const pw = vx(12);
  const ph = vx(15);
  const px0 = Math.round((yard.x0 + yard.x1) / 2 - pw / 2);
  const py0 = yard.y0 + vx(3);
  const portal = { x0: px0, y0: py0, x1: px0 + pw, y1: py0 + ph };
  const helipad = (() => {
    const [x, y] = f.toWorld(pl.dist + APRON_D * 0.6, APRON_W * 0.22);
    return { x, y, r: vx(9) };
  })();
  // the gate gap in the apron fence where the road leaves
  const g = pl.gate;
  const gate = g.side === "N" || g.side === "S" ? { x0: g.x - vx(4), x1: g.x + vx(4), y0: g.y, y1: g.y } : { x0: g.x, x1: g.x, y0: g.y - vx(4), y1: g.y + vx(4) };
  const apronM = { x0: pl.apron.x0 - vx(22), y0: pl.apron.y0 - vx(22), x1: pl.apron.x1 + vx(22), y1: pl.apron.y1 + vx(22) };
  const roadM = pl.road.map((q) => ({ x0: q.rect.x0 - q.margin, y0: q.rect.y0 - q.margin, x1: q.rect.x1 + q.margin, y1: q.rect.y1 + q.margin }));
  const fp = { ...pl.footprint };
  for (const r of roadM) {
    fp.x0 = Math.min(fp.x0, r.x0);
    fp.y0 = Math.min(fp.y0, r.y0);
    fp.x1 = Math.max(fp.x1, r.x1);
    fp.y1 = Math.max(fp.y1, r.y1);
  }
  return {
    flavor,
    frame: f,
    side: pl.side,
    gateSide: pl.gate.side,
    gate,
    cavern,
    road,
    lots,
    yard,
    portal,
    portals: [portal],
    helipad,
    apron: pl.apron,
    tunnel: pl.tunnel,
    dist: pl.dist,
    themes: set.themes,
    accent: set.accent,
    clear: [apronM],
    bounds: { x0: Math.min(fp.x0, apronM.x0) - vx(8), y0: Math.min(fp.y0, apronM.y0) - vx(8), x1: Math.max(fp.x1, apronM.x1) + vx(8), y1: Math.max(fp.y1, apronM.y1) + vx(8) },
    levels: [rng.int(2, 3), rng.int(2, 3)],
    under: null,
  };
}

/** Buildings on the cavern floor (underground lots: they leave the surface above alone). */
function surface(world, site) {
  const rng = new Rng(site.seed ^ 0x51ed);
  const district = DISTRICTS.get("stronghold");
  const cav = site.plan.cavern;
  const envs = [];
  site.plan.lots.forEach((l, k) => {
    const lot = {
      id: `C${site.cell.i}_${site.cell.j}/${site.type}${site.a}_${site.b}/l${k}`,
      rect: l.rect,
      front: l.front,
      frontages: [{ side: l.front, cls: "local" }],
      corner: false,
      district: "stronghold",
      groundZ: cav.zf,
      block: site.id,
      cell: `C${site.cell.i}_${site.cell.j}`,
      underground: true,
    };
    const env = planBuildingEnvelopeAs(lot, l.kind, l.style, district, rng.fork(`b${k}`), { u: 0.1, core: 0.3, groundZ: cav.zf, config: world.config });
    if (!env) return;
    // must clear the vault everywhere over its footprint
    const b = env.bounds;
    let low = Infinity;
    for (const [x, y] of [[b.x0, b.y0], [b.x1, b.y0], [b.x0, b.y1], [b.x1, b.y1], [(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2]]) {
      const r = cavernR(cav, x, y);
      low = Math.min(low, r >= 1 ? -Infinity : cavernCeil(cav, x, y, r));
    }
    if (env.topZ + vx(2) < low) envs.push({ lot, env });
  });
  return envs;
}

/** The apron: concrete with a painted helipad and a lane from the gate to the portal. */
function ground(site, x, y, out) {
  const p = site.plan;
  const a = p.apron;
  const pad = out.pad;
  if (pad?.road) {
    if (out.inside) {
      out.mat = pad._c < 0.7 && pad._s % 48 < 24 ? MAT.LINE_YELLOW : pad._c > ROAD_HALF - 1.2 ? MAT.LINE_WHITE : MAT.ASPHALT_WORN;
      out.sub = MAT.GRAVEL;
    } else {
      out.mat = out.natural - out.z > vx(1) ? MAT.ROCK : MAT.GRAVEL;
      out.sub = out.mat;
    }
    return;
  }
  if (x < a.x0 || x > a.x1 || y < a.y0 || y > a.y1) {
    // the cut into the flank shows bare rock; fill and verges are gravel
    if (Math.abs(out.z - out.natural) > 2 || out.z < site.padZ + vx(2)) {
      out.mat = out.natural - out.z > vx(1) ? MAT.ROCK : MAT.GRAVEL;
      out.sub = out.mat;
    }
    return;
  }
  const h = p.helipad;
  const dh = Math.hypot(x - h.x, y - h.y);
  if (dh < h.r) {
    const hx = Math.abs(x - h.x);
    const hy = Math.abs(y - h.y);
    const isH = dh > h.r - 3 || (hy < vx(3.5) && (Math.abs(hx - vx(2.5)) < 3 || (hx < vx(2.5) && hy < 3)));
    out.mat = isH ? MAT.LINE_WHITE : MAT.CONCRETE_DARK;
    out.sub = MAT.CONCRETE;
    return;
  }
  const alongX = p.side === "E" || p.side === "W";
  const c = alongX ? y - site.center.y : x - site.center.x;
  if (Math.abs(c) < TUNNEL_W / 2) {
    out.mat = Math.abs(c) < 0.6 ? MAT.LINE_YELLOW : MAT.ASPHALT;
    out.sub = MAT.GRAVEL;
    return;
  }
  out.mat = ((x >> 5) + (y >> 5)) & 1 ? MAT.CONCRETE : MAT.CONCRETE_LIGHT;
  out.sub = MAT.CONCRETE;
}

// ------------------------------------------------------------ structures

/** Portal headwall with blast doors, cut-and-cover section and the bored tunnel. */
function emitTunnel(lists, site) {
  const { shells, carves, details } = lists;
  const p = site.plan;
  const f = p.frame;
  const z = site.padZ;
  const L = site.placed.L;
  const d = p.dist;
  const hw = TUNNEL_W / 2;
  const R = (u0, v0, u1, v1) => f.rect(u0, v0, u1, v1);
  const put = (list, r, z0, z1, m, mode = 0) => box(list, r.x0, r.y0, z0, r.x1, r.y1, z1, m, mode);
  // headwall: a massive concrete face in the cut slope
  put(shells, R(d - vx(4), -vx(15), d, vx(15)), z + 1, z + vx(17), MAT.CONCRETE_DARK);
  put(shells, R(d - vx(4), -vx(16), d + vx(1), vx(16)), z + vx(17) + 1, z + vx(18), MAT.CONCRETE);
  // cut and cover: a concrete box around the tunnel where the cut left air
  put(shells, R(d - vx(46), -hw - vx(1.5), d - vx(4), hw + vx(1.5)), z + 1, z + TUNNEL_H + vx(1.5), MAT.CONCRETE, 1);
  // bored tunnel with a concrete lining and a chamfered crown
  const u0 = L * 0.8;
  const uw = L * 1.08; // beyond the (rough) cavern wall
  put(shells, R(uw, -hw - 2, d, hw + 2), z - 1, z + TUNNEL_H + 2, MAT.CONCRETE, 2);
  put(carves, R(u0, -hw, d + 1, hw), z + 1, z + TUNNEL_H - 4, 0);
  put(carves, R(u0, -hw + 4, d + 1, hw - 4), z + TUNNEL_H - 3, z + TUNNEL_H, 0);
  put(details, R(u0, -hw, d, hw), z, z, MAT.ASPHALT);
  put(details, R(u0, -1, d, 0), z, z, MAT.LINE_YELLOW);
  put(details, R(uw, -hw, d, -hw + 3), z + 1, z + 2, MAT.CURB);
  put(details, R(uw, hw - 3, d, hw), z + 1, z + 2, MAT.CURB);
  for (let u = uw + vx(3); u < d - vx(1); u += vx(8)) put(details, R(u, -1, u + vx(3), 0), z + TUNNEL_H, z + TUNNEL_H, MAT.LIGHT_STRIP);
  for (let u = uw + vx(6); u < d - vx(6); u += vx(24)) {
    put(details, R(u, -hw + 1, u + 3, -hw + 1), z + vx(2), z + vx(3.5), MAT.SIGNAL_GREEN);
    put(details, R(u, hw - 1, u + 3, hw - 1), z + vx(2), z + vx(3.5), MAT.EMERGENCY_RED);
  }
  // blast doors: two thick leaves parked half open in pockets of the headwall
  put(details, R(d - vx(3), -hw, d - vx(1.5), -hw + vx(2)), z + 1, z + TUNNEL_H - 4, MAT.METAL_PANEL_DARK);
  put(details, R(d - vx(3), hw - vx(2), d - vx(1.5), hw), z + 1, z + TUNNEL_H - 4, MAT.METAL_PANEL_DARK);
  for (let k = 0; k < TUNNEL_H - 6; k += 8) {
    put(details, R(d - vx(1.5), -hw + vx(2) - 2, d - vx(1.4), -hw + vx(2)), z + 1 + k, z + 4 + k, MAT.HAZARD_YELLOW);
    put(details, R(d - vx(1.5), hw - vx(2), d - vx(1.4), hw - vx(2) + 2), z + 1 + k, z + 4 + k, MAT.HAZARD_YELLOW);
  }
  // portal frame, sign and beacons
  put(details, R(d, -hw - 3, d + 1, -hw - 1), z + 1, z + TUNNEL_H + 2, MAT.HAZARD_YELLOW);
  put(details, R(d, hw + 1, d + 1, hw + 3), z + 1, z + TUNNEL_H + 2, MAT.HAZARD_YELLOW);
  put(details, R(d, -hw - 3, d + 1, hw + 3), z + TUNNEL_H + 1, z + TUNNEL_H + 3, MAT.HAZARD_BLACK);
  put(details, R(d, -vx(6), d + 1, vx(6)), z + TUNNEL_H + vx(1.5), z + TUNNEL_H + vx(3), p.accent === MAT.SIGN_BLUE ? MAT.SIGNAGE_BLUE : MAT.SIGNAGE_RED);
  for (const v of [-vx(13), vx(13)]) put(details, R(d - vx(1), v - 1, d, v + 1), z + vx(18) + 1, z + vx(18) + 4, MAT.SIGNAL_RED);
  // retaining walls along the apron sides
  put(shells, R(d, -APRON_W / 2 - vx(1), d + vx(14), -APRON_W / 2), z + 1, z + vx(5), MAT.CONCRETE, 1);
  put(shells, R(d, APRON_W / 2, d + vx(14), APRON_W / 2 + vx(1)), z + 1, z + vx(5), MAT.CONCRETE, 1);
}

/** Cavern floor: the main road, the yard with the portal block, tanks and trucks. */
function emitCavernFloor(world, lists, site, rng) {
  const { details } = lists;
  const p = site.plan;
  const z = p.cavern.zf;
  const r = p.road;
  box(details, r.x0, r.y0, z, r.x1, r.y1, z, MAT.ASPHALT);
  const alongX = p.side === "E" || p.side === "W";
  if (alongX) box(details, r.x0, site.center.y - 1, z, r.x1, site.center.y, z, MAT.LINE_YELLOW);
  else box(details, site.center.x - 1, r.y0, z, site.center.x, r.y1, z, MAT.LINE_YELLOW);
  const y = p.yard;
  box(details, y.x0, y.y0, z, y.x1, y.y1, z, MAT.FLOOR_EPOXY);
  for (let x = y.x0; x <= y.x1; x += 4) box(details, x, y.y0, z, x + 1, y.y0, z, (x >> 2) & 1 ? MAT.HAZARD_BLACK : MAT.HAZARD_YELLOW);
  portalBlock(lists, p.portal, z, p.accent);
  // fuel tanks and a truck park wherever the yard has room (the portal door stays clear)
  const q = p.portal;
  const taken = [{ x0: q.x0 - vx(2), y0: q.y0 - vx(2), x1: q.x1 + vx(2), y1: q.y1 + vx(9) }];
  const spot = (w, h) => {
    for (let yy = y.y0 + vx(2); yy + h <= y.y1 - vx(2); yy += 8)
      for (let xx = y.x0 + vx(2); xx + w <= y.x1 - vx(2); xx += 8) {
        const r = { x0: xx, y0: yy, x1: xx + w, y1: yy + h };
        if (taken.some((t) => r.x0 <= t.x1 && t.x0 <= r.x1 && r.y0 <= t.y1 && t.y0 <= r.y1)) continue;
        taken.push({ x0: r.x0 - vx(1.5), y0: r.y0 - vx(1.5), x1: r.x1 + vx(1.5), y1: r.y1 + vx(1.5) });
        return r;
      }
    return null;
  };
  const tanks = spot(vx(9) + 44, 44);
  if (tanks) fuelTanks(details, tanks.x0 + 22, tanks.y0 + 22, z, 2);
  for (let k = 0; k < 3; k += 1) {
    const t = spot(18, 54);
    if (t && world.config.vehicles?.parked) truck(details, t.x0, t.y0, z + 1, rng);
  }
  // floodlight masts at the road ends
  for (const [x, yy] of [[r.x0 + 2, r.y0 + 2], [r.x1 - 2, r.y1 - 2]]) {
    box(details, x - 1, yy - 1, z + 1, x + 1, yy + 1, z + vx(9), MAT.STEEL_BEAM);
    box(details, x - 4, yy - 4, z + vx(9) + 1, x + 4, yy + 4, z + vx(9) + 2, MAT.LAMP_LIGHT);
  }
}

/** Apron dressing: fence with a gate on the downhill side, guard booth, vehicles. */
function emitApron(world, lists, site, rng) {
  const { details } = lists;
  const p = site.plan;
  const z = site.padZ;
  const a = p.apron;
  fence(details, a, z, p.gateSide, p.gate, 22, { [{ N: "S", S: "N", E: "W", W: "E" }[p.side]]: true });
  gateBooth(details, p.gate, p.gateSide, z);
  const [x, y] = p.frame.toWorld(p.dist + vx(10), -APRON_W * 0.3);
  if (world.config.vehicles?.parked) truck(details, x, y, z + 1, rng);
}

function structure(world, site) {
  if (site.plan.structure) return site.plan.structure;
  const rng = new Rng(site.seed ^ 0x77);
  const p = site.plan;
  const lists = { shells: [], carves: [], details: [], custom: [cavernVolume(p.cavern, p.side)] };
  emitTunnel(lists, site);
  emitApron(world, lists, site, rng);
  const under = planUnderground(site, rng);
  p.under = under;
  emitCavernFloor(world, lists, site, rng);
  emitComplex(lists, under, rng);
  p.structure = finishStructure(lists);
  return p.structure;
}

// ------------------------------------------------------------ underground

/** Two sectors under the cavern (split across the tunnel axis), a tram below, stairs from the yard. */
function planUnderground(site, rng) {
  const p = site.plan;
  const pl = site.placed;
  const f = p.frame;
  const halfL = pl.L * 1.05;
  const halfW = pl.Wd * 1.05;
  const quads = [f.rect(vx(6), -halfW, halfL, halfW), f.rect(-halfL, -halfW, -vx(6), halfW)];
  const top = p.cavern.zf;
  const sectors = quads.map((b, k) => ({ bounds: b, z0: top - vx(18) - k * vx(8), levels: p.levels[k], theme: p.themes[k], rooms: [8, 12] }));
  let deepest = Infinity;
  for (const s of sectors) deepest = Math.min(deepest, s.z0 - (s.levels - 1) * vx(15));
  const sd = stairDims(30);
  const q = p.portal;
  const cx = Math.round((q.x0 + q.x1) / 2);
  const rect = { x0: cx - Math.floor(sd.W / 2), x1: cx - Math.floor(sd.W / 2) + sd.W - 1, y0: q.y0 + 12, y1: q.y0 + 12 + sd.L - 1 };
  let sector = quads.findIndex((b) => cx >= b.x0 && cx <= b.x1 && q.y0 >= b.y0 && q.y0 <= b.y1);
  if (sector < 0) sector = 0;
  return planComplex(rng, { sectors, entries: [{ rect, zTop: top + 1, sector, dir: -1, openTop: true }], tram: { z: deepest - vx(15) } });
}

// ------------------------------------------------------------ points of interest

function pois(world, site) {
  const p = site.plan;
  structure(world, site);
  // on the apron, facing the blast doors
  const [gx, gy] = p.frame.toWorld(p.dist + APRON_D * 0.45, -APRON_W * 0.2);
  const [tx, ty] = p.frame.toWorld(site.placed.L * 0.6, 0);
  const u = p.under;
  const sh = u.shafts[u.shafts.length - 1];
  return [
    { label: "Mountain stronghold portal", x: gx, y: gy, z: site.padZ + 1 },
    { label: "Mountain stronghold cavern", x: tx, y: ty, z: p.cavern.zf + 1 },
    { label: "Mountain stronghold, tram level", x: sh.rect.x0 + 8, y: sh.rect.y0 - 28, z: u.tram.z + 1 },
  ];
}

/** Link-tunnel port: the tram station nearest the linked site. */
function port(world, site, toward) {
  structure(world, site);
  const u = site.plan.under;
  if (!u.tram?.stations.length) return null;
  let best = null;
  for (const st of u.tram.stations) {
    const x = Math.round((st.hall.x0 + st.hall.x1) / 2);
    const y = Math.round((st.hall.y0 + st.hall.y1) / 2);
    const d = toward ? Math.hypot(toward.x - x, toward.y - y) : 0;
    if (!best || d < best.d) best = { x, y, z: u.tram.z, d };
  }
  return best;
}

SITES.register({
  id: "mountainBase",
  label: "Mountain stronghold",
  frequency: 0.3,
  minU: 0,
  maxU: 0.02,
  size: [190, 190],
  margin: 30,
  place,
  plan,
  surface,
  ground,
  structure,
  pois,
  port,
});
