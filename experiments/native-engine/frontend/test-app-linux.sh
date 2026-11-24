#!/usr/bin/env bash
# The app (the services program started with no arguments) without a console, an account or a network: one container,
# loopback only, a screenless X server, Mesa's software Vulkan driver. Each case gets a fresh app folder
# (build/native-engine-gfx/run-app/<case>/.../melee-nx-netplay, the layout of sdmc:/switch/melee-nx-netplay) with a
# launcher.ini naming the disc (mounted read-only) and the test aids the app itself reads (script=, frames=).
#   ./test-app-linux.sh /path/to/disc-folder CASE...
# Cases:
#   boot     no arguments, a made-up user.json: the game boots into its menus (Slippi's Online Play page)
#   menu     as boot, and the controller script opens the in-game menu (L + R + Start), lowers the volume, lowers the music
#            on the Game options page, turns the HUD on, turns on Training's "Inputs and frame advantage" (a feature module's
#            row, saved as lab_overlay 1) and closes it; then a second start reads the changes back
#   handover the menu with the D-pad, closed with B held for 12 retraces, then A in the game: the D-pad moves the cursor and
#            changes the volume, the closing B never reaches the game (it stays on Online Play), the later A does (Unranked
#            opens Slippi's character select, scene 8)
#   signin   no user.json: the sign-in screen comes first; B (Play offline) through the test pad file starts the game offline
#   update   a fake build computer on 127.0.0.1 (python http.server) publishes the version after VERSION: the prompt, A, the download, the
#            check, the swap of melee-nx-netplay.nro (a dummy), the notice, A: the app closes; then the same version: no prompt
#   online   two apps, made-up users, the loopback stand-in for matchmaking: each player's controller script goes through
#            the menus to Unranked and plays the match (no generated inputs); the HUD (ping, delay, frame time) is on
# Output: build/native-engine-gfx/run-app/<case>/ (console.log, the app folder with Logs/latest.log, shots/).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-app"
mkdir -p "$root"

# An app folder: launcher.ini, optionally a made-up user.json and a controller script.
#   make_app DIR USER_INDEX(0: none) FRAMES SCRIPT_FILE(or -) EXTRA_INI_LINES...
make_app() {
  local dir=$1 user=$2 frames=$3 script=$4; shift 4
  rm -rf "$dir"; mkdir -p "$dir/melee-nx-netplay/User/Slippi" "$dir/shots"
  { echo "iso=/iso/GALE01.iso"; echo "frames=$frames"; [ "$script" = - ] || echo "script=/out/$(basename "$script")"; for l in "$@"; do echo "$l"; done; } > "$dir/melee-nx-netplay/launcher.ini"
  [ "$script" = - ] || cp "$script" "$dir/"
  if [ "$user" != 0 ]; then
    printf '{"uid":"mock-test-%s","playKey":"mock-test-only","connectCode":"MOCK#00%s","displayName":"Mock %s"}\n' "$user" "$user" "$user" > "$dir/melee-nx-netplay/User/Slippi/user.json"
  fi
}

# Runs the container. $1 the case folder (mounted at /out), $2 the shell commands (DISPLAY is set; `shots N` takes a
# screenshot every N seconds in the background into /out/shots; `wait_log FILE PATTERN SECONDS`; `pad TEXT` writes the
# test pad file the app's input code reads, MELEE_PAD_FILE).
run() {
  local dir=$1 body=$2
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$here/../tools:/t:ro" -v "$dir:/out" \
    -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      shots() { (n=0; while true; do sleep "$1"; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true; done) & }
      shot() { xwd -root -silent 2>/dev/null | convert xwd:- "/out/shots/$1.png" 2>/dev/null || true; }
      wait_log() { for i in $(seq $(( $3 * 10 ))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; echo "TIMEOUT waiting for: $2" >> /out/harness.log; return 1; }
      pad() { printf "%s\n" "$1" > /out/pad.txt; }
      '"$body" || echo "test-app-linux: container ended with $?"
}

report() {
  local dir=$1
  echo "== $(basename "$dir")"
  [ -f "$dir/harness.log" ] && cat "$dir/harness.log"
  grep -hE "^app:|^play: (started|settings|options|online|match|stopped|cannot)|^update:|^menu:|^picture: controller script|sign-in|services check|online connection|opponent found|exit code|instance exit|DIVERGED|desync|STOPPED|---- done" "$dir"/console*.log 2>/dev/null | cut -c1-240 | tail -${MU_REPORT_LINES:-40}
  echo "shots: $(ls "$dir/shots" 2>/dev/null | wc -l | tr -d ' ')"
}

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  boot)
    make_app "$dir" 1 1500 -
    run "$dir" 'shots 4; cd /out && /b/mu-native-gfx-linux-services > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log'
    ;;
  menu)
    cat > "$root/menu-script.txt" <<'EOF'
# The in-game menu from the main menu (scene 01 00): L + R + Start held, then the console's rows: Texture filtering,
# Aspect ratio, Screen filter, Antialiasing, Volume (4 down), Online delay, Light shield, Stick, then the four settings
# pages, the feature modules' three pages (Game, Training, Replays), Controls, Performance HUD, Resume, Quit.
@scene 01 00
300 L+R+START
345
360 sy=-127
364
372 sy=-127
376
384 sy=-127
388
396 sy=-127
400
410 sx=-127
414
424 sx=-127
428
438 sx=-127
442
452 sy=-127
456
464 sy=-127
468
476 sy=-127
480
488 sy=-127
492
504 A
508
520 sy=-127
524
532 sy=-127
536
544 sy=-127
548
560 sx=-127
564
574 sx=-127
578
600 B
604
616 sy=-127
620
628 sy=-127
632
640 sy=-127
644
652 sy=-127
656
664 sy=-127
668
676 sy=-127
680
688 sy=-127
692
700 sy=-127
704
716 sx=127
720
# up 3 to the Training page (a feature module's), A, down 4 to "Inputs and frame advantage", right: lab_overlay 1
728 sy=127
732
740 sy=127
744
752 sy=127
756
768 A
772
784 sy=-127
788
796 sy=-127
800
808 sy=-127
812
820 sy=-127
824
836 sx=127
840
860 START
864
EOF
    make_app "$dir" 1 2100 "$root/menu-script.txt"
    run "$dir" 'cd /out && (/b/mu-native-gfx-linux-services > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "menu: opened" 120 && { sleep 2.5; shot menu-main; }
      wait_log /out/console.log "play: settings: music" 60 && { sleep 0.3; shot menu-page; }
      wait_log /out/console.log "menu: closed" 60 && { sleep 1.5; shot hud-on; }
      wait $app
      # Second start: what the menu wrote is read back.
      sed -i "s/^script=.*//" /out/melee-nx-netplay/launcher.ini; sed -i "s/^frames=.*/frames=400/" /out/melee-nx-netplay/launcher.ini
      cd /out && /b/mu-native-gfx-linux-services > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log'
    echo "launcher.ini after the menu:"; grep -E "^(volume|hud|keyboard)=" "$dir/melee-nx-netplay/launcher.ini" || true
    echo "settings.ini lines set:"; grep -vE "^#|^$" "$dir/melee-nx-netplay/settings.ini" || true
    ;;
  handover)
    cat > "$root/handover-script.txt" <<'EOF'
# The console's sequence: open the menu, D-pad down to Volume (4 rows), D-pad left 3 times (100 -> 70), close it with B
# held for 12 retraces, then A in the game (on Online Play: Unranked).
@scene 01 00
300 L+R+START
345
360 DD
364
372 DD
376
384 DD
388
396 DD
400
410 DL
414
424 DL
428
438 DL
442
470 B
482
560 A
566
EOF
    [ -z "${MU_HANDOVER_SCRIPT:-}" ] || cp "$MU_HANDOVER_SCRIPT" "$root/handover-script.txt"   # another sequence (diagnosis)
    make_app "$dir" 1 1200 "$root/handover-script.txt" ${MU_HANDOVER_INI:-}
    run "$dir" 'cd /out && (/b/mu-native-gfx-linux-services > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "menu: opened" 120 && { sleep 2; shot menu-dpad; }
      wait_log /out/console.log "menu: closed with" 60 && { sleep 1; shot after-close; }
      wait_log /out/console.log "scene 8/0" 60 && { sleep 2; shot after-a; }
      wait $app'
    echo "launcher.ini volume: $(grep '^volume=' "$dir/melee-nx-netplay/launcher.ini")"
    grep -E "menu:|play: scene|online menu" "$dir/console.log" | cut -c1-160
    ;;
  signin)
    make_app "$dir" 0 1200 -
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      cd /out && (/b/mu-native-gfx-linux-services > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "the sign-in screen comes first" 120 && { sleep 4; shot signin; pad "B"; }
      wait_log /out/console.log "sign-in: playing offline" 30 && rm -f /out/pad.txt
      sleep 12; shot game-offline
      wait $app'
    ;;
  update)
    make_app "$dir" 1 600 - "update_host=http://127.0.0.1:8080"
    mkdir -p "$dir/server"
    head -c 3000000 /dev/urandom > "$dir/server/melee-nx-netplay-test.nro"
    sha=$(shasum -a 256 "$dir/server/melee-nx-netplay-test.nro" | cut -d' ' -f1)
    same=$(cat "$here/VERSION"); newer="${same%.*}.$(( ${same##*.} + 1 ))"
    printf '{"schema":1,"app_version":"'"$newer"'","slippi_version":"3.7.0","built":"2026-11-23T00:00:00Z","commit":"test","files":{"game":{"name":"melee-nx-netplay-test.nro","size":3000000,"sha256":"%s"}},"pending":null}\n' "$sha" > "$dir/server/latest.json"
    echo "the installed build (a dummy)" > "$dir/melee-nx-netplay/melee-nx-netplay.nro"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt NEWER='"$newer"' SAME='"$same"'; pad ""
      (cd /out/server && python3 -m http.server 8080 --bind 127.0.0.1 > /out/server.log 2>&1) & server=$!
      sleep 1
      cd /out && (/b/mu-native-gfx-linux-services > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "update: the build computer has version" 60 && { sleep 3; shot prompt; pad "A"; sleep 0.5; pad ""; }
      wait_log /out/console.log "the new version is" 60 && { sleep 2; shot installed; pad "A"; sleep 0.5; pad ""; }
      wait $app
      # The same version published: no prompt, the game starts.
      sed -i "s/\"$NEWER\"/\"$SAME\"/" /out/server/latest.json
      cd /out && /b/mu-native-gfx-linux-services > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log
      kill $server'
    echo "installed file now: $(shasum -a 256 "$dir/melee-nx-netplay/melee-nx-netplay.nro" | cut -c1-16) (published $(echo $sha | cut -c1-16)); backup: $(cat "$dir/melee-nx-netplay/melee-nx-netplay.nro.bak" 2>/dev/null)"
    ;;
  online)
    rm -rf "$dir"; mkdir -p "$dir/shots"
    # The menu script as committed (Online Play > Unranked), side b one fighter further, then moves keyed on the retrace.
    python3 -I - "$here/scripts/online_unranked.txt" "$dir" <<'PY'
import sys
menu = open(sys.argv[1]).read()
for side, period, stick in (("a", 47, 80), ("b", 61, -80)):
    text = menu if side == "a" else menu.replace("132\n150 A", "132 sx=127\n138\n150 A")
    head, gates = text.split("@scene", 1)
    lines = []
    for t in range(600, 30000, period):
        lines += [f"{t} sx={stick}", f"{t + period // 3} A sx={stick}", f"{t + period // 3 + 4} sx={-stick}", f"{t + 2 * period // 3} X", f"{t + 2 * period // 3 + 4}"]
    open(f"{sys.argv[2]}/pads-{side}.txt", "w").write(head + "\n".join(lines) + "\n@scene" + gates)
PY
    for side in a b; do
      user=1; hud=hud=1; [ $side = b ] && user=2
      mkdir -p "$dir/$side"; make_app "$dir/$side" $user "${MU_ONLINE_FRAMES:-4200}" "$dir/pads-$side.txt" "$hud"
    done
    run "$dir" 'shots 2
      (while true; do python3 /t/mock_matchmaking.py 8765; sleep 0.2; done) > /out/mock.txt 2>&1 & mock=$!
      sleep 1
      (cd /out/a && sed -i "s|^script=/out/|script=/out/a/|" melee-nx-netplay/launcher.ini && /b/mu-native-gfx-linux-services > /out/console-a.log 2>&1; echo "exit code a $?" >> /out/console-a.log) & a=$!
      (cd /out/b && sed -i "s|^script=/out/|script=/out/b/|" melee-nx-netplay/launcher.ini && /b/mu-native-gfx-linux-services > /out/console-b.log 2>&1; echo "exit code b $?" >> /out/console-b.log) & b=$!
      wait $a; wait $b; kill $mock 2>/dev/null || true'
    cat "$dir/mock.txt" 2>/dev/null | tail -5
    for side in a b; do awk -F, '$2==8 && $3==2' "$dir/$side/melee-nx-netplay/Logs/digest-net-confirmed.csv" > "$dir/$side/match.csv" 2>/dev/null || true; done
    [ -s "$dir/a/match.csv" ] && python3 -I "$here/../tools/compare_net_digests.py" "$dir/a/match.csv" "$dir/b/match.csv" | sed 's/^/confirmed states in the match scene: /'
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  report "$dir"
done
