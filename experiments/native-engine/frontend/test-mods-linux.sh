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
#   install         the app's Mods page with a controller script: rogueMelee's .xdelta in mods/Downloads, Install, "Play it at
#                   the next start"; then play mode with that settings.ini starts Rogue Mode as the rogue case does
#   squad           offline Squad Strike (patches/0006): --auto-vs-menus through the VS character select twice, settings.ini
#                   squad_strike 0 (the default) and 1, Z pressed on the hovered fighter before the coin goes down
#                   (MU_AUTO_VS_Z): only the second makes a squad ("squad: P1 ..." at the match start)
#   practice-cpu    Training with the Practice CPU on and every Practice CPU row of the in-game menu stepped once to the right
#                   through the rows' own code (--feature-test practice-rows): the game's 0xF6 request gets those values
#                   ("training: practice CPU sent to the game: ...")
#   dsp             settings.ini dsp_lowpass 1 with no User/GC/dsp_coef.bin: the free table written to Sys/GC and read by the
#                   mixer (its Adler-32 in the log), then a player's own file in User/GC taking priority
#   tmce-lab        Training Mode CE (MU_TMCE_MODS, default build/mod-discs: tmce/tmce.iso, built from the official TM-CE.zip by
#                   MU_MOD_INSTALL_KEEP=build/mod-discs/tmce ../tools/mod_install_test.sh <disc folder> tmce): the event select's
#                   page change, Training Lab, its character and stage select, then in the match the Lab menu (Start): CPU
#                   Options (CPU Percent up 6, Tech Options), Recording (Save Positions, HMN Mode Record, a walk and a jump,
#                   HMN Mode Playback: the recording plays back), Slot Management > Modify Inputs (Alter Inputs), Export to
#                   memory card slot A (card select, name, confirm: a new file in the profile's card folder), then L+R+A
#                   (the Additional Codes' back to the select)
#   skins           skins (src/features/cosmetics.cpp): two costumes made here from the disc's own PlFcNr.dat in
#                   <out>/Cosmetics/Costumes (one with a joint moved by 1e-4, inside melee-unlocked's tolerance, one with a joint
#                   moved by 5 units), the mod discs of MU_MODS_DIR as skins; --feature-test cosmetics-verdicts logs upstream's
#                   online verdict for every costume skin; --auto-vs with R on the picked Falco (MU_AUTO_VS_SKIN) steps his
#                   costume to its first skin, and the match loads it
#   hackpack-hands  the 20XX Hack Pack disc's profile (MU_MODS_DIR hackpack/): VS Mode > Melee, Z with the hand over Pichu
#                   swaps in Master Hand ("Controlled By All Ports"), P2 a computer player, Onett, and Master Hand played
#                   from port 1 by the script (walk, A)
#   tmce-ess        the TM-CE event select's controls: R (help with its QR code, R again closes it), the pages with the
#                   D-pad, X and Y (jumps), then Training Lab and in it the Additional Codes: A+B held (the match starts
#                   again) and L+R+A (back to the select)
# MU_DOCKER_NAME: container name prefix (default mnx-mods).
# MU_MODS_DIR: a folder with mod profiles (mounted as <out>/mods) for vanilla-only. Output:
# build/native-engine-gfx/run-mods-test/<case>/ (console.log, host.log, settings.ini, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
# No case named: every case (a run that names none must not pass by running nothing).
[ $# -gt 0 ] || set -- training-menu frozen vanilla-only vanilla-refuse mex rogue install squad practice-cpu dsp tmce-lab tmce-ess skins hackpack-hands
# The mod discs: MU_MODS_DIR, else the disc folder's mods/ when it has one.
[ -n "${MU_MODS_DIR:-}" ] || { [ -d "$disc/mods" ] && export MU_MODS_DIR="$disc/mods"; } || true
work="$repo/build/native-engine-gfx"; root="$work/run-mods-test"
mkdir -p "$root"

# $1 case folder, $2 frames, $3 seconds between screenshots, $4 extra play-mode arguments, $5 disc file in /iso or /out.
run() {
  local dir=$1 frames=$2 every=$3 extra=$4 iso=${5:-/iso/GALE01.iso} name
  name="${MU_DOCKER_NAME:-mnx-mods}-$(basename "$dir")"
  local mods=(); [ -n "${MU_MODS_DIR:-}" ] && mods=(-v "$MU_MODS_DIR:/out/mods:ro")
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "$name" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
    ${mods[@]+"${mods[@]}"} ${MU_RUN_ENV:-} -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$extra" -e ISO="$iso" -e SDL_AUDIODRIVER=dummy \
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
    # The in-game menu is the app's, melee-unlocked's settings panel (play mode has none): an app folder as test-app-linux.sh makes
    # it, a made-up user.json, driven through the test pad file. GAME > Training: Practice CPU one step right (the first preset),
    # Random numbers one step (Same each start), seed number 3, save to slot 1, Practice CPU back off, load slot 1.
    fresh "$dir"; mkdir -p "$dir/melee-nx-netplay/User/Slippi"
    printf '%s\n' "iso=/iso/GALE01.iso" "frames=6000" "home=0" > "$dir/melee-nx-netplay/launcher.ini"
    printf '{"uid":"mock-test-1","playKey":"mock-test-only","connectCode":"MOCK#001","displayName":"Mock 1"}\n' > "$dir/melee-nx-netplay/User/Slippi/user.json"
    timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-mods}-training-menu" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
      -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp -e MELEE_PAD_FILE=/out/pad.txt "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
        Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
        export DISPLAY=:99
        for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
        pad() { printf "%s\n" "$1" > /out/pad.txt; }
        tap() { pad "$1"; sleep 0.2; pad ""; sleep 0.45; }
        focus() { for i in $(seq 140); do [ "$(grep -a "settings: focus" /out/console.log | tail -1)" = "settings: focus $1" ] && return 0; tap ${2:-DD}; done; echo "TIMEOUT focus $1"; return 1; }
        wait_log() { for i in $(seq $(( $2 * 10 ))); do grep -aq "$1" /out/console.log 2>/dev/null && return 0; sleep 0.1; done; return 1; }
        pad ""
        cd /out && /b/mu-native-gfx-linux-services > /out/console.log 2>&1 &
        pid=$!
        (n=0; while kill -0 $pid 2>/dev/null; do sleep 2; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true; done) &
        wait_log "online menu: row 1" 120 && sleep 3
        pad "L R START"; sleep 1.2; pad ""; sleep 1.5
        tap DD; tap DD; tap A; sleep 0.5
        focus "Practice CPU"; tap DR
        focus "Random numbers (items, turnips)"; tap DR
        focus "Random numbers: seed number"; tap DR; tap DR; tap DR
        focus "Practice CPU: save to slot 1"; tap A
        focus "Practice CPU" DU; tap DL
        focus "Practice CPU: load slot 1"; tap A
        tap B; tap START
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
  install)
    # The settings panel's MODS page (src/mu_panel.cpp over src/mu_mods.cpp) driven through the test pad file in the app: rogueMelee (a link-only catalog entry: its
    # file is put in mods/Downloads as the player would), Install, then "Play it at the next start"; the installer builds
    # the disc from the vanilla disc and checks it against the catalog. Then play mode with the settings.ini the page
    # wrote: 1-P Mode > Regular Match > All-Star, which Rogue Mode replaces on that disc (as the rogue case).
    # The .xdelta comes from build/mod-downloads (tools/mod_install_test.sh fills it), else from the catalog's own URL.
    cache="$repo/build/mod-downloads"; patch="$cache/rogueMelee-v3.xdelta"
    want=b0e987a3944f71750694d89c7af94cbf04c3c88ad7ca7516945c37d8a9538b9e
    if [ ! -f "$patch" ]; then
      mkdir -p "$cache"
      curl -fsSL --max-time 300 -o "$patch.part" https://github.com/hatsonhats/rogueMelee-v2/releases/download/v3/rogueMelee-v3.xdelta && mv "$patch.part" "$patch"
    fi
    [ "$(shasum -a 256 "$patch" | cut -c1-64)" = "$want" ] || { echo "rogueMelee-v3.xdelta does not match the catalog" >&2; exit 1; }
    fresh "$dir"; app="$dir/melee-nx-netplay"; mkdir -p "$app/User/Slippi" "$app/mods/Downloads"
    cp "$patch" "$app/mods/Downloads/"
    # Start + D-pad Down + Z opens melee-unlocked's settings panel on its MODS page (MU_CONTROLS_TEST_PAGE=Mods), through the test
    # pad file: the focus walks to rogueMelee's button ("Add file (mods/Downloads)": its file is there), A installs; once the log
    # says it is installed, A on the same button ("Play it at the next start") chooses it. Start closes the panel.
    printf '%s\n' "iso=/iso/GALE01.iso" "frames=6000" "home=0" > "$app/launcher.ini"
    printf '{"uid":"mock-test-1","playKey":"mock-test-only","connectCode":"MOCK#001","displayName":"Mock 1"}\n' > "$app/User/Slippi/user.json"
    timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-modinstall}-app-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
      -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp -e MU_CONTROLS_TEST_PAGE=Mods -e MELEE_PAD_FILE=/out/pad.txt "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
        Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
        export DISPLAY=:99
        for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
        pad() { printf "%s\n" "$1" > /out/pad.txt; }
        tap() { pad "$1"; sleep 0.15; pad ""; sleep 0.5; }
        last() { grep -a "settings: focus" /out/console.log | tail -1; }
        wait_log() { for i in $(seq $(( $2 * 10 ))); do grep -aq "$1" /out/console.log 2>/dev/null && return 0; sleep 0.1; done; echo "TIMEOUT waiting for: $1" >> /out/harness.log; return 1; }
        pad ""
        cd /out && /b/mu-native-gfx-linux-services > /out/console.log 2>&1 &
        pid=$!
        (n=0; while kill -0 $pid 2>/dev/null; do sleep 2; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true; done) &
        wait_log "online menu: row 1" 120 && sleep 3
        pad "START DD Z"; sleep 0.3; pad ""; sleep 2
        for i in $(seq 80); do [ "$(last)" = "settings: focus Add file (mods/Downloads)" ] && break; tap DD; done
        tap A
        wait_log "roguemelee: installed" 600 && { sleep 1; tap A; sleep 1; }
        tap START
        wait $pid; echo "exit code $?" >> /out/console.log' || echo "test-mods-linux: container ended with $?"
    grep -hE "^mods|mods page|mod_profile" "$dir/console.log" "$app/Logs/latest.log" 2>/dev/null | sort -u | cut -c1-200 || true
    echo "installed.txt: $(tr '\n' ' ' < "$app/mods/roguemelee/installed.txt" 2>/dev/null || echo missing)"
    echo "disc: $(ls -l "$app/mods/roguemelee/roguemelee.iso" 2>/dev/null | awk '{print $5, $NF}' || echo missing)"
    echo "settings.ini: $(grep '^mod_profile' "$app/settings.ini" 2>/dev/null || echo 'no mod_profile line')"
    report "$dir"
    # Then the match on the installed mod, in play mode with the app folder's mods and settings.ini.
    dir="$root/install-play"; fresh "$dir"
    cp "$app/settings.ini" "$dir/settings.ini"
    printf '%s\n' "130 A" "140" "400 START" "410" "520 A" "524" "600 A" "604" "660 sy=-127" "664" "690 sy=-127" "694" "740 A" "744" \
      "1000 sy=127" "1010" "1030 A" "1034" "1100 START" "1104" "1200 START" "1204" "1400 A" "1404" "1500 A" "1504" > "$dir/script.txt"
    MU_MODS_DIR="$app/mods" run "$dir" 3000 3 ""
    ;;
  squad)
    for on in 0 1; do
      dir="$root/squad-$on"; fresh "$dir"
      printf '%s\n' "squad_strike $on" > "$dir/settings.ini"
      MU_RUN_ENV="-e MU_AUTO_VS_Z=1" run "$dir" 1500 5 "--auto-vs-menus"
      echo "squad_strike $on: $(grep -h 'offline Squad Strike' "$dir/console.log" | head -1); the match's lineup: $(grep -h 'squad: P[12] ' "$dir/console.log" | tr '\n' ';')"
      report "$dir"
    done
    continue
    ;;
  dsp)
    fresh "$dir"
    printf '%s\n' "dsp_lowpass 1" > "$dir/settings.ini"
    run "$dir" 400 5 "--auto-vs"
    grep -h "low-pass\|DSP coefficient" "$dir/console.log" "$dir/host.log" 2>/dev/null | cut -c1-200
    mkdir -p "$dir/User/GC"; cp "$dir/Sys/GC/dsp_coef.bin" "$dir/User/GC/dsp_coef.bin"
    run "$dir" 400 5 "--auto-vs"
    grep -h "low-pass\|DSP coefficient" "$dir/console.log" "$dir/host.log" 2>/dev/null | cut -c1-200
    continue
    ;;
  tmce-lab|tmce-ess)
    tmce_mods="${MU_TMCE_MODS:-$repo/build/mod-discs}"
    [ -f "$tmce_mods/tmce/tmce.iso" ] || { echo "== $case: skipped: no $tmce_mods/tmce/tmce.iso (see the case's note)"; continue; }
    fresh "$dir"
    printf '%s\n' "mod_profile tmce" > "$dir/settings.ini"
    # Times from the start to the event select (1-P Mode > Event Match), then @scene blocks: 2B 00 the Lab's character
    # select, 2B 02 its stage select, 2B 01 the match.
    python3 -I - "$dir/script.txt" "$case" <<'PY'
import sys
case = sys.argv[2]
L = ["130 A", "140", "400 START", "410", "520 A", "524", "600 sy=-127", "604", "660 A", "664"]
t = 900
def p(w, n=1, gap=20, hold=4):
    global t
    for _ in range(n): L.extend([f"{t} {w}", f"{t+hold}"]); t += gap
if case == "tmce-ess":
    p("R", gap=120); p("R", gap=80)               # help (QR code), closed again
    p("DR", gap=80)                                # page 2 (General Tech) with the D-pad
    p("X", gap=60); p("Y", gap=60)                 # jumps down and up
    p("DL", gap=80); p("DR", gap=80)               # back a page and forward again
    p("A", gap=60)                                 # Training Lab
else:
    p("sx=127", gap=80); p("A", gap=60)            # page 2 (General Tech) > Training Lab
L += ["@scene 2b 00", "60 sy=127", "70", "72 sx=127", "78", "100 A", "104", "180 START", "184",   # Fox, then Start
      "@scene 2b 02", "60 sy=127", "66", "90 A", "94",                                          # a stage
      "@scene 2b 01"]
t = 200
if case == "tmce-ess":
    t = 300; L.extend([f"{t} sx=127", f"{t+40}"]); t += 120      # walk, then A+B held: the Lab starts again (salty runback)
    L.extend([f"{t} A+B", f"{t+90}"]); t += 300
    L.extend([f"{t} L+R+A", f"{t+60}"]); t += 200                 # back to the select (Additional Codes)
else:
    p("START", gap=60); p("sy=-127", gap=30); p("A", gap=60)      # menu > CPU Options
    p("sx=127", 6, gap=12); t += 40                               # CPU Percent up
    p("sy=-127", 2); p("sx=127", gap=40); t += 40                 # Tech Options
    p("B", gap=60); p("sy=-127", gap=30); p("A", gap=60)          # back > Recording
    p("A", gap=60)                                                # Save Positions
    p("sy=-127", gap=30); p("sx=127", gap=40)                     # HMN Mode: Record
    p("START", gap=40)                                            # play: the recording runs
    L.extend([f"{t} sx=127", f"{t+50}", f"{t+60} X", f"{t+64}", f"{t+120} sx=-127", f"{t+150}"]); t += 240
    p("START", gap=60); p("sx=127", gap=40)                       # HMN Mode: Playback
    p("START", gap=300)                                           # the recording plays back by itself
    p("START", gap=60); p("sy=127", 20, gap=12)                   # the menu again (Recording): to its top (no wrap)
    p("sy=-127", 13, gap=16); p("A", gap=60)                      # Slot Management (Mirrored Playback is skipped, greyed)
    p("sy=-127", 2, gap=20); p("A", gap=120)                      # Modify Inputs (Alter Inputs)
    p("B", gap=60); p("B", gap=60)                                # back to Recording (Slot Management selected)
    p("sy=-127", 3, gap=20); p("A", gap=120)                      # Export
    p("A", gap=240); p("A", gap=60); p("START", gap=240); p("A", gap=240)   # slot A, the name "1", Start, Yes: a new file
    p("B", gap=80); p("START", gap=80)                            # out of the card screen, close the menu
    L.extend([f"{t} L+R+A", f"{t+60}"]); t += 200                 # back to the select (Additional Codes)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
    MU_MODS_DIR="$tmce_mods" run "$dir" "$([ "$case" = tmce-lab ] && echo 5600 || echo 2600)" 2 ""
    echo "card folder: $(ls "$dir/card-tmce" 2>/dev/null | tr '\n' ' ')"
    grep -h "scene: major" "$dir/console.log" | tail -4
    ;;
  hackpack-hands)
    fresh "$dir"
    printf '%s\n' "mod_profile hackpack" > "$dir/settings.ini"
    printf '%s\n' "130 A" "140" "400 START" "410" "520 sy=-127" "524" "580 A" "584" "660 A" "664" \
      "@scene 02 00" "240 sy=127" "246" "248 sx=127" "254" "270 Z" "274" "290 A" "294" "305 sy=-127" "310" "320 sx=127" "326" "350 A" "356" "420 START" "428" \
      "@scene 02 01" "200 sy=100" "240" "260 A" "270" \
      "@scene 02 02" "200 sx=127" "260" "300 A" "304" "360 B" "364" "420 A" "424" "480 sx=-127" "540" "560 A" "564" > "$dir/script.txt"
    run "$dir" 2600 3 ""
    grep -h "20xx-hp\] character select" "$dir/console.log" | cut -c1-160
    ;;
  skins)
    fresh "$dir"; mkdir -p "$dir/Cosmetics/Costumes"
    # The disc's own PlFcNr.dat (read here, never committed), and two copies with the root's first child joint moved.
    python3 -I - "$disc/GALE01.iso" "$dir/Cosmetics/Costumes" <<'PY'
import struct, sys
iso, out = sys.argv[1], sys.argv[2]
f = open(iso, "rb"); f.seek(0x424); fst_off, fst_size = struct.unpack(">II", f.read(8)); f.seek(fst_off); fst = f.read(fst_size)
count = struct.unpack(">I", fst[8:12])[0]; names = count * 12
for i in range(1, count):
    e = fst[i*12:i*12+12]
    if e[0]: continue
    n = fst[names + (struct.unpack(">I", e[0:4])[0] & 0xFFFFFF):].split(b"\0")[0].decode()
    if n.lower() == "plfcnr.dat":
        off, size = struct.unpack(">II", e[4:12]); f.seek(off); dat = bytearray(f.read(size)); break
dsize, nrel, nroot, nref = struct.unpack(">IIII", dat[4:20])
table = 0x20 + dsize + nrel * 4; strings = table + (nroot + nref) * 8
for r in range(nroot):
    j, name_at = struct.unpack(">II", dat[table + r*8: table + r*8 + 8])
    if dat[strings + name_at:].split(b"\0")[0].endswith(b"_Share_joint"): root = j; break
child = struct.unpack(">I", dat[0x20 + root + 8: 0x20 + root + 12])[0]
at = 0x20 + child + 0x2C                                    # the joint's rest translation x
for label, delta in (("Nudged", 1e-4), ("Moved bone", 5.0)):
    b = bytearray(dat); x = struct.unpack(">f", b[at:at+4])[0]; b[at:at+4] = struct.pack(">f", x + delta)
    open(f"{out}/PlFcNr - {label}.dat", "wb").write(bytes(b))
PY
    : > "$dir/settings.ini"
    MU_RUN_ENV="-e MU_AUTO_VS_SKIN=1" run "$dir" 1500 3 "--auto-vs --feature-test cosmetics-verdicts"
    grep -h "cosmetics" "$dir/console.log" | grep -v "verdict" | cut -c1-200
    echo "verdicts: $(grep -c 'cosmetics: verdict' "$dir/console.log") ($(grep -c 'stays on online' "$dir/console.log") stay on online)"
    grep -h "cosmetics: verdict /plfcnr.dat" "$dir/console.log" | cut -c1-200
    echo "selection.txt: $(grep -v '^#' "$dir/Cosmetics/selection.txt" 2>/dev/null | tr '\t\n' '= ')"
    ;;
  practice-cpu)
    fresh "$dir"
    printf '%s\n' "practicecpu_on 1" > "$dir/settings.ini"
    run "$dir" 1200 3 "--feature-test training --feature-test practice-rows"
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  report "$dir"
done
