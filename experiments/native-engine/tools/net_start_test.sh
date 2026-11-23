#!/usr/bin/env bash
# The awkward start of a console-versus-computer match, reproduced on one machine (one container, no
# network): the two instances start STAGGER seconds apart, and the later one reaches the earlier one
# through tools/udp_nat_forwarder.py, so the earlier one sees it under a source port it was never
# told (as behind a published container port) while its own connect goes nowhere.
#   MU_DISC_DIR=<folder with GALE01.iso> net_start_test.sh [STAGGER_SECONDS] [RETRY_SECONDS] [FRAMES_AFTER_START] [relay DELAY_MS JITTER_MS]
# With "relay" the first instance also sits behind tools/udp_delay_relay.py, as tools/net_peer.sh --delay sets it up.
# Output: build/native-engine/net-start/{first,second}/ and a summary.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
stagger=${1:-20}; retry=${2:-90}; frames=${3:-2400}; relay=${4:-}; delay=${5:-40}; jitter=${6:-30}
disc="${MU_DISC_DIR:?set MU_DISC_DIR to the folder holding GALE01.iso}"
out="$repo/build/native-engine/net-start"; rm -rf "$out"; mkdir -p "$out/first" "$out/second"
timeout $(( stagger + frames / 60 + 120 )) docker run --rm --network none -v "$disc":/iso:ro -v "$repo/build/native-engine/out":/b:ro \
  -v "$here":/t:ro -v "$out":/out "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -c '
  stagger=$1; retry=$2; frames=$3; relay=$4; delay=$5; jitter=$6
  first_net=0:51000:127.0.0.1:59999; target=51000
  if [ "$relay" = relay ]; then
    # As net_peer.sh --delay: the relay owns the port the forwarder delivers to; the instance talks to it over loopback.
    python3 /t/udp_delay_relay.py 51900 53000 59999 51901 "$delay" "$jitter" 0 1 127.0.0.1 & relay_pid=$!
    first_net=0:51900:127.0.0.1:51901; target=53000
  fi
  python3 /t/udp_nat_forwarder.py 52000 $target > /out/forwarder.txt 2>&1 & forwarder=$!
  # First (the computer in the real case): listens on 51000 behind the "published port" 52000. The peer
  # port it is given (59999) answers nothing, like the gateway address behind a published port.
  (cd /out/first && /b/mu-native-linux --iso /iso/GALE01.iso --out /out/first --frames $(( frames + stagger * 60 )) \
     --net $first_net --net-retry "$retry" > /out/first/stdout.txt 2>&1) & first=$!
  sleep "$stagger"
  # Second (the console in the real case): dials the published port.
  (cd /out/second && /b/mu-native-linux --iso /iso/GALE01.iso --out /out/second --frames "$frames" \
     --net 1:51001:127.0.0.1:52000 --net-retry "$retry" > /out/second/stdout.txt 2>&1) & second=$!
  wait $first; rf=$?; wait $second; rs=$?
  kill $forwarder ${relay_pid:-} 2>/dev/null
  echo "instance exit codes: first=$rf second=$rs"' start "$stagger" "$retry" "$frames" "$relay" "$delay" "$jitter" || echo "net_start_test: container ended with $?"
cat "$out/forwarder.txt"
for side in first second; do
  echo "== $side"
  grep -E "^net: (attempt|connected|player|[0-9]+ state)|^net check|slippi: (connected to|online connection|incoming ENet|a connection attempt|disconnect cause)|online: match starts|matchmaking failed|^stopped" "$out/$side/stdout.txt" | cut -c1-210 | head -24
done
