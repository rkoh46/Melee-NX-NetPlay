#!/usr/bin/env bash
# The real-services build (MU_NET_SERVICES=1 ./build.sh linux) up to the network boundary: two headless
# instances find each other through tools/mock_matchmaking.py and play, inside ONE container with no
# network. No Slippi server, no account: the user files are made up and the service is the mock.
#   MU_DISC_DIR=<folder with GALE01.iso> mock_online_test.sh [FRAMES] [MODE]     (MODE: unranked, ranked, ...)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
frames=${1:-2400}; mode=${2:-unranked}
disc="${MU_DISC_DIR:?set MU_DISC_DIR to the folder holding GALE01.iso}"
out="$repo/build/native-engine/mock-online"; rm -rf "$out"; mkdir -p "$out/a" "$out/b"
timeout $(( frames / 60 + 120 )) docker run --rm --network none -e SLIPPI_MM_URL=ws://127.0.0.1:8765 -v "$disc":/iso:ro \
  -v "$repo/build/native-engine/out":/b:ro -v "$here":/t:ro -v "$out":/out "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -c '
  python3 /t/mock_matchmaking.py 8765 > /out/mock.txt 2>&1 & mock=$!
  sleep 1
  for side in a b; do
    seed=1; [ $side = b ] && seed=2
    (cd /out/$side && /b/mu-native-linux-services --iso /iso/GALE01.iso --out /out/$side --frames '"$frames"' --online '"$mode"' --fake-user \
       --net-inputs $seed --net-character $(( seed == 1 ? 2 : 9 )) > /out/$side/stdout.txt 2>&1) &
  done
  wait %2 %3; kill $mock 2>/dev/null; true' || echo "mock_online_test: container ended with $?"
cat "$out/mock.txt"
for side in a b; do
  echo "== $side"
  grep -E "^online|^net|slippi: (match id|opponent found|connected|online connection|matchmaking|logged in|stun|this network)|online test|online: |^stopped|Ranked" "$out/$side/stdout.txt" | tail -22 | cut -c1-220
done
