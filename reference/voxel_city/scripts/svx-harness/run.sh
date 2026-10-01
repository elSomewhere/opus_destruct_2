#!/bin/sh
# The export in structvox itself (docs/MERGE_SVX.md §11): builds the engine's core and game
# libraries from a checkout of opus_destruct_2, builds the harness (harness.cpp: the export's dump
# behind a ChunkSource), dumps four districts through src/engine/svx and checks each: the
# materials' ids after the game's, the chunks streaming in, every part a grid at its exported
# frame, the district standing under its own weight, the lanes on the road's surface.
#
#   scripts/svx-harness/run.sh <opus_destruct_2 checkout> [build dir]
set -e
SVX=${1:?"usage: run.sh <opus_destruct_2 checkout> [build dir]"}
BUILD=${2:-/tmp/svx-harness-build}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$BUILD"
cmake -S "$SVX" -B "$BUILD/svx" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSVX_BUILD_GAME=ON -DSVX_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "$BUILD/svx" --target svx_core svx_game >/dev/null
L="$BUILD/svx"
c++ -std=c++20 -O2 -I"$SVX/core/include" -I"$SVX/game/include" "$HERE/harness.cpp" -o "$BUILD/harness" \
  "$L/libsvx_game.a" "$L/libsvx_anim.a" "$L/libsvx_env.a" "$L/libsvx_mesh.a" "$L/libsvx_core.a" -pthread
cd "$HERE/../.."
fail=0
# (a district on pitched road slabs, one of turned buildings and wings, one of the grid city's bays, and a
# junction of four elevated highways with its decks on piers)
for district in "angledNordicTown slabs 12 road" "angledOldHarbourTown harbour 12" "angledCities city 12" "infiniteCity highways 12 @428,-573"; do
  # shellcheck disable=SC2086
  set -- $district
  echo "== $1 ($2)"
  node scripts/svx-harness/dump.js "$1" "$BUILD/$2.bin" "$3" ${4:-} && "$BUILD/harness" "$BUILD/$2.bin" || fail=1
done
exit $fail
