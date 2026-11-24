#!/usr/bin/env bash
# Online play from the game's own menus, without a console, an account or a network: two play-mode instances of
# the services program in ONE container (loopback only), each with a picture on a screenless X server, each driven by
# a controller script through the menus to Online Play > Unranked, matched by the loopback stand-in for the
# matchmaking service (../tools/mock_matchmaking.py, made-up user files), playing each other.
#   ./test-online-linux.sh /path/to/disc-folder [retraces] [extra arguments for both instances]
#   MU_PADS_A, MU_PADS_B   controller scripts (default scripts/online_unranked.txt, side b one fighter further, plus moves for the match)
#   MU_INI_A, MU_INI_B     settings files (default: "slippi_menus 1")
#   MU_ARGS_A, MU_ARGS_B   extra arguments for one side (for example "--online unranked" to enter without menus)
#   MU_ONLY_A=1            one instance and no stand-in: to look at the menus
#   MU_REARM=1             the scripts search again after each match (the stand-in is started again for every pair)
#   MU_NET_DELAY, MU_NET_JITTER   ms; MU_SHOT_EVERY seconds between screenshots (default 5)
#   MU_TWO_SCREENS=1       side b gets its own screenless X server: shots/shot-NNN.png show a, shots/b-NNN.png show b
#   MU_DEAD_FIRST=N        the stand-in matches each side's first N searches with a peer that never answers: each of
#                          those connection attempts fails after 8 s and the side searches again (the console's
#                          failed-connection-then-search-again path), then the two sides are paired
#   MU_SANITIZE=address|thread   run the sanitizer build (build.sh with MU_SANITIZE): its reports go to {a,b}/san.*
#   MU_VARIANT=null        the program without a graphics device (no picture, no controllers: pass MU_ARGS_A and MU_ARGS_B
#                          "--online unranked"; the match is played by generated inputs). ThreadSanitizer runs use it: the
#                          sanitizer's runtime can hang inside Mesa's software driver
# Output: build/native-engine-gfx/run-online/{a,b}/ (console.log, picture.log, digest-net-confirmed.csv), mock.txt and shots/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; frames="${2:-4200}"; shift $(( $# > 2 ? 2 : $# ))
work="$repo/build/native-engine-gfx"; run="$work/run-online"
rm -rf "$run"; mkdir -p "$run/a" "$run/b" "$run/shots"
# The menu script as committed for side a; side b moves its hand one icon further (Fox). Then, for both, lines keyed
# on the retrace count, which the script uses on every screen it has no block for: in the match each side walks,
# jumps and attacks on its own rhythm.
python3 -I - "$here/scripts/online_unranked.txt" "$run" "${MU_PADS_A:-}" "${MU_PADS_B:-}" "${MU_REARM:-0}" <<'PY'
import sys
menu = open(sys.argv[1]).read()
for side, period, stick, given in (("a", 47, 80, sys.argv[3]), ("b", 61, -80, sys.argv[4])):
    text = open(given).read() if given else menu
    if side == "b" and not given: text = text.replace("132\n150 A", "132 sx=127\n138\n150 A")
    head, gates = text.split("@scene", 1)
    lines = []
    for t in range(600, 30000, period):
        lines += [f"{t} sx={stick}", f"{t + period // 3} A sx={stick}", f"{t + period // 3 + 4} sx={-stick}", f"{t + 2 * period // 3} X", f"{t + 2 * period // 3 + 4}"]
    rearm = "@rearm\n" if sys.argv[5] == "1" else ""      # (a shell $(...) would drop the newline)
    open(f"{sys.argv[2]}/{side}/pads.txt", "w").write(head + rearm + "\n".join(lines) + "\n@scene" + gates)
PY
for side in a b; do
  ini="MU_INI_$(echo $side | tr a-z A-Z)"
  if [ -n "${!ini:-}" ]; then cp "${!ini}" "$run/$side/settings.ini"; else echo "slippi_menus 1" > "$run/$side/settings.ini"; fi
done
bin=mu-native-gfx-${MU_VARIANT:-linux}-services; san_env=""
case "${MU_SANITIZE:-}" in
  "") ;;
  address) bin=$bin-asan; san_env="ASAN_OPTIONS=detect_leaks=0:handle_segv=0:allow_user_segv_handler=1:abort_on_error=0:log_path=san" ;;
  thread) bin=$bin-tsan; san_env="TSAN_OPTIONS=handle_segv=0:history_size=4:second_deadlock_stack=1:suppressions=/t/tsan_graphics.supp:log_path=san" ;;
  *) echo "MU_SANITIZE must be address or thread" >&2; exit 2 ;;
esac
mock_args=""; [ -z "${MU_DEAD_FIRST:-}" ] || mock_args="--dead-first $MU_DEAD_FIRST"
timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "mnx-${MU_DOCKER_TAG:-test}-online-$$" --network none -e SLIPPI_MM_URL=ws://127.0.0.1:8765 -e BIN="$bin" -e SAN_ENV="$san_env" -e MOCK_ARGS="$mock_args" -v "$disc:/iso:ro" -v "$work/out:/b:ro" \
  -v "$here/../tools:/t:ro" -v "$run:/out" -e FRAMES="$frames" -e EXTRA="$*" -e ARGS_A="${MU_ARGS_A:-}" -e ARGS_B="${MU_ARGS_B:-}" -e ONLY_A="${MU_ONLY_A:-0}" \
  -e EVERY="${MU_SHOT_EVERY:-5}" -e TWO="${MU_TWO_SCREENS:-0}" -e DELAY="${MU_NET_DELAY:-0}" -e JITTER="${MU_NET_JITTER:-0}" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
    export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
    for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    display_b=:99
    if [ "$TWO" = 1 ]; then Xvfb :98 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb-b.log 2>&1 & display_b=:98
      for i in $(seq 100); do xwd -display :98 -root -silent >/dev/null 2>&1 && break; sleep 0.1; done; fi
    [ "$ONLY_A" = 1 ] || { (while true; do python3 /t/mock_matchmaking.py 8765 $MOCK_ARGS; sleep 0.2; done) > /out/mock.txt 2>&1 & mock=$!; sleep 1; }
    common="--play --iso /iso/GALE01.iso --frames $FRAMES --novsync --window 640x480 --fake-user $EXTRA"
    [ -z "$SAN_ENV" ] || export "$SAN_ENV"
    cd /out/a && /b/$BIN $common --out /out/a --settings /out/a/settings.ini --play-script /out/a/pads.txt --net-inputs 1 $ARGS_A >/out/a/console.log 2>&1 & a=$!
    b=$a
    [ "$ONLY_A" = 1 ] || { cd /out/b && DISPLAY=$display_b /b/$BIN $common --out /out/b --settings /out/b/settings.ini --play-script /out/b/pads.txt --net-inputs 2 $ARGS_B >/out/b/console.log 2>&1 & b=$!; }
    n=0
    while kill -0 $a 2>/dev/null || kill -0 $b 2>/dev/null; do
      sleep "$EVERY"; n=$((n+1))
      xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      [ "$TWO" != 1 ] || xwd -display :98 -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/b-%03d.png $n)" 2>/dev/null || true
    done
    wait $a; ra=$?; rb=0; [ "$ONLY_A" = 1 ] || { wait $b; rb=$?; }
    [ -n "${mock:-}" ] && kill $mock 2>/dev/null
    echo "instance exit codes: a=$ra b=$rb"' || echo "test-online-linux: container ended with $?"
[ -f "$run/mock.txt" ] && cat "$run/mock.txt"
# Only the rows of the online match scene (8/2): after a match the game is back on Slippi's character select with the
# last match frame still in its state and a menu RNG that the two sides do not share, which would read as "different".
if [ -f "$run/b/digest-net-confirmed.csv" ]; then
  for side in a b; do awk -F, '$2==8 && $3==2' "$run/$side/digest-net-confirmed.csv" > "$run/$side/digest-net-match.csv"; done
  python3 -I "$here/../tools/compare_net_digests.py" "$run/a/digest-net-match.csv" "$run/b/digest-net-match.csv" | sed 's/^/confirmed states in the match scene: /'
fi
for side in a b; do
  [ -z "${MU_SANITIZE:-}" ] || echo "sanitizer reports $side: $(ls "$run/$side"/san.* 2>/dev/null | wc -l | tr -d ' ') files$(cat "$run/$side"/san.* 2>/dev/null | grep -cE '^(WARNING: ThreadSanitizer|==[0-9]+==ERROR: AddressSanitizer)' | sed 's/^/, reports: /')"
  [ -z "${MU_DEAD_FIRST:-}" ] || echo "searches again after a failed connection, $side: $(grep -c 'connection attempt failed, searching again' "$run/$side/console.log" 2>/dev/null)"
done
for side in a b; do
  [ -f "$run/$side/console.log" ] || continue
  echo "== $side"
  grep -E "^net|^play:|^online|slippi: (match id|opponent found|connected|online connection|matchmaking|logged in)|DIVERGED|desync|Desync|cannot|panic|STOPPED|picture: [0-9]+ frames finished" "$run/$side/console.log" | grep -v "net: slow frame\|^\[net\] slippi: \(netplay\|ping\)" | tail -40 | cut -c1-260
done
