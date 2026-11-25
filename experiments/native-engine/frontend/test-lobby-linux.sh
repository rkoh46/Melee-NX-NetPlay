#!/usr/bin/env bash
# melee-unlocked's Multiplayer Lobby (src/lobby.cpp, src/mu_home_lobby.inl) without a console, the public DHT or any real server:
# two apps (the services program, no arguments) in ONE container on an internal Docker network (no route out, no DNS), a local
# one-node BitTorrent DHT as their only entry point (../tools/dht_bootstrap.py, given to both in lobby-profile.json as
# "url": "dht://ADDRESS:6881", upstream's own developer key), the loopback stand-in for Slippi's matchmaking
# (../tools/mock_matchmaking.py) and made-up Slippi users. The lobby's peer-to-peer traffic and the DHT use the container's own
# address (the DHT refuses loopback addresses); nothing leaves the container.
#
# Each app is driven through its home screen with the test pad file (MELEE_PAD_FILE): Multiplayer Lobby, Go Online; both wait until
# the DHT has shown each the other; side a writes in the lobby chat (the software keyboard's test answer), picks side b on the
# Players tab and sends a match request (A on the player); side b's Accept takes the focus and is pressed. Both home screens close, each game goes to
# Training as the player's first main and searches Slippi Direct for the other's code; the stand-in pairs them; the Direct match is
# played with generated moves (the controller scripts' @match section).
#   ./test-lobby-linux.sh /path/to/disc-folder [frames]
# Output: build/native-engine-gfx/run-lobby/ (a/, b/ app folders with console.log and Logs/, dht.txt, mock.txt, shots/: a-*.png and
# b-*.png, the two screens). Checked: both lists show the other player, the request and the accept, the hand-over, the Direct
# connection, the match scene's confirmed states identical on both sides, no desync line.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; frames="${2:-9000}"
work="$repo/build/native-engine-gfx"; run="$work/run-lobby"
rm -rf "$run"; mkdir -p "$run/shots"
for side in a b; do
  app="$run/$side/melee-nx-netplay"
  mkdir -p "$app/User/Slippi" "$app/Lobby"
  code=LOBA#101; name="Lobby A"; mains="[20, 2, 9]"; where="Test bench A"; period=47; stick=80
  [ $side = b ] && { code=LOBB#202; name="Lobby B"; mains="[2, 20, 9]"; where="Test bench B"; period=61; stick=-80; }
  printf '%s\n' "iso=/iso/GALE01.iso" "frames=$frames" "script=/out/$side/pads.txt" "hud=1" > "$app/launcher.ini"
  printf '{"uid":"mock-test-lobby-%s","playKey":"mock-test-only","connectCode":"%s","displayName":"%s"}\n' "$side" "$code" "$name" > "$app/User/Slippi/user.json"
  printf '{"mode": "peer", "url": "dht://BOOTSTRAP:6881", "name": "", "location": "%s", "mains": %s}\n' "$where" "$mains" > "$app/Lobby/lobby-profile.json"
  # The game's controller script: nothing on the menus the lobby hand-over presses through (main menu, Training's selects,
  # Slippi's select), moves keyed on the retrace everywhere else (the Training wait and the match), each side on its own
  # rhythm, as test-online-linux.sh plays its matches.
  python3 -I - "$run/$side/pads.txt" "$period" "$stick" <<'PY'
import sys
path, period, stick = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = ["# test-lobby-linux.sh: moves outside the menus"]
for t in range(600, 30000, period):
    lines += ["%d sx=%d" % (t, stick), "%d A sx=%d" % (t + period // 3, stick), "%d sx=%d" % (t + period // 3 + 4, -stick), "%d X" % (t + 2 * period // 3), "%d" % (t + 2 * period // 3 + 4)]
for gate in ("01 00", "01 01", "1C 00", "1C 01", "08 00", "08 01"):
    lines += ["@scene " + gate, "0"]
open(path, "w").write("\n".join(lines) + "\n")
PY
  : > "$run/$side/pad.txt"
done
# A third lobby: melee-unlocked's own lobby code without patches/0142 (both transports, as its Windows launcher with Slippi
# announces itself), headless (../tools/lobby_peer.cpp), standing in for a Melee Unlocked PC: it must list both apps, then see
# them "In game" and "In match" with their stocks; both apps must list it.
mu="$repo/build/native-engine-gfx/mu"; g="$repo/build/native-engine-gfx/gen/lobby"
ver=$(head -1 "$repo/build/native-engine/mu/VERSION")
timeout 600 docker run --rm --name "${MU_DOCKER_NAME:-mnx-test}-lobbypeer-build-$$" --network none -v "$repo:/w" -w /w "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -ec "
  o=build/native-engine-gfx/obj-lobby-peer; mkdir -p \$o
  for f in dht/dht.c monocypher/monocypher.c monocypher/monocypher-ed25519.c; do gcc -O2 -w -c build/native-engine-gfx/mu/port/third_party/\$f -o \$o/\$(basename \$f .c).o; done
  g++ -std=gnu++17 -O2 -DMU_PEER_BUILD='\"$ver:source\"' -Iexperiments/native-engine/frontend/src/lobby_shim -Ibuild/native-engine-gfx/gen/lobby \
    -Ibuild/native-engine-gfx/mu/port/third_party/dht -Ibuild/native-engine-gfx/mu/port/third_party/monocypher -Iport/third_party \
    experiments/native-engine/tools/lobby_peer.cpp build/native-engine-gfx/gen/lobby/launcher_lobby_p2p.cpp experiments/native-engine/frontend/src/lobby_shim/lobby_shim.cpp \
    \$o/*.o -static-libstdc++ -static-libgcc -pthread -o build/native-engine-gfx/out/lobby-peer"
net="${MU_DOCKER_NAME:-mnx-test}-lobby-net-$$"
docker network create --internal "$net" >/dev/null
cleanup() { docker rm -f "${MU_DOCKER_NAME:-mnx-test}-lobby-$$" >/dev/null 2>&1 || true; docker network rm "$net" >/dev/null 2>&1 || true; }
trap cleanup EXIT
timeout "${MU_TEST_LIMIT:-1200}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-test}-lobby-$$" --network "$net" -v "$disc:/iso:ro" -v "$work/out:/b:ro" \
  -v "$here/../tools:/t:ro" -v "$run:/out" -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp -e LP_NUM_THREADS "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    ip=$(hostname -i | awk "{print \$1}")
    echo "container address $ip; route out: $(getent hosts router.bittorrent.com >/dev/null 2>&1 && echo yes || echo none)" > /out/net.txt
    sed -i "s/BOOTSTRAP/$ip/" /out/a/melee-nx-netplay/Lobby/lobby-profile.json /out/b/melee-nx-netplay/Lobby/lobby-profile.json
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb-a.log 2>&1 &
    Xvfb :98 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb-b.log 2>&1 &
    for i in $(seq 100); do xwd -display :99 -root -silent >/dev/null 2>&1 && xwd -display :98 -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    python3 /t/dht_bootstrap.py "$ip" 6881 > /out/dht.txt 2>&1 & dht=$!
    (while true; do python3 /t/mock_matchmaking.py 8765; sleep 0.2; done) > /out/mock.txt 2>&1 & mock=$!
    mkdir -p /out/peer; (sleep 20; exec /b/lobby-peer /out/peer "dht://$ip:6881" 900 "Lobby PC" "LOBC#303") > /out/peer.txt 2>&1 & peer=$!
    sleep 1
    shot() { xwd -display $([ $1 = a ] && echo :99 || echo :98) -root -silent 2>/dev/null | convert xwd:- "/out/shots/$1-$2.png" 2>/dev/null || true; }
    wait_log() { for i in $(seq $(( $3 * 10 ))); do grep -aq "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; echo "TIMEOUT waiting for: $2 in $1" >> /out/harness.log; return 1; }
    pad() { printf "%s\n" "$2" > /out/$1/pad.txt; }
    # A press of 0.2 s: seen by a slow frame (two apps draw with the software Vulkan driver), shorter than the home screen
    # D-pad repeat (20 frames).
    tap() { pad $1 "$2"; sleep 0.2; pad $1 ""; sleep 0.6; }
    # rail SIDE PAGE: B to the page nav, then D-pad down until the nav names PAGE ("home: rail" lines), A.
    rail() { tap $1 B; for i in $(seq 8); do [ "$(grep -a "home: rail" /out/$1/console.log | tail -1)" = "home: rail $2" ] && { tap $1 A; return 0; }; tap $1 DD; done; echo "TIMEOUT rail $1 $2" >> /out/harness.log; return 1; }
    # focus SIDE LABEL: D-pad down until the page names LABEL as focused ("home: lobby focus" lines).
    focus() { for i in $(seq 30); do [ "$(grep -a "home: lobby focus" /out/$1/console.log | tail -1)" = "home: lobby focus $2" ] && return 0; tap $1 DD; done; echo "TIMEOUT focus $1 $2" >> /out/harness.log; return 1; }
    (cd /out/a && DISPLAY=:99 MELEE_PAD_FILE=/out/a/pad.txt MU_TEST_KEYBOARD="Hello from the Switch app|-" /b/mu-native-gfx-linux-services > /out/a/console.log 2>&1; echo "exit code $?" >> /out/a/console.log) & a=$!
    (cd /out/b && DISPLAY=:98 MELEE_PAD_FILE=/out/b/pad.txt /b/mu-native-gfx-linux-services > /out/b/console.log 2>&1; echo "exit code $?" >> /out/b/console.log) & b=$!
    wait_log /out/a/console.log "home: drawn" 180; wait_log /out/b/console.log "home: drawn" 180; sleep 1
    rail a "Multiplayer Lobby"; rail b "Multiplayer Lobby"
    sleep 1; shot a lobby-offline
    tap a A; sleep 0.8; shot a lobby-connecting; sleep 2; shot a lobby-searching
    tap b A
    wait_log /out/a/console.log "lobby: 2 players in the lobby" 180; wait_log /out/b/console.log "lobby: 2 players in the lobby" 180
    sleep 2; shot a lobby-roster; shot b lobby-roster
    # Side a: the lobby chat (Tab, Right, Write a message: the keyboard answers), then back to Players.
    focus a "Tab Players"; tap a DR; focus a "Write a message"; tap a A
    wait_log /out/b/console.log "lobby: chat message from Lobby A" 30
    focus b "Tab Players"; tap b DR; sleep 1; shot b lobby-chat; sleep 0.3; shot a lobby-chat; tap b DL
    tap a DU; tap a DL
    # Side a: Lobby B on the Players tab, Send match request; side b: the request takes the focus to Accept.
    focus a "Player Lobby B"; sleep 0.5; shot a lobby-picked; tap a A
    wait_log /out/b/console.log "Accept focused" 30; sleep 1.5; shot b lobby-incoming; shot a lobby-outgoing
    tap b A
    wait_log /out/a/console.log "home: lobby match against" 30; wait_log /out/b/console.log "home: lobby match against" 30
    # The test pad file wins over the controller script while it exists (input_sdl.cpp): gone now, the script plays the game.
    rm -f /out/a/pad.txt /out/b/pad.txt
    sleep 1; shot a handover
    wait_log /out/a/console.log "lobby: into Training" 180 && { sleep 4; shot a training-search; shot b training-search; }
    wait_log /out/a/console.log "lobby: connected to" 150; wait_log /out/b/console.log "lobby: connected to" 150
    sleep 3; shot a direct-select; shot b direct-select
    n=0
    while kill -0 $a 2>/dev/null || kill -0 $b 2>/dev/null; do sleep 8; n=$((n+1)); [ $n -le 40 ] && { shot a "match-$(printf %02d $n)"; shot b "match-$(printf %02d $n)"; }; done
    wait $a; wait $b
    kill $dht $mock $peer 2>/dev/null || true' || echo "test-lobby-linux: container ended with $?"
cat "$run/net.txt" 2>/dev/null
[ -f "$run/harness.log" ] && cat "$run/harness.log"
echo "== DHT node"; grep -E "node on|announce on the public|get_peers on the public" "$run/dht.txt" | sort | uniq -c | sort -rn | head -8
echo "== the melee-unlocked PC stand-in (upstream lobby code, both transports)"; grep -E "online as|player" "$run/peer.txt" | cut -c1-260 | head -12
echo "== matchmaking stand-in"; tail -4 "$run/mock.txt" 2>/dev/null
for side in a b; do
  echo "== $side"
  grep -aE "^lobby:|home: lobby (Go Online|match|request|chat)|^search:|slippi: (connected|opponent found)|DIVERGED|desync|Desync|exit code|---- done" "$run/$side/console.log" | grep -v "lobby: focus" | cut -c1-220 | head -40
  awk -F, '$2==8 && $3==2' "$run/$side/melee-nx-netplay/Logs/digest-net-confirmed.csv" > "$run/$side/match.csv" 2>/dev/null || true
done
if [ -s "$run/a/match.csv" ] && [ -s "$run/b/match.csv" ]; then
  python3 -I "$here/../tools/compare_net_digests.py" "$run/a/match.csv" "$run/b/match.csv" | sed 's/^/confirmed states in the match scene: /'
else
  echo "confirmed states in the match scene: MISSING (no online match rows on one side)"
fi
# The game's own checksum comparison with the other side (net check's count), and any DESYNC line the game wrote.
for side in a b; do
  echo "desync, $side: $(grep -aoE '[0-9]+ desync messages from the game' "$run/$side/console.log" | tail -1 || echo 'no net check line'); DESYNC lines $(grep -ac 'DESYNC\|desync risk' "$run/$side/console.log" || true)"
done
echo "shots: $(ls "$run/shots" | wc -l | tr -d ' ') in $run/shots"
