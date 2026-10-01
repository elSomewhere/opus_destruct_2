import { islandTerrain } from "../world/island.js";

/**
 * World configuration. Every length is in METERS; generators convert with
 * `vx()` from core/units. Structural vocabularies (district types, building
 * archetypes, styles, zones) live in registries, not here — this file only
 * holds tunable numbers.
 */
export const DEFAULT_CONFIG = {
  seed: 1337,

  rendering: {
    /** Viewer-only surface extraction; generation and collision remain voxel-exact. */
    mesher: "greedy",
  },

  world: {
    /**
     * 'cities': settlements scattered in countryside; 'infiniteCity': urban
     * everywhere; 'island': one island of `island.radius` in an endless sea
     * (world/island.js)
     */
    mode: "cities",
    /**
     * 'flat' (unbounded plane), 'torus' (the plane wraps around every `size`
     * meters in x and y: a flat planet) or 'cube' (one face of a cube-sphere
     * planet), see world/chart.js
     */
    chart: "flat",
    /** period (m) of the 'torus' chart; rounded to a multiple of 240 m */
    size: 96000,
    /**
     * 'torus' chart: temperature follows a latitude that runs from an
     * equator to a pole and back once around the world (y), the spawn at
     * mid latitudes
     */
    latitude: true,
    /** planet used by the 'cube' chart: radius (m) and face (0-3 equatorial, 4 north, 5 south) */
    planet: { radius: 240000, face: 0 },
    /** wilderness band (m) along planet face edges: no settlements, sites or roads */
    faceMargin: 6000,
    seaLevel: 0,
    settlementCell: 9000,
    settlementChance: 0.55,
    cityRadius: [1600, 3600],
    /** the spawn settlement is forced at the origin with this radius */
    spawnCityRadius: 3400,
    /** villages and hamlets on a finer lattice between the towns (m) */
    villageCell: 3600,
    villageChance: 0.55,
    villageRadius: [180, 760],
    /** lattice of special sites (military bases, research complexes, strongholds), m */
    siteCell: 5200,
    /** horizontal scale of temperature / moisture variation (m): biome zone size */
    climateScale: 32000,
    /**
     * Optional climate override (null = the default noise fields):
     * { temperature, temperatureVar, moisture, moistureVar } set the mean
     * sea-level temperature / moisture (0..1) and their regional variation;
     * snowCover (0..1) lays seasonal snow on low ground, roofs and trees;
     * freeze is the local temperature below which lakes, ponds and rivers
     * freeze (the sea never does).
     */
    climate: null,
    /**
     * Island mode (world/island.js): the land is one island in an endless
     * sea; its main town sits at the origin. Lengths in meters.
     */
    island: {
      /** mean coast radius */
      radius: 8000,
      /** length / width of the island */
      elongation: 1.5,
      /** 0..1: bays, peninsulas and headlands along the coast */
      roughness: 0.5,
      /** 0..1: share of the island taken by highlands */
      highlands: 0.45,
      /** highest peaks (m) */
      peak: 950,
      /** 0..1: fjords cut into the highlands */
      fjords: 0.6,
      /** 0..1: rocky islets on the shelf */
      skerries: 0.5,
      /** 0..1: share of the coast that ends in cliffs (highland coasts always do) */
      cliffs: 0.35,
      /** width of the shallow shelf around the island */
      shelf: 2200,
      /** people in the main town (its size follows) */
      population: 20000,
      /** smaller places: a small town, villages, hamlets */
      towns: 1,
      villages: 2,
      hamlets: 2,
      /** 0..1: cabins in the woods and along the shores */
      cabins: 0.6,
      /** flavor of the main town (city/flavors.js), null = picked by climate */
      flavor: null,
    },
    /**
     * The angled world (ANGLED_WORLD_PLAN.md): streets, buildings and
     * ramps placed at exact rational angles (core/placement.js) instead of
     * only the four proper rotations. Off: the axis-aligned world, bit for
     * bit (test/golden.test.js); every preset keeps it off, angled
     * variants are presets of their own.
     */
    angles: {
      enabled: false,
      /** "triples100": the 132 exact yaws of core/placement.js; "cardinal": the four proper rotations */
      yawSet: "triples100",
      /** "grades": the pitch family matching the road classes' grade limits */
      pitchSet: "grades",
      /**
       * Physics budget (structvox, ANGLED_WORLD_PLAN.md §3.2): every oriented
       * part is a resident grid, a global cost. At most this many parts are
       * at home in one chunk...
       */
      maxPartsPerChunk: 1,
      /** ...and on average one per this much city footprint (m²): ~6-8 in a 96 m disc... */
      partArea: 3600,
      /** ...granted in squares of this many times partArea, holding as many (a run of road pieces shares one)... */
      partCluster: 4,
      /** ...and never more than this in any disc of residentRadius (structvox's resident grids round a player), whichever cells they are of... */
      maxResident: 8,
      /** ...(m): structvox's load radius (its StreamConfig.load_radius); its evict radius where a player roams back and forth */
      residentRadius: 96,
      /**
       * "grid": every part's content is drawn into the world grid as well
       * (turned walls and pitched roads stepped, as this engine's own chunk
       * meshes show them); "separate": the world grid leaves parts out, each
       * is drawn in its own lattice (world/partRaster.js), as structvox's
       * generate() must leave out what its grids hold (render-iso and
       * render-slice --parts 1)
       */
      partsMode: "grid",
      /** distance between neighbouring diagonal boulevards of a family (m), city/diagonals.js */
      diagonalSpacing: 2000,
      /**
       * roads: angled streets (S1); vegetation: organic trees, turned and
       * tilted boulders, logs and snags (S2); buildings: turned buildings
       * (S3); ramps: inclined road surfaces (S4); wings: wings, corner and
       * canted bays, parts of their own on buildings square to the grid
       * (S5: off unless asked for; the angled presets ask, the budget audit
       * showing the headroom)
       */
      features: { roads: true, vegetation: true, buildings: true, ramps: true, wings: false },
    },
  },

  /** Terrain landforms (terrain/landforms.js); heights and scales in meters. */
  terrain: {
    cityRelief: 4,
    cityReliefScale: 2600,
    lowlandBase: 45,
    continentScale: 36000,
    continentAmplitude: 110,
    hillAmplitude: 38,
    hillScale: 2800,
    detailAmplitude: 9,
    detailScale: 700,
    /** broad uplift of a range core and ridge relief on top (peaks reach ~6 km) */
    mountainUplift: 2300,
    /** uplift (m) of the foothills of every range (on top of mountainUplift) */
    mountainBase: 400,
    mountainHeight: 3700,
    mountainScale: 27000,
    /** arêtes and couloirs: mid-scale ridged relief (m) growing with height */
    mountainDetail: 340,
    mountainDetailScale: 2600,
    /** U-shaped glacial trunk valleys cut into the ranges */
    valleyScale: 9000,
    valleyDepth: 700,
    /** octave gain of the ridged relief: lower = smoother, less cliffy flanks */
    mountainGain: 0.4,
    mountainBeltScale: 200000,
    /** belt value (1 at the range crest) ramping mountainness 0 -> 1; a wide ramp = broad foothills */
    mountainBelt: [0.8, 0.99],
    mountainBeltWarp: 0.25,
    mountainMassifScale: 70000,
    mountainMassif: [0.48, 0.95],
    /** the spawn city sees a high range between these distances (km) */
    spawnMountains: [10, 35],
    /** uplift of arid country (m) */
    plateauHeight: 260,
    mesaStep: 32,
    canyonScale: 9000,
    canyonDepth: 260,
    ravineScale: 2600,
    ravineDepth: 36,
  },


  /** Macro street structure. Arterials are global lines; collectors split each arterial cell. */
  city: {
    arterialSpacing: 620,
    arterialJitter: 0.14,
    collectorUrbanThreshold: 0.35,
    ruralRoadWobble: 28,
    /** chance that an arterial-grid edge in open country carries a country road */
    ruralRoadChance: 0.6,
    lotGroundStep: 1,
  },

  /** Road cross sections (meters). */
  roads: {
    arterial: { lanes: 4, laneWidth: 3.25, median: 2.0, parking: 0, sidewalk: 5.0, cornerRadius: 6 },
    collector: { lanes: 2, laneWidth: 3.25, median: 0, parking: 2.25, sidewalk: 4.0, cornerRadius: 5 },
    local: { lanes: 2, laneWidth: 3.0, median: 0, parking: 2.0, sidewalk: 3.0, cornerRadius: 4 },
    /** village main street: two narrow lanes, slim sidewalks, no parking lanes */
    village: { lanes: 2, laneWidth: 2.9, median: 0, parking: 0, sidewalk: 1.75, cornerRadius: 4 },
    alley: { lanes: 1, laneWidth: 5.0, median: 0, parking: 0, sidewalk: 0, cornerRadius: 1.5 },
    /** old-town lane (smau / veita): a narrow cobbled passage between the houses */
    lane: { lanes: 1, laneWidth: 3.5, median: 0, parking: 0, sidewalk: 0, cornerRadius: 1 },
    pedestrian: { lanes: 0, laneWidth: 0, median: 0, parking: 0, sidewalk: 6.0, cornerRadius: 3 },
    rural: { lanes: 2, laneWidth: 3.25, median: 0, parking: 0, sidewalk: 0, shoulder: 1.25, cornerRadius: 8 },
  },

  highways: {
    enabled: true,
    nodeSpacing: 3400,
    jitter: 0.28,
    edgeChance: 0.7,
    minUrbanization: 0.12,
    deckHeight: 11,
    lanesPerSide: 2,
    laneWidth: 3.6,
    shoulder: 1.2,
    pierSpacing: 32,
    corridorMargin: 4,
  },

  /** natural caves (nature/caves.js): depth of the cave band below the surface (m) */
  caves: {
    enabled: true,
    depth: 70,
  },

  /** natural lakes (nature/lakes.js): one candidate per ~3.5 km cell */
  lakes: {
    enabled: true,
    /** lattice of lake candidates (m) */
    cell: 3500,
    chance: 0.4,
    /** share of lowland lakes that are big (1.4-4 km radius) */
    bigChance: 0.18,
  },

  /** meandering rivers (nature/rivers.js); widths in meters */
  rivers: {
    enabled: true,
    maxHalfWidth: 18,
  },

  subway: {
    enabled: true,
    lineEvery: 2,
    minUrbanization: 0.45,
    depth: 14,
    stationLength: 96,
  },

  buildings: {
    residentialStory: 3.0,
    officeStory: 3.75,
    retailStory: 4.5,
    industrialStory: 8.0,
    slab: 0.25,
    exteriorWall: 0.25,
    interiorWall: 0.125,
    doorWidth: 1.0,
    doorHeight: 2.125,
  },

  /**
   * Parked vehicles (street parking, lots, garage decks, trucks at sites).
   * Off by default: traffic is meant to be simulated later.
   */
  vehicles: { parked: false },

  streaming: {
    /** LOD0 tiles are refined while distance < lodFactor * tileSize */
    lodFactor: 5,
    /** coarsest tiles: 32 * 2^maxLod voxels (LOD 9 = 2 km tiles for 30 km views) */
    maxLod: 9,
    /** above this LOD, tiles skip street-level planning (cities become skyline blocks) */
    cityDetailLod: 6,
    /** from this LOD on forests are a canopy heightfield instead of voxel trees */
    canopyLod: 4,
    workers: 0,
  },
};

export function makeConfig(overrides = {}) {
  return wrapLattices(deepMerge(deepMerge(structuredClone(DEFAULT_CONFIG), modeDefaults(overrides)), overrides));
}

/**
 * Defaults that depend on the world mode, layered between DEFAULT_CONFIG
 * and the caller's overrides (so explicit settings still win, and merging
 * an already merged config again changes nothing): an island's terrain is
 * scaled to its size and peak height.
 */
function modeDefaults(overrides) {
  if (overrides?.world?.mode !== "island") return {};
  const island = { ...DEFAULT_CONFIG.world.island, ...(overrides.world.island ?? {}) };
  return { terrain: islandTerrain({ world: { island } }) };
}

/**
 * A wrapping world: its size is rounded to a multiple of 240 m (forest,
 * boulder and field-parcel lattices then fit a whole number of times), and
 * every coarse lattice gets an integer number of cells around the world
 * (arterials an even number, so every second one keeps its subway line).
 * Idempotent, so merging an already merged config again changes nothing.
 */
function wrapLattices(cfg) {
  const w = cfg.world;
  if (w.chart !== "torus") return cfg;
  const S = Math.max(9600, Math.round(w.size / 240) * 240);
  w.size = S;
  const fit = (spacing, even = false) => {
    const n = even ? Math.max(2, 2 * Math.round(S / spacing / 2)) : Math.max(1, Math.round(S / spacing));
    return S / n;
  };
  cfg.city.arterialSpacing = fit(cfg.city.arterialSpacing, true);
  w.settlementCell = fit(w.settlementCell);
  w.villageCell = fit(w.villageCell);
  w.siteCell = fit(w.siteCell);
  cfg.highways.nodeSpacing = fit(cfg.highways.nodeSpacing);
  cfg.lakes.cell = fit(cfg.lakes.cell);
  return cfg;
}

function deepMerge(target, src) {
  for (const [k, v] of Object.entries(src ?? {})) {
    if (v && typeof v === "object" && !Array.isArray(v) && target[k] && typeof target[k] === "object") {
      deepMerge(target[k], v);
    } else {
      target[k] = v;
    }
  }
  return target;
}
