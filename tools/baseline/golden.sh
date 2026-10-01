#!/usr/bin/env bash
# The engine's pinned behaviour (docs/BASELINE.md): scenarios run on a build, their world hashes
# checked against tools/baseline/golden.txt. Each line of that file is
#
#   <mode> <world hash> <tool> <arguments...>
#
# mode "engine": the engine as it is (its defaults) - a change of behaviour shows here, and is
#   pinned again deliberately (--update) with the change that makes it;
# mode "parity": the engine with the switches that restore the structural reference's solver
#   (compare.sh --parity), against the reference's own hash (oriented_1, built with clang): the
#   engine still reproduces it bit for bit. These never change.
#
# usage: tools/baseline/golden.sh TOOLS_DIR [--update] [--threads T] [--filter REGEX]
#   TOOLS_DIR: a build's tools/ (svx_engine_demo, svx_env_bench)
#   --update: write the engine lines' hashes as they are now (parity lines are never rewritten)
# Exit status 1 if any hash differs. A scenario's hash is the same on any thread count, platform
# and compiler (docs/CORE.md: determinism).
set -u
DIR=${1:?tools dir}
shift
UPDATE=0
THREADS=4
FILTER=
while [ $# -gt 0 ]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    --threads) THREADS=$2; shift 2 ;;
    --filter) FILTER=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
HERE=$(cd "$(dirname "$0")" && pwd)
GOLDEN=$HERE/golden.txt
PARITY=(--tune impact_penetration=0 --tune restart_diverging_solves=0 --tune jointed_keep_identity=0
  --tune spread_per_partner=0 --tune plastic_hinges=0 --tune rigid.piece_ccd=0 --tune load_trigger_gap=0
  --tune design_in_place=0 --tune patch_cut_structures=0 --tune evict_scan_ticks=1 --tune release_solvers=0
  --tune recheck_vacated=0 --tune true_solve_work=0 --tune rebuild_stale_only=0 --tune rigid.busy_hold=0
  --tune rigid.warm_to_step=0 --tune coarsen_dense_levels=0 --tune reaggregate_levels=0
  --tune shards_hold_together=0 --tune cluster_cubes=0 --tune fair_solve_order=0)

world_hash() {  # (the world's hash from a tool's output: the last "world <hex>")
  printf '%s\n' "$1" | grep -o 'world [0-9a-f]\{16\}' | tail -1 | grep -o '[0-9a-f]*$'
}

fail=0
out=()
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    ''|'#'*) out+=("$line"); continue ;;
  esac
  read -r mode want tool args <<<"$line"
  if [ -n "$FILTER" ] && ! printf '%s' "$line" | grep -Eq "$FILTER"; then
    out+=("$line")
    continue
  fi
  extra=()
  [ "$mode" = parity ] && extra=("${PARITY[@]}")
  t0=$(date +%s.%N)
  # shellcheck disable=SC2086
  res=$("$DIR/svx_$tool" $args "${extra[@]}" --threads "$THREADS" 2>&1)
  secs=$(echo "$(date +%s.%N) - $t0" | bc)
  got=$(world_hash "$res")
  if [ "$got" = "$want" ]; then
    verdict=ok
  elif [ "$UPDATE" = 1 ] && [ "$mode" = engine ] && [ -n "$got" ]; then
    verdict="updated (was $want)"
    line="$mode $got $tool $args"
  else
    verdict="DIFF (got ${got:-none})"
    fail=1
  fi
  out+=("$line")
  printf '%-7s %-16s %6.1fs  %-60s %s\n' "$mode" "$want" "$secs" "$tool $args" "$verdict"
done <"$GOLDEN"
if [ "$UPDATE" = 1 ]; then
  printf '%s\n' "${out[@]}" >"$GOLDEN"
fi
exit $fail
