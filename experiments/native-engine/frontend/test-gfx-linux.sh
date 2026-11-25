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
#   lab-stick, lab-capsules, lab-minimal, lab-blobs   the Lab view's looks that need no packs (stick figures and outline; the
#             Capsules and Minimal presets; blobs in the player's own colours on a hidden stage)
#   lab       the Lab view's silhouettes, melee-unlocked's fighter packs in place (MU_LAB_ZIP: its content release's zip)
#   lab-download   the services program fetching those packs from a local stand-in of the release (MU_LAB_ZIP), then drawing them
#   video     menu video backgrounds: made-up clips as css.mp4 and sss.mp4 (needs build.sh ffmpeg)
#   border-png   border art from a PNG (Borders/sunset.png, made here) beside the 4:3 picture
#   border-video  border art from a looping MP4 (Borders/loop.mp4, made here; needs build.sh ffmpeg): the bars change between shots
#   fingerprint   mod_profile akaneia (MU_MODS_DIR): melee-unlocked's content fingerprint recognises the disc as Akaneia 1.0.1
#   all       every case above
# Output: build/native-engine-gfx/run-gfx/<case>/ (console.log, picture.log, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-gfx"
mkdir -p "$root"
cases=("$@"); [ "${#cases[@]}" -eq 0 ] || [ "${cases[0]}" = all ] && cases=(textures textures-same ssao capture flicker lab-stick lab-capsules lab-minimal lab-blobs lab lab-download)

# $1 case folder, $2 frames, $3 extra docker arguments, $4 extra play-mode arguments
run() {
  local dir=$1 frames=$2 env=$3 extra=${4:---auto-vs}
  local mods=(); [ -n "${MODS:-}" ] && mods=(-v "$MODS:/out/mods:ro")
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-$(basename "$dir")-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" ${mods[@]+"${mods[@]}"} $env ${MU_TEST_ENV:-} \
    -e FRAMES="$frames" -e EXTRA="$extra" -e BIN="${BIN:-mu-native-gfx-linux}" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      [ -d /out/mirror ] && (cd /out/mirror && python3 -m http.server 8099 --bind 127.0.0.1 >/out/mirror.log 2>&1 &)
      /b/$BIN --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" $EXTRA >/out/console.log 2>&1 &
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
    timeout 600 docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-pack-$$" --network none -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
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
  lab-stick|lab-capsules|lab-minimal|lab-blobs)
    fresh "$dir"
    case "$c" in
      lab-stick) printf '%s\n' "labview 1" "look_lab_fighters 1" "look_lab_stage 1" > "$dir/settings.ini" ;;
      lab-capsules) printf '%s\n' "look 13" > "$dir/settings.ini" ;;      # the Capsules preset (stage as blocks)
      lab-minimal) printf '%s\n' "look 14" > "$dir/settings.ini" ;;       # the Minimal preset
      lab-blobs) printf '%s\n' "labview 1" "look_lab_fighters 3" "look_lab_stage 3" "lab_colour_p1 FF8000" "lab_colour_p2 00A0FF" > "$dir/settings.ini" ;;
    esac
    run "$dir" 1300 "-e MELEE_LAB_NO_DOWNLOAD=1"
    echo "== $c"; show "$dir"
    ;;
  lab)
    # The silhouettes from melee-unlocked's fighter packs, already in place (MU_LAB_ZIP: the content release's
    # MeleeUnlocked-Lab.zip, fetched beforehand from its official place; unpacked here).
    fresh "$dir"; zip="${MU_LAB_ZIP:?MU_LAB_ZIP: melee-unlocked content-1 MeleeUnlocked-Lab.zip}"
    mkdir -p "$dir/Lab"; python3 -I -c "import sys, zipfile, os; z = zipfile.ZipFile(sys.argv[1]); [open(os.path.join(sys.argv[2], os.path.basename(n)), 'wb').write(z.read(n)) for n in z.namelist() if n.endswith('.lab')]" "$zip" "$dir/Lab"
    echo "packs: $(ls "$dir/Lab" | wc -l | tr -d ' ')"
    printf '%s\n' "labview 1" > "$dir/settings.ini"
    run "$dir" 1300 ""
    echo "== lab"; show "$dir"
    ;;
  lab-download)
    # The services program fetches the packs the first time the silhouettes find none, from a local stand-in of the release
    # (MU_LAB_ZIP served on the container's loopback; the program checks its SHA-256 as for the real one).
    fresh "$dir"; zip="${MU_LAB_ZIP:?MU_LAB_ZIP: melee-unlocked content-1 MeleeUnlocked-Lab.zip}"
    printf '%s\n' "labview 1" > "$dir/settings.ini"
    mkdir -p "$dir/mirror"; cp "$zip" "$dir/mirror/MeleeUnlocked-Lab.zip"
    BIN=mu-native-gfx-linux-services run "$dir" 1500 "-e MU_LAB_URL=http://127.0.0.1:8099/MeleeUnlocked-Lab.zip -e MELEE_PRO_AI_TEST_MIRROR=1"
    rm -rf "$dir/mirror"
    echo "== lab-download"; show "$dir"
    echo "packs: $(ls "$dir/Lab" 2>/dev/null | grep -c '\.lab$')"
    ;;
  video)
    # Looping menu video backgrounds: two made-up clips (a moving colour pattern, MPEG-4 in MP4, made by build.sh ffmpeg's ffmpeg
    # from frames this script writes) as VideoBackgrounds/css.mp4 and sss.mp4; --auto-vs passes the stage select (its background
    # learned, then the clip in its place); css.mp4 is drawn as a full-screen layer behind the character select's foreground.
    fresh "$dir"; mkdir -p "$dir/VideoBackgrounds"; printf '%s\n' "videobackgrounds 1" > "$dir/settings.ini"
    timeout 600 docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-clips-$$" --network none -v "$work/ffmpeg-linux:/f:ro" -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      for clip in css:0 sss:1; do name=${clip%%:*}; tint=${clip##*:}
        python3 -I -c "
import sys
w, h = 320, 240
out = open(\"/tmp/clip.rgb\", \"wb\")
for f in range(90):
    row = bytearray()
    for y in range(h):
        for x in range(w):
            r = (x * 2 + f * 6) % 256; g = (y * 3 + f * 4) % 256; b = ((x + y) // 2 + 128 * int(sys.argv[1])) % 256
            if ((x // 40) + (y // 40) + f // 15) % 2: r, g, b = 255 - r, g, 255 - b
            row += bytes((r, g, b))
    out.write(bytes(row))
" $tint && /f/bin/ffmpeg -loglevel error -f rawvideo -pix_fmt rgb24 -s 320x240 -r 30 -i /tmp/clip.rgb -c:v mpeg4 -q:v 3 -pix_fmt yuv420p /out/VideoBackgrounds/$name.mp4
      done; ls -la /out/VideoBackgrounds'
    run "$dir" 1100 ""
    echo "== video"; grep -hE "^video backgrounds" "$dir/console.log" | head -12
    ;;
  border-png)
    # Border art in PNG (upstream's picture formats through FFmpeg's decoder; .bmp was the only one before): a made-up picture
    # in Borders/, the 4:3 picture's side bars show it.
    fresh "$dir"; mkdir -p "$dir/Borders"
    timeout 300 docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-png-$$" --network none -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" \
      convert -size 640x360 gradient:orange-purple -fill white -draw "circle 80,180 80,120" -draw "circle 560,180 560,120" /out/Borders/sunset.png
    printf '%s\n' "borderart sunset.png" "borderedge 1" "aspect 2" > "$dir/settings.ini"
    run "$dir" 1100 ""
    echo "== border-png"; grep -hE "border art" "$dir/console.log" | head -3
    ;;
  border-video)
    # Border art as a looping video (upstream's Border: an .mp4 in Borders plays in the bars): a made-up clip of moving colour bands.
    fresh "$dir"; mkdir -p "$dir/Borders"
    timeout 600 docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-loop-$$" --network none -v "$work/ffmpeg-linux:/f:ro" -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      python3 -I -c "
w, h = 320, 180
out = open(\"/tmp/loop.rgb\", \"wb\")
for f in range(120):
    row = bytearray()
    for y in range(h):
        for x in range(w):
            band = ((x + f * 4) // 20) % 3
            row += bytes((255, 60, 30) if band == 0 else (30, 200, 80) if band == 1 else (40, 90, 255))
    out.write(bytes(row))
" && /f/bin/ffmpeg -loglevel error -f rawvideo -pix_fmt rgb24 -s 320x180 -r 30 -i /tmp/loop.rgb -c:v mpeg4 -q:v 3 -pix_fmt yuv420p /out/Borders/loop.mp4; ls -la /out/Borders'
    printf '%s\n' "borderart loop.mp4" "aspect 2" > "$dir/settings.ini"
    run "$dir" 900 ""
    echo "== border-video"; grep -hE "border art" "$dir/console.log" | head -4
    # The left bar in two shots a few seconds apart: a playing video makes them differ.
    timeout 300 docker run --rm --name "${MU_DOCKER_NAME:-mnx-gfx}-cmp-$$" --network none -v "$dir:/out" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      a=$(ls /out/shots | sed -n 4p); b=$(ls /out/shots | sed -n 6p)
      echo "left bar, $a vs $b: $(compare -metric AE -crop 140x700+5+10 /out/shots/$a /out/shots/$b null: 2>&1) pixels differ"' || true
    ;;
  fingerprint)
    # A mod disc's content fingerprint as melee-unlocked computes it (mod_fingerprint.cpp): the Akaneia disc of MU_MODS_DIR must be
    # recognised as "Akaneia 1.0.1" by upstream's own table, the check that the 2D-style build (no disc here) is found the same way.
    fresh "$dir"; printf '%s\n' "mod_profile ${MU_FP_PROFILE:-akaneia}" > "$dir/settings.ini"
    MODS="${MU_MODS_DIR:?MU_MODS_DIR: the mods folder}" run "$dir" 400 "" ""
    echo "== fingerprint"; grep -hE "^mods: (content|fingerprinted|.*found)" "$dir/console.log" | head
    ;;
  *) echo "unknown case $c" >&2; exit 2 ;;
  esac
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
