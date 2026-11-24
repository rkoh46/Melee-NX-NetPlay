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
#   home     the home screen's pages (Play, Replay Viewer, Mods), the panel from its Settings page, PLAY
#   panel    the panel in the game: each of its eight categories (screenshots)
#   menu     the panel (L + R + Start): AUDIO Volume 100 -> 70, Music volume 100 -> 80, OVERLAYS Performance HUD on, the Practice
#            chord opens GAME on Training: "Inputs and frame advantage" on (saved as lab_overlay 1); a second start reads them back
#   handover the panel with the D-pad, closed with B held for 12 retraces, then A in the game: the closing B never reaches the game
#            (it stays on Online Play), the later A does (Unranked opens Slippi's character select, scene 8)
#   signin   no user.json: the home screen's Sign in opens the sign-in screen; B (Play offline) back to the home screen; PLAY: offline
#   update   a fake build computer on 127.0.0.1 (python http.server) publishes the version after VERSION: offered on the home screen's
#            Play page, "Update and restart", the download, the check, the swap of melee-nx-netplay.nro (a dummy), the notice, A: the app
#            closes; then the same version: nothing offered, PLAY
#   overlaymenu  the panel's OVERLAYS page and VIDEO's Picture section: one row changed on each (saved to settings.ini), screenshots
#   controls the controller setup (src/controls.cpp) on port 1 read as GameCube adapter port 1 (MU_CONTROLS_TEST_ADAPTER=1) with a
#            saved setup in launcher.ini (X and Y swapped): Start + D-pad Down + Z opens the panel on Controller profiles
#            (MU_CONTROLS_TEST_PAGE), "Save as a new profile", the A binding pressed onto Z, stick left/right inverted, Stick radius 90
#            with the inverted stick, Start closes; the game gets Y for X, A for Z, the stick inverted and scaled; a second start loads it
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
      tap() { pad "$1"; sleep ${2:-0.15}; pad ""; sleep ${3:-0.5}; }
      focus() { for i in $(seq 70); do [ "$(grep -a "settings: focus" ${LOG:-/out/console.log} | tail -1)" = "settings: focus $1" ] && return 0; tap ${2:-DD}; done; echo "TIMEOUT focus $1" >> /out/harness.log; return 1; }
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
    # No arguments, a made-up user.json: melee-unlocked's launcher comes up on its Play page with PLAY focused; A starts the game.
    make_app "$dir" 1 1500 -
    run "$dir" 'export MELEE_PAD_FILE=/out/pad.txt; pad ""; shots 4
      cd /out && (/b/$BIN > /out/console.log 2>&1; echo "exit code $?" >> /out/console.log) &
      app=$!
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot home; tap A; }
      wait $app'
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
  controls)
    # The controller setup (src/controls.cpp) on port 1 read as GameCube adapter port 1 (MU_CONTROLS_TEST_ADAPTER=1), a saved setup
    # in launcher.ini (X and Y swapped), through the test pad file: Start + D-pad Down + Z opens the panel on the Controller profiles
    # section (MU_CONTROLS_TEST_PAGE); "Save as a new profile"; up to the bindings, A bound to Z by pressing it; up to the setup,
    # stick left/right inverted (Z, which is A now), Stick radius 100 -> 90 with the inverted stick; Start closes. Then the game gets
    # Y for X, A for Z and the stick inverted and scaled (the "the game gets" lines). A second start loads the profile.
    make_app "$dir" 1 3600 - "padx.gc1=m2=2048,m3=1024" home=0
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
      focus "Load this profile"; tap A; sleep 0.5; tap START; sleep 1.5; tap X 0.2
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
        tap B; tap DD; tap DD; sleep 3; shot home-replays; tap DD; sleep 1; shot home-mods
        tap DU; tap DU; tap A; sleep 3; shot home-settings; tap A; sleep 2; shot home-settings-video
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
      wait_log /out/console.log "home: drawn" 120 && { sleep 2; shot home-no-account; tap DD; tap A; }
      wait_log /out/console.log "home: Sign in" 30 && { sleep 3; shot signin; tap B; }
      wait_log /out/console.log "sign-in: playing offline" 30 && { sleep 2; tap DU; tap A; }
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
      wait_log /out/console.log "update: offered on the Play page" 60 && { sleep 3; shot prompt; tap DD; tap A; }
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
