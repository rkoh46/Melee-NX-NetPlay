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
#   overlaymenu  the menu's Overlays and Picture pages: one row changed on each (saved to settings.ini), screenshots
#   controls the controller setup (src/controls.cpp) on port 1 read as GameCube adapter port 1 (MU_CONTROLS_TEST_ADAPTER=1)
#            with a saved setup in launcher.ini (X and Y swapped): Start + D-pad Down + Z opens the menu (on the profiles page,
#            MU_CONTROLS_TEST_PAGE), "Save as a new profile", the bindings page (A bound to Z by pressing it), the setup page
#            (stick left/right inverted, then radius 90% with the inverted stick), Start closes; then the game gets Y for X, A for Z and the stick inverted
#            and scaled (the menu's "the game gets" lines); launcher.ini padx.gc1 and the profile file are printed
#   online   two apps, made-up users, the loopback stand-in for matchmaking: each player's controller script goes through
#            the menus to Unranked and plays the match (no generated inputs); the HUD (ping, delay, frame time) is on
# MU_APP_SETTINGS="key value;key value" gives each app folder a settings.ini with those lines (the overlays' online check:
# MU_APP_SETTINGS="playernicknames 1" ./test-app-linux.sh DISC online).
# MU_SANITIZE=address|thread runs the sanitizer build (build.sh with MU_SANITIZE; reports in san.* beside each console log);
# MU_DEAD_FIRST=N (online case): the stand-in matches each app's first N searches with a peer that never answers, so each
# of those connection attempts fails and the app searches again before the two apps are paired.
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
  # MU_APP_SETTINGS="key value;key value": a settings.ini with these lines (the app writes its template around them).
  [ -z "${MU_APP_SETTINGS:-}" ] || printf '%s\n' "$MU_APP_SETTINGS" | tr ';' '\n' > "$dir/melee-nx-netplay/settings.ini"
  if [ "$user" != 0 ]; then
    printf '{"uid":"mock-test-%s","playKey":"mock-test-only","connectCode":"MOCK#00%s","displayName":"Mock %s"}\n' "$user" "$user" "$user" > "$dir/melee-nx-netplay/User/Slippi/user.json"
  fi
}

# Runs the container. $1 the case folder (mounted at /out), $2 the shell commands (DISPLAY is set; `shots N` takes a
# screenshot every N seconds in the background into /out/shots; `wait_log FILE PATTERN SECONDS`; `pad TEXT` writes the
# test pad file the app's input code reads, MELEE_PAD_FILE).
bin=mu-native-gfx-linux-services; san_env=""
case "${MU_SANITIZE:-}" in
  "") ;;
  address) bin=$bin-asan; san_env="ASAN_OPTIONS=detect_leaks=0:handle_segv=0:allow_user_segv_handler=1:log_path=san" ;;
  thread) bin=$bin-tsan; san_env="TSAN_OPTIONS=handle_segv=0:history_size=4:second_deadlock_stack=1:suppressions=/t/tsan_graphics.supp:log_path=san" ;;
  *) echo "MU_SANITIZE must be address or thread" >&2; exit 2 ;;
esac
mock_args=""; [ -z "${MU_DEAD_FIRST:-}" ] || mock_args="--dead-first $MU_DEAD_FIRST"
run() {
  local dir=$1 body=$2
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --network none -e BIN="$bin" -e SAN_ENV="$san_env" -e MOCK_ARGS="$mock_args" -e LP_NUM_THREADS -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$here/../tools:/t:ro" -v "$dir:/out" \
    -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99
      [ -z "$SAN_ENV" ] || export "$SAN_ENV"
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
  if [ -n "${MU_SANITIZE:-}" ]; then
    echo "sanitizer reports: $(find "$dir" -name 'san.*' | wc -l | tr -d ' ') files, $(find "$dir" -name 'san.*' -exec cat {} + 2>/dev/null | grep -cE '^(WARNING: ThreadSanitizer|==[0-9]+==ERROR: AddressSanitizer)') reports"
  fi
  [ -z "${MU_DEAD_FIRST:-}" ] || echo "searches again after a failed connection: $(cat "$dir"/console*.log 2>/dev/null | grep -c 'connection attempt failed, searching again')"
}

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  boot)
    make_app "$dir" 1 1500 -
    run "$dir" 'shots 4; cd /out && /b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log'
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
    run "$dir" 'cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "menu: opened" 120 && { sleep 2.5; shot menu-main; }
      wait_log /out/console.log "play: settings: music" 60 && { sleep 0.3; shot menu-page; }
      wait_log /out/console.log "menu: closed" 60 && { sleep 1.5; shot hud-on; }
      wait $app
      # Second start: what the menu wrote is read back.
      sed -i "s/^script=.*//" /out/melee-nx-netplay/launcher.ini; sed -i "s/^frames=.*/frames=400/" /out/melee-nx-netplay/launcher.ini
      cd /out && /b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log'
    echo "launcher.ini after the menu:"; grep -E "^(volume|hud|keyboard)=" "$dir/melee-nx-netplay/launcher.ini" || true
    echo "settings.ini lines set:"; grep -vE "^#|^$" "$dir/melee-nx-netplay/settings.ini" || true
    ;;
  controls)
    python3 -I - "$root/controls-script.txt" <<'PY'
import sys
lines = ["@scene 01 00"]
t = 300
def press(what, hold=4, gap=14):
    global t
    lines.extend([f"{t} {what}", f"{t+hold}"]); t += hold + gap
press("START+DD+Z", 6, 40)                  # the chord: the menu opens on Controller profiles
for i in range(4): press("sy=-127")         # down to "Save as a new profile"
press("A", 4, 40)
press("B", 4, 30)                           # back to the main page, on Controller profiles
press("sy=127")                             # up: Controller bindings
press("A", 4, 40)
for i in range(2): press("sy=-127")         # row 2: A (row 1 is the hint)
press("A", 4, 30)                           # capture starts (waits for A to be let go)
press("Z", 6, 40)                           # A is now bound to Z
press("B", 4, 30)                           # main page, on Controller bindings
press("sy=127")                             # up: Controller setup
press("Z", 4, 40)                           # Z is A now
for i in range(2): press("sy=-127")         # Invert stick left/right
press("sx=127", 4, 30)
for i in range(4): press("sy=-127")         # Stick radius
for i in range(10): press("sx=127", 3, 9)   # 100 -> 90: the menu reads the stick as the game will, so right is now left
t += 60
press("START", 4, 60)                       # close
press("X", 6, 30)                           # the game gets Y (the saved swap)
lines.extend([f"{t} sx=127", f"{t+10} sx=127 Y", f"{t+20} sx=127", f"{t+30}"]); t += 60   # stick right, inverted and scaled; Y gives X
press("Z", 6, 30)                           # the game gets A and Z (the new binding; Z keeps its own); A opens Unranked
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    make_app "$dir" 1 1900 "$root/controls-script.txt" "padx.gc1=m2=2048,m3=1024"
    run "$dir" 'shots 2; cd /out && MU_CONTROLS_TEST_ADAPTER=1 MU_CONTROLS_TEST_PAGE="Controller profiles" /b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log'
    grep -E "controls:|menu: (opened|closed|the game gets)" "$dir/console.log" | cut -c1-160
    echo "launcher.ini: $(grep '^padx' "$dir/melee-nx-netplay/launcher.ini")"
    for f in "$dir"/melee-nx-netplay/ControllerProfiles/*.profile; do [ -f "$f" ] && { echo "== $(basename "$f")"; grep -vE "^opt|^app" "$f" | tr '\n' ' '; echo; }; done
    ;;
  overlaymenu)
    # The in-game menu's Overlays and Picture pages (feature module pages 4 and 5, after Game, Training and Replays): down
    # 15 rows from the first, A; on Overlays, right on the first row (input display on); B; down one more, A; on Picture,
    # down two rows (Internal resolution) and right; B; Start closes the menu.
    python3 -I - "$root/overlaymenu-script.txt" <<'PY'
import sys
lines = ["@scene 01 00", "300 L+R+START", "345"]
t = 360
for i in range(15): lines += [f"{t} sy=-127", f"{t+4}"]; t += 12
lines += [f"{t+20} A", f"{t+24}"]; t += 120
lines += [f"{t} sx=127", f"{t+4}"]; t += 120
lines += [f"{t} B", f"{t+4}"]; t += 30
lines += [f"{t} sy=-127", f"{t+4}", f"{t+20} A", f"{t+24}"]; t += 120
for i in range(2): lines += [f"{t} sy=-127", f"{t+4}"]; t += 12
lines += [f"{t} sx=127", f"{t+4}"]; t += 120
lines += [f"{t} B", f"{t+4}", f"{t+40} START", f"{t+44}"]
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    make_app "$dir" 1 1800 "$root/overlaymenu-script.txt"
    run "$dir" 'shots 1; cd /out && /b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log'
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
    run "$dir" 'cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
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
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
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
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "update: the build computer has version" 60 && { sleep 3; shot prompt; pad "A"; sleep 0.5; pad ""; }
      wait_log /out/console.log "the new version is" 60 && { sleep 2; shot installed; pad "A"; sleep 0.5; pad ""; }
      wait $app
      # The same version published: no prompt, the game starts.
      sed -i "s/\"$NEWER\"/\"$SAME\"/" /out/server/latest.json
      cd /out && /b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log
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
      (while true; do python3 /t/mock_matchmaking.py 8765 $MOCK_ARGS; sleep 0.2; done) > /out/mock.txt 2>&1 & mock=$!
      sleep 1
      (cd /out/a && sed -i "s|^script=/out/|script=/out/a/|" melee-nx-netplay/launcher.ini && /b/$BIN > /out/console-a.log 2>&1; echo "exit code a $?" >> /out/console-a.log) & a=$!
      (cd /out/b && sed -i "s|^script=/out/|script=/out/b/|" melee-nx-netplay/launcher.ini && /b/$BIN > /out/console-b.log 2>&1; echo "exit code b $?" >> /out/console-b.log) & b=$!
      wait $a; wait $b; kill $mock 2>/dev/null || true'
    cat "$dir/mock.txt" 2>/dev/null | tail -5
    for side in a b; do awk -F, '$2==8 && $3==2' "$dir/$side/melee-nx-netplay/Logs/digest-net-confirmed.csv" > "$dir/$side/match.csv" 2>/dev/null || true; done
    [ -s "$dir/a/match.csv" ] && python3 -I "$here/../tools/compare_net_digests.py" "$dir/a/match.csv" "$dir/b/match.csv" | sed 's/^/confirmed states in the match scene: /'
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  report "$dir"
done
