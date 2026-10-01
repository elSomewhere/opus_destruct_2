import { MAT } from "../../voxel/materials.js";
import { OUT } from "./grid.js";
import { floorZ } from "../archetypes.js";

/**
 * Street fronts of civic buildings and shops (canonical boxes, LOD 0):
 *
 *   shop signs     a lit sign board over every shop door, coloured by the
 *                  kind of shop (a green cross for a pharmacy, a hanging
 *                  sign for a pub, red neon for a club)
 *   env.sign       fascia (a sign band across the front: supermarkets,
 *                  department stores, petrol stations), marquee (a lit
 *                  canopy over a cinema or concert hall entrance), neon (a
 *                  vertical sign by a club door), cross (a hospital's red
 *                  cross and an ambulance porch), police (a blue band and
 *                  a blue lamp), fire (a red band over the engine bays),
 *                  banners (hanging banners either side of a museum door),
 *                  plate (a name plate over the door)
 *   env.portico    stone columns, an entablature and a pediment in front of
 *                  the entrance, steps up to it
 *   petrol pumps   islands with pumps under the canopy (civic.js canopy annex)
 *   car park       a trolley shelter at a supermarket
 */

const SHOP_SIGN = {
  pharmacy: MAT.NEON_GREEN,
  pub: MAT.SIGNAGE_RED,
  venueFloor: MAT.NEON_MAGENTA,
  bakery: MAT.SIGNAGE,
  bank: MAT.SIGN_BLUE,
  gallery: MAT.SIGN_WHITE,
  produkty: MAT.SIGNAGE_BLUE,
  fishmonger: MAT.SIGNAGE_BLUE,
  cafe: MAT.SIGNAGE,
  restaurant: MAT.SIGNAGE_RED,
  kiosk: MAT.SIGNAGE,
  butcher: MAT.SIGNAGE_RED,
  florist: MAT.SIGN_GREEN,
};

/** Direction (du, dv) from a door towards the outside. */
function outward(grid, d) {
  if (d.orient === "h") return grid.get(d.u0, d.v0 - 1) === OUT ? [0, -1] : [0, 1];
  return grid.get(d.u0 - 1, d.v0) === OUT ? [-1, 0] : [1, 0];
}

export function civicDressing(add, plan, env, rng) {
  const F0 = plan.floorByIndex.get(0);
  if (!F0) return;
  const grid = F0.grid;
  const z0 = F0.z + 2;
  // shop signs over every shop door facing the street
  for (const d of grid.doors) {
    if (d.b !== -1 || d.kind !== "shopfront" || d.orient !== "h") continue;
    const [, dv] = outward(grid, d);
    if (dv > 0) continue;
    const type = grid.rooms[d.a]?.type;
    const m = SHOP_SIGN[type] ?? rng.pick([MAT.SIGNAGE, MAT.SIGNAGE_BLUE, MAT.SIGN_WHITE, MAT.SIGNAGE_RED, MAT.SIGN_GREEN]);
    const top = z0 + (d.height ?? 17) + 3;
    const v = d.v0 - 1;
    add({ x0: d.u0 - 2, x1: d.u1 + 2, y0: v, y1: v, z0: top, z1: top + 2, m });
    if (type === "pharmacy") {
      // a projecting green cross
      const u = d.u1 + 4;
      add({ x0: u, x1: u, y0: v - 5, y1: v - 1, z0: top + 1, z1: top + 1, m: MAT.NEON_GREEN });
      add({ x0: u, x1: u, y0: v - 3, y1: v - 3, z0: top - 1, z1: top + 3, m: MAT.NEON_GREEN });
    } else if (type === "pub" || type === "bakery" || type === "barber") {
      // a hanging sign on a bracket
      const u = d.u0 - 4;
      add({ x0: u, x1: u, y0: v - 5, y1: v - 1, z0: top + 3, z1: top + 3, m: MAT.METAL_BLACK });
      add({ x0: u, x1: u, y0: v - 5, y1: v - 2, z0: top - 1, z1: top + 2, m: type === "barber" ? MAT.SIGN_RED : MAT.WOOD_DARK });
    }
  }
  if (!env.civic) return;
  const main = grid.doors.find((d) => d.b === -1 && (d.kind === "entrance" || d.kind === "shopfront") && d.orient === "h" && outward(grid, d)[1] < 0) ?? null;
  const fp = env.tiers[0].rects[0];
  const H0 = env.storyH[0];
  const zTop = floorZ(env, env.floors);
  const zTop0 = floorZ(env, 1);
  const color = env.signColor ?? MAT.SIGNAGE_RED;
  const front = fp.y0 - 1;
  const uc = main ? Math.round((main.u0 + main.u1) / 2) : Math.round((fp.x0 + fp.x1) / 2);
  switch (env.sign) {
    case "fascia": {
      // a sign band across the front at the top of the ground floor, a logo panel over the door
      const zb = Math.min(zTop, zTop0) - 7;
      add({ x0: fp.x0 + 2, x1: fp.x1 - 2, y0: front, y1: front, z0: zb, z1: zb + 4, m: color });
      add({ x0: uc - 10, x1: uc + 10, y0: front - 1, y1: front - 1, z0: zb - 1, z1: zb + 5, m: MAT.SIGN_WHITE });
      add({ x0: uc - 7, x1: uc + 7, y0: front - 2, y1: front - 2, z0: zb + 1, z1: zb + 3, m: color });
      break;
    }
    case "marquee": {
      // a lit canopy over the doors, a sign panel on top of it
      const w = Math.min(fp.x1 - fp.x0 - 8, 60);
      const zc = z0 + 22;
      add({ x0: uc - w / 2, x1: uc + w / 2, y0: front - 20, y1: front, z0: zc, z1: zc + 1, m: MAT.METAL_PANEL_DARK });
      for (let u = Math.round(uc - w / 2); u <= uc + w / 2; u += 3) add({ x0: u, x1: u, y0: front - 20, y1: front - 20, z0: zc - 1, z1: zc - 1, m: MAT.STAGE_LIGHT });
      add({ x0: uc - w / 2 + 4, x1: uc + w / 2 - 4, y0: front - 18, y1: front - 18, z0: zc + 2, z1: zc + 7, m: color });
      add({ x0: uc - w / 2 + 5, x1: uc + w / 2 - 5, y0: front - 19, y1: front - 19, z0: zc + 3, z1: zc + 6, m: MAT.SIGN_WHITE });
      break;
    }
    case "neon": {
      const u = main ? main.u1 + 4 : uc + 10;
      add({ x0: u, x1: u, y0: front - 5, y1: front - 1, z0: z0 + 20, z1: Math.min(zTop - 2, z0 + 34), m: MAT.PANEL_GRAPHITE });
      add({ x0: u, x1: u, y0: front - 5, y1: front - 5, z0: z0 + 21, z1: Math.min(zTop - 3, z0 + 33), m: rng.pick([MAT.NEON_RED, MAT.NEON_MAGENTA, MAT.NEON_CYAN]) });
      break;
    }
    case "cross": {
      // a white panel with a red cross high on the front; an ambulance porch over the entrance
      const zc = zTop - 18;
      add({ x0: uc - 7, x1: uc + 7, y0: front, y1: front, z0: zc - 7, z1: zc + 7, m: MAT.PLASTIC_WHITE });
      add({ x0: uc - 5, x1: uc + 5, y0: front - 1, y1: front - 1, z0: zc - 1, z1: zc + 1, m: MAT.SIGNAL_RED });
      add({ x0: uc - 1, x1: uc + 1, y0: front - 1, y1: front - 1, z0: zc - 5, z1: zc + 5, m: MAT.SIGNAL_RED });
      const pw = 28;
      add({ x0: uc - pw, x1: uc + pw, y0: front - 40, y1: front, z0: z0 + 26, z1: z0 + 27, m: MAT.CONCRETE_LIGHT });
      for (const u of [uc - pw, uc + pw]) add({ x0: u, x1: u + 1, y0: front - 40, y1: front - 39, z0: z0 - 2, z1: z0 + 25, m: MAT.CONCRETE_LIGHT });
      add({ x0: uc - pw, x1: uc + pw, y0: front - 40, y1: front - 40, z0: z0 + 24, z1: z0 + 25, m: MAT.SIGNAL_RED });
      break;
    }
    case "police": {
      const zb = zTop0 - 6;
      add({ x0: uc - 16, x1: uc + 16, y0: front, y1: front, z0: zb, z1: zb + 3, m: MAT.POLICE_BLUE });
      add({ x0: uc - 12, x1: uc + 12, y0: front - 1, y1: front - 1, z0: zb + 1, z1: zb + 2, m: MAT.SIGN_WHITE });
      for (const u of [uc - 20, uc + 20]) add({ x0: u, x1: u + 1, y0: front - 2, y1: front - 1, z0: zb + 1, z1: zb + 3, m: MAT.SIGNAGE_BLUE });
      break;
    }
    case "fire": {
      const zb = zTop0 - 5;
      add({ x0: fp.x0 + 2, x1: fp.x1 - 2, y0: front, y1: front, z0: zb, z1: zb + 2, m: MAT.FIRE_RED });
      break;
    }
    case "banners": {
      const zb = zTop - 30;
      for (const u of [uc - 14, uc + 12]) {
        add({ x0: u, x1: u + 2, y0: front - 1, y1: front - 1, z0: zb, z1: zTop - 4, m: rng.pick([MAT.ART_CRIMSON, MAT.ART_BLUE, MAT.ART_TEAL, MAT.AWNING_RED]) });
        add({ x0: u - 1, x1: u + 3, y0: front - 2, y1: front - 1, z0: zTop - 3, z1: zTop - 3, m: MAT.METAL_BLACK });
      }
      break;
    }
    case "plate": {
      const top = z0 + 20;
      add({ x0: uc - 8, x1: uc + 8, y0: front, y1: front, z0: top, z1: top + 2, m: rng.pick([MAT.GOLD, MAT.SIGN_WHITE, MAT.METAL_BLACK]) });
      break;
    }
    default:
  }
  if (env.portico && main) portico(add, env, fp, uc, z0 - 1, rng);
  if (env.annexes.some((a) => a.kind === "canopy")) pumps(add, env, rng);
}

/**
 * A classical portico in front of the entrance: a stylobate with steps,
 * a row of columns, an entablature at the top of the second floor (or the
 * first on a one-storey building) and a triangular pediment above it.
 */
function portico(add, env, fp, uc, zg, rng) {
  const stone = MAT.LIMESTONE;
  const trim = MAT.TRIM_STONE;
  const nCol = fp.x1 - fp.x0 > 240 ? 6 : 4;
  const span = (nCol - 1) * 24;
  const u0 = Math.max(fp.x0 + 8, Math.min(fp.x1 - 8 - span, Math.round(uc - span / 2)));
  // (the building frame starts at the front wall: the portico stands at v < 0, in the forecourt)
  const depth = Math.max(12, Math.min(28, (env.setF ?? 36) - 8));
  const vF = fp.y0 - depth;
  // the stylobate's top is level with the ground floor (zg: the slab's top voxel), one step down to the forecourt
  const zb = zg;
  const zTop = floorZ(env, Math.min(env.floors, 2)) - 1;
  add({ x0: u0 - 6, x1: u0 + span + 6, y0: vF, y1: fp.y0 - 1, z0: zb - 1, z1: zb, m: MAT.GRANITE_LIGHT });
  add({ x0: u0 - 5, x1: u0 + span + 5, y0: vF - 2, y1: vF - 1, z0: zb - 1, z1: zb - 1, m: MAT.GRANITE_LIGHT });
  // columns with a base and a capital
  for (let c = 0; c < nCol; c += 1) {
    const u = u0 + c * 24;
    add({ x0: u - 2, x1: u + 2, y0: vF + 1, y1: vF + 5, z0: zb + 1, z1: zb + 1, m: trim });
    add({ x0: u - 1, x1: u + 1, y0: vF + 2, y1: vF + 4, z0: zb + 2, z1: zTop - 4, m: stone });
    add({ x0: u - 2, x1: u + 2, y0: vF + 1, y1: vF + 5, z0: zTop - 3, z1: zTop - 3, m: trim });
  }
  // entablature and pediment
  add({ x0: u0 - 5, x1: u0 + span + 5, y0: vF, y1: fp.y0 - 1, z0: zTop - 2, z1: zTop + 2, m: stone });
  add({ x0: u0 - 6, x1: u0 + span + 6, y0: vF - 1, y1: vF - 1, z0: zTop + 2, z1: zTop + 2, m: trim });
  const half = span / 2 + 5;
  for (let k = 0; k * 2 < half; k += 1) add({ x0: Math.round(u0 - 5 + 2 * k), x1: Math.round(u0 + span + 5 - 2 * k), y0: vF, y1: fp.y0 - 1, z0: zTop + 3 + k, z1: zTop + 3 + k, m: k === 0 ? trim : stone });
  void rng;
}

/** Two pump islands under the petrol canopy. */
function pumps(add, env, rng) {
  const c = env.annexes.find((a) => a.kind === "canopy");
  const fp = env.tiers[0].rects[0];
  // canopy rect in the building frame (the annex rect is in lot coordinates; the building's front is at fp.y0)
  const depth = c.rect.y1 - c.rect.y0 + 1;
  const vMid = fp.y0 - 16 - Math.round(depth / 2);
  const zg = env.groundZ + 1;
  const color = env.signColor ?? MAT.SIGNAGE_RED;
  const w = fp.x1 - fp.x0;
  for (const du of [-0.25, 0.25]) {
    const u = Math.round((fp.x0 + fp.x1) / 2 + du * Math.max(w * 1.6, 80));
    add({ x0: u - 3, x1: u + 3, y0: vMid - 16, y1: vMid + 16, z0: zg, z1: zg, m: MAT.CONCRETE_LIGHT });
    for (const dv of [-8, 8]) {
      add({ x0: u - 2, x1: u + 2, y0: vMid + dv - 1, y1: vMid + dv + 1, z0: zg + 1, z1: zg + 12, m: MAT.PLASTIC_WHITE });
      add({ x0: u - 2, x1: u + 2, y0: vMid + dv - 1, y1: vMid + dv + 1, z0: zg + 13, z1: zg + 14, m: color });
      add({ x0: u - 1, x1: u + 1, y0: vMid + dv - 2, y1: vMid + dv + 2, z0: zg + 8, z1: zg + 9, m: MAT.SCREEN });
      add({ x0: u + 3, x1: u + 3, y0: vMid + dv, y1: vMid + dv, z0: zg + 4, z1: zg + 8, m: MAT.PLASTIC_BLACK });
    }
  }
  void rng;
}
