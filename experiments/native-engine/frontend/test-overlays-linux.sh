#!/usr/bin/env bash
# The overlays and picture options without a console: play mode (mu-native-gfx-linux) in a container on a screenless X
# server with Mesa's software Vulkan driver, a settings.ini per case, a controller script, screenshots.
#   ./test-overlays-linux.sh /path/to/disc-folder CASE...
# Cases:
#   match     --auto-vs, then the script walks to the computer player, jabs and does short-hop aerials (some landings
#             L-cancelled, some not): input display with values for ports 1 and 2, frame counter, performance graph,
#             adapter rate line, combo counter, L-cancel flash (both)
#   training  Go to Training (--feature-test training) with lab_overlay 1: the Training panel (actions, inputs) and the
#             frame advantage of jabs on the dummy (command 0xF4)
#   latency   --auto-vs, latencytest 1: X + D-pad Down twice (black, then white), X + D-pad Up (the picture again)
#   te        the 20XX TE save's profile (MU_MODS_DIR) with its input display bit set (te_options2 2000 in settings.ini.state):
#             the input display in a VS match without inputoverlay
#   picture   --auto-vs, brightness, contrast, vibrance, sharpening, the home television CRT look, 73:60, grid border art
#   studio    --auto-vs, the studio monitor CRT look, 4:3, a .bmp border (made here), shadow off
#   scale1, scale2   --auto-vs with internal resolution 640x480 and 1280x960
# Output: build/native-engine-gfx/run-overlays/<case>/ (console.log, picture.log, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-overlays"
mkdir -p "$root"

# $1 case folder, $2 frames, $3 seconds between screenshots, $4 extra play-mode arguments; settings.ini and script.txt
# are already in the folder.
run() {
  local dir=$1 frames=$2 every=$3 extra=$4
  local mods=(); [ -n "${MU_MODS_DIR:-}" ] && mods=(-v "$MU_MODS_DIR:/out/mods:ro")
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" ${mods[@]+"${mods[@]}"} \
    -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$extra" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      script=""; [ -f /out/script.txt ] && script="--play-script /out/script.txt"
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" --novsync $script $EXTRA >/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep "$EVERY"; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log' || echo "test-overlays-linux: container ended with $?"
}
fresh() { rm -rf "$1"; mkdir -p "$1/shots"; }
# The match script after --auto-vs hands over (frames count from the match scene, 02 02, being entered): walk right to the computer player,
# jab a few times, then short-hop neutral airs, the even ones L-cancelled (L pressed just before landing).
match_script() {
  python3 -I - "$1" <<'PY'
import sys
lines = ["@scene 02 02"]
t = 200
lines += [f"{t} sx=127", f"{t+70}"]                        # walk to the computer player
t += 80
for i in range(12):                                         # jabs
    lines += [f"{t} A", f"{t+3}"]; t += 9
t += 20
for i in range(10):                                         # short-hop neutral airs; L just before landing on even ones
    lines += [f"{t} X", f"{t+2}", f"{t+5} A", f"{t+8}"]
    if i % 2 == 0: lines += [f"{t+29} L", f"{t+31}"]
    t += 60
for i in range(12):
    lines += [f"{t} A", f"{t+3}"]; t += 9
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
}

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  match)
    fresh "$dir"; match_script "$dir/script.txt"
    printf '%s\n' "inputoverlay 1" "inputoverlayports 3" "inputoverlayvalues 1" "showfps 1" "performance 1" "adapterrate 1" \
      "arcade_combo_counter 1" "lcancel_flash_mode 6" "lcancel_success_flash 2" > "$dir/settings.ini"
    run "$dir" 2400 1 "--auto-vs"
    ;;
  training)
    fresh "$dir"
    # Training starts with the dummy in front; frames count from the scene (0x1C) being entered.
    python3 -I - "$dir/script.txt" <<'PY'
import sys
lines = ["@scene 1c 02", "100 sx=127", "136"]
t = 160
for i in range(16): lines += [f"{t} A", f"{t+3}"]; t += 24          # jabs: frame advantage on hit, a combo
for i in range(12):                                                  # short-hop neutral airs; L 4 frames before landing on even ones
    lines += [f"{t} X", f"{t+2}", f"{t+5} A", f"{t+8}"]
    if i % 2 == 0: lines += [f"{t+20} L", f"{t+22}"]
    t += 70
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    printf '%s\n' "lab 1" "lab_overlay 1" "arcade_combo_counter 1" "lcancel_flash_mode 6" "lcancel_success_flash 2" > "$dir/settings.ini"
    run "$dir" 2400 1 "--feature-test training"
    ;;
  latency)
    fresh "$dir"
    printf '%s\n' "@scene 02 02" "300 X" "310 X DD" "316 X" "320" "480 X" "490 X DD" "496 X" "500" "660 X" "670 X DU" "676 X" "680" > "$dir/script.txt"
    printf '%s\n' "latencytest 1" > "$dir/settings.ini"
    run "$dir" 1500 1 "--auto-vs"
    ;;
  te)
    fresh "$dir"; match_script "$dir/script.txt"
    printf '%s\n' "mod_profile te" > "$dir/settings.ini"
    printf '%s\n' "te_options2 2000" > "$dir/settings.ini.state"
    run "$dir" 2200 2 "--auto-vs"
    ;;
  picture)
    fresh "$dir"
    printf '%s\n' "brightness 1.20" "contrast 1.15" "vibrance 1.60" "sharpness 1.00" "crt_filter 2" "crt_curve 1.50" "aspect 1" "borderart grid" "borderedge 1" > "$dir/settings.ini"
    run "$dir" 1300 2 "--auto-vs"
    ;;
  studio)
    fresh "$dir"; mkdir -p "$dir/Borders"
    # A made-up border picture: a diagonal colour ramp (24-bit BMP written here).
    python3 -I - "$dir/Borders/ramp.bmp" <<'PY'
import struct, sys
w, h = 320, 180
rows = []
for y in range(h):
    row = bytearray()
    for x in range(w):
        row += bytes(((x * 255) // w, (y * 255) // h, 160))   # B G R
    row += b"\0" * ((4 - len(row) % 4) % 4)
    rows.append(bytes(row))
data = b"".join(reversed(rows))
hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
open(sys.argv[1], "wb").write(hdr + data)
PY
    printf '%s\n' "crt_filter 1" "aspect 2" "borderart ramp.bmp" "borderedge 0" > "$dir/settings.ini"
    run "$dir" 1300 2 "--auto-vs"
    ;;
  scale1|scale2)
    fresh "$dir"
    printf '%s\n' "scale ${case#scale}" > "$dir/settings.ini"
    run "$dir" 1300 2 "--auto-vs"
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  echo "== $case"
  grep -hE "^overlay|^overlays|^training|^play: (options|match|stopped)|exit code|STOPPED|rror" "$dir/console.log" | cut -c1-220 | tail -25
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
