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
#     --retry S   keep asking for the match for S seconds while the other machine is not there yet
#                 (default 180; give the other machine --net-retry as well, so start order does not matter)
#     --peer-via-gateway   behind a published port the other machine's packets arrive from the container's
#                 default gateway under a different port per flow: use that gateway as the peer address.
#                 Only the other machine's connect can then succeed; this side's own goes nowhere, which
#                 the netplay code allows for (it accepts the peer's address on any port).
#     --delay MS --jitter MS   put a delay relay between this instance and the network
#   Anything after -- goes to the runner (for example --selftest 30:5 --net-delay 2).
# The other machine runs:  --net <its index>:PEER_PORT:<this machine's address>:<--port value> --net-retry S --frames N
# Output: build/native-engine/net-peer/ (stdout.txt with every line, run.log, digest-net-confirmed.csv).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
[ $# -ge 3 ] || { sed -n '2,23p' "$0"; exit 2; }
index=$1; peer_ip=$2; peer_port=$3; shift 3
port=51000; frames=7200; delay=0; jitter=0; retry=180; via_gateway=0
while [ $# -gt 0 ]; do
  case "$1" in
    --port) port=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --retry) retry=$2; shift 2 ;;
    --peer-via-gateway) via_gateway=1; shift ;;
    --delay) delay=$2; shift 2 ;;
    --jitter) jitter=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "net_peer: unknown option $1" >&2; exit 2 ;;
  esac
done
disc="${MU_DISC_DIR:?set MU_DISC_DIR to the folder holding GALE01.iso}"
out="$repo/build/native-engine/net-peer"; rm -rf "$out"; mkdir -p "$out"
limit=$(( frames / 60 + retry + 180 ))
name="mu-net-peer-$$"
set +e
timeout "$limit" docker run --name "$name" --rm -p "$port:$port/udp" -v "$disc":/iso:ro -v "$repo/build/native-engine/out":/b:ro \
  -v "$here":/t:ro -v "$out":/out "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -c '
  index=$1; peer_ip=$2; peer_port=$3; port=$4; frames=$5; delay=$6; jitter=$7; retry=$8; via_gateway=$9; shift 9
  : > /out/stdout.txt
  if [ "$via_gateway" = 1 ]; then
    # The default route of this container, from the kernel routing table.
    gateway=$(python3 /t/default_gateway.py)
    [ -n "$gateway" ] || { echo "net_peer: no default gateway found in the container" | tee -a /out/stdout.txt; exit 1; }
    echo "net_peer: the other machine is expected to appear as the gateway of this container" | tee -a /out/stdout.txt
    peer_ip=$gateway
  fi
  if [ "$delay" != 0 ] || [ "$jitter" != 0 ]; then
    # The relay owns the published port; the instance talks to it over loopback.
    python3 /t/udp_delay_relay.py 51900 "$port" "$peer_port" 51901 "$delay" "$jitter" 0 1 "$peer_ip" & relay=$!
    net="$index:51900:127.0.0.1:51901"
  else net="$index:$port:$peer_ip:$peer_port"; fi
  cd /out && /b/mu-native-linux --iso /iso/GALE01.iso --out /out --frames "$frames" --net "$net" --net-retry "$retry" "$@" 2>&1 | tee -a /out/stdout.txt | grep --line-buffered -E "^net|^\[net\] slippi: (local peer|connected|online connection|peer connected|received selections|incoming ENet|a connection attempt|disconnect cause)|online test|online: |^stopped|^scenes|^---- "
  [ -n "${relay:-}" ] && kill $relay 2>/dev/null; true' peer "$index" "$peer_ip" "$peer_port" "$port" "$frames" "$delay" "$jitter" "$retry" "$via_gateway" "$@"
rc=$?
[ $rc = 124 ] && { echo "net_peer: time limit reached"; timeout 15 docker kill "$name" >/dev/null 2>&1; }
echo "net_peer: output in $out"
exit $rc
