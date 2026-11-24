#!/usr/bin/env bash
# Netplay test without a console or a network: two play-mode instances of mu-native-gfx-linux in ONE container
# (loopback only), each with a picture on the same screenless X server, playing each other through the engine's
# own online code and this repository's Slippi netplay, with pads from a controller script per side.
#   ./test-net-linux.sh /path/to/disc-folder [retraces] [extra arguments for both instances]
#   MU_NET_DELAY, MU_NET_JITTER   ms of delay and jitter between the two (../tools/udp_delay_relay.py): rollbacks
#   MU_NET_CONTROLLERS=1          the controller scripts play the match instead of the game's generated test inputs
#   MU_SUGGEST_LEAD_MS=N          with it: each script change is also read N ms before the poll that takes it, as a
#                                 player's change between frames would be (sent as a Slippi input suggestion)
#   MU_NET_ARGS_A, MU_NET_ARGS_B  extra arguments for one side only (e.g. --net-clock-skew 0.4: time sync's frame-rate nudge)
#   MU_NET_SUGGESTIONS=0|1        input suggestions off or on (default: on); MU_SUGGEST_TRACE=1 logs the first ones
# Output: build/native-engine-gfx/run-net/{a,b}/ (console.log, digest-net-confirmed.csv) and shots/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; frames="${2:-3000}"; shift $(( $# > 2 ? 2 : $# ))
work="$repo/build/native-engine-gfx"; run="$work/run-net"
rm -rf "$run"; mkdir -p "$run/a" "$run/b" "$run/shots"
# Two different pad scripts in the app's format: each side walks, jumps and attacks on its own rhythm.
python3 -I - "$run" <<'PY'
import sys
for side, period, stick in (("a", 47, 80), ("b", 61, -80)):
    lines = []
    for t in range(200, 20000, period):
        lines += [f"{t} sx={stick}", f"{t + period // 3} A sx={stick}", f"{t + period // 3 + 4} sx={-stick}", f"{t + 2 * period // 3} X", f"{t + 2 * period // 3 + 4}"]
    open(f"{sys.argv[1]}/{side}/pads.txt", "w").write("\n".join(lines) + "\n")
PY
timeout "${MU_TEST_LIMIT:-600}" docker run --rm --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$run:/out" -v "$here/../tools:/t:ro" \
  -e FRAMES="$frames" -e EXTRA="$*" -e DELAY="${MU_NET_DELAY:-0}" -e JITTER="${MU_NET_JITTER:-0}" -e SDL_AUDIODRIVER=dummy \
  -e MU_NET_ARGS_A -e MU_NET_ARGS_B -e MU_NET_CONTROLLERS -e MU_SUGGEST_LEAD_MS -e MU_NET_SUGGESTIONS -e MU_SUGGEST_TRACE "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
    export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
    for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    common="--play --iso /iso/GALE01.iso --frames $FRAMES --novsync --window 640x480 $EXTRA"
    if [ "$DELAY" != 0 ] || [ "$JITTER" != 0 ]; then
      python3 /t/udp_delay_relay.py 51001 52001 51002 52002 "$DELAY" "$JITTER" & relay=$!
      peer_of_a=52002; peer_of_b=52001
    else peer_of_a=51002; peer_of_b=51001; fi
    cd /out/a && /b/mu-native-gfx-linux $common --out /out/a --play-script /out/a/pads.txt --net 0:51001:127.0.0.1:$peer_of_a ${MU_NET_ARGS_A:-} >/out/a/console.log 2>&1 & a=$!
    cd /out/b && /b/mu-native-gfx-linux $common --out /out/b --play-script /out/b/pads.txt --net 1:51002:127.0.0.1:$peer_of_b ${MU_NET_ARGS_B:-} >/out/b/console.log 2>&1 & b=$!
    n=0
    while kill -0 $a 2>/dev/null || kill -0 $b 2>/dev/null; do
      sleep 5; n=$((n+1))
      xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
    done
    wait $a; ra=$?; wait $b; rb=$?
    [ -n "${relay:-}" ] && kill $relay 2>/dev/null
    echo "instance exit codes: a=$ra b=$rb"'
python3 -I "$here/../tools/compare_net_digests.py" "$run/a/digest-net-confirmed.csv" "$run/b/digest-net-confirmed.csv" | sed 's/^/confirmed states: /'
for side in a b; do
  echo "== $side"
  grep -E "^net|^play:|time sync|picture: sound:|selftest|DIVERGED|desync|Desync|rollback|input suggestions|cannot|panic|STOPPED|picture: [0-9]+ frames finished" "$run/$side/console.log" | grep -v "^\[net\] slippi: \(netplay\|ping\)\|net: slow frame \([4-9]\|1[0-9]\)" | tail -30 | cut -c1-330
  grep -E "picture: (simulation thread: work \(|simulation thread: then|match frames with|slow frame [1-3]:|netplay:|decoder thread: all work: [0-9]+ frames, avg)" "$run/$side/console.log" | head -9 | cut -c1-420
done
