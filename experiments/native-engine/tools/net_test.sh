#!/usr/bin/env bash
# Two headless instances of the Linux test runner play each other through the engine's own online
# code and the app's Slippi netplay, inside ONE container with no network (loopback only).
#   net_test.sh FRAMES DELAY_MS JITTER_MS [extra runner arguments for both instances...]
# Output: build/native-engine/net-test/{a,b}/ (run.log, digest CSV) and a summary on stdout.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
frames=${1:-3000}; delay=${2:-0}; jitter=${3:-0}; shift $(( $# < 3 ? $# : 3 ))
disc="${MU_DISC_DIR:?set MU_DISC_DIR to the folder holding GALE01.iso}"
out="$repo/build/native-engine/net-test"; rm -rf "$out"; mkdir -p "$out/a" "$out/b"
limit=$(( frames / 60 + 90 ))
timeout "$limit" docker run --rm --network none -v "$disc":/iso:ro -v "$repo/build/native-engine/out":/b:ro \
  -v "$repo/experiments/native-engine/tools":/t:ro -v "$out":/out "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -c '
  frames=$1; delay=$2; jitter=$3; shift 3
  if [ "$delay" != 0 ] || [ "$jitter" != 0 ]; then
    python3 /t/udp_delay_relay.py 51001 52001 51002 52002 "$delay" "$jitter" & relay=$!
    peer_of_a=52002; peer_of_b=52001
  else peer_of_a=51002; peer_of_b=51001; fi
  cd /out/a && /b/mu-native-linux --iso /iso/GALE01.iso --out /out/a --frames "$frames" --net 0:51001:127.0.0.1:$peer_of_a "$@" > /out/a/stdout.txt 2>&1 & a=$!
  cd /out/b && /b/mu-native-linux --iso /iso/GALE01.iso --out /out/b --frames "$frames" --net 1:51002:127.0.0.1:$peer_of_b "$@" > /out/b/stdout.txt 2>&1 & b=$!
  wait $a; ra=$?; wait $b; rb=$?
  [ -n "${relay:-}" ] && kill $relay 2>/dev/null
  echo "instance exit codes: a=$ra b=$rb"' net "$frames" "$delay" "$jitter" "$@" || echo "net_test: container ended with $?"
python3 -I "$here/compare_net_digests.py" "$out/a/digest-net-confirmed.csv" "$out/b/digest-net-confirmed.csv" | sed 's/^/confirmed states: /'
for side in a b; do
  echo "== $side"
  grep -E "^net|^scenes|^stopped|selftest|DIVERGED|desync|Desync|rollback done|cannot|panic" "$out/$side/stdout.txt" | grep -v "^\[net\] slippi: \(netplay\|ping\)" | tail -25 | cut -c1-230
done
