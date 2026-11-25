#!/usr/bin/env bash
# melee-unlocked's Multiplayer Lobby (src/lobby.cpp, src/mu_home_lobby.inl) without a console, the public DHT or any real server:
# two apps (the services program, no arguments) and a stand-in for a Melee Unlocked PC in ONE container on an internal Docker
# network (no route out, no DNS), a local one-node BitTorrent DHT as their only entry point (../tools/dht_bootstrap.py, given to
# every lobby in lobby-profile.json as "url": "dht://ADDRESS:6881", upstream's own developer key), the loopback stand-in for
# Slippi's matchmaking (../tools/mock_matchmaking.py) and made-up Slippi users. The DHT refuses loopback addresses, so the lobbies
# use the container's own address; nothing leaves the container.
#
# The PC stand-in (../tools/lobby_peer.cpp) is melee-unlocked's own lobby code as fetched (both match transports in its hello); it
# takes commands from a file and starts melee-unlocked's peer-to-peer game for an accepted match as its launcher does, with
# p2p_arguments(...), on the native engine's headless runner (../build.sh linux: mu-native-linux, generated inputs).
#
# What happens, each app driven through its home screen with the test pad file (MELEE_PAD_FILE):
#   1  both apps: Multiplayer Lobby, Go Online; every lobby lists the other two through the DHT
#   2  public chat: side a writes (the software keyboard's test answer), side b receives it
#   3  private chat: side a asks side b for a private chat (Left / Right on b's row, A), b accepts, a writes in the room, b receives
#   4  auto reject: side a switches "Auto reject invites" on; the PC stand-in sends a Slippi Direct request to a: declined at once
#   5  peer-to-peer match: the PC stand-in asks side b for melee-unlocked's own match; b hears the request chime (SDL's disk audio
#      driver records it: 660 and 880 Hz), accepts; b's lobby closes, its game plays the mu_net session against the stand-in's
#      runner; the match runs to its end (the scripts make the fighters leave the stage); b is back on the lobby page, the lobby
#      open again; the result in b's match history
#   6  Slippi Direct match: side a asks side b, b accepts; both go to Training and meet on Slippi Direct; the match runs to its end;
#      both apps are back on the lobby page; the result in both histories (one win, one loss)
#   7  the Match history tab on both (screenshots)
#   ./test-lobby-linux.sh /path/to/disc-folder
# Output: build/native-engine-gfx/run-lobby/ (a/, b/ app folders with console.log and Logs/, pc/ the stand-in, pc-game/ its runner,
# dht.txt, mock.txt, shots/: a-*.png and b-*.png, the two screens).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"
work="$repo/build/native-engine-gfx"; run="$work/run-lobby"
[ -x "$repo/build/native-engine/out/mu-native-linux" ] || { echo "test-lobby: run ../build.sh linux first (the PC stand-in's game)" >&2; exit 1; }
rm -rf "$run"; mkdir -p "$run/shots" "$run/pc" "$run/pc-game"
for side in a b; do
  app="$run/$side/melee-nx-netplay"
  mkdir -p "$app/User/Slippi" "$app/Lobby"
  code=LOBA#101; name="Lobby A"; mains="[20, 2, 9]"; where="Test bench A"; period=20; stick=-128
  [ $side = b ] && { code=LOBB#202; name="Lobby B"; mains="[2, 20, 9]"; where="Test bench B"; period=60; stick=-100; }
  printf '%s\n' "iso=/iso/GALE01.iso" "frames=0" "script=/out/$side/pads.txt" "hud=1" > "$app/launcher.ini"
  printf '{"uid":"mock-test-lobby-%s","playKey":"mock-test-only","connectCode":"%s","displayName":"%s"}\n' "$side" "$code" "$name" > "$app/User/Slippi/user.json"
  printf '{"mode": "peer", "url": "dht://BOOTSTRAP:6881", "name": "", "location": "%s", "mains": %s}\n' "$where" "$mains" > "$app/Lobby/lobby-profile.json"
  # The game's controller script: nothing on the menus the lobby hand-over presses through (main menu, Training's selects, Slippi's
  # select), and in a match the stick held towards the stage's edge with jumps, so each match ends with one side out of stocks
  # (side a every 20 frames, side b every 60: a leaves the stage sooner).
  python3 -I - "$run/$side/pads.txt" "$period" "$stick" <<'PY'
import sys
path, period, stick = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = ["# test-lobby-linux.sh: towards the edge with jumps outside the menus"]
for t in range(600, 60000, period):
    lines += ["%d sx=%d" % (t, stick), "%d X sx=%d" % (t + period // 2, stick), "%d sx=%d" % (t + period // 2 + 4, stick)]
for gate in ("01 00", "01 01", "1C 00", "1C 01", "08 00", "08 01"):
    lines += ["@scene " + gate, "0"]
open(path, "w").write("\n".join(lines) + "\n")
PY
  : > "$run/$side/pad.txt"
done
: > "$run/pc/commands.txt"
# The PC stand-in: melee-unlocked's lobby code as fetched, headless (../tools/lobby_peer.cpp), built here.
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
timeout "${MU_TEST_LIMIT:-2400}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-test}-lobby-$$" --network "$net" -v "$disc:/iso:ro" -v "$work/out:/b:ro" \
  -v "$repo/build/native-engine/out:/r:ro" -v "$here/../tools:/t:ro" -v "$run:/out" -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp -e LP_NUM_THREADS "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    ip=$(hostname -i | awk "{print \$1}")
    echo "container address $ip; route out: $(getent hosts router.bittorrent.com >/dev/null 2>&1 && echo yes || echo none)" > /out/net.txt
    sed -i "s/BOOTSTRAP/$ip/" /out/a/melee-nx-netplay/Lobby/lobby-profile.json /out/b/melee-nx-netplay/Lobby/lobby-profile.json
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb-a.log 2>&1 &
    Xvfb :98 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb-b.log 2>&1 &
    for i in $(seq 100); do xwd -display :99 -root -silent >/dev/null 2>&1 && xwd -display :98 -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    python3 /t/dht_bootstrap.py "$ip" 6881 > /out/dht.txt 2>&1 & dht=$!
    (while true; do python3 /t/mock_matchmaking.py 8765; sleep 0.2; done) > /out/mock.txt 2>&1 & mock=$!
    (sleep 20; cd /out/pc && PEER_GAME=/r/mu-native-linux PEER_ISO=/iso/GALE01.iso PEER_OUT=/out/pc-game exec /b/lobby-peer /out/pc "dht://$ip:6881" 1800 "Lobby PC" "LOBC#303" /out/pc/commands.txt) > /out/peer.txt 2>&1 & peer=$!
    sleep 1
    shot() { xwd -display $([ $1 = a ] && echo :99 || echo :98) -root -silent 2>/dev/null | convert xwd:- "/out/shots/$1-$2.png" 2>/dev/null || true; }
    wait_log() { for i in $(seq $(( $3 * 10 ))); do grep -aq "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; echo "TIMEOUT waiting for: $2 in $1" >> /out/harness.log; return 1; }
    # count_log FILE PATTERN N SECONDS: until PATTERN has appeared N times.
    count_log() { for i in $(seq $(( $4 * 10 ))); do [ "$(grep -ac "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0; sleep 0.1; done; echo "TIMEOUT waiting for $3 x: $2 in $1" >> /out/harness.log; return 1; }
    pad() { printf "%s\n" "$2" > /out/$1/pad.txt; }
    # A press of 0.2 s: seen by a slow frame (two apps draw with the software Vulkan driver), shorter than the home screen D-pad repeat.
    tap() { pad $1 "$2"; sleep 0.2; pad $1 ""; sleep 0.6; }
    # rail SIDE PAGE: B to the page nav, then D-pad down until the nav names PAGE ("home: rail" lines), A.
    rail() { tap $1 B; for i in $(seq 8); do [ "$(grep -a "home: rail" /out/$1/console.log | tail -1)" = "home: rail $2" ] && { tap $1 A; return 0; }; tap $1 DD; done; echo "TIMEOUT rail $1 $2" >> /out/harness.log; return 1; }
    lastfocus() { grep -a "home: lobby focus" /out/$1/console.log | tail -1 | sed "s/^home: lobby focus //"; }
    # focus SIDE LABEL [KEY]: D-pad down (or KEY) until the page names LABEL as focused ("home: lobby focus" lines).
    focus() { for i in $(seq 30); do [ "$(lastfocus $1)" = "$2" ] && return 0; tap $1 ${3:-DD}; done; echo "TIMEOUT focus $1 $2" >> /out/harness.log; return 1; }
    # tab SIDE NAME: up to the tabs row, then Right until the tab NAME shows.
    tab() {
      for i in $(seq 30); do case "$(lastfocus $1)" in "Tab "*) break ;; "Leave"|"Go Online"|"Accept"|"Decline"|"Cancel request") tap $1 DD ;; *) tap $1 DU ;; esac; done
      for i in $(seq 6); do [ "$(lastfocus $1)" = "Tab $2" ] && return 0; tap $1 DR; done; echo "TIMEOUT tab $1 $2" >> /out/harness.log; return 1; }
    start_app() {   # side, display, extra environment
      (cd /out/$1 && env DISPLAY=$2 MELEE_PAD_FILE=/out/$1/pad.txt $3 /b/mu-native-gfx-linux-services > /out/$1/console.log 2>&1; echo "exit code $?" >> /out/$1/console.log) &
    }
    start_app a :99 "MU_TEST_KEYBOARD=Hello_from_the_Switch_app|A_private_hello"; a=$!
    start_app b :98 "SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=/out/b/audio.raw"; b=$!
    wait_log /out/a/console.log "home: drawn" 180; wait_log /out/b/console.log "home: drawn" 180; sleep 1
    # 1 Go Online.
    rail a "Multiplayer Lobby"; rail b "Multiplayer Lobby"
    sleep 1; shot a lobby-offline
    tap a A; sleep 0.8; shot a lobby-connecting; sleep 2; shot a lobby-searching
    tap b A
    wait_log /out/a/console.log "lobby: 2 players in the lobby" 180; wait_log /out/b/console.log "lobby: 2 players in the lobby" 180
    sleep 2; shot a lobby-roster; shot b lobby-roster
    # 2 Public chat.
    tab a "Lobby chat"; focus a "Write a message"; tap a A
    wait_log /out/b/console.log "lobby: chat message from Lobby A" 30
    tab b "Lobby chat"; sleep 1; shot b lobby-chat
    # 3 Private chat: a picks Private chat on b row (Right twice), b accepts in its chat tab, a writes in the room.
    tab a "Players"; focus a "Player Lobby B"; tap a DR; tap a DR; sleep 0.5; shot a pm-pick; tap a A
    wait_log /out/b/console.log "home: lobby private chat request from Lobby A" 30; sleep 1.5; shot b pm-incoming
    focus b "Accept private chat"; tap b A
    wait_log /out/a/console.log "lobby: private chat with Lobby B: open" 30
    tab a "Lobby chat"; focus a "Room"; tap a DR; focus a "Write a message"; tap a A
    wait_log /out/b/console.log "lobby: private message from Lobby A" 30; sleep 1.5; shot b pm-open; shot a pm-open
    # 4 Auto reject invites on side a; the PC stand-in asks a for a Slippi Direct match.
    tab a "Your profile"; focus a "Auto reject invites"; tap a A; sleep 0.5; shot a profile-auto-reject
    echo "request Lobby A" >> /out/pc/commands.txt
    wait_log /out/a/console.log "automatically declined" 40; sleep 1; shot a auto-rejected
    # 5 The peer-to-peer match: the PC stand-in asks side b; the chime; b accepts (the request takes the focus to Accept).
    tab b "Players"
    echo "p2p Lobby B" >> /out/pc/commands.txt
    wait_log /out/b/console.log "home: lobby request from Lobby PC (peer to peer): Accept focused" 40; sleep 1.5; shot b p2p-incoming
    wait_log /out/b/console.log "home: lobby request sound played" 10 && cp /out/b/audio.raw /out/b/chime.raw
    tap b A
    wait_log /out/b/console.log "lobby: closed for a match" 30
    rm -f /out/b/pad.txt; wait_log /out/b/console.log "p2p: connected to the other player" 120
    sleep 6; shot b p2p-match-1; sleep 15; shot b p2p-match-2
    wait_log /out/b/console.log "back to the home screen" 400 && { cp /out/b/melee-nx-netplay/Logs/digest-net-confirmed.csv /out/b/p2p-digest.csv 2>/dev/null; : > /out/b/pad.txt; }
    wait_log /out/b/console.log "lobby: open again" 60; wait_log /out/b/console.log "home: drawn (Multiplayer Lobby page)" 120; sleep 3; shot b back-after-p2p
    # 6 The Slippi Direct match: a asks b (A on b row: Send match request), b accepts.
    tab a "Players"; focus a "Player Lobby B"; tap a DL; tap a DL; tap a A
    wait_log /out/b/console.log "home: lobby request from Lobby A: Accept focused" 40; sleep 1; shot b direct-incoming; tap b A
    wait_log /out/a/console.log "home: lobby match against" 40; wait_log /out/b/console.log "home: lobby match against Lobby A" 40
    rm -f /out/a/pad.txt /out/b/pad.txt
    wait_log /out/a/console.log "lobby: connected to" 200; wait_log /out/b/console.log "lobby: connected to" 200
    sleep 12; shot a direct-match-1; shot b direct-match-1
    count_log /out/a/console.log "back to the home screen" 1 400 && cp /out/a/melee-nx-netplay/Logs/digest-net-confirmed.csv /out/a/direct-digest.csv 2>/dev/null
    count_log /out/b/console.log "back to the home screen" 2 400 && cp /out/b/melee-nx-netplay/Logs/digest-net-confirmed.csv /out/b/direct-digest.csv 2>/dev/null
    : > /out/a/pad.txt; : > /out/b/pad.txt
    count_log /out/a/console.log "home: drawn (Multiplayer Lobby page)" 1 120; count_log /out/b/console.log "home: drawn (Multiplayer Lobby page)" 2 120
    sleep 3; shot a back-after-direct; shot b back-after-direct
    # 7 The Match history tab on both.
    tab a "Match history"; tab b "Match history"; sleep 1.5; shot a history; shot b history
    sleep 2
    kill $peer 2>/dev/null; pkill -f mu-native-gfx-linux-services; sleep 3
    wait $a; wait $b
    kill $dht $mock 2>/dev/null || true' || echo "test-lobby-linux: container ended with $?"
cat "$run/net.txt" 2>/dev/null
[ -f "$run/harness.log" ] && cat "$run/harness.log"
echo "== DHT node"; grep -E "node on|announce on the public|get_peers on the public" "$run/dht.txt" | sort | uniq -c | sort -rn | head -8
echo "== the melee-unlocked PC stand-in (upstream lobby code as fetched, both transports)"; grep -vE "^peer: [0-9]+ player" "$run/peer.txt" | cut -c1-240 | head -30
grep -E "^peer: [0-9]+ player" "$run/peer.txt" | cut -c1-300 | tail -6
echo "== the stand-in's game (headless runner)"; grep -aE "^p2p: (connected|both sides|session|game end|result|leaving)|net check:|stopped with" "$run/pc-game/console.log" 2>/dev/null | cut -c1-220 | head -12
echo "== matchmaking stand-in"; tail -4 "$run/mock.txt" 2>/dev/null
for side in a b; do
  echo "== $side"
  grep -aE "^lobby:|home: lobby (Go Online|match|request|chat|private|auto|action)|home: back|^p2p: (connected|both sides|game end|session|leaving)|request sound|---- back|^search: Practice|DIVERGED|desync messages|exit code|---- done" "$run/$side/console.log" | grep -v "lobby: focus" | cut -c1-220 | head -70
done
# The request chime in side b's recording: 660 and 880 Hz against the whole recording (Goertzel), in the format the log names.
python3 -I - "$run/b/chime.raw" "$run/b/console.log" <<'PY' || true
import math, re, struct, sys
try: data = open(sys.argv[1], "rb").read()
except OSError: print("request sound: no recording"); sys.exit(0)
m = re.findall(r"request sound played .*output (\d+) Hz, (\d+) channels, format ([0-9A-F]+)", open(sys.argv[2], errors="replace").read())
if not m: print("request sound: not played"); sys.exit(0)
rate, ch, fmt = int(m[-1][0]), int(m[-1][1]), int(m[-1][2], 16)
if fmt == 0x8120: x = struct.unpack("<%df" % (len(data) // 4), data[:len(data) // 4 * 4])
elif fmt == 0x8010: x = [v / 32768 for v in struct.unpack("<%dh" % (len(data) // 2), data[:len(data) // 2 * 2])]
else: print("request sound: format %04X not read here" % fmt); sys.exit(0)
x = x[0::ch]
energy = sum(v * v for v in x) or 1
def power(freq):
    k = 2 * math.cos(2 * math.pi * freq / rate); s1 = s2 = 0.0
    for v in x: s = v + k * s1 - s2; s2 = s1; s1 = s
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / (energy * len(x) / 2)
print("request sound: %.2f s recorded at %d Hz; share of the sound at 660 Hz %.2f, at 880 Hz %.2f, at 440 Hz (control) %.3f" % (len(x) / rate, rate, power(660), power(880), power(440)))
PY
# The peer-to-peer match: side b's game against the stand-in's runner (every confirmed state of the match frames).
if [ -s "$run/b/p2p-digest.csv" ] && [ -s "$run/pc-game/digest-net-confirmed.csv" ]; then
  awk -F, '$4>0' "$run/b/p2p-digest.csv" > "$run/b/p2p-match.csv"; awk -F, '$4>0' "$run/pc-game/digest-net-confirmed.csv" > "$run/pc-game/match.csv"
  python3 -I "$here/../tools/compare_net_digests.py" "$run/b/p2p-match.csv" "$run/pc-game/match.csv" | sed 's/^/peer-to-peer match, confirmed states (app b vs the stand-in): /'
else echo "peer-to-peer match, confirmed states: MISSING"; fi
# The Slippi Direct match: side a against side b (the online match scene).
if [ -s "$run/a/direct-digest.csv" ] && [ -s "$run/b/direct-digest.csv" ]; then
  for side in a b; do awk -F, '$2==8 && $3==2' "$run/$side/direct-digest.csv" > "$run/$side/direct-match.csv"; done
  python3 -I "$here/../tools/compare_net_digests.py" "$run/a/direct-match.csv" "$run/b/direct-match.csv" | sed 's/^/Slippi Direct match, confirmed states (a vs b): /'
else echo "Slippi Direct match, confirmed states: MISSING"; fi
for side in a b; do
  echo "history $side: $(python3 -I -c 'import json,sys; h=json.load(open(sys.argv[1])); print("; ".join("%s vs %s (%s)" % (g["result"], g["opponent"], g.get("transport","")) for g in h))' "$run/$side/melee-nx-netplay/Lobby/lobby-history.json" 2>/dev/null || echo none)"
  echo "desync, $side: $(grep -aoE '[0-9]+ desync messages from the game' "$run/$side/console.log" | tr '\n' ' '); DESYNC lines $(grep -ac 'DESYNC\|desync risk' "$run/$side/console.log" || true); p2p checksums: $(grep -aoE '[0-9]+ checksums compared, [0-9]+ mismatched' "$run/$side/console.log" | tail -1)"
done
echo "stand-in game: $(grep -aoE '[0-9]+ checksums compared, [0-9]+ mismatched' "$run/pc-game/console.log" 2>/dev/null | tail -1)"
echo "shots: $(ls "$run/shots" | wc -l | tr -d ' ') in $run/shots"
