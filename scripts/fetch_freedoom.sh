#!/usr/bin/env bash
# Downloads Freedoom 0.13.0 (BSD-3-Clause) into data/freedoom/ for the Doom pipeline and tests.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p data/freedoom
if [[ -f data/freedoom/freedoom1.wad && -f data/freedoom/freedoom2.wad ]]; then
  echo "Freedoom already present"; exit 0
fi
tmp=$(mktemp -d)
curl -sSfL -o "$tmp/fd.zip" https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
unzip -q -o "$tmp/fd.zip" -d "$tmp"
cp "$tmp"/freedoom-0.13.0/freedoom1.wad "$tmp"/freedoom-0.13.0/freedoom2.wad "$tmp"/freedoom-0.13.0/COPYING.txt data/freedoom/
rm -rf "$tmp"
echo "Freedoom installed in data/freedoom/"
