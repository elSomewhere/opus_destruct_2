# Presets: worlds as data

A preset is a world to load, written as data: `data/presets/<group>/<name>.json`. Every build
carries the presets (CMake embeds them at build time: `cmake/embed_presets.cmake`, WASM included),
and a new preset needs no code as long as its generator exists.

- **C ABI** (`svx/game/api/svx_api.h`): `svx_load_preset(engine, id, seed)` loads one (seed 0: the
  preset's own; non-zero on failure, `svx_last_error` says why); `svx_presets(engine)` lists them as
  JSON (`id`, `label`, `group`, `description`, `generator`, `experimental`, `available`);
  `svx_default_preset()` is the default world's id; `svx_preset_atmosphere(engine)` is the loaded
  preset's atmosphere for the front end. `svx_load_procedural` keeps the legacy names.
- **C++** (`svx/procgen/presets.hpp`): `presets()`, `find_preset(id)`, `parse_preset(json)`,
  `load_preset(game, preset, seed, h)`.

## Format

```json
{
  "id": "city/angledInfiniteCity",          // its path under data/presets, without .json
  "label": "Infinite city, angled",
  "group": "city",                          // "city" or "legacy"
  "description": "...",
  "generator": "city",                      // drive | city1km | level | city
  "level": "tower",                         // (generator "level": the procedural level's kind)
  "params": { "preset": "angledInfiniteCity", "size": "medium", "season": "winter" },
  "seed": 1337,                             // used when the caller gives none
  "experimental": true,                     // (listed as such: not on the critical path)
  "streaming": { "load_radius": 112, "evict_radius": 144, "chunks_per_tick": 48,
                 "archive_mb": 128, "max_resident_mb": 0, "forget_after_s": 0 },
  "far": { "radius": 520, "tile": 8, "factor": 8, "tiles_per_tick": 1 },
  "tunables": { "pretouch_radius": 48 },    // world tunables by name (svx/world/tunables.hpp)
  "env": { },                               // environment parameters by name (svx/env/env.hpp)
  "traffic": { "enabled": true, "cars": 14, "parked": 18 },
  "pedestrians": { "enabled": true, "count": 24, "bodies": 2 },
  "atmosphere": { "sky": "#a6bdd0", "fog": 1.25, "time_of_day": 13 },   // for the front end
  "spawn": { "pos": [0, 0, 1.8], "dir": [1, 0, 0] }
}
```

Every field but `id` and `generator` is optional; a field of the wrong kind refuses the whole
preset (`parse_preset` names it), and an unknown tunable or environment parameter fails the load.
The tests (`tests/game/test_presets.cpp`) parse every preset the build carries and check that the
legacy presets load exactly what `svx_load_procedural` loads.

## Generators

| Generator | World | Parameters |
| --- | --- | --- |
| `drive` | the endless drive city (`procgen/drive_city.hpp`), streamed | - |
| `city1km` | the 1 km streamed city, some buildings turned (`procgen/city.hpp`) | - |
| `level` | a bounded procedural level (`procgen/levels.hpp`): `level` names it | - |
| `city` | the city generator ([`CITY.md`](CITY.md)), streamed | `preset` (a voxel_city preset id), `size`, `season`, `config` (overrides) |

## The presets

- **City** (the default world, once the city generator is integrated: `city/angledInfiniteCity`,
  seed 1337): `angledInfiniteCity`, `infiniteCity` (the axis-aligned control of benches and
  tests), `cities`, `angledCities`, `island`, `nordicIsland` (each with `-small` and `-large`
  variants), `nordicTown`, `angledNordicTown`, `oldHarbourTown`, `angledOldHarbourTown`,
  `whiteSeaTown`; experimental: `wrapWorld` (an unwrapped plane), `planetEquator`, `planetNorth`
  (one flat face; gravity stays vertical). They stream with a 112 m load radius and pre-touch
  structures within 48 m.
- **Legacy** (the baseline's and the tests' worlds, unchanged): `legacy/drive` (the default until
  the city is integrated), `legacy/city1km`, and the test levels `rooms`, `tower`, `yard`, `slab`,
  `chimney`, `bridge`, `angles`, `machines`. Doom maps load through `svx_load_wad`.
