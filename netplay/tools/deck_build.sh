#!/bin/bash
# Codegen + hand patch + build of the Deck tree in Docker (amd64 under Rosetta).
set -euo pipefail
T=/Users/cole/AC6_recomp_deck
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B=out/build/linux-amd64-relwithdebinfo
run() { docker run --rm --name ac6np-deck-build --platform linux/amd64 --cpus=7 --user "$(id -u):$(id -g)" \
  -v "$T":/src -w /src ac6-build:amd64 bash -lc "$1"; }
echo "== configure + codegen $(date)"
run "cmake --preset linux-amd64-relwithdebinfo >/dev/null && cmake --build $B --target ac6recomp_codegen -j 7"
echo "== patch $(date)"
python3 -I $S/deck_patch_generated.py "$T"
echo "== configure + build $(date)"
run "cmake --preset linux-amd64-relwithdebinfo >/dev/null && cmake --build $B --target ac6recomp -j 7 -- -k 0"
ls -la "$T/$B/ac6recomp"
echo "== done $(date)"
