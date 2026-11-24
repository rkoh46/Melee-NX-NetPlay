#!/usr/bin/env bash
# Picture test without a console: runs mu-native-gfx-linux in a container on an X server without a screen
# (Xvfb), drawing with Mesa's software Vulkan driver, and saves a screenshot of the window every few seconds.
#   ./test-linux.sh /path/to/disc-folder [script] [retraces] [seconds between screenshots] [extra runner arguments]
# The folder must hold GALE01.iso; it is mounted read-only. Output: build/native-engine-gfx/run-linux/
# (console.log, picture.log, shots/*.png). No network in the container. Sound goes to SDL's dummy device. MU_TEST_LIMIT is the time limit in seconds.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; script="${2:-parity_vs_onett}"; frames="${3:-9000}"; every="${4:-5}"; shift $(( $# > 4 ? 4 : $# ))
work="$repo/build/native-engine-gfx"; run="$work/run-linux"
rm -rf "$run/shots" "$run"/*.log; mkdir -p "$run/shots"     # run-linux/aurora (the pipeline cache) is kept, so a second run tests the warm-up
mods=(); [ -n "${MU_MODS_DIR:-}" ] && mods=(-v "$MU_MODS_DIR:/out/mods:ro")     # mod discs and saves, read-only, where play mode looks for them
[ -n "${MU_FRAME_MODE:-}" ] && mods+=(-e "MU_FRAME_MODE=$MU_FRAME_MODE")     # sub-frame presentation in a scripted run (features/present)
timeout "${MU_TEST_LIMIT:-1500}" docker run --rm --name "${MU_DOCKER_NAME:-mnx-test}-linux-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$run:/out" ${mods[@]+"${mods[@]}"} \
  -e SCRIPT="$script" -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$*" -e MU_GFX_DRAW_DUMP="${MU_GFX_DRAW_DUMP:-}" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
    export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
    for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    /b/mu-native-gfx-linux --iso /iso/GALE01.iso --out /out --script "$SCRIPT" --frames "$FRAMES" --novsync $EXTRA >/out/console.log 2>&1 &
    pid=$!; n=0
    while kill -0 $pid 2>/dev/null; do
      sleep "$EVERY"; n=$((n+1))
      xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
    done
    wait $pid; echo "exit code $?" >>/out/console.log'
grep -v "^renderer:\|^gx: [0-9]* draws\|^retrace " "$run/console.log" | tail -40
ls "$run/shots" | wc -l
