#!/usr/bin/env bash
# The looks without a console: play mode (mu-native-gfx-linux) in a container on a screenless X server with Mesa's
# software Vulkan driver, --auto-vs into a VS match, a settings.ini per case, screenshots.
#   ./test-looks-linux.sh /path/to/disc-folder CASE...
# Cases: off, light, lighter, diet-classic, diet-64, crystal, crystal-ports, wireframe, potato, 64-style, silhouettes,
#   classic (the presets, by `look N`), arcade-plain (arcade_backdrop 1 with the stage's own look), effects (Visual
#   effects: No decorative particles alone), all (every case above).
# MU_LOOKS_STAGE: settings.ini lines added to every case (for example "stage 31"); MU_LOOKS_FRAMES (default 1500).
# Output: build/native-engine-gfx/run-looks/<case>/ (console.log with the `looks:` lines and the picture timing, shots/).
# MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-looks"
mkdir -p "$root"
cases=("$@"); [ "${#cases[@]}" -eq 0 ] || [ "${cases[0]}" = all ] && cases=(off light lighter diet-classic diet-64 crystal crystal-ports wireframe potato 64-style silhouettes classic arcade-plain effects widescreen widescreen-floats widescreen-off-floats)

run() {
  local dir=$1 name=$2 extra=${3:---auto-vs}
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "mnx-looks-$name" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
    -e FRAMES="${MU_LOOKS_FRAMES:-1500}" -e EXTRA="$extra" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" $EXTRA >/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep 2; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log' || echo "test-looks-linux: container ended with $?"
}

for c in "${cases[@]}"; do
  dir="$root/$c"; rm -rf "$dir"; mkdir -p "$dir/shots"
  case "$c" in
    off) set -- "look 0" ;;
    light) set -- "look 1" ;;
    lighter) set -- "look 2" ;;
    diet-classic) set -- "look 3" ;;
    diet-64) set -- "look 4" ;;
    crystal) set -- "look 5" ;;
    crystal-ports) set -- "look 6" ;;
    wireframe) set -- "look 7" ;;
    potato) set -- "look 8" ;;
    64-style) set -- "look 9" ;;
    silhouettes) set -- "look 10" ;;
    classic) set -- "look 11" ;;
    arcade-plain) set -- "arcade_backdrop 1" "arcade_background_rgb 203060" ;;
    effects) set -- "effects 3" ;;
    widescreen) set -- "truewidescreen 1" "playertags 1" ;;
    widescreen-floats) set -- "truewidescreen 1" "playertags 1" ;;
    widescreen-off-floats) set -- "playertags 1" ;;
    *) echo "unknown case $c" >&2; exit 2 ;;
  esac
  printf '%s\n' "$@" > "$dir/settings.ini"
  [ -n "${MU_LOOKS_STAGE:-}" ] && printf '%s\n' "$MU_LOOKS_STAGE" >> "$dir/settings.ini"
  case "$c" in *floats) run "$dir" "$c" "--feature-test training:2:17" ;; *) run "$dir" "$c" ;; esac
  echo "== $c"
  grep -hE "^looks:|render thread: (all work|  of that, converting)|exit code|STOPPED|rror" "$dir/console.log" | cut -c1-260 | tail -12
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
