#!/usr/bin/env bash
# The engine's behaviour against a reference build (docs/BASELINE.md): the structural scenarios of
# svx_engine_demo and the environment scenarios of svx_env_bench, run on both builds, compared
# by what they make of the world - the world's session hash (its whole state, bit for bit), the
# pieces, the bonds broken, the voxels detached - and by what they cost (wall time, tick mean).
#
# usage: tools/baseline/compare.sh REF_TOOLS_DIR NEW_TOOLS_DIR [--threads T] [--quick] [--no-env]
#                                  [--parity] [--new-tune NAME=VALUE ...]
#   REF_TOOLS_DIR, NEW_TOOLS_DIR: build directories' tools/ (svx_engine_demo, svx_env_bench)
#   --quick: the short scenarios only (a CI gate); --no-env: no environment scenarios
#   --new-tune: a tunable set on the new build's worlds (not the reference's)
#   --parity: the new build with the switches that restore the oriented_1 reference's solver
#             (docs/BASELINE.md: everything it added since, off)
# Exit status 1 if any scenario's hash differs (the table says how: same outcome counts, or not).
# A build that prints no world hash (older than the flag) is compared by the hash it prints.
set -u
REF=${1:?reference tools dir}
NEW=${2:?new tools dir}
shift 2
THREADS=4
QUICK=0
ENV=1
NEW_TUNES=()
PARITY_TUNES=(impact_penetration=0 restart_diverging_solves=0 jointed_keep_identity=0 spread_per_partner=0 plastic_hinges=0
  rigid.piece_ccd=0 load_trigger_gap=0 design_in_place=0 patch_cut_structures=0 evict_scan_ticks=1)
while [ $# -gt 0 ]; do
  case "$1" in
    --threads) THREADS=$2; shift 2 ;;
    --quick) QUICK=1; shift ;;
    --no-env) ENV=0; shift ;;
    --new-tune) NEW_TUNES+=(--tune "$2"); shift 2 ;;
    --parity) for t in "${PARITY_TUNES[@]}"; do NEW_TUNES+=(--tune "$t"); done; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

SCENARIOS=(
  "--world tower --scenario side --seconds 5"
  "--world tower --scenario core --seconds 5"
  "--world tower --scenario pillars --seconds 5 --turn 30"
  "--world rooms --seconds 5"
  "--world slab --seconds 5"
  "--world bridge --seconds 5"
)
if [ "$QUICK" = 0 ]; then
  SCENARIOS+=(
    "--world tower --scenario all --seconds 5"
    "--world tower --scenario rockets --seconds 5"
    "--world tower --scenario pillars --seconds 8"
    "--world chimney --seconds 5"
    "--world yard --seconds 8"
    "--world machines --seconds 12"
    "--world angles --seconds 6"
    "--world city --seconds 6"
  )
fi

# "done: 300 ticks in 4.1 s wall; voxels 1123210, pieces 9, broken 32, detached 502 voxels, hash a7ec..., world 51f0..."
# run_demo DIR TUNES_COUNT [TUNES...] SCENARIO ARGS...
run_demo() {
  local bin=$1/svx_engine_demo
  local n=$2
  shift 2
  local tunes=("${@:1:$n}")
  shift "$n"
  local out
  out=$("$bin" "$@" "${tunes[@]}" --threads "$THREADS" 2>&1)
  local done_line
  done_line=$(printf '%s\n' "$out" | grep '^done:' | tail -1)
  local wall pieces broken detached hash mean
  wall=$(printf '%s' "$done_line" | sed -n 's/.* in \([0-9.]*\) s wall.*/\1/p')
  pieces=$(printf '%s' "$done_line" | sed -n 's/.*pieces \([0-9]*\).*/\1/p')
  broken=$(printf '%s' "$done_line" | sed -n 's/.*broken \([0-9]*\).*/\1/p')
  detached=$(printf '%s' "$done_line" | sed -n 's/.*detached \([0-9]*\) voxels.*/\1/p')
  hash=$(printf '%s' "$done_line" | sed -n 's/.*world \([0-9a-f]*\).*/\1/p')
  [ -n "$hash" ] || hash=$(printf '%s' "$done_line" | sed -n 's/.*hash \([0-9a-f]*\).*/\1/p')
  echo "${hash:-none} ${pieces:-?} ${broken:-?} ${detached:-?} ${wall:-?}"
}

fail=0
printf '%-52s | %-16s %6s %7s %8s %6s | %-16s %6s %7s %8s %6s | %s\n' "scenario" "ref hash" "pieces" "broken" "detached" "wall" "new hash" "pieces" "broken" "detached" "wall" "verdict"
for s in "${SCENARIOS[@]}"; do
  # shellcheck disable=SC2086
  read -r ha pa ba da wa <<<"$(run_demo "$REF" 0 $s)"
  # shellcheck disable=SC2086
  read -r hb pb bb db wb <<<"$(run_demo "$NEW" ${#NEW_TUNES[@]} "${NEW_TUNES[@]}" $s)"
  if [ "$ha" = "$hb" ] && [ "$ha" != none ]; then
    verdict=SAME
  elif [ "$pa" = "$pb" ] && [ "$ba" = "$bb" ] && [ "$da" = "$db" ]; then
    verdict="DIFF (same counts)"
    fail=1
  else
    verdict=DIFF
    fail=1
  fi
  printf '%-52s | %-16s %6s %7s %8s %6s | %-16s %6s %7s %8s %6s | %s\n' "$s" "$ha" "$pa" "$ba" "$da" "$wa" "$hb" "$pb" "$bb" "$db" "$wb" "$verdict"
done

if [ "$ENV" = 1 ]; then
  env_hash() {  # (the world's hash when the bench prints it, else the session's)
    local h
    h=$(printf '%s\n' "$1" | grep -o 'world [0-9a-f]\{16\}' | tail -1 | grep -o '[0-9a-f]*$')
    [ -n "$h" ] || h=$(printf '%s\n' "$1" | grep -o 'hash [0-9a-f]\{16\}' | tail -1 | grep -o '[0-9a-f]*$')
    echo "$h"
  }
  for sc in fire flood city; do
    [ "$QUICK" = 1 ] && [ "$sc" = city ] && continue
    a=$("$REF/svx_env_bench" --scenario $sc --threads "$THREADS" 2>&1)
    b=$("$NEW/svx_env_bench" --scenario $sc --threads "$THREADS" "${NEW_TUNES[@]}" 2>&1)
    ha=$(env_hash "$a")
    hb=$(env_hash "$b")
    ma=$(printf '%s\n' "$a" | grep -io 'mean[ =:]*[0-9.]*' | head -1 | grep -o '[0-9.]*$')
    mb=$(printf '%s\n' "$b" | grep -io 'mean[ =:]*[0-9.]*' | head -1 | grep -o '[0-9.]*$')
    if [ -n "$ha" ] && [ "$ha" = "$hb" ]; then verdict=SAME; else verdict=DIFF; fail=1; fi
    printf '%-52s | %-16s %6s %7s %8s %6s | %-16s %6s %7s %8s %6s | %s\n' "env_bench --scenario $sc" "$ha" "" "" "" "${ma}ms" "$hb" "" "" "" "${mb}ms" "$verdict"
  done
fi
exit $fail
