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

