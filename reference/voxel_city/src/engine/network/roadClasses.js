import { vx } from "../core/units.js";

/**
 * Road cross sections in voxels, derived from the metric config.
 *
 *   |sidewalk|parking|lanes...|median|...lanes|parking|sidewalk|
 *   hr = half right-of-way, hc = half carriageway (curb to curb)
 */
export const ROAD_CLASSES = ["highway", "arterial", "collector", "local", "village", "alley", "lane", "pedestrian", "rural", "path"];

export const CLASS_RANK = {
  highway: 6,
  arterial: 5,
  collector: 4,
  rural: 3,
  local: 3,
  village: 3,
  alley: 1,
  lane: 1,
  pedestrian: 2,
  path: 0,
};

const cache = new WeakMap();

export function roadSpecs(config) {
  let specs = cache.get(config);
  if (specs) return specs;
  specs = {};
  for (const [cls, c] of Object.entries(config.roads)) {
    const travel = c.lanes * c.laneWidth;
    const carriage = travel + c.median + 2 * c.parking + 2 * (c.shoulder ?? 0);
    const hc = Math.round(vx(carriage) / 2);
    const hr = hc + vx(c.sidewalk);
    specs[cls] = {
      cls,
      lanes: c.lanes,
      lane: vx(c.laneWidth),
      median: vx(c.median),
      parking: vx(c.parking),
      shoulder: vx(c.shoulder ?? 0),
      sidewalk: vx(c.sidewalk),
      hc,
      hr,
      corner: vx(c.cornerRadius),
      paved: cls !== "rural" || true,
    };
  }
  // footpaths inside parks
  specs.path = { cls: "path", lanes: 0, lane: 0, median: 0, parking: 0, shoulder: 0, sidewalk: vx(2.5), hc: 0, hr: vx(1.25), corner: vx(1), paved: true };
  cache.set(config, specs);
  return specs;
}
