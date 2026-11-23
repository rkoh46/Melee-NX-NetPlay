#!/usr/bin/env bash
# One headless instance of the Linux test runner as the network peer of another machine (the console)
# in a local-peer match. This is the ONLY script here that gives a game-running container a network:
# the default bridge with one published UDP port. The game image imports only C library functions;
# the network code in the process is this repository's own netplay.
#
#   MU_DISC_DIR=<folder with GALE01.iso> net_peer.sh INDEX PEER_IP PEER_PORT [options] [-- runner arguments]
#     INDEX       0 or 1: which player this instance is (the other machine takes the other number)
#     PEER_IP     the other machine's address on the local network
#     PEER_PORT   the UDP port the other machine listens on (its --net LOCAL_PORT)
#     --port N    the UDP port published here for the other machine to send to (default 51000)
#     --frames N  retraces to run (default 7200, two minutes)
#     --delay MS --jitter MS   put a delay relay between this instance and the network
#   Anything after -- goes to the runner (for example --selftest 30:5 --net-delay 2).
# The other machine runs:  --net <its index>:PEER_PORT:<this machine's address>:<--port value> --frames N
# Output: build/native-engine/net-peer/ (stdout.txt, run.log, digest-net-confirmed.csv).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
[ $# -ge 3 ] || { sed -n '2,17p' "$0"; exit 2; }
index=$1; peer_ip=$2; peer_port=$3; shift 3
port=51000; frames=7200; delay=0; jitter=0
while [ $# -gt 0 ]; do
  case "$1" in
    --port) port=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --delay) delay=$2; shift 2 ;;
    --jitter) jitter=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "net_peer: unknown option $1" >&2; exit 2 ;;
  esac
done
disc="${MU_DISC_DIR:?set MU_DISC_DIR to the folder holding GALE01.iso}"
out="$repo/build/native-engine/net-peer"; rm -rf "$out"; mkdir -p "$out"
limit=$(( frames / 60 + 180 ))
name="mu-net-peer-$$"
set +e
timeout "$limit" docker run --name "$name" --rm -p "$port:$port/udp" -v "$disc":/iso:ro -v "$repo/build/native-engine/out":/b:ro \
  -v "$here":/t:ro -v "$out":/out "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -c '
  index=$1; peer_ip=$2; peer_port=$3; port=$4; frames=$5; delay=$6; jitter=$7; shift 7
  if [ "$delay" != 0 ] || [ "$jitter" != 0 ]; then
    # The relay owns the published port; the instance talks to it over loopback.
    python3 /t/udp_delay_relay.py 51900 "$port" "$peer_port" 51901 "$delay" "$jitter" 0 1 "$peer_ip" & relay=$!
    net="$index:51900:127.0.0.1:51901"
  else net="$index:$port:$peer_ip:$peer_port"; fi
  cd /out && /b/mu-native-linux --iso /iso/GALE01.iso --out /out --frames "$frames" --net "$net" "$@" 2>&1 | tee /out/stdout.txt | grep --line-buffered -E "^net|^\[net\] slippi: (local peer|connected|online connection|peer connected|received selections)|online test|online: |^stopped|^scenes|^---- "
  [ -n "${relay:-}" ] && kill $relay 2>/dev/null; true' peer "$index" "$peer_ip" "$peer_port" "$port" "$frames" "$delay" "$jitter" "$@"
rc=$?
[ $rc = 124 ] && { echo "net_peer: time limit reached"; timeout 15 docker kill "$name" >/dev/null 2>&1; }
echo "net_peer: output in $out"
exit $rc
