#!/usr/bin/env bash
# Fight a pro and the Practice menu without a console: the app (mu-native-gfx-linux-services, app mode) in a container on
# a screenless X server with Mesa's software Vulkan driver, a controller script, screenshots.
#   ./test-proai-linux.sh /path/to/disc-folder MODELS_DIR CASE...
# MODELS_DIR holds Phillip's policies (tools/proai_check.sh's folder: any depth; the three base policies
# marth/fox/yoshi_d18_imitation_v3.onnx are used). Cases:
#   fight      the base policies copied into ProAI by hand; the in-game menu's Go to Training, then in Training
#              Start + D-pad Up + Z opens the Practice menu: Fight a pro > Zain > (the Stronger Pro AI question) Not now:
#              Training starts again with a Marth CPU that Phillip plays (command 0xEA) for about a minute
#   download   ProAI empty; a local server in the container stands in for Hugging Face (MELEE_PRO_AI_TEST_MIRROR; the
#              container has no network): Fight a pro > Download Phillip's AI > Download (about 64 MB): the three files
#              are fetched, checked against the catalog's SHA-256 and used; then Zain as in `fight`
#   look-N     the menu open on the Fight a pro page in look N (0..3: practicestyle), for screenshots
# Output: build/native-engine-gfx/run-proai-test/<case>/ (console.log, picture.log, settings.ini, shots/).
# MU_TEST_LIMIT: time limit in s (default 1400).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; models="$(cd "${2:?models folder}" && pwd -P)"; shift 2
work="$repo/build/native-engine-gfx"; root="$work/run-proai-test"
mkdir -p "$root"

# The app goes to Training as Fox from the title screen (launcher.ini feature_test=training:2, the Training module's test
# aid); the script, keyed on the Training match (scene 1c 02, times from the first time it is up), presses the Practice
# menu's buttons.
make_script() {   # $1 file, $2 the Training part as "time buttons" pairs
  python3 -I - "$1" "$2" <<'PY'
import sys
lines = ["@scene 1c 02"]
for pair in sys.argv[2].split(";"):
    if pair.strip() and pair.strip() != "fight":
        lines.append(pair.strip())
if "fight" in sys.argv[2].split(";"):
    # After the pick, the player plays too: runs at the CPU, attacks, jumps, shields, back off, over and over.
    t = 1400
    while t < 7000:
        for at, what in ((0, "sx=127"), (40, "A"), (44, ""), (70, "sx=-127"), (90, "sy=127"), (94, ""), (130, "sx=127+B"), (134, ""),
                         (170, "R"), (200, ""), (230, "sx=-127"), (260, "")):
            lines.append(f"{t + at} {what}".strip())
        t += 300
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
}

run() {   # $1 case folder, $2 frames, $3 extra docker arguments, $4 commands before the app starts
  local dir=$1 frames=$2 extra=$3 before=$4
  printf '%s\n' "iso=/iso/GALE01.iso" "frames=$frames" "script=/out/script.txt" "feature_test=training:2" > "$dir/melee-nx-netplay/launcher.ini"
  printf '{"uid":"mock-test-1","playKey":"mock-test-only","connectCode":"MOCK#001","displayName":"Mock 1"}\n' > "$dir/melee-nx-netplay/User/Slippi/user.json"
  # shellcheck disable=SC2086
  timeout "${MU_TEST_LIMIT:-1400}" docker run --rm --name "mnx-proai-$(basename "$dir")" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" \
    -v "$models:/models:ro" -v "$dir:/out" -e SDL_AUDIODRIVER=dummy -e XDG_RUNTIME_DIR=/tmp -e MELEE_UI_DIAG=1 -e MU_PROAI_LOG=1 $extra \
    -e BEFORE="$before" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      eval "$BEFORE"
      cd /out && /b/mu-native-gfx-linux-services > /out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep 4; n=$((n+1)); xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >> /out/console.log
      [ -z "${SERVER:-}" ] || kill "$SERVER" 2>/dev/null || true' || echo "test-proai-linux: container ended with $?"
}
report() {
  echo "== $(basename "$1")"
  grep -hE "pro ai|practice menu|controls: Start|training: (into|in the)|^training: practice CPU|exit code|rror" "$1/console.log" "$1/melee-nx-netplay/picture.log" 2>/dev/null \
    | grep -v "^menu:" | cut -c1-230 | awk '!seen[$0]++' | tail -45 || true
  echo "shots: $(ls "$1/shots" 2>/dev/null | wc -l | tr -d ' ')"
}
fresh() { rm -rf "$1"; mkdir -p "$1/shots" "$1/melee-nx-netplay/User/Slippi" "$1/melee-nx-netplay/ProAI"; }
base_models() { for n in marth_d18_imitation_v3 fox_d18_imitation_v3 yoshi_d18_imitation_v3; do find "$models" -name "$n.onnx" | head -1; done; }

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  fight)
    fresh "$dir"
    # A manual copy: the three base policies straight into ProAI (the catalog is written by the app); the steps of Zain's
    # policy (its timed check, then the match) are recorded (MELEE_PRO_AI_RECORD) for tools/proai_replay.py.
    base_models | while read -r f; do cp "$f" "$dir/melee-nx-netplay/ProAI/"; done
    # Training: open the menu, down to Fight a pro, A; A on Zain (the question comes up), down to Not now, A; then play.
    make_script "$dir/script.txt" "240 START+DU+Z;250;300 sy=-127;304;330 A;334;420 A;424;470 sy=-127;474;500 A;504;fight"
    run "$dir" "${MU_PROAI_FRAMES:-6000}" "-e MELEE_PRO_AI_RECORD=/out/proai.rec -e MELEE_PRO_AI_RECORD_MODEL=marth_d18_imitation" ""
    ;;
  download)
    fresh "$dir"
    # Fight a pro (Download) > Download Phillip's AI > Download (about 64 MB); later Zain > Not now.
    make_script "$dir/script.txt" "240 START+DU+Z;250;300 sy=-127;304;330 A;334;380 A;384;430 A;434;1100 A;1104;1150 sy=-127;1154;1180 A;1184"
    run "$dir" 7000 "-e MELEE_PRO_AI_TEST_MIRROR=http://127.0.0.1:8765/" \
      'mkdir -p /tmp/mirror; for n in marth_d18_imitation_v3 fox_d18_imitation_v3 yoshi_d18_imitation_v3; do cp "$(find /models -name "$n.onnx" | head -1)" /tmp/mirror/; done;
       (cd /tmp/mirror && python3 -m http.server 8765 --bind 127.0.0.1 >/out/mirror.log 2>&1) & SERVER=$!; sleep 1'
    ;;
  look-[0-3])
    fresh "$dir"
    base_models | while read -r f; do cp "$f" "$dir/melee-nx-netplay/ProAI/"; done
    printf '%s\n' "practicestyle ${case#look-}" > "$dir/melee-nx-netplay/settings.ini"
    # Character select slots: left and right pick the row.
    next="sy=-127"; [ "$case" = look-2 ] && next="sx=127"
    make_script "$dir/script.txt" "240 START+DU+Z;250;300 $next;304;330 A;334"
    run "$dir" 2400 "" ""
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  report "$dir"
  # The recorded steps (the inputs the game's observations became) through onnxruntime, when the venv is there.
  py="$repo/build/proai-venv/bin/python"
  if [ -s "$dir/proai.rec" ] && [ -x "$py" ]; then
    first="$(basename "$(cat "$dir/proai.rec.model" 2>/dev/null)")"   # the policy whose steps were recorded (the first session)
    m="$(find "$dir/melee-nx-netplay/ProAI" -name "$first" | head -1)"
    [ -n "$m" ] && echo "onnxruntime on the recorded steps of $first: $("$py" -I "$here/../tools/proai_replay.py" "$m" "$dir/proai.rec")"
  fi
done
