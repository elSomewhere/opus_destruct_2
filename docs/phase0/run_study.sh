#!/bin/bash
# Phase 0 content study runs (see docs/phase0/PHASE0_REPORT.md). Usage: docs/phase0/run_study.sh
set -u
cd "$(dirname "$0")/../.."
BIN=./build/native-release/tools/svx_content_study
run() { # wad map mode events
  local out=docs/phase0/$2_$3
  echo "=== $1 $2 $3 ($(date +%H:%M:%S))"
  /usr/bin/time -l $BIN --wad data/freedoom/$1.wad --map $2 --mode $3 --events $4 --validate 2 --max-solve 6000000 --json $out.json > $out.log 2>&1
  grep -E "map |components|largest|statics|utilization|anchor|events|NO-BUBBLE|dUtil|validation|maximum resident" $out.log
}
run freedoom2 MAP01 rock 200
run freedoom2 MAP01 air 200
run freedoom1 E1M1 rock 200
# MAP11 (16.6M-voxel structure) needs the memory-lean Phase 1 solver (~2 KB/voxel today).
