// The worlds the stages of the world base sample (tests/city/worlds.hpp is the C++ twin): every
// golden preset and size variant of the reference (scripts/lib/golden.js GOLDEN_PRESETS), plus two
// angled presets. [key, preset id, size id | null].
export const WORLDS = [
  ["cities", "cities", null],
  ["infiniteCity", "infiniteCity", null],
  ["wrapWorld:small", "wrapWorld", "small"],
  ["wrapWorld:medium", "wrapWorld", "medium"],
  ["wrapWorld:large", "wrapWorld", "large"],
  ["island:small", "island", "small"],
  ["island:medium", "island", "medium"],
  ["island:large", "island", "large"],
  ["nordicIsland:small", "nordicIsland", "small"],
  ["nordicIsland:medium", "nordicIsland", "medium"],
  ["nordicIsland:large", "nordicIsland", "large"],
  ["nordicTown:skerry", "nordicTown", "skerry"],
  ["nordicTown:fjord", "nordicTown", "fjord"],
  ["nordicTown:forest", "nordicTown", "forest"],
  ["oldHarbourTown", "oldHarbourTown", null],
  ["whiteSeaTown", "whiteSeaTown", null],
  ["planetEquator", "planetEquator", null],
  ["planetNorth", "planetNorth", null],
  ["angledCities", "angledCities", null],
  ["angledOldHarbourTown", "angledOldHarbourTown", null],
];

/** The golden sample points (metres): the spawn town's centre, its streets, the outskirts, open country, far away. */
export const GOLDEN_POINTS = [
  [0, 0],
  [180, -140],
  [1400, 900],
  [-3200, 2600],
  [7000, -5200],
];

/**
 * Sample points (voxels) of a World, drawn from r (samples()): the golden points, 1200 within
 * 40 km of the spawn, 300 within 400 km (other climates: deserts, canyons), 8 round every town
 * (within 20 km) and village (within 10 km) of the spawn, 800 over an island's bounds (its coasts),
 * and 50 off the voxel grid.
 */
export function samplePoints(w, r) {
  const pts = GOLDEN_POINTS.map(([mx, my]) => [Math.round(mx * 8), Math.round(my * 8)]);
  for (let k = 0; k < 1200; k += 1) {
    const x = Math.round((r() - 0.5) * 640000);
    const y = Math.round((r() - 0.5) * 640000);
    pts.push([x, y]);
  }
  for (let k = 0; k < 300; k += 1) {
    const x = Math.round((r() - 0.5) * 6400000);
    const y = Math.round((r() - 0.5) * 6400000);
    pts.push([x, y]);
  }
  const F = w.fields;
  const near = [...F.settlementsIn({ x0: -160000, y0: -160000, x1: 160000, y1: 160000 }), ...F.villagesIn({ x0: -80000, y0: -80000, x1: 80000, y1: 80000 })];
  for (const s of near)
    for (let k = 0; k < 8; k += 1) {
      const a = r() * 2 * Math.PI;
      const d = r() * 3 * s.radius;
      pts.push([Math.round(s.x + Math.cos(a) * d), Math.round(s.y + Math.sin(a) * d)]);
    }
  if (F.island) {
    const b = F.island.bounds();
    for (let k = 0; k < 800; k += 1) {
      const x = Math.round((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const y = Math.round((b.y0 + r() * (b.y1 - b.y0)) * 8);
      pts.push([x, y]);
    }
  }
  for (let k = 0; k < 50; k += 1) {
    const x = (r() - 0.5) * 100000;
    const y = (r() - 0.5) * 100000;
    pts.push([x, y]);
  }
  return pts;
}

/** A settlement record as a line: every field JS gives it ("-" where JS leaves it undefined or null). */
export function settlementFields(s) {
  return [s.id, s.i, s.j, s.village ?? false, s.hamlet ?? false, s.x, s.y, s.radius, s.importance, s.style, s.peak ?? 0, s.cx ?? "-", s.cy ?? "-", s.t, s.m, s.flavor ?? "-", s.island ?? false];
}
