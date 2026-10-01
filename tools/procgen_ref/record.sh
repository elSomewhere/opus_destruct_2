#!/usr/bin/env bash
# Records the reference's digest of conformance stages in tests/city/conformance.txt (one line
# per stage: "<stage> <first 16 hex digits of the SHA-256 of its records>"), the digests the C++
# port's tests check (tests/city/records.hpp). Run it when a stage is added or the pinned
# reference changes - never to make a failing port pass.
#
#   tools/procgen_ref/record.sh STAGE...      (the reference's node_modules are not needed)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FILE=$ROOT/tests/city/conformance.txt
touch "$FILE"
for stage in "$@"; do
  digest=$(node "$HERE/dump.mjs" "$stage" | sha256sum | cut -c1-16)
  grep -v "^$stage " "$FILE" > "$FILE.tmp" || true
  echo "$stage $digest" >> "$FILE.tmp"
  sort "$FILE.tmp" > "$FILE"
  rm -f "$FILE.tmp"
  echo "$stage $digest"
done
