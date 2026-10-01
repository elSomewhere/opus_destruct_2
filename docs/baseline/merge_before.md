# Before the merge: the engine's numbers at `0eda3ca` (merge_procgen_1, 2026-10-01)

Recorded on a 4-core Linux container (GCC 13.3, Release), 4 threads, before any change of the
merge (docs/PROCGEN_MERGE_PLAN.md §11, phase 0). Later phases compare against these.

## Tests

`ctest`: svx_core_tests, svx_env_tests, svx_anim_tests, svx_game_tests all pass (164 s).

## Golden hashes (`tools/baseline/golden.sh build/native-release/tools --threads 4`)

All 19 lines (7 parity, 12 engine) reproduce `tools/baseline/golden.txt` (93 s).

## The reference district testbed (`svx_district_check`)

`node tools/procgen_ref/district.mjs infiniteCity downtown.bin 6 @0,0` (13 x 13 columns at the
origin of the infinite city, 270 chunks with content, 36,598 prop voxels; 7.5 s in Node):

- streamed (load radius 40 m): 1,183 resident chunks for 270 with content (every column whole
  from the extent's bottom to its top: plan §8.1); grid memory 17.4 MB, 66.6 KB a chunk (voxels
  plus the look layer: plan §8.4);
- a first shot (0.25 m, 2 kJ) at the building with the most free voxels within 30 m: its tick
  152 ms (25,183 nodes extracted, 63,416 voxels strengthened by the design; plan §8.5).
