#!/usr/bin/env bash
# The display wave's graphics options (0.4.8) without a console: play mode (mu-native-gfx-linux) in a container on a screenless X
# server with Mesa's software Vulkan driver, --auto-vs into a VS match, a settings.ini per case, screenshots.
#   ./test-gfx-linux.sh /path/to/disc-folder CASE...
# Cases:
#   textures  two runs: dumptextures 1 writes every texture the match draws into Dump/Textures/GALE01 (PNG, Dolphin's names);
#             the dumps are turned into a pack (colours inverted by ImageMagick) in TexturePacks/inverted; customtextures 1
#             then draws the match with it (screenshots of both runs)
#   textures-same   the textures case's dumps as a pack, unchanged (the picture must not change)
#   ssao      ssao 1 (screen-space ambient occlusion at full strength)
#   capture   a capture burst of 30 frames at retrace 900 (MU_CAPTURE_AT): capture/burst-1/*.png
#   flicker   flickerscan 1 with one frame drawn twice as bright (MU_TEST_FLICKER_AT): the detector's report and its grids
#   lab, lab-stick, lab-capsules, lab-minimal, lab-packs   the Lab view (see the lab cases below)
#   all       every case above
# Output: build/native-engine-gfx/run-gfx/<case>/ (console.log, picture.log, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-gfx"
mkdir -p "$root"
cases=("$@"); [ "${#cases[@]}" -eq 0 ] || [ "${cases[0]}" = all ] && cases=(textures ssao capture flicker lab lab-stick lab-capsules lab-minimal)

# $1 case folder, $2 frames, $3 extra docker arguments, $4 extra play-mode arguments
run() {
  local dir=$1 frames=$2 env=$3 extra=${4:---auto-vs}
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "mnx-gfx2-$(basename "$dir")-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" $env \
    -e FRAMES="$frames" -e EXTRA="$extra" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" $EXTRA >/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep 2; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log' || echo "test-gfx-linux: container ended with $?"
}
fresh() { rm -rf "$1"; mkdir -p "$1/shots"; }
show() { grep -hE "^gfx:|^lab|lab view|^overlay: picture|flicker:|render thread: (all work|  of that, converting)|exit code|STOPPED|rror" "$1/console.log" | cut -c1-240 | tail -${2:-14}; }

for c in "${cases[@]}"; do
  dir="$root/$c"
  case "$c" in
  textures)
    fresh "$dir"; dump="$dir/dump"; mkdir -p "$dump/shots"
    printf '%s\n' "dumptextures 1" > "$dump/settings.ini"
    run "$dump" 1300 ""
    echo "== textures (dump)"; show "$dump" 6
    n=$(ls "$dump/Dump/Textures/GALE01" 2>/dev/null | wc -l | tr -d ' ')
    echo "dumped: $n PNG files"
    # A pack made from the dumps: every texture with its colours inverted (alpha kept), under the dump's own names.
    mkdir -p "$dir/TexturePacks/inverted"
    timeout 600 docker run --rm --name "mnx-gfx2-pack-$$" --network none -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      cd /out/dump/Dump/Textures/GALE01 && ls | head -4000 | while read f; do convert "$f" -channel RGB -negate +channel "/out/TexturePacks/inverted/$f"; done'
    echo "pack: $(ls "$dir/TexturePacks/inverted" | wc -l | tr -d ' ') files"
    printf '%s\n' "customtextures 1" > "$dir/settings.ini"
    run "$dir" 1300 ""
    echo "== textures (pack)"; show "$dir" 8
    ;;
  textures-same)
    # The dumps of the textures case as a pack, unchanged: the match must look as it does without a pack.
    src="$root/textures/dump/Dump/Textures/GALE01"; [ -d "$src" ] || { echo "run the textures case first" >&2; exit 2; }
    fresh "$dir"; mkdir -p "$dir/TexturePacks/same"; cp "$src"/*.png "$dir/TexturePacks/same/"
    printf '%s\n' "customtextures 1" > "$dir/settings.ini"
    run "$dir" 1300 ""
    echo "== textures-same"; show "$dir" 6
    ;;
  ssao)
    fresh "$dir"; printf '%s\n' "ssao 1" > "$dir/settings.ini"; run "$dir" 1300 ""
    echo "== ssao"; show "$dir"
    ;;
  capture)
    fresh "$dir"; : > "$dir/settings.ini"; run "$dir" 1300 "-e MU_CAPTURE_AT=900:30"
    echo "== capture"; show "$dir"
    echo "captured: $(ls "$dir"/capture/burst-1 2>/dev/null | wc -l | tr -d ' ') PNG files"
    ;;
  flicker)
    fresh "$dir"; printf '%s\n' "flickerscan 1" > "$dir/settings.ini"; run "$dir" 1300 "-e MU_TEST_FLICKER_AT=700"
    echo "== flicker"; show "$dir" 20
    echo "grids: $(ls "$dir"/flicker 2>/dev/null | wc -l | tr -d ' ') files"
    ;;
  *) echo "unknown case $c" >&2; exit 2 ;;
  esac
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
