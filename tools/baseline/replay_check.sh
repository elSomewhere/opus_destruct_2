#!/usr/bin/env bash
# Replay determinism (docs/PROCGEN_MERGE_PLAN.md §11 phase 4): records a scripted session of a
# world on 4 threads, replays it on 1, 4 and 8 threads - and under Node from a WASM build when
# WASM_TOOLS is given - and checks that every replay prints the recording's session hashes (every
# 10 s of simulated time and at the end).
#
#   tools/baseline/replay_check.sh TOOLS WORLD [SECONDS] [-- record options]
#     TOOLS   the native build's tools (build/native-release/tools)
#     WORLD   preset:ID | rooms | tower | city | wad:PATH:MAP (svx_replay --world)
#   WASM_TOOLS=build/wasm-release/tools (svx_replay.js) adds the replay under Node.
set -u
TOOLS=${1:?tools dir}
WORLD=${2:?world}
SECONDS_=${3:-30}
shift 3 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
hashes() { grep -E '^t=' "$1" | sed -E 's/^t= *([0-9.]+)s hash ([0-9a-f]+).*/\1 \2/'; }
"$TOOLS/svx_replay" record --world "$WORLD" --seconds "$SECONDS_" --threads 4 --out "$TMP/session.svxl" "$@" > "$TMP/record.txt" 2>&1 || {
  echo "record failed"; cat "$TMP/record.txt"; exit 2; }
hashes "$TMP/record.txt" > "$TMP/want.txt"
[ -s "$TMP/want.txt" ] || { echo "no hashes recorded"; cat "$TMP/record.txt"; exit 2; }
fail=0
check() {  # label, output file
  hashes "$2" > "$TMP/got.txt"
  if cmp -s "$TMP/want.txt" "$TMP/got.txt"; then
    echo "$1: ok ($(wc -l < "$TMP/got.txt") hashes, last $(tail -1 "$TMP/got.txt" | cut -d' ' -f2))"
  else
    echo "$1: DIFF"; diff "$TMP/want.txt" "$TMP/got.txt" | head -6; fail=1
  fi
}
for t in 1 4 8; do
  "$TOOLS/svx_replay" play --world "$WORLD" --log "$TMP/session.svxl" --seconds "$SECONDS_" --threads "$t" > "$TMP/play$t.txt" 2>&1
  check "native, $t threads" "$TMP/play$t.txt"
done
if [ -n "${WASM_TOOLS:-}" ]; then
  node "$WASM_TOOLS/svx_replay.js" play --world "$WORLD" --log "$TMP/session.svxl" --seconds "$SECONDS_" --threads 4 > "$TMP/wasm.txt" 2>&1
  check "wasm (node), 4 threads" "$TMP/wasm.txt"
fi
exit $fail
