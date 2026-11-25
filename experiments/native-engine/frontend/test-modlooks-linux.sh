#!/usr/bin/env bash
# Get mod looks (src/modlooks.cpp, ported from the macOS build) and the Diet presets' look packs without a console or a network.
# MU_LOOKS_TEST_DIR answers every download from fixtures made here and gives the pins (pins.txt): a MediaFire-style page naming a
# download host, the archives (.tar.gz for Diet Melee, .zip for Animelee) holding xdelta3 patches from the player's own disc to
# small made-up outputs (xdelta3 on the build computer only writes the fixtures; the app applies them with its own VCDIFF decoder).
#   ./test-modlooks-linux.sh /path/to/disc-folder CASE...
# Cases: animelee (one patch: mods/animelee), diet (the Classic patch, then Crystal and Diet 64 as differences of it), person (a page
# without a download address: the install stops and says so), lookpack (look 3 with a disc in mods/diet-classic: the Diet Classic
# preset loads it offline on the ordinary card; MU_LOOKPACK_DISC names a real mod disc to stand in, default the Akaneia disc).
# Needs xdelta3, zip and tar on the build computer. Output: build/native-engine-gfx/run-modlooks/<case>/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-modlooks"; fx="$root/fx"
iso="$disc/GALE01.iso"
command -v xdelta3 > /dev/null || { echo "test-modlooks-linux: needs xdelta3 for the fixtures" >&2; exit 1; }
size() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
fixtures() {
  rm -rf "$fx"; mkdir -p "$fx"
  san() { python3 -I -c 'import sys; print("".join(c if c.isalnum() else "_" for c in sys.argv[1]))' "$1"; }
  pinline() { printf '%s\t%s\t%s\n' "$1" "$(size "$2")" "$(shasum -a 256 "$2" | cut -d' ' -f1)" >> "$fx/pins.txt"; }
  pinline retail "$iso"
  mk() { head -c 3145728 "$iso" > "$2"; printf "$1" | dd of="$2" bs=1 seek=1048576 conv=notrunc 2> /dev/null; }
  mk "ANIMELEE-FIXTURE" "$fx/animelee.out"; mk "CLASSIC-FIXTURE" "$fx/classic.out"; mk "CRYSTAL-FIXTURE!" "$fx/crystal.out"; mk "DIET64-FIXTURE!!" "$fx/d64.out"
  xdelta3 -e -f -s "$iso" "$fx/animelee.out" "$fx/ANIMELEE - COMPLETE EDITION.xdelta"
  xdelta3 -e -f -s "$iso" "$fx/classic.out" "$fx/DietMeleeClassic.xdelta"
  xdelta3 -e -f -s "$iso" "$fx/crystal.out" "$fx/crystal.xdelta"; xdelta3 -e -f -s "$iso" "$fx/d64.out" "$fx/64.xdelta"
  xdelta3 -e -f -s "$fx/DietMeleeClassic.xdelta" "$fx/crystal.xdelta" "$fx/crystaldiff.xdelta"
  xdelta3 -e -f -s "$fx/DietMeleeClassic.xdelta" "$fx/64.xdelta" "$fx/64diff.xdelta"
  (cd "$fx" && zip -q -0 "Animelee CE 1.0.zip" "ANIMELEE - COMPLETE EDITION.xdelta")
  (cd "$fx" && COPYFILE_DISABLE=1 tar --format ustar -czf "DietMeleeLinuxPatcher.tar.gz" DietMeleeClassic.xdelta crystaldiff.xdelta 64diff.xdelta)
  for f in "Animelee CE 1.0.zip" "ANIMELEE - COMPLETE EDITION.xdelta" DietMeleeLinuxPatcher.tar.gz DietMeleeClassic.xdelta crystaldiff.xdelta 64diff.xdelta; do pinline "$f" "$fx/$f"; done
  pinline "Animelee CE 1.0.iso" "$fx/animelee.out"; pinline "Diet Classic.iso" "$fx/classic.out"; pinline "Crystal.iso" "$fx/crystal.out"; pinline "Diet 64.iso" "$fx/d64.out"
  page() { printf '<html><body><a class="input popsok" aria-label="Download file" href="%s" id="downloadButton">Download</a></body></html>\n' "$2" > "$fx/$(san "$1")"; }
  ap="https://www.mediafire.com/file/2ztqob93nrug8yg/ANIMELEE_-_COMPLETE_EDITION_-_PATCH_1.0.zip/file"; al="https://download1500.mediafire.com/fixture/a/ANIMELEE.zip"
  dp="https://www.mediafire.com/file/v6v5yuqd9le7jav/DietMeleeLinuxPatcher.tar.gz/file"; dl="https://download2201.mediafire.com/fixture/d/DietMeleeLinuxPatcher.tar.gz"
  page "$ap" "$al"; cp "$fx/Animelee CE 1.0.zip" "$fx/$(san "$al")"
  page "$dp" "$dl"; cp "$fx/DietMeleeLinuxPatcher.tar.gz" "$fx/$(san "$dl")"
  rm -f "$fx"/*.out "$fx/crystal.xdelta" "$fx/64.xdelta"
  echo "fixtures: $(ls "$fx" | wc -l | tr -d ' ') files"
}
# $1 case folder, $2 frames, $3 docker -e arguments, $4 settings lines
run() {
  local dir=$1 frames=$2 env=$3
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "mnx-gfx2-modlooks-$(basename "$dir")-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" -v "$fx:/fx:ro" $env \
    -e FRAMES="$frames" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" >/out/console.log 2>&1
      echo "exit code $?" >>/out/console.log' || echo "test-modlooks-linux: container ended with $?"
}
[ -f "$fx/pins.txt" ] && [ "$fx/pins.txt" -nt "$here/test-modlooks-linux.sh" ] || fixtures
for c in "$@"; do
  dir="$root/$c"; rm -rf "$dir"; mkdir -p "$dir"; : > "$dir/settings.ini"
  case "$c" in
    animelee) run "$dir" 900 "-e MU_LOOKS_TEST_DIR=/fx -e MU_LOOKS_TEST_INSTALL=1" ;;
    diet) run "$dir" 1200 "-e MU_LOOKS_TEST_DIR=/fx -e MU_LOOKS_TEST_INSTALL=0" ;;
    person)
      cp -R "$fx" "$dir/fx-person"
      printf '<html><body>Please complete the security check to continue.</body></html>\n' > "$dir/fx-person/$(python3 -I -c 'import sys; print("".join(c if c.isalnum() else "_" for c in sys.argv[1]))' "https://www.mediafire.com/file/2ztqob93nrug8yg/ANIMELEE_-_COMPLETE_EDITION_-_PATCH_1.0.zip/file")"
      run "$dir" 600 "-e MU_LOOKS_TEST_DIR=/out/fx-person -e MU_LOOKS_TEST_INSTALL=1" ;;
    lookpack)
      mkdir -p "$dir/mods/diet-classic"; cp "${MU_LOOKPACK_DISC:-$disc/mods/akaneia/akaneia.iso}" "$dir/mods/diet-classic/diet-classic.iso"
      printf '%s\n' "look 3" > "$dir/settings.ini"
      run "$dir" 600 "" ;;
    *) echo "unknown case $c" >&2; exit 2 ;;
  esac
  echo "== $c"; grep -hE "^looks: |look pack|draws with your build|^mods: (profile|look)" "$dir/console.log" | cut -c1-220 | grep -v "looks: Off\|match frames" | head -14
  find "$dir/mods" -name "*.iso" 2>/dev/null | sed "s|$dir/||" | head
  rm -rf "$dir/mods/diet-classic" 2>/dev/null || true
done
