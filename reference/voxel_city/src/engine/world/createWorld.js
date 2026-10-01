import { World } from "./World.js";
import { LandCover } from "../nature/landcover.js";
import { Rivers } from "../nature/rivers.js";
import { Lakes } from "../nature/lakes.js";
import { Caves, caveSource } from "../nature/caves.js";
import { Forest, forestSource } from "../nature/forest.js";
import { Boulders, boulderSource } from "../nature/boulders.js";
import { buildingSource } from "../buildings/source.js";
import { planBuilding } from "../buildings/interior/plan.js";
import { voxelizeBuilding } from "../buildings/interior/voxelize.js";
import { planDressing } from "../city/dressing.js";
import { dressingSource } from "../city/dressingSource.js";
import { LRU } from "../core/lru.js";
import { HighwayNetwork, highwaySource } from "../network/highways.js";
import { Subway, subwaySource } from "../underground/subway.js";
import { Sewers, sewerSource } from "../underground/sewers.js";
import { SiteLayer, siteSource } from "./sites.js";
import "../sites/militaryBase.js";
import "../sites/researchComplex.js";
import "../sites/mountainBase.js";
import { SiteLinks, siteLinkSource } from "../sites/links.js";
import { roadLevelAt } from "../network/roadLevel.js";
import { skybridgeSource } from "../city/skybridges.js";
import { Landmarks, landmarkSource } from "./landmarks.js";
import { groundcoverSource } from "../nature/groundcover.js";

/**
 * Build a World with the default feature sources installed. Other world
 * types (planet charts, special biomes) compose their own set here.
 */
export function createWorld(config = {}) {
  const world = new World(config);
  world.landCover = new LandCover(world);
  world.rivers = new Rivers(world);
  world.lakes = new Lakes(world);
  // harbour towns: a terrace 3 m above the water along the port lake's shore, the town rising behind it
  world.terrain.portGrade = (x, y, h) => {
    for (const s of world.fields.nearestSettlements(x, y)) {
      const L = world.lakes.portLakeOf(s);
      if (!L) continue;
      const dx = x - L.x;
      const dy = y - L.y;
      if (Math.abs(dx) > L.r0 * 1.35 + 11200 || Math.abs(dy) > L.r0 * 1.35 + 11200) continue;
      const d = (world.lakes.shoreK(L, x, y, true) - 1) * L.r0;
      if (d > 11200) continue;
      // along the town's waterfront, fading out over its outskirts: a flat
      // harbour terrace within 400 m of the shore, then the town rises
      const q = Math.hypot(x - s.x, y - s.y) / s.radius;
      const near = 1 - Math.max(0, Math.min(1, (q - 1.1) / 0.9));
      const t = Math.max(0, Math.min(1, (d - 3200) / 8000));
      const k = t * t * (3 - 2 * t);
      return h + (L.level / 8 + 3 - h) * (1 - k) * near * near * (3 - 2 * near);
    }
    return h;
  };
  world.caves = new Caves(world);
  const island = world.fields.island;
  /** Island mode: is (x, y) at sea (or within marginM of the shore)? */
  world.seaAt = (x, y, marginM = 0) => (island ? island.coast(x / 8, y / 8) < marginM : false);
  /** Is (x, y) in (or within marginM of) a river channel, a lake or the sea? */
  world.isWet = (x, y, marginM = 2) => {
    if (island && island.coast(x / 8, y / 8) < marginM) return true;
    const ri = world.rivers.at(x, y);
    if (ri && ri.d < ri.half + marginM) return true;
    const lk = world.lakes.at(x, y);
    return lk !== null && lk.k < 1 + (marginM * 8) / lk.lake.r0;
  };
  /** Island mode: does a rect (voxels) reach within marginM of the sea? Sampled every ~12 m. */
  world.seaHitsRect = (r, marginM = 6) => {
    if (!island) return false;
    const step = 96;
    for (let y = r.y0; y <= r.y1 + step - 1; y += step)
      for (let x = r.x0; x <= r.x1 + step - 1; x += step) if (island.coast(Math.min(x, r.x1) / 8, Math.min(y, r.y1) / 8) < marginM + 8.5) return true;
    return false;
  };
  /** Share (0..1) of a rect (voxels) that lies in the sea, from a 5 x 5 sample. */
  world.seaShare = (r) => {
    if (!island) return 0;
    let n = 0;
    for (let j = 0; j < 5; j += 1)
      for (let i = 0; i < 5; i += 1) if (island.coast((r.x0 + ((r.x1 - r.x0) * (i + 0.5)) / 5) / 8, (r.y0 + ((r.y1 - r.y0) * (j + 0.5)) / 5) / 8) < 0) n += 1;
    return n / 25;
  };
  /** Island mode: does the segment a-b (voxels) cross the sea (within marginM of it)? */
  world.seaHitsSeg = (ax, ay, bx, by, marginM = 4) => {
    if (!island) return false;
    const n = Math.max(2, Math.ceil(Math.hypot(bx - ax, by - ay) / 240));
    for (let k = 0; k <= n; k += 1) if (island.coast((ax + ((bx - ax) * k) / n) / 8, (ay + ((by - ay) * k) / n) / 8) < marginM) return true;
    return false;
  };
  /** Open water (a lake or the sea) at a point? */
  world.openWaterAt = (x, y) => {
    if (island && island.coast(x / 8, y / 8) < 0) return true;
    const lk = world.lakes.at(x, y);
    return !!lk && lk.k < 1;
  };
  /**
   * Nearest shore a harbour can face: a big lake's, or on an island the
   * sea's at the town's harbour. { level (voxels), dist (voxels to the
   * water's edge, negative in the water), nx, ny (unit vector pointing
   * inland) } or null.
   */
  world.shoreNear = (x, y, maxDist) => {
    const lk = world.lakes.shoreNear(x, y, maxDist);
    if (lk) return { level: lk.lake.level, dist: lk.dist, nx: lk.nx, ny: lk.ny, lake: lk.lake };
    if (!island) return null;
    const c = island.coast(x / 8, y / 8);
    if (Math.abs(c) * 8 > maxDist) return null;
    const e = 6;
    const gx = island.coast(x / 8 + e, y / 8) - c;
    const gy = island.coast(x / 8, y / 8 + e) - c;
    const g = Math.hypot(gx, gy) || 1;
    return { level: Math.round(world.config.world.seaLevel * 8), dist: c * 8, nx: gx / g, ny: gy / g, lake: null };
  };
  /** Street surface level (voxels) at a point: the nearest road's graded level, else the terrain. */
  world.streetLevel = (x, y) => {
    const c = world.cellAt(x, y);
    const r = roadLevelAt(world, world.roadView(c.i, c.j), x, y, 24);
    return r ? r.z : world.terrain.sample(x, y).h;
  };
  /** Does a rect touch open water (rivers, lakes)? */
  world.waterHitsRect = (r, marginM = 6) => world.seaHitsRect(r, marginM) || world.rivers.hitsRect(r, marginM) || world.lakes.hitsRect(r, marginM);
  // island landmarks: lighthouse, boathouses, fish racks, cairns (world/landmarks.js)
  world.landmarks = island ? new Landmarks(world) : null;
  world.forest = new Forest(world);
  world.boulders = new Boulders(world);
  const cfg = world.config;
  // small places (islands) go without elevated highways and a subway
  world.highways = cfg.highways.enabled === false ? null : new HighwayNetwork(world);
  world.subway = cfg.subway.enabled === false ? null : new Subway(world);
  world.sewers = new Sewers(world);
  world.sites = new SiteLayer(world);
  world.siteLinks = new SiteLinks(world);
  const dressings = new LRU(32);
  world.dressing = (i, j) => dressings.getOrCreate(`${i},${j}`, () => planDressing(world, i, j));
  // (keyed by position too: a wrapping world has the same building, same id, once per lap)
  world.buildingPlan = (env) => world.buildingPlans.getOrCreate(`${env.id}@${env.R.x0},${env.R.y0}`, () => planBuilding(world, env));
  world.voxelizeBuilding = (env, chunk) => voxelizeBuilding(world, env, chunk);
  /** Openings in the ground (subway / sewer stairs, open manholes) that street props must avoid. */
  world.blocksSurface = (x, y) => (world.subway ? world.subway.blocksSurface(x, y) : false) || world.sewers.blocksSurface(x, y);
  world.addFeatureSource(caveSource);
  world.addFeatureSource(sewerSource);
  if (world.subway) world.addFeatureSource(subwaySource);
  world.addFeatureSource(siteLinkSource);
  world.addFeatureSource(siteSource);
  if (world.highways) world.addFeatureSource(highwaySource);
  world.addFeatureSource(buildingSource);
  world.addFeatureSource(dressingSource);
  world.addFeatureSource(skybridgeSource);
  world.addFeatureSource(forestSource);
  world.addFeatureSource(boulderSource);
  world.addFeatureSource(groundcoverSource);
  if (world.landmarks) world.addFeatureSource(landmarkSource);
  return world;
}
