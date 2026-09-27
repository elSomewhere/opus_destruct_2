#!/bin/zsh
# The Phase 3 determinism gate: record a 10-minute scripted rooms session on native ARM with one
# thread, replay it on ARM with 2 / 4 / 8 threads, on x86-64 (Rosetta) with 1 / 4 threads and on
# WASM (Node), and diff the 60 checkpoint hashes against the record.
#
# usage: tools/replay/gate.sh OUTDIR   (builds: native-release, native-x86_64, wasm-release)
set -u
cd "$(dirname "$0")/../.."
out=${1:?usage: gate.sh OUTDIR}
mkdir -p "$out"
arm=build/native-release/tools/svx_replay
x86=build/native-x86_64/tools/svx_replay
wasm=build/wasm-release/tools/svx_replay.js
log="$out/rooms600.svxl"
SVX_REPLAY_STATS=1 $arm record --world rooms --seconds 600 --out "$log" --threads 1 > "$out/record.txt" 2>&1
echo "exit $?" > "$out/record_exit.txt"
$arm play --world rooms --log "$log" --seconds 600 --threads 2 > "$out/arm-t2.txt" 2>&1 &
$arm play --world rooms --log "$log" --seconds 600 --threads 4 > "$out/arm-t4.txt" 2>&1 &
$arm play --world rooms --log "$log" --seconds 600 --threads 8 > "$out/arm-t8.txt" 2>&1 &
arch -x86_64 $x86 play --world rooms --log "$log" --seconds 600 --threads 1 > "$out/x86-t1.txt" 2>&1 &
arch -x86_64 $x86 play --world rooms --log "$log" --seconds 600 --threads 4 > "$out/x86-t4.txt" 2>&1 &
node $wasm play --world rooms --log "$log" --seconds 600 > "$out/wasm-t1.txt" 2>&1 &
wait
ref=$(grep "^t=" "$out/record.txt" | awk '{print $2, $4}')
for f in arm-t2 arm-t4 arm-t8 x86-t1 x86-t4 wasm-t1; do
  got=$(grep "^t=" "$out/$f.txt" | awk '{print $2, $4}')
  n=$(echo "$got" | grep -c .)
  if [ "$got" = "$ref" ]; then echo "$f IDENTICAL ($n checkpoints; $(tail -1 "$out/$f.txt"))"
  else echo "$f DIFFERS (first: $(diff <(echo "$ref") <(echo "$got") | grep '^>' | head -1))"; fi
done
