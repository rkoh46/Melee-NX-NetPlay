#!/usr/bin/env bash
# Mod and Training session options without a console: play mode (mu-native-gfx-linux) in a container on a screenless X
# server with Mesa's software Vulkan driver, a settings.ini per case, a controller script, screenshots.
#   ./test-mods-linux.sh /path/to/disc-folder CASE...
# Cases:
#   training-menu   the in-game menu's Training page: Practice CPU on (first preset), Random numbers "Same each start",
#                   seed number 3, save to slot 1, Practice CPU off, load slot 1, then Go to Training: the settings land
#                   in settings.ini (practicecpuslot1, practice_rng, practice_rng_seed) and the game seeds its random
#                   numbers at the match start ("[pc] rng seed ... (same each start)")
#   frozen          Training with practice_rng 2 (frozen) from settings.ini
#   vanilla-only    --vanilla-only with a mod profile and CPU options in settings.ini: no profile loaded, the memory card
#                   folder <card>/VanillaOnly, the gameplay option words without mods or CPU options; --auto-vs match
#   mex             the Akaneia disc's profile (MU_MODS_DIR; MU_MEX_PROFILE=ace the ACE disc): its m-ex stage rows and
#                   stage select, then a Training match on an added stage picked on the select's second page
#   rogue           the rogueMelee disc's profile (MU_MODS_DIR): 1-P Mode > Regular Match > All-Star, which Rogue Mode replaces
#   vanilla-refuse  --vanilla-only with a disc that is not GALE01 revision 2 (an 8-byte stand-in made here): refused
# MU_MODS_DIR: a folder with mod profiles (mounted as <out>/mods) for vanilla-only. Output:
# build/native-engine-gfx/run-mods-test/<case>/ (console.log, host.log, settings.ini, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-mods-test"
mkdir -p "$root"

# $1 case folder, $2 frames, $3 seconds between screenshots, $4 extra play-mode arguments, $5 disc file in /iso or /out.
run() {
  local dir=$1 frames=$2 every=$3 extra=$4 iso=${5:-/iso/GALE01.iso} name
  name="mnx-mods-$(basename "$dir")"
  local mods=(); [ -n "${MU_MODS_DIR:-}" ] && mods=(-v "$MU_MODS_DIR:/out/mods:ro")
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "$name" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
    ${mods[@]+"${mods[@]}"} -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$extra" -e ISO="$iso" -e SDL_AUDIODRIVER=dummy \
    "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      script=""; [ -f /out/script.txt ] && script="--play-script /out/script.txt"
      /b/mu-native-gfx-linux --play --iso "$ISO" --out /out --settings /out/settings.ini --frames "$FRAMES" $script $EXTRA >/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep "$EVERY"; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log' || echo "test-mods-linux: container ended with $?"
}
report() {
  echo "== $(basename "$1")"
  grep -hE "^training|^mods|^play: (options|match|stopped|started|vanilla|a vanilla)|\[pc\]|\[mex\] (stages|stage select)|\[rogue\]|\[game\].*(rng|stage)|^replay|exit code|rror" "$1/console.log" "$1/host.log" 2>/dev/null | cut -c1-220 | tail -25 || true
  echo "shots: $(ls "$1/shots" 2>/dev/null | wc -l | tr -d ' ')"
}
fresh() { rm -rf "$1"; mkdir -p "$1/shots"; }

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  training-menu)
    # The in-game menu is the app's (play mode has none): an app folder as test-app-linux.sh makes it, a made-up user.json.
    fresh "$dir"; mkdir -p "$dir/melee-nx-netplay/User/Slippi"
    # The menu (L+R+Start) lists its own pages and then the feature pages (Game, Training, ...): Training is 13 rows down.
    # On the Training page the rows are: 0 Go to Training, 6 Practice CPU, 7 Random numbers, 8 seed number,
    # 9 save to slot 1, 10 load slot 1.
    python3 -I - "$dir/script.txt" <<'PY'
import sys
lines = ["@scene 01 00", "300 L+R+START", "345"]
t = 360
def press(what, n=1, gap=12, hold=4):
    global t
    for _ in range(n): lines.extend([f"{t} {what}", f"{t+hold}"]); t += gap
press("sy=-127", 13); t += 20; press("A", gap=60)
press("sy=-127", 6); press("sx=127", gap=30)          # Practice CPU: the first preset
press("sy=-127"); press("sx=127", gap=30)             # Random numbers: Same each start
press("sy=-127"); press("sx=127", 3)                  # seed number 3
press("sy=-127"); press("A", gap=40)                  # save to slot 1
press("sy=127", 3); press("sx=-127", gap=30)          # Practice CPU off
press("sy=-127", 4); press("A", gap=40)               # load slot 1
press("B", gap=30); press("START", gap=30)            # close the menu (Training's menus are then pressed through)
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    printf '%s\n' "iso=/iso/GALE01.iso" "frames=3600" "script=/out/script.txt" > "$dir/melee-nx-netplay/launcher.ini"
    printf '{"uid":"mock-test-1","playKey":"mock-test-only","connectCode":"MOCK#001","displayName":"Mock 1"}\n' > "$dir/melee-nx-netplay/User/Slippi/user.json"
    timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name mnx-mods-training-menu --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
      -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
        Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
        export DISPLAY=:99
        for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
        cd /out && /b/mu-native-gfx-linux-services > /out/console.log 2>&1 &
        pid=$!; n=0
        while kill -0 $pid 2>/dev/null; do
          sleep 2; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
        done
        wait $pid; echo "exit code $?" >> /out/console.log' || echo "test-mods-linux: container ended with $?"
    echo "settings.ini lines set:"; grep -vE "^#|^$" "$dir/melee-nx-netplay/settings.ini" || true
    report "$dir"
    # Then play mode with the settings.ini the menu wrote: Training from the title screen (--feature-test training).
    dir="$root/training-menu-play"; fresh "$dir"
    cp "$root/training-menu/melee-nx-netplay/settings.ini" "$dir/settings.ini"
    run "$dir" 1500 3 "--feature-test training"
    ;;
  frozen)
    fresh "$dir"
    printf '%s\n' "practice_rng 2" "practice_rng_seed 9" > "$dir/settings.ini"
    run "$dir" 1200 3 "--feature-test training"
    ;;
  vanilla-only)
    fresh "$dir"
    printf '%s\n' "mod_profile hackpack" "cpu_20xx 1" "cpu_tech 1" "practice_rng 2" > "$dir/settings.ini"
    run "$dir" 1500 3 "--auto-vs --vanilla-only"
    ;;
  mex)
    # MU_MODS_DIR with the Akaneia disc in akaneia/ (profile "akaneia"; MU_MEX_PROFILE=ace for ace/): the disc's m-ex
    # stage rows as the game builds them and its stage select, then a Training match on a stage picked on the select's
    # second page by the script (to the top left corner, then MU_MEX_MOVE / MU_MEX_MOVE_END: the last stick line and its release), or on stage id MU_MEX_STAGE (up to 127).
    dir="$root/mex-${MU_MEX_PROFILE:-akaneia}"; fresh "$dir"
    printf "%s\n" "mod_profile ${MU_MEX_PROFILE:-akaneia}" > "$dir/settings.ini"
    if [ "${MU_MEX_STAGE:-pick}" = pick ]; then
      # The stage select left to the script: R turns to the next page (the added stages), the stick moves the cursor
      # (each hovered icon is logged with its stage), A picks.
      printf '%s\n' "@scene 1c 01" "60 R" "64" "120 sx=-127" "150" "180 sy=127" "200" "${MU_MEX_MOVE:-210 sx=127}" "${MU_MEX_MOVE_END:-218}" "300 A" "304" > "$dir/script.txt"
      run "$dir" 1800 2 "--feature-test training:2:-1"
    else
      run "$dir" 1500 3 "--feature-test training:2:$MU_MEX_STAGE"
    fi
    ;;
  rogue)
    # MU_MODS_DIR with the rogueMelee disc in roguemelee/: 1-P Mode > Regular Match > the third row (All-Star, which
    # Rogue Mode takes over with this disc), then the character select: up onto a fighter, A, Start.
    fresh "$dir"
    printf '%s\n' "mod_profile roguemelee" > "$dir/settings.ini"
    # Times from the start (as the Hack Pack checks did): A at the intro, Start on the title screen, then the menus.
    printf '%s\n' "130 A" "140" "400 START" "410" "520 A" "524" "600 A" "604" "660 sy=-127" "664" "690 sy=-127" "694" "740 A" "744" \
      "1000 sy=127" "1010" "1030 A" "1034" "1100 START" "1104" "1200 START" "1204" "1400 A" "1404" "1500 A" "1504" > "$dir/script.txt"
    run "$dir" 3000 3 ""
    ;;
  vanilla-refuse)
    fresh "$dir"
    printf 'GALE01\0\001' > "$dir/not-retail.iso"
    : > "$dir/settings.ini"
    run "$dir" 300 1 "--vanilla-only" /out/not-retail.iso
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  report "$dir"
done
