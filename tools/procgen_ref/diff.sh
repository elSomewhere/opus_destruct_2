#!/usr/bin/env bash
# The first differences between the port's records of a stage and the reference's:
#   SVX_CITY_RECORDS=/tmp/rec build/native-release/tests/svx_city_tests -tc='*STAGE*'
#   tools/procgen_ref/diff.sh STAGE /tmp/rec
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
STAGE=${1:?stage}
DIR=${2:?records dir}
node "$HERE/dump.mjs" "$STAGE" > "$DIR/$STAGE.ref.txt"
if cmp -s "$DIR/$STAGE.ref.txt" "$DIR/$STAGE.txt"; then echo "$STAGE: identical"; exit 0; fi
diff <(nl -ba "$DIR/$STAGE.ref.txt" | cut -c1-${WIDTH:-400}) <(nl -ba "$DIR/$STAGE.txt" | cut -c1-${WIDTH:-400}) | head -${LINES_SHOWN:-40}
exit 1
