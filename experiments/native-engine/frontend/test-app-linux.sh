#!/usr/bin/env bash
# The app (the services program started with no arguments) without a console, an account or a network: one container,
# loopback only, a screenless X server, Mesa's software Vulkan driver. Each case gets a fresh app folder
# (build/native-engine-gfx/run-app/<case>/.../melee-nx-netplay, the layout of sdmc:/switch/melee-nx-netplay) with a
# launcher.ini naming the disc (mounted read-only) and the test aids the app itself reads (script=, frames=).
#   ./test-app-linux.sh /path/to/disc-folder CASE...
# Cases:
#   (The home screen and the settings panel are melee-unlocked's, src/mu_home.cpp and src/mu_panel.cpp; the cases drive them through
#   the test pad file, MELEE_PAD_FILE, and follow the panel's focus by its "settings: focus" log lines.)
#   boot     no arguments, a made-up user.json: the home screen, A (PLAY): the game boots into its menus (Slippi's Online Play page)
#   home     the home screen's pages (Play, Multiplayer Lobby, Replay Viewer, Mods), the panel from its Settings page, PLAY
#   panel    the panel in the game: each of its eight categories (screenshots)
#   menu     the panel (L + R + Start): AUDIO Volume 100 -> 70, Music volume 100 -> 80, OVERLAYS Performance HUD on, the Practice
#            chord opens GAME on Training: "Inputs and frame advantage" on (saved as lab_overlay 1); a second start reads them back
#   handover the panel with the D-pad, closed with B held for 12 retraces, then A in the game: the closing B never reaches the game
#            (it stays on Online Play), the later A does (Unranked opens Slippi's character select, scene 8)
#   signin   no user.json: the home screen's Sign in (past PLAY and the version line's What's new) opens the sign-in screen; B (Play offline) back to the home screen; PLAY: offline
#   update   a fake build computer on 127.0.0.1 (python http.server) publishes the version after VERSION: offered on the home screen's
#            Play page, "Update and restart", the download, the check, the swap of melee-nx-netplay.nro (a dummy), the notice, A: the app
#            closes; then the same version: nothing offered, PLAY
#   overlaymenu  the panel's OVERLAYS page and VIDEO's Picture section: one row changed on each (saved to settings.ini), screenshots
#   controls the controller setup (src/controls.cpp) on port 1 read as GameCube adapter port 1 (MU_CONTROLS_TEST_ADAPTER=1) with a
#            saved setup in launcher.ini (X and Y swapped): Start + D-pad Down + Z opens the panel on Controller profiles
#            (MU_CONTROLS_TEST_PAGE), "Save as a new profile", the A binding pressed onto Z, stick left/right inverted, Stick radius 90
#            with the inverted stick, Start closes; the game gets Y for X, A for Z, the stick inverted and scaled; a second start loads it
#   phob     melee-unlocked's PhobGCC menu with a scripted GameCube adapter: PhobVision, setups, PhobScope's plots, the reaction test, the
#            remap editor, Phob Check, the coordinate viewer (screenshots; see the case)
#   dolphin  a Dolphin GCPad profile imported onto an SDL virtual gamepad (MU_TEST_VIRTUAL_PAD) from the Controller profiles page
#   pcsettings  "PC Settings" in Melee's Options menu: Z there opens the settings panel
#   box      box mode (Mod X bound) on an adapter port: B0XX coordinates, Mod X, second-input priority (the "the game gets" lines)
#   styles   melee-unlocked's six menu styles in the game and its two legacy menus from the home screen (screenshots); the
#            settings search with the software keyboard's test answer
#   keyboard a USB keyboard (src/usb_keyboard.cpp, the test key file in place of libnx's HID keyboard): melee-unlocked's layout as
#            port 1, the settings search typed in the box over the game, a key rebound on Controller bindings (see the case)
#   launcher the home screen's What's new, Language (Deutsch), launcher colour wheel and a rail picture (screenshots)
#   replaylib the home screen's Replay Viewer: search, the session trace graph, compress all, delete to .trash and undo, the
#            folder browser; then As played and the watch queue (needs the record and online runs, see the case)
#   notices  the "Settings" reminder in the menus (screenshot) and the panel's menu sounds (their log lines)
#   crashreport  "Save crash report" on the home screen: the zip and its companion in Logs/, the account's name and code absent
#   egg      the character select easter egg (Bailey and Walter), screenshots
#   rvz      a Dolphin RVZ image in place of the disc: the conversion's progress screen (melee-unlocked's small fixture, slowed down)
#   online   two apps, made-up users, the loopback stand-in for matchmaking: each player's controller script goes through
#            the menus to Unranked and plays the match (no generated inputs); the HUD (ping, delay, frame time) is on
# MU_APP_SETTINGS="key value;key value" gives each app folder a settings.ini with those lines (the overlays' online check:
# MU_APP_SETTINGS="playernicknames 1" ./test-app-linux.sh DISC online).
# MU_APP_SKIN=1 (online case): the first app has a skin the online rule turns off: the "skins are off" note (screenshots).
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
  # MU_APP_SKIN=1: user 1's app folder gets a Falco skin made from the disc (../tools/make_test_skin.py, joint moved 5 units), picked
  # for Falco's standard costume: melee-unlocked's online rule turns it off, and the online screens show the panel's note.
  if [ "${MU_APP_SKIN:-0}" = 1 ] && [ "$user" = 1 ]; then
    mkdir -p "$dir/melee-nx-netplay/Cosmetics/Costumes"
    python3 -I "$here/../tools/make_test_skin.py" "$disc/GALE01.iso" "$dir/melee-nx-netplay/Cosmetics/Costumes/PlFcNr - Moved bone.dat"
    printf '# test\n/plfcnr.dat\tMoved bone\n' > "$dir/melee-nx-netplay/Cosmetics/selection.txt"
  fi
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
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-app}-$case-$$-$RANDOM" --network none -e BIN="$bin" -e SAN_ENV="$san_env" -e MOCK_ARGS="$mock_args" -e LP_NUM_THREADS -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$here/../tools:/t:ro" -v "$dir:/out" \
    -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99
      [ -z "$SAN_ENV" ] || export "$SAN_ENV"
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      shots() { (n=0; while true; do sleep "$1"; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true; done) & }
      shot() { xwd -root -silent 2>/dev/null | convert xwd:- "/out/shots/$1.png" 2>/dev/null || true; }
      wait_log() { for i in $(seq $(( $3 * 10 ))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; echo "TIMEOUT waiting for: $2" >> /out/harness.log; return 1; }
      pad() { printf "%s\n" "$1" > /out/pad.txt; }
      # The test pad file (MELEE_PAD_FILE) drives the home screen and the settings panel: tap BUTTONS; focus LABEL [BUTTON] taps
      # (D-pad down by default) until the panel names LABEL as the focused row ("settings: focus" in the log).
      tap() { pad "$1"; sleep ${2:-0.12}; pad ""; sleep ${3:-0.5}; }
      focus() { for i in $(seq 140); do [ "$(grep -a "settings: focus" ${LOG:-/out/console.log} | tail -1)" = "settings: focus $1" ] && return 0; tap ${2:-DD}; done; echo "TIMEOUT focus $1" >> /out/harness.log; return 1; }
      '"$body" || echo "test-app-linux: container ended with $?"
}

report() {
  local dir=$1
  echo "== $(basename "$dir")"
  [ -f "$dir/harness.log" ] && cat "$dir/harness.log"
  grep -hE "^app:|^play: (started|settings|options|online|match|stopped|cannot)|^update:|^menu:|^picture: controller script|sign-in|skins off online|services check|online connection|opponent found|exit code|instance exit|DIVERGED|desync|STOPPED|---- done" "$dir"/console*.log 2>/dev/null | cut -c1-240 | tail -${MU_REPORT_LINES:-40} || true   # a case with sub-runs (phob) has no console.log of its own
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
    # No arguments, a made-up user.json: melee-unlocked's launcher comes up on its Play page with PLAY focused; A starts the game.
    make_app "$dir" 1 1500 -
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""; shots 4
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot home; tap A; }
      wait $app'
    ;;
  rvz)
    # A Dolphin RVZ image in place of GALE01.iso: the conversion's progress screen (bar, gigabytes, time left) while it writes the
    # plain image. melee-unlocked's own small RVZ fixture (a made-up GameCube disc, tests/data/rvz) under MU_TEST_RVZ_ANY, slowed
    # down by MU_TEST_RVZ_SLOW_MS so the screen is seen; the made-up disc then does not boot, which is expected.
    make_app "$dir" 1 300 -
    sed -i.bak 's|^iso=.*|iso=/out/disc/GALE01.iso|' "$dir/melee-nx-netplay/launcher.ini"; rm -f "$dir/melee-nx-netplay/launcher.ini.bak"
    mkdir -p "$dir/disc"; cp "$repo/build/native-engine/mu/port/tests/data/rvz/rvz-zstd.rvz" "$dir/disc/GALE01.rvz"
    run "$dir" 'export MU_TEST_RVZ_ANY=1 MU_TEST_RVZ_SLOW_MS=700
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "converting the disc: [3-9]0%" 120 && { sleep 0.3; shot rvz-progress; }
      wait_log /out/console.log "written in" 120; sleep 0.5; kill $app 2>/dev/null; wait $app'
    echo "image written: $(ls -la "$dir/disc/GALE01.iso" 2>/dev/null | awk '{print $5}') bytes (the fixture's disc is 176185)"
    ;;
  menu)
    # The settings panel in the game (L + R + Start on the main menu), through the test pad file: AUDIO > Volume 100 -> 70 (A, then
    # left three times), Music volume 100 -> 80; OVERLAYS > Performance HUD on; the Practice chord (Start + D-pad Up + Z) opens GAME on
    # its Training section: "Inputs and frame advantage" on (a feature module's row, saved as lab_overlay 1); Start closes it. A
    # second start reads the changes back.
    make_app "$dir" 1 4500 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "L R START"; sleep 1.2; pad ""; sleep 1.5; shot menu-main
      tap DD; tap A; focus Volume; tap A; tap DL; tap DL; tap DL; sleep 0.5; shot menu-volume; tap B
      tap A; focus "Music volume"; tap DL; tap DL; sleep 0.5; shot menu-page; tap B
      tap DD; tap DD; tap DD; tap A; focus "Performance HUD"; tap A; tap B; sleep 0.5
      tap START; sleep 1
      pad "START DU Z"; sleep 0.3; pad ""; sleep 1.5; focus "Inputs and frame advantage"; tap A; sleep 0.5; shot menu-training
      tap START; sleep 2; shot hud-on
      wait $app
      # Second start: what the panel wrote is read back.
      sed -i "s/^frames=.*/frames=400/" /out/melee-nx-netplay/launcher.ini
      cd /out && /b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log'
    echo "launcher.ini after the menu:"; grep -E "^(volume|hud|keyboard)=" "$dir/melee-nx-netplay/launcher.ini" || true
    echo "settings.ini lines set:"; grep -vE "^#|^$" "$dir/melee-nx-netplay/settings.ini" || true
    grep -E "launcher.ini:|music [0-9]+%" "$dir/console2.log" | cut -c1-200 | head -3
    ;;
  phob)
    # melee-unlocked's PhobGCC menu (src/phob_menu_glue.cpp, features/phob.cpp) with a scripted GameCube adapter in socket 1
    # (MELEE_TEST_ADAPTER_SCRIPT, upstream's test aid: raw adapter reports at 1 kHz through src/phob_adapter.cpp). Each run opens the
    # panel on the PhobGCC page (Start + D-pad Down + Z with MU_CONTROLS_TEST_PAGE) and presses "Open the PhobGCC menu" or "Controller
    # test"; MELEE_TEST_PHOB=<screen> (upstream's aid) starts it on that screen and logs its screen and controller ("phob test:").
    #   nav       PhobVision's top screen: PhobGCC Settings on (A), Setups > Save New Setup (PhobGCC/Setups/Setup 1.ini), PhobScope >
    #             Input Viewer, back (B) to the top and back to the panel, Start
    #   scope     Value vs Time Plots (snapback on the left stick X, then dashback): flicks from the script, the results, then Phob Check
    #   stickmap  Stickmap Plots: Start records, the script rolls the stick around; then the Gate Visualizer
    #   reaction  Reaction Time Test: Start, the square, the script's press; the delay logged
    #   remap     Button Remapping, listening for A (upstream's remap:0): the script presses B, so B presses A (port1.ini remap)
    #   coord     Controller test (PhobScope as the top screen): Coordinate Viewer at 4x with the stick in a notch
    # MU_PHOB_RUNS="nav scope ..." runs only those.
    phob_run() {   # $1 name, $2 MELEE_TEST_PHOB screen, $3 action, $4 the open row, $5 adapter script, $6 the taps once it is open
      case " ${MU_PHOB_RUNS:-nav scope stickmap reaction remap coord} " in *" $1 "*) ;; *) return 0 ;; esac
      local sub="$dir/$1"; make_app "$sub" 1 3600 - home=0
      printf '%s\n' "$5" > "$sub/adapter.txt"
      run "$sub" 'export MELEE_PAD_FILE=/out/pad.txt MU_CONTROLS_TEST_PAGE=PhobGCC MELEE_TEST_ADAPTER_SCRIPT=/out/adapter.txt MELEE_TEST_PHOB='"$2"' MELEE_TEST_PHOB_ACTION="'"$3"'"
        [ "'"$1"'" = coord ] && export MELEE_TEST_CONTROLLER_TEST=1
        pad ""
        cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
        app=$!
        wait_log /out/console.log "online menu: row 1" 120 && sleep 3
        pad "START DD Z"; sleep 0.3; pad ""; sleep 1.5
        focus "'"$4"'"; shot panel; tap A; sleep 2; shot open
        '"$6"'
        wait $app'
      echo "== $1"; grep -aE "phob|gc adapter: test|exit code" "$sub/console.log" | grep -v "menu pad" | cut -c1-200 | tail -25
      grep -a "phob test: menu pad" "$sub/console.log" | awk '{print $NF}' | uniq | tr '\n' ' '; echo "(screens visited)"
    }
    mkdir -p "$dir"
    phob_run nav 0 "" "Open the PhobGCC menu (PhobVision, setups, remap)" "60000 128 128 128 128 0 0" \
      'tap DD; tap A; sleep 0.5; shot settings-on; for i in 1 2 3 4 5; do tap DD; done; tap A; sleep 0.8; shot setups
       tap DD; tap DD; tap A; sleep 0.8; shot setup-saved; tap B; sleep 0.5; tap DU; tap DU; tap A; sleep 0.5; tap A; sleep 2; shot inputview
       tap B; sleep 0.4; tap B; sleep 0.4; tap B; sleep 2; shot back-to-panel; tap START; sleep 1'
    echo "setups: $(ls "$dir/nav/melee-nx-netplay/PhobGCC/Setups" 2>/dev/null | tr '\n' ' ')"; echo "port1.ini: $(grep -iE '^(enabled|phob_hardware)' "$dir/nav/melee-nx-netplay/PhobGCC/port1.ini" 2>/dev/null | tr '\n' ' ')"
    # Flicks: from the rim (X 230) let go to the centre with an overshoot the other way (snapback), every 1.5 s; then dashes.
    flicks=""; for i in 1 2 3 4 5 6; do flicks="$flicks
1200 128 128 128 128 0 0
250 230 128 128 128 0 0
4 175 128 128 128 0 0
4 110 128 128 128 0 0
5 92 128 128 128 0 0
6 118 128 128 128 0 0"; done
    phob_run scope 27 "input:0" "Open the PhobGCC menu (PhobVision, setups, remap)" "4000 128 128 128 128 0 0$flicks
2000 128 128 128 128 0 0" \
      'sleep 12; shot snapback; tap B; sleep 0.4; tap B; sleep 0.4; for i in 1 2 3 4 5; do tap DD; done; tap A; sleep 1.5; shot phobcheck; tap START'
    circle=""; for a in 0 30 60 90 120 150 180 210 240 270 300 330 360; do x=$(python3 -c "import math;print(int(128+100*math.cos(math.radians($a))))"); y=$(python3 -c "import math;print(int(128+100*math.sin(math.radians($a))))"); circle="$circle
40 $x $y 128 128 0 0"; done
    phob_run stickmap 26 "map:2" "Open the PhobGCC menu (PhobVision, setups, remap)" "6000 128 128 128 128 0 0$circle$circle$circle
8000 128 128 128 128 0 0$circle$circle$circle$circle
3000 128 128 128 128 0 0" \
      'tap START; sleep 4; shot stickmap; tap B; sleep 0.4; for i in 1 2 3 4 5 6; do tap DD; done; tap A; sleep 4; shot gate; tap START'
    presses=""; for i in $(seq 70); do presses="$presses
60 128 128 128 128 0 0 A
140 128 128 128 128 0 0"; done
    phob_run reaction 29 "" "Open the PhobGCC menu (PhobVision, setups, remap)" "3000 128 128 128 128 0 0$presses" \
      'tap START; sleep 11; shot reaction; tap B; sleep 0.5; tap START'
    phob_run remap 18 "remap:0" "Open the PhobGCC menu (PhobVision, setups, remap)" "3000 128 128 128 128 0 0
120 128 128 128 128 0 0 B
30000 128 128 128 128 0 0" \
      'sleep 5; shot remap; tap START'
    echo "port1.ini after the remap: $(grep -i 'remap' "$dir/remap/melee-nx-netplay/PhobGCC/port1.ini" 2>/dev/null | tr '\n' ' ')"
    phob_run coord 35 "zoom:2" "Controller test (PhobScope, Phob Check)" "3000 128 128 128 128 0 0
2500 200 185 128 128 0 0
30000 128 128 128 128 0 0" \
      'sleep 2; shot coord; sleep 1.5; tap B; sleep 0.5; shot scope-top; tap START'
    ;;
  box)
    # Box controllers (upstream's box_melee.h through src/controls.cpp) on port 1 read as GameCube adapter port 1
    # (MU_CONTROLS_TEST_ADAPTER=1), launcher.ini binding the stick's left/right to the D-pad's left/right and Mod X to D-pad up
    # (the D-pad's own actions unbound): box mode. The panel is opened and closed (so the "the game gets" lines are logged),
    # then D-pad right (the stick right: B0XX's full 1.0), D-pad up + right (Mod X: 0.6625), right then left held (second
    # input priority: left), left let go (right again, no reactivation: neutral), all let go.
    make_app "$dir" 1 2400 - "padx.gc1=m8=0,m10=0,m11=0,m18=1,m19=2,m22=8" home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_CONTROLS_TEST_ADAPTER=1; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "START DD Z"; sleep 0.3; pad ""; sleep 1.5; tap START; sleep 1.5
      pad "DR"; sleep 0.6; pad "DR DU"; sleep 0.6; pad "DR"; sleep 0.4; pad "DR DL"; sleep 0.6; pad "DR"; sleep 0.6; pad ""; sleep 1
      wait $app'
    grep -E "controls:|menu: (closed|the game gets)" "$dir/console.log" | cut -c1-160 | tail -14
    ;;
  dolphin)
    # Dolphin profile import (controls_files.cpp) onto an SDL controller: an SDL virtual gamepad (MU_TEST_VIRTUAL_PAD=1, no
    # controller in the container) and a Dolphin GCPad profile in <app folder>/Dolphin/Melee.ini (SDL bindings, sticks swapped,
    # a dead zone): the panel on Controller profiles, "Import it into this controller"; the import's line and the saved profile.
    make_app "$dir" 1 2400 - home=0
    mkdir -p "$dir/melee-nx-netplay/Dolphin"
    printf '%s\n' '[Profile]' 'Device = SDL/0/Test virtual gamepad' 'Buttons/A = `Button E`' 'Buttons/B = `Button S`' 'Buttons/X = `Button N`' \
      'Main Stick/Up = `Right Y+`' 'Main Stick/Down = `Right Y-`' 'Main Stick/Left = `Right X-`' 'Main Stick/Right = `Right X+`' \
      'C-Stick/Up = `Left Y+`' 'C-Stick/Down = `Left Y-`' 'Main Stick/Dead Zone = 12.0' > "$dir/melee-nx-netplay/Dolphin/Melee.ini"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_TEST_VIRTUAL_PAD=1 MU_CONTROLS_TEST_PAGE="Controller profiles"; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "START DD Z"; sleep 0.3; pad ""; sleep 1.5
      # An SDL controller has many binding rows above the profiles: a longer walk than focus allows.
      for i in $(seq 420); do [ "$(grep -a "settings: focus" /out/console.log | tail -1)" = "settings: focus Import it into this controller" ] && break; tap DD 0.08 0.25; done
      sleep 0.5; shot import; tap A; sleep 1; shot imported; tap START
      wait $app'
    grep -E "controls:" "$dir/console.log" | cut -c1-200 | tail -6
    for f in "$dir"/melee-nx-netplay/ControllerProfiles/*.profile; do [ -f "$f" ] && { echo "== $(basename "$f")"; head -5 "$f" | tr '\n' ' '; echo; }; done
    ;;
  pcsettings)
    # The "PC Settings" row in Melee's Options menu (features/pcsettings.cpp): from Online Play (menu 8), B to 1P (menu 1), B to
    # the main menu (menu 0), down to Options, A (menu 4), down twice, Z: the settings panel opens (the Z is kept from the game).
    # feature_test=menu-trace logs the menu and row as they change (the fourth slot, upstream's PC Settings row, is skipped).
    make_app "$dir" 1 3000 - home=0 feature_test=menu-trace
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""; shots 4
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      tap B; sleep 1.5; tap B; sleep 1.5
      for i in 1 2 3 4 5 6; do grep -aq "pc settings: menu 0 row 3" /out/console.log && break; tap DD 0.12 0.6; done; tap A; sleep 1.5
      tap DD; tap DD; sleep 0.5; shot options-row; tap Z; sleep 1.5; shot panel
      wait $app'
    grep -E "pc settings:|settings: opened" "$dir/console.log" | cut -c1-160 | tail -12
    ;;
  controls)
    # The controller setup (src/controls.cpp) on port 1 read as GameCube adapter port 1 (MU_CONTROLS_TEST_ADAPTER=1), a saved setup
    # in launcher.ini (X and Y swapped), through the test pad file: Start + D-pad Down + Z opens the panel on the Controller profiles
    # section (MU_CONTROLS_TEST_PAGE); "Save as a new profile"; up to the bindings, A bound to Z by pressing it; up to the setup,
    # stick left/right inverted (Z, which is A now), Stick radius 100 -> 90 with the inverted stick; Start closes. Then the game gets
    # Y for X, A for Z and the stick inverted and scaled (the "the game gets" lines). A second start loads the profile.
    make_app "$dir" 1 7200 - "padx.gc1=m2=2048,m3=1024" home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_CONTROLS_TEST_ADAPTER=1 MU_CONTROLS_TEST_PAGE="Controller profiles"; pad ""; shots 3
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "START DD Z"; sleep 0.3; pad ""; sleep 1.5
      focus "Save as a new profile"; tap A; sleep 0.5
      focus A DU; tap A; sleep 0.8; tap Z 0.3; sleep 1
      focus "Invert stick left/right" DU; tap Z; sleep 0.8
      focus "Stick radius"; for i in 1 2 3 4 5 6 7 8 9 10; do tap "sx=127" 0.12 0.35; done; sleep 0.5; shot setup
      tap START; sleep 1.5
      tap X 0.2; pad "sx=127"; sleep 0.2; pad "sx=127 Y"; sleep 0.2; pad "sx=127"; sleep 0.2; pad ""; sleep 1; tap Z 0.2
      wait $app'
    grep -E "controls:|menu: (opened|closed|the game gets)" "$dir/console.log" | cut -c1-160
    echo "launcher.ini: $(grep '^padx' "$dir/melee-nx-netplay/launcher.ini")"
    # Second start without the setup: the chord, "Load this profile" (GameCube 1, saved above with the X/Y swap), Start; then X
    # reaches the game as Y again.
    sed -i.bak -e '/^padx\./d' -e 's/^frames=.*/frames=2400/' "$dir/melee-nx-netplay/launcher.ini"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_CONTROLS_TEST_ADAPTER=1 MU_CONTROLS_TEST_PAGE="Controller profiles" LOG=/out/console2.log; pad ""
      cd /out && (/b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log) &
      app=$!
      wait_log /out/console2.log "online menu: row 1" 120 && sleep 3
      pad "START DD Z"; sleep 0.3; pad ""; sleep 1.5
      # The rows above the profiles grew (PhobGCC): a longer walk than focus allows (it timed out since 0.4.7).
      for i in $(seq 420); do [ "$(grep -a "settings: focus" /out/console2.log | tail -1)" = "settings: focus Load this profile" ] && break; tap DD 0.08 0.25; done
      tap A; sleep 0.5; tap START; sleep 1.5; tap X 0.2
      wait $app'
    echo "second start:"; grep -E "controls:|menu: the game gets" "$dir/console2.log" | cut -c1-160
    echo "launcher.ini: $(grep '^padx' "$dir/melee-nx-netplay/launcher.ini")"
    for f in "$dir"/melee-nx-netplay/ControllerProfiles/*.profile; do [ -f "$f" ] && { echo "== $(basename "$f")"; grep -vE "^opt|^app" "$f" | tr '\n' ' '; echo; }; done
    ;;
  overlaymenu)
    # The panel's OVERLAYS page (Controller input display on) and its VIDEO page's Picture section (Internal resolution one step to
    # the right), through the test pad file; each saved to settings.ini.
    make_app "$dir" 1 3600 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "L R START"; sleep 1.2; pad ""; sleep 1.5
      tap DD; tap DD; tap DD; tap DD; tap A; focus "Controller input display"; tap A; sleep 0.8; shot overlays; tap B
      tap DU; tap DU; tap DU; tap DU; tap A; focus "Internal resolution"; tap DR; sleep 0.8; shot picture; tap B
      tap START; sleep 3; shot closed
      wait $app'
    echo "settings.ini lines set:"; grep -vE "^#|^$" "$dir/melee-nx-netplay/settings.ini" || true
    ;;
  handover)
    # Open the panel, D-pad down to AUDIO, A, Volume (A, then D-pad left three times: 100 -> 70), B back to the categories, then
    # B held for 12 retraces closes it, then A in the game (on Online Play: Unranked). The closing B must not reach the game; the A must.
    make_app "$dir" 1 2400 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      pad "L R START"; sleep 1.2; pad ""; sleep 1.5
      tap DD; tap A; focus Volume; tap A; tap DL; tap DL; tap DL; sleep 0.5; shot menu-dpad; tap B
      pad B; sleep 0.2; pad ""; sleep 1
      wait_log /out/console.log "menu: closed with" 10 && { sleep 1; shot after-close; }
      sleep 1; tap A
      wait_log /out/console.log "scene 8/0" 30 && { sleep 2; shot after-a; }
      wait $app'
    echo "launcher.ini volume: $(grep '^volume=' "$dir/melee-nx-netplay/launcher.ini")"
    grep -E "menu: (opened|closed|L\+R)|play: scene|online menu" "$dir/console.log" | cut -c1-160
    ;;
  home)
    # melee-unlocked's launcher as the home screen, driven by the test pad file only (MELEE_PAD_FILE): every page, the settings
    # panel opened from its Settings page (a row changed there), then PLAY; screenshots shots/home-*.png.
    make_app "$dir" 1 900 -
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.8; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot home-play
        tap B; tap DD; tap DD; sleep 3; shot home-lobby; tap DD; sleep 3; shot home-replays; tap DD; sleep 1; shot home-mods
        tap DU; tap DU; tap DU; tap A; sleep 3; shot home-settings; tap A; sleep 2; shot home-settings-video
        tap B; tap B; sleep 2; shot home-after-settings
        tap DU; tap A; sleep 1; shot home-play-focus; tap A; }
      wait_log /out/console.log "play: options" 60 && sleep 12 && shot game
      wait $app'
    ;;
  panel)
    # melee-unlocked's settings panel in the game, driven by the test pad file only: L + R + Start on the main menu, each of its
    # eight categories (A, two rows down, B, down), Start closes it; screenshots shots/panel-*.png. The home screen is skipped.
    make_app "$dir" 1 7000 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.8; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "play: options" 120 && sleep 20
      shot game-before
      pad "L R START"; sleep 1.2; pad ""; sleep 2; shot panel-home
      for i in 0 1 2 3 4 5 6 7; do
        tap A; sleep 2; shot "panel-$i"
        tap DD; tap DD; sleep 0.5; shot "panel-$i-down"
        tap B; sleep 1; tap DD
      done
      tap START; sleep 2; shot game-after
      wait $app'
    ;;
  styles)
    # melee-unlocked's menu styles (CUSTOMIZE > Menu style, launcher.ini overlaystyle 0..5) in the game, one start each: the
    # panel's first page, a category entered (A), two rows down; then the legacy menus (legacymenu=1, legacymenustyle 6 and 7)
    # from the home screen's Settings. Screenshots shots/style-N-*.png. The search (MU_TEST_KEYBOARD answers the software
    # keyboard): "dead" typed on the first style's VIDEO page.
    make_app "$dir" 1 2400 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_TEST_KEYBOARD="dead"; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.8; }
      for style in 0 1 2 3 4 5; do
        sed -i -e "/^overlaystyle=/d" -e "/^frames=/d" /out/melee-nx-netplay/launcher.ini
        printf "overlaystyle=%s\nframes=2400\n" $style >> /out/melee-nx-netplay/launcher.ini
        LOG=/out/console-$style.log
        cd /out && (/b/$BIN > $LOG 2>&1; echo "exit code $?" >> $LOG) &
        app=$!
        wait_log $LOG "online menu: row 1" 120 && sleep 3
        pad "L R START"; sleep 1.2; pad ""; sleep 2; shot style-$style-home
        tap A; sleep 2; shot style-$style-page
        if [ $style = 0 ]; then tap A; sleep 2; shot style-0-search; fi
        tap DD; tap DD; sleep 0.8; shot style-$style-down
        tap B; sleep 1; tap START; sleep 1
        pkill -f "/b/$BIN" 2>/dev/null; wait $app 2>/dev/null; sleep 1
      done
      for legacy in 6 7; do
        sed -i -e "/^legacymenu/d" -e "/^home=/d" -e "/^frames=/d" -e "/^overlaystyle=/d" /out/melee-nx-netplay/launcher.ini
        printf "legacymenu=1\nlegacymenustyle=%s\nframes=900\n" $legacy >> /out/melee-nx-netplay/launcher.ini
        LOG=/out/console-legacy-$legacy.log
        cd /out && (/b/$BIN > $LOG 2>&1; echo "exit code $?" >> $LOG) &
        app=$!
        wait_log $LOG "home: drawn" 120 && { sleep 2; tap B; tap DD; tap A; sleep 2.5; shot style-$legacy-home; tap DD; tap DD; sleep 1; shot style-$legacy-down; }
        pkill -f "/b/$BIN" 2>/dev/null; wait $app 2>/dev/null; sleep 1
      done'
    grep -hE "settings: (opened|search|focus|VIDEO|categories)|keyboard:" "$dir"/console-*.log | cut -c1-160 | head -60
    ;;
  keyboard)
    # A USB keyboard (src/usb_keyboard.cpp; on the console libnx's HID keyboard, here the test key file MU_TEST_KEY_FILE that stands
    # in for it, read at the same place): (1) melee-unlocked's keyboard layout as port 1, every key of it once (the "keyboard: port 1
    # gets" lines: Z X C V = A B X Y, Q W E = L R Z, T F G H the D-pad, I J K L the C-stick, the arrows the stick, Shift walks);
    # (2) the settings search typed on it: the panel's search box asks for words, the box over the game takes "dead" and Enter;
    # (3) remapping through the menu: Enter + G + E (Start + D-pad Down + Z) opens Controller bindings (MU_CONTROLS_TEST_PAGE) on
    # the USB keyboard, the A row captures N; the panel closed (launcher.ini padx.kb=m0=78); then Z no longer gives A and N does:
    # N on 1-P Mode's Online Play opens its page (Unranked, Direct, Teams, Party). Screenshots shots/kb-*.png.
    make_app "$dir" 1 7200 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_TEST_KEY_FILE=/out/keys.txt MU_CONTROLS_TEST_PAGE="Controller bindings"; pad ""; : > /out/keys.txt
      keys() { printf "%s\n" "$1" > /out/keys.txt; }
      press() { keys "$1"; sleep ${2:-0.25}; keys ""; sleep ${3:-0.5}; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3
      # (Z, which is A, is left for the end: on Online Play it would open Unranked.)
      for k in X C V Q W E T G F H I K J L Up Down Left Right "Shift Right"; do press "$k" 0.2 0.3; done
      shot kb-layout
      pad "L R START"; sleep 1.2; pad ""; sleep 2
      tap A; sleep 1; focus "Search all settings" DU; tap A; sleep 1.5; shot kb-box
      keys "type dead"; sleep 2; shot kb-box-typed
      press Enter 0.2 1.5; shot kb-search
      focus "Clear the search"; tap A; sleep 0.8
      tap B; sleep 1; tap START; sleep 1.5
      press "Enter G E" 0.3 1.5; shot kb-bindings
      focus A; tap A; sleep 0.8; press N 0.3 1; shot kb-bound
      tap START; sleep 1.5
      press Z 0.2 1; press N 0.2 2; shot kb-remapped
      wait $app'
    grep -aE "keyboard:|controls: USB|settings: search|menu: (opened|closed)|scene: major 08" "$dir/console.log" | cut -c1-170 | head -70
    echo "launcher.ini: $(grep '^padx' "$dir/melee-nx-netplay/launcher.ini")"
    ;;
  launcher)
    # The home screen's Play page extras: What's new (launcher.ini notesseen from an older version: it opens by itself), the
    # Language button (Deutsch picked), the launcher colour's wheel (hue moved, applied: accent=#...), a rail picture of the
    # player's own (rail-image.png in the app folder). Screenshots shots/launcher-*.png.
    make_app "$dir" 1 600 - notesseen=0.0.1
    cp "$work/gen/mu_ui/mu_icon.png" "$dir/melee-nx-netplay/rail-image.png"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.6; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot launcher-notes; tap A
        tap DD; tap DD; tap DD; tap A; sleep 1; shot launcher-language; tap DD; tap DD; tap DD; tap DD; tap A; sleep 1.5; shot launcher-deutsch
        tap DD; tap A; sleep 1; tap DR; tap DR; tap DR; tap DR; tap DR; tap DR; tap DD; tap DL; tap DL; sleep 1; shot launcher-wheel; tap A; sleep 1; shot launcher-colour
        tap DU; tap DU; tap DU; tap DU; tap A; }
      wait $app'
    grep -E "home: |launcher.ini" "$dir/console.log" | cut -c1-160
    echo "launcher.ini: $(grep -E "^(language|accent|notesseen)=" "$dir/melee-nx-netplay/launcher.ini" | tr '\n' ' ')"
    ;;
  replaylib)
    # The home screen's Replay Viewer (melee-unlocked's library): needs run-replays/replay.slp (test-replays-linux.sh record) and an
    # online match's replay with its session trace (MU_APP_SETTINGS="session_trace 2" ./test-app-linux.sh DISC online). First start:
    # the list; Search (the software keyboard's test answers: the online game's date, then nothing to clear it) with the session
    # trace graph; Compress all (.slpz); Delete (to .trash) and Undo; the folder browser. Second start: "As played" on, the recorded
    # replay and then the online one queued, Watch queue: the first plays to its end, the online one follows, shown as it was played.
    online="$root/online/a/melee-nx-netplay/Replays"
    [ -f "$repo/build/native-engine-gfx/run-replays/replay.slp" ] && ls "$online"/*.trace >/dev/null 2>&1 || { echo "replaylib needs run-replays/replay.slp and an online run with session_trace 2" >&2; exit 1; }
    make_app "$dir" 1 9000 -
    R="$dir/melee-nx-netplay/Replays"; mkdir -p "$R"
    cp "$repo/build/native-engine-gfx/run-replays/replay.slp" "$R/Game_20261124T120000.slp"
    cp "$repo/build/native-engine-gfx/run-replays/replay.slp" "$R/Game_20261124T110000.slp"
    cp "$online"/*.slp "$online"/*.trace "$R/"
    touch -t 202611240000 "$R"/*
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt MU_TEST_KEYBOARD="20261125|"; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.7; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; tap B; tap DD; tap DD; tap DD; tap A; sleep 3; shot replaylib-list
        for i in 1 2 3 4 5; do tap DR; done; tap A; sleep 3; shot replaylib-search; tap A; sleep 3
        tap DR; tap DR; tap DR; tap A; sleep 5; shot replaylib-compressed
        tap DL; tap DL; tap DL; tap DL; tap A; tap A; sleep 2; shot replaylib-deleted
        for i in 1 2 3 4 5; do tap DR; done; tap A; sleep 2; shot replaylib-undone
        tap DL; tap DL; tap DL; tap A; sleep 1.5; shot replaylib-folder; tap B
        pkill -f "/b/$BIN"; }
      wait $app'
    echo "folder after the first start:"; ls "$R" "$R/.trash" 2>/dev/null | tr '\n' ' '; echo
    # Second start: a fresh folder, the online replay and one other, As played on, both queued.
    rm -rf "$R"; mkdir -p "$R"
    cp "$online"/*.slp "$online"/*.trace "$R/"; cp "$repo/build/native-engine-gfx/run-replays/replay.slp" "$R/Game_20261124T120000.slp"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt LOG=/out/console2.log; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.7; }
      cd /out && (/b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log) &
      app=$!
      wait_log /out/console2.log "home: drawn" 120 && { sleep 2; tap B; tap DD; tap DD; tap DD; tap A; sleep 3
        tap DR; tap A; tap DR; tap DD; sleep 1; tap A; tap DU; sleep 1; tap A; sleep 1; shot replaylib-queued; tap DR; tap A; }
      wait_log /out/console2.log "as it was played" 200 && { sleep 6; shot replaylib-asplayed; }
      wait $app'
    grep -hE "home: |library: |replay: (showing|as played|no session|the watch queue|playback finished|the session trace)|keyboard:" "$dir"/console*.log | cut -c1-180 | head -60
    ;;
  notices)
    # The "Settings" reminder (settingsreminder=1) in the menus, and the panel's menu sounds (menusounds=1): open, a move, a page, close.
    make_app "$dir" 1 1800 - home=0 settingsreminder=1 menusounds=1
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.8; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && { sleep 3; shot reminder
        pad "L R START"; sleep 1.2; pad ""; sleep 1.5; tap DD; tap A; sleep 1; tap B; tap START; sleep 1; }
      wait $app'
    grep -E "settings: (reminder|menu sound)" "$dir/console.log" | head
    ;;
  crashreport)
    # The home screen's "Save crash report": melee-unlocked's zip (privacy-scrubbed logs) and its .md companion in Logs/. A first start
    # leaves a log behind; the second start saves the report from it. Then: the zip's parts, and none of the account's name or code.
    make_app "$dir" 1 300 - home=0
    run "$dir" 'cd /out && /b/$BIN > /out/console0.log 2>&1; echo "exit code $?" >> /out/console0.log'
    sed -i.bak "/^home=/d" "$dir/melee-nx-netplay/launcher.ini"
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep 0.7; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; tap DD; tap DD; tap DD; tap A; sleep 2; shot crash-saved; tap A; tap DU; tap DU; tap DU; tap A; }
      wait $app
      cd /out/melee-nx-netplay/Logs && unzip -l melee_crash_report.zip > /out/zip-list.txt 2>&1; unzip -p melee_crash_report.zip > /out/zip-all.txt 2>&1 || true'
    grep -E "crash report" "$dir/console.log" | cut -c1-220
    cat "$dir/zip-list.txt"
    echo "account name or code in the zip: $(grep -c -E "Mock 1|MOCK#001|mock-test" "$dir/zip-all.txt") lines"
    ;;
  egg)
    # melee-unlocked's character select easter egg: Unranked (Slippi's character select, scene 8), then Up Up Down Down Left Right
    # Left Right B A (Bailey) and the same with A B (Walter); screenshots of the faces bouncing.
    make_app "$dir" 1 2400 - home=0
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      tap() { pad "$1"; sleep ${2:-0.1}; pad ""; sleep 0.15; }
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "online menu: row 1" 120 && sleep 3; tap A
      wait_log /out/console.log "scene 8/0" 30 && { sleep 3
        for b in DU DU DD DD DL DR DL DR B A; do tap $b; done; sleep 2; shot egg-bailey
        for b in DU DU DD DD DL DR DL DR A B; do tap $b; done; sleep 3; shot egg-walter; sleep 2; shot egg-later; }
      wait $app'
    grep -E "walter egg|scene 8" "$dir/console.log" | head
    ;;
  explore)
    # Diagnosis: walks every category of the panel row by row (the log's "settings: focus" lines name each row in order).
    python3 -I - "$root/explore-script.txt" <<'PY'
import sys
lines = ["@scene 01 00", "300 L+R+START", "345"]
t = 380
for cat in range(8):
    lines += [f"{t} A", f"{t+4}", f"{t+5} # category {cat}"]; t += 40
    for i in range(int(sys.argv[2]) if len(sys.argv) > 2 else 34): lines += [f"{t} DD", f"{t+4}"]; t += 12
    lines += [f"{t} B", f"{t+4}"]; t += 40
    lines += [f"{t} DD", f"{t+4}"]; t += 30
open(sys.argv[1], "w").write("\n".join(l for l in lines if "#" not in l) + "\n")
PY
    make_app "$dir" 1 6000 "$root/explore-script.txt" home=0
    run "$dir" 'cd /out && /b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log'
    grep -E "settings: focus|menu: (opened|closed)" "$dir/console.log"
    ;;
  signin)
    # No user.json: the home screen says so on its Play page; Sign in (D-pad down from PLAY, A) opens the sign-in screen; B (Play
    # offline) returns to the home screen; up to PLAY, A: the game starts offline (Online Play shows Log-in).
    make_app "$dir" 0 1200 -
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot home-no-account; tap DD; tap DD; tap A; }
      wait_log /out/console.log "home: Sign in" 30 && { sleep 3; shot signin; tap B; }
      wait_log /out/console.log "sign-in: playing offline" 30 && { sleep 2; tap DU; tap DU; tap A; }
      wait_log /out/console.log "home: PLAY" 30 && rm -f /out/pad.txt
      sleep 14; shot game-offline
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
      wait_log /out/console.log "update: offered on the Play page" 60 && { sleep 3; shot prompt; tap DD; tap DD; tap A; }
      wait_log /out/console.log "the new version is" 60 && { sleep 2; shot installed; tap A; }
      wait $app
      # The same version published: no update offered, PLAY starts the game.
      sed -i "s/\"$NEWER\"/\"$SAME\"/" /out/server/latest.json
      cd /out && (/b/$BIN > /out/console2.log 2>&1; echo "exit code $?" >> /out/console2.log) & app=$!
      LOG=/out/console2.log wait_log /out/console2.log "update: the build computer has version" 60; sleep 2; wait_log /out/console2.log "home: drawn" 30 && { shot second-start; tap A; }
      wait $app
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
      mkdir -p "$dir/$side"; make_app "$dir/$side" $user "${MU_ONLINE_FRAMES:-4200}" "$dir/pads-$side.txt" "$hud" home=0
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
