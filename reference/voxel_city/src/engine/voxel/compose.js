import { ChunkBuffer, P, P2 } from "./chunk.js";
import { MAT } from "./materials.js";
import { CHUNK, vx } from "../core/units.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";
import { lotSurface, spaceSurface } from "../city/landscape.js";
import { hash32 } from "../core/hash.js";
import { LAPSE } from "../nature/landcover.js";
import { quaySide } from "../city/industry.js";
import { segmentLevel } from "../network/roadLevel.js";
import { slabFoot } from "../network/roadParts.js";
import { wrapOf } from "../world/wrap.js";
import { seasonOf, patchNoise } from "../world/season.js";

const STRATA_ROCK = [MAT.ROCK, MAT.ROCK_DARK, MAT.STONE, MAT.ROCK, MAT.ROCK_LIGHT, MAT.GRANITE, MAT.ROCK, MAT.ROCK_DARK];
const STRATA_DRY = [MAT.SANDSTONE, MAT.RED_EARTH, MAT.SANDSTONE, MAT.CLAY, MAT.SAND_DUNE, MAT.SANDSTONE, MAT.RED_EARTH, MAT.SANDSTONE];

/** How far beyond a road's right-of-way the ground is cut / filled towards it (voxels). */
const EMBANK = 112;

/**
 * Ground tile column kinds (tile.kind): 1 road, 2 lot, 3 open space,
 * 4 natural ground, 5 site, 6 cultivated field.
 */

/**
 * Chunk composition.
 *
 * 1. Ground tile (per LOD column tile, cached): for each of the 34x34 padded
 *    columns, the surface height, top/sub materials and water level, composed
 *    from terrain + roads + lots + open spaces + natural land cover.
 * 2. Ground fill of the chunk from the tile.
 * 3. Feature sources (buildings, highways, subway, trees, props...) rasterize
 *    on top in `order`. Sources may carve (write AIR) for underground spaces.
 */

const NO_WATER = -2147483648;

/** Ground materials that take seasonal snow (world/season.js). */
const SNOWABLE = new Uint8Array(1024);
for (const m of ["GRASS", "GRASS_DRY", "GRASS_LAWN", "FOREST_FLOOR", "MOSS", "TUNDRA", "DIRT", "MARSH_GRASS", "SAVANNA_GRASS", "JUNGLE_FLOOR", "MUD", "RED_EARTH", "CLAY", "SOIL_BED", "LICHEN", "HEDGE", "GRASS_DARK", "NEEDLE_LITTER", "LEAF_LITTER", "GRASS_TALL", "GRASS_STRAW", "HAY", "CROP_WHEAT", "CROP_RAPE", "CROP_GREEN"]) if (MAT[m] !== undefined) SNOWABLE[MAT[m]] = 1;


export function groundTile(world, lod, cx, cy) {
  const key = `${lod},${cx},${cy}`;
  const cached = world.groundTiles.get(key);
  if (cached) return cached;
  const tile = computeGroundTile(world, lod, cx, cy);
  world.groundTiles.set(key, tile);
  return tile;
}

function computeGroundTile(world, lod, cx, cy) {
  const separate = world.config.world.angles?.partsMode === "separate";
  const s = 1 << lod;
  const half = s >> 1;
  const bx = (cx * CHUNK - 1) * s;
  const by = (cy * CHUNK - 1) * s;
  const n = P * P;
  const h = new Float32Array(n);
  // heights without the small-scale roughness: land cover slopes
  const hs = new Float32Array(n);
  const u = new Float32Array(n);
  const z = new Int32Array(n);
  const top = new Uint16Array(n);
  const sub = new Uint16Array(n);
  const water = new Int32Array(n).fill(NO_WATER);
  const kind = new Uint8Array(n);
  // rivers: deck codes 1 bridge deck, 2 deck + railing, 3 deck + pier, 4 quay railing
  const deck = new Uint8Array(n);
  const bed = new Int32Array(n);
  const { terrain, config } = world;
  const seaZ = Math.round(config.world.seaLevel * 8);
  const cover = world.landCover;
  const seed = world.seed;

  const core = new Float32Array(n);
  // island mode: distance to the shore (m); planned surfaces stop at the water's edge
  const coast = world.fields.island ? new Float32Array(n) : null;
  let subDepth = null;
  let rock = null;
  // stream channels of landforms (canyon floors, ravine creeks); up close only
  let streams = null;
  // bare granite of outcrops and shore slabs (landforms.js outcrops)
  let outc = null;
  for (let j = 0; j < P; j += 1) {
    const y = by + j * s + half;
    for (let i = 0; i < P; i += 1) {
      const x = bx + i * s + half;
      const ts = terrain.sample(x, y);
      h[i + j * P] = ts.h;
      hs[i + j * P] = ts.h - ts.rough * 8;
      u[i + j * P] = ts.u;
      core[i + j * P] = ts.core;
      if (coast) coast[i + j * P] = ts.coast;
      if (ts.outcrop > 0.4) (outc ??= new Float32Array(n))[i + j * P] = ts.outcrop;
      if (ts.stream && lod <= 3) {
        if (!streams) streams = new Array(n).fill(null);
        streams[i + j * P] = ts.stream;
      }
    }
  }
  const carveStream = (idx) => {
    const st = streams[idx];
    if (!st || st.d > st.half + 1) return;
    const axis = z[idx] - Math.round(st.extra * 8);
    if (st.d < st.half) {
      const t = st.d / st.half;
      const bed = axis - Math.round((0.35 + (st.wet ? 0.7 : 0.25) * (1 - t * t)) * 8);
      z[idx] = Math.min(z[idx], bed);
      top[idx] = st.wet ? (t < 0.6 ? MAT.GRAVEL : MAT.SAND) : MAT.SAND;
      sub[idx] = MAT.GRAVEL;
      if (st.wet) water[idx] = axis - 2;
    } else {
      // the bank slopes down to the water (no vertical step); a wet margin of mud and pebbles
      const q = 1 - (st.d - st.half);
      z[idx] = Math.min(z[idx], axis - Math.round(0.35 * 8 * q * q));
      if (st.wet) top[idx] = (hash32(seed, idx, cx, cy) & 3) === 0 ? MAT.GRAVEL : MAT.MUD;
    }
  };
  // far tiles: no street-level planning, cities become skyline blocks
  const far = lod > (config.streaming.cityDetailLod ?? 6);
  if (far) subDepth = new Int32Array(n);

  // natural ground: land cover, ponds, sea, rivers; forest canopy blankets at coarse LODs
  const canopyLod = lod >= (config.streaming.canopyLod ?? 4);
  const season = seasonOf(config);
  // a wrapping world: crown and skyline lattices repeat with it
  const wrap = wrapOf(config);
  const nCrown = wrap.on ? wrap.sizeV / 40 : 0;
  // (farm: the column is in farm country, a rural block of the cell plan: fields, no woods)
  const naturalColumn = (idx, i, j, x, y, hz, farm = false) => {
    const il = i > 0 ? i - 1 : i;
    const ir = i < P - 1 ? i + 1 : i;
    const jl = j > 0 ? j - 1 : j;
    const jr = j < P - 1 ? j + 1 : j;
    const gx = (hs[ir + j * P] - hs[il + j * P]) / ((ir - il) * s);
    const gy = (hs[i + jr * P] - hs[i + jl * P]) / ((jr - jl) * s);
    const slope = Math.hypot(gx, gy);
    const [t, sb, pond, bump, field] = cover.surface(x, y, hz, slope, u[idx], farm);
    z[idx] = hz;
    top[idx] = t;
    sub[idx] = sb;
    if (bump) {
      // a field wall of cleared stones
      z[idx] = hz + bump;
      (subDepth ??= new Int32Array(n))[idx] = bump;
    }
    // granite outcrops: bare pinkish rock with grey lichen, moss in the cracks
    const oc = outc ? outc[idx] : 0;
    if (oc > 0.45 && hz >= seaZ && t !== MAT.SNOW) {
      const g = hash32(wrap.vi(x) >> 3, wrap.vi(y) >> 3, 0x6c) & 7;
      top[idx] = oc > 0.6 ? (g < 2 ? MAT.LICHEN : g < 5 ? MAT.GRANITE_PINK : MAT.ROCK_LIGHT) : g < 3 ? MAT.LICHEN : g < 5 ? MAT.MOSS : t;
      sub[idx] = MAT.GRANITE;
    }
    // bare rock: cliff faces show layered strata (sandstone in dry country)
    if (sb === MAT.ROCK || sb === MAT.STONE || t === MAT.ROCK || t === MAT.ROCK_DARK) (rock ??= new Uint8Array(n))[idx] = 1;
    else if (sb === MAT.SANDSTONE) (rock ??= new Uint8Array(n))[idx] = 2;
    if (pond) {
      // wetland ponds: the ground dips below a still water surface
      z[idx] = hz - pond * s;
      water[idx] = hz;
    }
    if (hz < seaZ) {
      water[idx] = seaZ;
      top[idx] = hz < seaZ - 12 ? MAT.MUD : MAT.SAND;
    }
    // (6: a cultivated field: no caves, boulders or wild plants break it up)
    kind[idx] = field ? 6 : 4;
    if (rivers) carveRiver(idx, x, y);
    if (lakes) carveLake(idx, x, y);
    if (streams && water[idx] === NO_WATER) carveStream(idx);
    if (canopyLod && water[idx] === NO_WATER && u[idx] < 0.2) {
      const hM = hz / 8;
      const fd = farm ? 0 : cover.forestDensity(x, y, u[idx], null, hM);
      if (fd > 0.3) {
        // a blanket of crowns: one dome per jittered 5 m cell (Worley-style);
        // towards the forest edge single crowns drop out
        const C = 40;
        const ci = Math.floor(x / C);
        const cj = Math.floor(y / C);
        let best = Infinity;
        let bk = 0;
        for (let dj = -1; dj <= 1; dj += 1)
          for (let di = -1; di <= 1; di += 1) {
            const hh = hash32(seed, wrap.canon(ci + di, nCrown), wrap.canon(cj + dj, nCrown), 17) >>> 0;
            const px = (ci + di) * C + (hh & 31) + 4;
            const py = (cj + dj) * C + ((hh >>> 5) & 31) + 4;
            const d2 = (px - x) * (px - x) + (py - y) * (py - y);
            if (d2 < best) {
              best = d2;
              bk = ((hh >>> 10) & 1023) / 1024;
            }
          }
        const dc = Math.sqrt(best) / 26;
        // the forest's clumps and gaps (nature/forest.js)
        const fc = world.forest ? world.forest.clumped(x, y, fd) : fd;
        if (fc > 0.3 + 0.35 * bk && dc < 1) {
          const { t: tl } = cover.climate(x, y, hM);
          const k = hash32(seed, wrap.vi(x) >> 6, wrap.vi(y) >> 6, 17) / 4294967296;
          // conifers and broadleaves in the stands of the near forest (nature/forest.js),
          // broadleaves in their seasonal colours
          const share = world.forest ? world.forest.coniferShare(x, y, hM) : tl < 0.42 ? 0.84 : 0;
          const conifer = ((bk * 13.7) % 1) < share;
          const m = season.canopy(conifer, tl, bk);
          // pointed spruce crowns, rounded broadleaf ones
          const crown = conifer ? Math.max(0, 1 - dc) * 1.35 : Math.sqrt(Math.max(0, 1 - dc * dc));
          const bump = Math.round((6 + 5 * bk + (conifer ? 8 : 5) * crown) * 8 * Math.min(1, fc * 1.4));
          z[idx] = hz + bump;
          // (foliage all the way down: a forest edge is a wall of leaves, not of stone)
          (subDepth ??= new Int32Array(n))[idx] = bump;
          top[idx] = m === MAT.LEAVES_LIGHT || m === MAT.LEAVES_DARK ? (k < 0.3 ? MAT.LEAVES_LIGHT : MAT.LEAVES_DARK) : m;
          // snow-capped crowns (their dark flanks still show); bare crowns hold less
          const cs = season.snow(tl) * (m === MAT.TWIGS ? 0.5 : 1);
          if (cs > 0 && crown > 0.45 && ((bk * 7.3) % 1) < cs * 0.8) top[idx] = MAT.SNOW;
          sub[idx] = m === MAT.TWIGS ? MAT.TWIGS : MAT.LEAVES_DARK;
        }
      }
    }
  };
  // distant cities: blocks of roughly the right height, streets between them
  const farCityColumn = (idx, x, y) => {
    const bs = 480;
    const lx = ((x % bs) + bs) % bs;
    const ly = ((y % bs) + bs) % bs;
    top[idx] = MAT.ASPHALT_WORN;
    sub[idx] = MAT.CONCRETE;
    if (lx < 96 || ly < 96) return;
    const k = hash32(seed, Math.floor(wrap.v(x) / bs), Math.floor(wrap.v(y) / bs), 71) / 4294967296;
    if (k < 0.2) {
      top[idx] = MAT.GRASS_LAWN;
      return;
    }
    const uu = u[idx];
    const c = core[idx];
    const hm = (5 + 14 * uu + 150 * c * c * k) * (0.6 + 0.8 * k);
    const bump = Math.round(hm * 8);
    z[idx] += bump;
    subDepth[idx] = bump;
    top[idx] = k < 0.5 ? MAT.ROOF_MEMBRANE : MAT.ROOF_GRAVEL;
    sub[idx] = c > 0.4 ? MAT.METAL_PANEL_DARK : k < 0.6 ? MAT.CONCRETE_LIGHT : MAT.BRICK_BROWN;
  };

  // per-cell caches for this tile
  const cellCache = new Map();
  const tileRect = { x0: bx, y0: by, x1: bx + P * s, y1: by + P * s };
  const getCell = (x, y) => {
    const c = world.cellAt(x, y);
    const k = `${c.i},${c.j}`;
    let e = cellCache.get(k);
    if (!e) {
      const view = world.roadView(c.i, c.j);
      const pad = view.maxReach + EMBANK;
      const cands = view.near({ x0: tileRect.x0 - pad, y0: tileRect.y0 - pad, x1: tileRect.x1 + pad, y1: tileRect.y1 + pad });
      e = { i: c.i, j: c.j, rect: world.arterials.cellRect(c.i, c.j), view, cands, plan: null };
      cellCache.set(k, e);
    }
    return e;
  };
  let lastCell = null;
  const rs = makeRoadSample(wrapOf(config).sizeV);
  const ss = { mat: 0, dz: 0, water: false };
  const sites = world.sites ? world.sites.sitesNear(tileRect) : [];
  const so = { z: 0, mat: 0, sub: 0, site: null, inside: false };
  const rivers = world.rivers && world.config.rivers.enabled ? world.rivers : null;
  const lakes = world.lakes && world.config.lakes.enabled && world.lakes.near(tileRect).length ? world.lakes : null;
  // lake basins for natural ground and open spaces
  const carveLake = (idx, x, y, quay = null) => {
    if (quay) {
      // harbour yard: a straight vertical quay wall, a dredged basin beyond it
      const s = quaySide(quay, x, y);
      if (s <= 0) {
        if (s > -vx(3)) {
          top[idx] = s > -4 ? MAT.LINE_YELLOW : MAT.CONCRETE;
          sub[idx] = MAT.CONCRETE;
        }
        return;
      }
      z[idx] = Math.min(z[idx], quay.level - vx(7));
      top[idx] = MAT.MUD;
      sub[idx] = MAT.CONCRETE;
      water[idx] = quay.level;
      return;
    }
    const lk = lakes.at(x, y);
    if (!lk) return;
    const g = lakes.groundAt(lk, z[idx]);
    if (g >= z[idx] && lk.k >= 1) return;
    z[idx] = Math.min(z[idx], g);
    if (lk.k < 1) {
      top[idx] = lk.k > 0.85 ? MAT.SAND : lk.k > 0.6 ? MAT.GRAVEL : MAT.MUD;
      sub[idx] = MAT.MUD;
      if (lk.level > z[idx]) water[idx] = lk.level;
    } else if (z[idx] <= lk.level + 4) top[idx] = MAT.SAND;
  };
  // river channel / banks / quays for natural ground and open spaces
  const carveRiver = (idx, x, y) => {
    const ri = rivers.at(x, y);
    if (!ri) return;
    const g = rivers.groundAt(ri, z[idx]);
    if (ri.d < ri.half) {
      z[idx] = g;
      top[idx] = ri.d > ri.half - 1.5 && ri.urban > 0.5 ? MAT.CONCRETE_LIGHT : MAT.GRAVEL;
      sub[idx] = MAT.MUD;
      if (ri.water > g) water[idx] = ri.water;
      return;
    }
    if (ri.urban > 0.5) {
      // quay: paved edge with a railing, concrete wall face below
      if (ri.d < ri.half + 2.5) {
        top[idx] = MAT.PLAZA_STONE_DARK;
        sub[idx] = MAT.CONCRETE_LIGHT;
        if (ri.d < ri.half + 0.2) deck[idx] = 4;
      }
      return;
    }
    if (g < z[idx]) {
      z[idx] = g;
      if (g <= ri.water + 3) top[idx] = MAT.SAND;
    }
  };

  for (let j = 0; j < P; j += 1) {
    const y = by + j * s + half;
    for (let i = 0; i < P; i += 1) {
      const x = bx + i * s + half;
      const idx = i + j * P;
      const hz = Math.round(h[idx]);
      if (far) {
        naturalColumn(idx, i, j, x, y, hz);
        if (u[idx] > 0.3) farCityColumn(idx, x, y);
        continue;
      }
      let cell = lastCell;
      if (!cell || x < cell.rect.x0 || x >= cell.rect.x1 || y < cell.rect.y0 || y >= cell.rect.y1) {
        cell = getCell(x, y);
        lastCell = cell;
      }
      sampleRoadSurface(cell.cands, x + 0.5, y + 0.5, rs, seed, EMBANK);
      if (rs.kind !== KIND.NONE) {
        // the road's own level: graded along, level across (network/roadLevel.js)
        z[idx] = Math.round(segmentLevel(world, rs.seg, rs.along)) + rs.dz;
        top[idx] = rs.mat;
        sub[idx] = rs.kind === KIND.CARRIAGE ? MAT.GRAVEL : MAT.CONCRETE;
        kind[idx] = 1;
        // parts mode: a pitched road piece holds the surface here, the ground stops at its slab's foot
        const piece = separate ? pitchedAt(world, rs) : null;
        if (piece) {
          z[idx] = Math.floor(slabFoot(piece, x + 0.5, y + 0.5));
          top[idx] = sub[idx];
        }
        let ri = rivers ? rivers.at(x, y) : null;
        if (!(ri && ri.d < ri.half + 0.5) && lakes) {
          // a road across a lake runs on a causeway bridge
          const lk = lakes.at(x, y);
          if (lk && lk.k < 1) ri = { d: 0, half: 1, bed: lk.bed, water: lk.level };
        }
        if (ri && ri.d < ri.half + 0.5) {
          // the road crosses the river on a bridge
          bed[idx] = ri.bed;
          water[idx] = ri.water;
          deck[idx] = 1;
          if (rs.sdfR > -1.5) deck[idx] = 2;
          else if (rs.seg) {
            const a = Math.floor(rs.seg.s0 + rs.along);
            if (((a % 96) + 96) % 96 < 6 && Math.abs(rs.side) < rs.seg.hc * 0.7) deck[idx] = 3;
          }
        }
        continue;
      }
      if (sites.length && world.sites.ground(x, y, h[idx], so)) {
        z[idx] = so.z;
        top[idx] = so.mat || MAT.GRASS_DRY;
        sub[idx] = so.sub || MAT.DIRT;
        kind[idx] = 5;
        continue;
      }
      if (!cell.plan) cell.plan = world.cellPlan(cell.i, cell.j);
      const lot = cell.plan.lotAt(x, y);
      // lots inside a site's cavern do not shape the surface above them
      if (lot && !lot.underground) {
        const env = lot.building ? cell.plan.buildingById.get(lot.building) : null;
        // a level pad under the building, the yard graded towards the block surface (city/grading.js)
        z[idx] = cell.plan.grading.at(x, y, lot.levelAt ? lot.levelAt(x, y) : hz, lot.block, lot);
        top[idx] = lot.underHighway ? underDeckParking(x, y) : lotSurface(lot, env, x, y, seed, wrap.v(x), wrap.v(y));
        sub[idx] = MAT.DIRT;
        kind[idx] = 2;
        continue;
      }
      const space = cell.plan.spaceAt(x, y);
      // (island: parks, plazas and yards end at the shore; a quay yard keeps its quay)
      if (space && !(coast && coast[idx] < 3 && !space.quay)) {
        spaceSurface(space, x, y, ss, wrap.v(x), wrap.v(y));
        // open spaces follow their block's surface (stretched between its sidewalks),
        // eased to the pads of the buildings on and beside them (city/grading.js)
        const base = space.levelAt ? cell.plan.grading.at(x, y, space.levelAt(x, y), space.block) : space.groundZ;
        z[idx] = base + Math.min(ss.dz, 0);
        top[idx] = ss.water ? MAT.MUD : ss.mat;
        if (ss.water) water[idx] = base - 1;
        if (ss.dz > 0) z[idx] = base + ss.dz;
        sub[idx] = MAT.DIRT;
        kind[idx] = 3;
        if (rivers) carveRiver(idx, x, y);
        if (space.quay) carveLake(idx, x, y, space.quay);
        else if (lakes) carveLake(idx, x, y);
        continue;
      }
      // leftover ground in an urban block follows the block surface; open
      // country is cut / filled towards the road edge next to a road
      const blk = cell.plan.blockAt ? cell.plan.blockAt(x, y) : null;
      if (blk?.levelAt && !(coast && coast[idx] < 3)) {
        const gz = blk.cuts ? vergeLevel(cell.plan, blk, x, y) : cell.plan.grading.at(x, y, blk.levelAt(x, y), blk.id);
        if (blk.cuts) {
          // (the angled world: the verge between a slanted street and its
          // block's lots is a mown lawn, trees on it, city/dressing.js)
          z[idx] = gz;
          top[idx] = MAT.GRASS_LAWN;
          sub[idx] = MAT.DIRT;
          kind[idx] = 3;
          if (rivers) carveRiver(idx, x, y);
          if (lakes) carveLake(idx, x, y);
          continue;
        }
        naturalColumn(idx, i, j, x, y, gz);
        continue;
      }
      naturalColumn(idx, i, j, x, y, embank(world, rs, hz), cell.plan.farmAt(x, y));
    }
  }

  // retaining walls: graded lots / spaces that drop more than 2 voxels to a
  // neighbour get a concrete edge instead of an exposed earth cliff
  for (let j = 1; j < P - 1; j += 1) {
    for (let i = 1; i < P - 1; i += 1) {
      const idx = i + j * P;
      const k = kind[idx];
      if (k !== 2 && k !== 3 && k !== 5) continue;
      const zz = z[idx];
      const drop = Math.max(zz - z[idx - 1], zz - z[idx + 1], zz - z[idx - P], zz - z[idx + P]);
      if (drop > 2 * s && top[idx] !== MAT.CONCRETE && water[idx] === NO_WATER) {
        top[idx] = MAT.PARAPET_CAP;
        sub[idx] = MAT.CONCRETE_LIGHT;
      }
    }
  }

  // seasons (world/season.js): snow on open ground, lawns, parks, yards and
  // forest floors (bare patches where it is thin), fresh or autumn grass,
  // leaf litter and heather, wild flowers on the meadows. Everything keys
  // on the local temperature (the tile's climate cooled by altitude).
  const tBase = tileTemperature(world, bx, by, P * s);
  // (wild flowers colour the ground at LOD 1; up close they grow as plants, nature/groundcover.js)
  const flowers = lod === 1;
  const nP = cover.nPatch;
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const k = i + j * P;
      const m = top[k];
      if (water[k] !== NO_WATER && water[k] >= z[k]) continue;
      if (!season.any && !(flowers && kind[k] === 4)) continue;
      const tl = tBase(i, j) - Math.max(0, z[k] / 8) / LAPSE;
      const x = bx + i * s + half;
      const y = by + j * s + half;
      if (SNOWABLE[m]) {
        const cov = season.snow(tl);
        if (cov > 0) {
          // bare patches where the snow is thin (wind, sun, under trees)
          const p = cover.fieldNoise(nP, x, y, 170, 5.3, 0) + 0.45 * cover.fieldNoise(nP, x, y, 36, 0, 2.1);
          if (p <= 2.1 * cov - 0.75) {
            top[k] = p > 2.1 * cov - 1.05 || (kind[k] === 4 && p < -0.9) ? MAT.SNOW_WIND : MAT.SNOW;
            continue;
          }
        }
      }
      const hx = wrap.v(x);
      const hy = wrap.v(y);
      let g = season.table ? season.ground(m, tl, 0.7 * patchNoise(seed, hx, hy, 240, 0x61) + 0.3 * patchNoise(seed, hx, hy, 48, 0x65)) : m;
      if (flowers && kind[k] === 4) {
        const f = season.flower(g, tl, patchNoise(seed, hx, hy, 48, 0x62), hash32(seed, wrap.vi(x), wrap.vi(y), 0x63) / 4294967296, patchNoise(seed, hx, hy, 192, 0x64));
        if (f) g = f;
      }
      top[k] = g;
    }

  let zMin = Infinity;
  let zMax = -Infinity;
  let anyDeck = false;
  for (let k = 0; k < n; k += 1) {
    const zz = z[k];
    if (zz < zMin) zMin = zz;
    let top2 = water[k] !== NO_WATER ? Math.max(zz, water[k]) : zz;
    if (deck[k]) {
      anyDeck = true;
      top2 = zz + 8;
      if (deck[k] !== 4 && bed[k] < zMin) zMin = bed[k];
    }
    if (top2 > zMax) zMax = top2;
  }
  // frozen water: lakes, ponds and rivers freeze below the season's
  // threshold (the snow line in summer); an island's sea stays open
  const freezeT = season.freezeT;
  let ice = null;
  for (let j = 0; j < P; j += 1)
    for (let i = 0; i < P; i += 1) {
      const k = i + j * P;
      if (water[k] === NO_WATER || water[k] <= z[k] || deck[k]) continue;
      if (coast && coast[k] < 0) continue;
      const { t } = cover.climate(bx + i * s + half, by + j * s + half, water[k] / 8);
      if (t >= freezeT) continue;
      if (!ice) ice = new Uint8Array(n);
      ice[k] = 1;
    }
  return { lod, cx, cy, z, top, sub, subDepth, water, ice, rock, kind, deck: anyDeck ? deck : null, bed, zMin, zMax, wrap, cells: [...cellCache.values()].map((c) => ({ i: c.i, j: c.j })) };
}

/**
 * Natural ground beside a road (rs: the road sample there, with the
 * distance to the right-of-way `sdfR`): cut or filled towards the road's
 * edge over a band that widens with the height difference (at most EMBANK).
 */
function embank(world, rs, hz) {
  if (!rs.seg || rs.sdfR >= EMBANK) return hz;
  const edge = segmentLevel(world, rs.seg, rs.along) + (rs.seg.sidewalk > 0 ? 1 : 0);
  const dist = Math.max(0, rs.sdfR);
  const band = Math.max(24, Math.min(EMBANK, Math.abs(hz - edge) * 1.6));
  if (dist >= band) return hz;
  const t = dist / band;
  return Math.round(edge + (hz - edge) * t * t * (3 - 2 * t));
}

/**
 * Ground of a polygon block's lawn (the angled world, compose above): the
 * graded ground of the block (its surface eased to the pads beside it),
 * rising back to the block surface, which meets the sidewalk, within a
 * band of a slanted street's property line: a lot beside it that sits
 * lower or higher than that street leaves a slope in the verge, not a
 * cliff at the kerb.
 */
export function vergeLevel(plan, blk, x, y) {
  const s = blk.levelAt(x, y);
  const g = plan.grading.at(x, y, s, blk.id);
  if (Math.abs(s - g) < 1) return g;
  // (distance to the nearest street: a slanted edge, or a side of its rect with a road)
  let d = Infinity;
  for (const k of blk.cuts) d = Math.min(d, k.nx * (x + 0.5) + k.ny * (y + 0.5) - k.c);
  const p = blk.prop;
  if (blk.sides.N.cls) d = Math.min(d, y - p.y0);
  if (blk.sides.S.cls) d = Math.min(d, p.y1 - y);
  if (blk.sides.W.cls) d = Math.min(d, x - p.x0);
  if (blk.sides.E.cls) d = Math.min(d, p.x1 - x);
  const band = Math.min(64, Math.max(12, 1.5 * Math.abs(s - g)));
  if (d >= band) return g;
  const t = Math.max(0, d) / band;
  return Math.round(g + (s - g) * (1 - t * t * (3 - 2 * t)));
}

const pointSample = makeRoadSample();

/**
 * Height (voxels) of the natural ground at a world column as the LOD 0
 * ground tile shapes it: the terrain, cut and filled beside roads, the
 * graded surface of an urban block's leftover ground. Trees and plants of
 * the wild ask here, so they stand on the ground, not on the raw terrain.
 */
export function naturalGroundAt(world, x, y) {
  const hz = Math.round(world.terrain.sample(x, y).h);
  const c = world.cellAt(x, y);
  const plan = world.cellPlan(c.i, c.j);
  const blk = plan.blockAt ? plan.blockAt(x, y) : null;
  if (blk?.levelAt && !(world.fields.island && world.terrain.sample(x, y).coast < 3)) return blk.cuts ? vergeLevel(plan, blk, x, y) : plan.grading.at(x, y, blk.levelAt(x, y), blk.id);
  const view = world.roadView(c.i, c.j);
  const pad = view.maxReach + EMBANK;
  const cands = view.near({ x0: x - pad, y0: y - pad, x1: x + pad, y1: y + pad });
  sampleRoadSurface(cands, x + 0.5, y + 0.5, pointSample, world.seed, EMBANK);
  return embank(world, pointSample, hz);
}

const spaceSample = { mat: 0, dz: 0, water: false };

/**
 * Height (voxels) of an open space's ground at a world column as the LOD 0
 * ground tile shapes it: the block surface eased to the pads beside it,
 * the space's own dips and rises (ponds, beds), river banks and lake
 * shores carved into it. Park trees, benches and lamps ask here.
 */
export function spaceGroundAt(world, plan, space, x, y) {
  const wrap = wrapOf(world.config);
  spaceSurface(space, x, y, spaceSample, wrap.v(x), wrap.v(y));
  const base = space.levelAt ? plan.grading.at(x, y, space.levelAt(x, y), space.block) : space.groundZ;
  let z = spaceSample.dz > 0 ? base + spaceSample.dz : base + Math.min(spaceSample.dz, 0);
  const rivers = world.rivers && world.config.rivers.enabled ? world.rivers : null;
  const ri = rivers ? rivers.at(x, y) : null;
  if (ri) {
    const g = rivers.groundAt(ri, z);
    if (ri.d < ri.half) z = g;
    else if (ri.urban <= 0.5 && g < z) z = g;
  }
  const lakes = world.lakes && world.config.lakes.enabled ? world.lakes : null;
  if (lakes && !space.quay) {
    const lk = lakes.at(x, y);
    if (lk) {
      const g = lakes.groundAt(lk, z);
      if (!(g >= z && lk.k >= 1)) z = Math.min(z, g);
    }
  }
  return z;
}

/**
 * Sea-level temperature over a column tile (0..1, the land cover's climate
 * with its micro-climate), bilinear from a 3 x 3 grid: (i, j) -> t. Callers
 * cool it by altitude (LAPSE).
 */
function tileTemperature(world, bx, by, span) {
  const g = new Float32Array(9);
  let done = false;
  const cover = world.landCover;
  return (i, j) => {
    if (!done) {
      for (let b = 0; b < 3; b += 1) for (let a = 0; a < 3; a += 1) g[a + b * 3] = cover.climate(bx + (a * span) / 2, by + (b * span) / 2, 0).t;
      done = true;
    }
    const u = Math.min(1.999, (i / (P - 1)) * 2);
    const v = Math.min(1.999, (j / (P - 1)) * 2);
    const a = Math.floor(u);
    const b = Math.floor(v);
    const fu = u - a;
    const fv = v - b;
    const q = a + b * 3;
    return (g[q] * (1 - fu) + g[q + 1] * fu) * (1 - fv) + (g[q + 3] * (1 - fu) + g[q + 4] * fu) * fv;
  };
}

/** Fill the ground part of a chunk from its tile. */
export function fillGround(chunk, tile) {
  const d = chunk.data;
  const s = chunk.s;
  for (let j = 0; j < P; j += 1) {
    for (let i = 0; i < P; i += 1) {
      const idx = i + j * P;
      if (tile.deck && tile.deck[idx]) {
        fillBridgeColumn(chunk, tile, idx, i, j);
        continue;
      }
      const gz = tile.z[idx];
      const wl = tile.water[idx];
      const tm = tile.top[idx];
      const sm = tile.sub[idx];
      const sd = tile.subDepth && tile.subDepth[idx] ? tile.subDepth[idx] : sm === MAT.CONCRETE_LIGHT ? 40 : 4;
      const rk = tile.rock ? tile.rock[idx] : 0;
      // tilted, slightly warped strata (bands of 1.5-4 m) for exposed rock faces
      // (canonical coordinates: a wrapping world repeats its rock bands)
      const wx = tile.wrap ? tile.wrap.vi(chunk.wx(i)) : chunk.wx(i);
      const wy = tile.wrap ? tile.wrap.vi(chunk.wy(j)) : chunk.wy(j);
      const tilt = rk ? wx * 0.09 - wy * 0.05 + ((hash32(wx >> 6, wy >> 6, 13) & 63) - 32) * 0.4 : 0;
      for (let k = 0; k < P; k += 1) {
        const wz = chunk.wz(k);
        let m = 0;
        if (wz <= gz) {
          const depth = gz - wz;
          if (rk && depth >= s) {
            const band = Math.floor((wz + tilt) / 22);
            const h = hash32(band, rk, 7) & 7;
            m = rk === 2 ? STRATA_DRY[h] : STRATA_ROCK[h];
          } else m = depth < s ? tm : depth < sd + s ? sm : MAT.STONE;
        } else if (wz <= wl) {
          if (tile.ice && tile.ice[idx] && wl - wz < Math.max(2, s)) {
            // ice sheet with drifts of snow on top
            const drift = wl - wz < s && ((hash32(wx >> 5, wy >> 5, 77) & 7) < 2 || (hash32(wx >> 3, wy >> 3, 78) & 15) === 0);
            m = drift ? MAT.SNOW : MAT.ICE;
          } else m = MAT.WATER;
        } else {
          break;
        }
        d[idx + k * P2] = m;
      }
    }
  }
}

/**
 * River crossings: a bridge deck at road level over air, water and the
 * river bed (with piers under the carriageway and railings at the edges),
 * or a quay edge with a railing.
 */
function fillBridgeColumn(chunk, tile, idx, i, j) {
  const d = chunk.data;
  const s = chunk.s;
  const code = tile.deck[idx];
  const gz = tile.z[idx];
  const post = ((chunk.wx(i) + chunk.wy(j)) & 7) === 0;
  for (let k = 0; k < P; k += 1) {
    const wz = chunk.wz(k);
    let m = 0;
    if (code === 4) {
      if (wz <= gz) m = gz - wz < s ? tile.top[idx] : MAT.CONCRETE_LIGHT;
      else if (wz <= gz + 7 && (wz === gz + 7 || post)) m = MAT.RAILING;
    } else {
      const bz = tile.bed[idx];
      const wl = tile.water[idx];
      if (wz <= bz) m = bz - wz < s ? MAT.GRAVEL : MAT.STONE;
      else if (wz > gz - 3 && wz <= gz) m = wz > gz - s ? tile.top[idx] : MAT.CONCRETE;
      else if (code === 3 && wz <= gz - 3) m = MAT.CONCRETE;
      else if (wz <= wl) m = MAT.WATER;
      else if (code === 2 && wz > gz && wz <= gz + 7 && (wz >= gz + 6 || post)) m = MAT.RAILING;
    }
    d[idx + k * P2] = m;
  }
}

/**
 * The granted pitched road piece holding road surface sample `rs` (the
 * road's owner cell's plan, `plan.pitched`: its arc range, within its
 * right-of-way), or null.
 */
function pitchedAt(world, rs) {
  const road = rs.seg.road;
  if (!road.home) return null;
  const pieces = world.cellPlan(road.home[0], road.home[1]).pitched?.get(road.id);
  if (!pieces) return null;
  const a = rs.seg.s0 + rs.along;
  for (const p of pieces) if (p.seg === rs.seg.idx && a >= p.s0 && a <= p.s1 && Math.abs(rs.side) <= p.hr) return p;
  return null;
}

/**
 * Content z-range of a column tile (LOD0 voxel units), used to decide which
 * chunks of the tile need to be built.
 */
export function tileContentRange(world, tile) {
  let lo = tile.zMin;
  let hi = tile.zMax;
  const s = 1 << tile.lod;
  const bx = (tile.cx * CHUNK - 1) * s;
  const by = (tile.cy * CHUNK - 1) * s;
  const rect = { x0: bx, y0: by, x1: bx + P * s - 1, y1: by + P * s - 1 };
  for (const src of world.featureSources) {
    if (src.maxLod !== undefined && tile.lod > src.maxLod) continue;
    const r = src.zRange?.(world, rect, tile.lod, tile);
    if (!r) continue;
    if (r[0] < lo) lo = r[0];
    if (r[1] > hi) hi = r[1];
  }
  return [lo, hi];
}

export function buildChunk(world, lod, cx, cy, cz, tile = null, { isolated = false } = {}) {
  const chunk = new ChunkBuffer(lod, cx, cy, cz);
  if (isolated) chunk.trackIsolated();
  const t = tile ?? groundTile(world, lod, cx, cy);
  const box = chunk.worldBox;
  if (box.z0 <= t.zMax) fillGround(chunk, t);
  for (const src of world.featureSources) if (src.maxLod === undefined || lod <= src.maxLod) src.rasterize(world, chunk, t);
  return chunk;
}

/**
 * The ground alone of a chunk: the column tile's fill (terrain, road
 * surfaces, embankments, water, bridge decks) before any feature source
 * draws: what the structvox export anchors where it still stands.
 */
export function groundChunk(world, lod, cx, cy, cz, tile = null) {
  const chunk = new ChunkBuffer(lod, cx, cy, cz);
  const t = tile ?? groundTile(world, lod, cx, cy);
  if (chunk.worldBox.z0 <= t.zMax) fillGround(chunk, t);
  return chunk;
}

/** Surface parking under elevated highways (stall lines every 2.5 m). */
function underDeckParking(x, y) {
  const a = ((x % 20) + 20) % 20;
  const b = ((y % 96) + 96) % 96;
  if (b < 2) return MAT.LINE_WHITE;
  if (a === 0 && b > 8 && b < 88) return MAT.LINE_WHITE;
  return (Math.floor(x / 64) + Math.floor(y / 64)) & 1 ? MAT.ASPHALT : MAT.ASPHALT_WORN;
}
