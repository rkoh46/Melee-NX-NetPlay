#!/usr/bin/env bash
# The replay viewer, takeover, coach and library without a console: play mode (mu-native-gfx-linux) in a container on a
# screenless X server with Mesa's software Vulkan driver, screenshots every second.
#   ./test-replays-linux.sh /path/to/disc-folder CASE...
# Cases:
#   record    --auto-vs and a controller script (walk to the computer player, jabs, short-hop aerials, some L-cancelled):
#             the match is recorded as a replay (written at the frame limit), kept as run-replays/replay.slp for the others
#   plain     plays replay.slp back untouched: the fidelity check (post-frame records against the file) must say 0 differ
#   controls  --feature-test replay-controls: pause, two frame steps, play, 5 s back, 5 s forward, 1/2 and 1/4 speed, fast
#             forward, the overlay hidden and shown; the fidelity check must still say 0 differ
#   tips      --feature-test replay-tips: the coach's tips read from the file, next and last tip jumps
#   takeover  --feature-test replay-takeover: P1 is taken over at frame 400 (the live player jabs), retried once, then
#             back to watching; the frames compared outside the takeover must not differ
#   practice  --feature-test replay-practice: the next tip jumped to, taken over there, with a try that ends when the combo
#             drops (settings: replay_try_length 0), the result, the retry
#   analyze   --feature-test replay-analyze: the whole replay run once unseen from where it is, then back
#   library   --feature-test replay-library in the app (--app): the in-game menu opened on its Replays page, the list, the
#             shown replay's stats (both copies), delete (A twice) of one copy, then Watch it on the other
# Output: build/native-engine-gfx/run-replays/<case>/ (console.log, picture.log, shots/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-replays"
mkdir -p "$root"

# $1 case folder, $2 frames, $3 seconds between screenshots, $4 extra play-mode arguments; settings.ini (and script.txt) are
# already in the folder.
run() {
  local dir=$1 frames=$2 every=$3 extra=$4 name
  name="mnx-replays-$(basename "$dir")-$$"
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "$name" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
    -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$extra" -e SDL_AUDIODRIVER=dummy -e MU_ENV="${MU_ENV:-}" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      [ -z "$MU_ENV" ] || export $MU_ENV
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      script=""; [ -f /out/script.txt ] && script="--play-script /out/script.txt"
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" --window 1280x720 --volume 0 $script $EXTRA >/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep "$EVERY"; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/shot-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log' || echo "test-replays-linux: container ended with $?"
  # timeout stops the docker client, not the container: make sure it is gone.
  timeout 60 docker rm -f "$name" >/dev/null 2>&1 || true
}
fresh() { rm -rf "$1"; mkdir -p "$1/shots" "$1/Replays"; : > "$1/settings.ini"; }
need_replay() { [ -f "$root/replay.slp" ] || { echo "run the record case first" >&2; exit 1; }; cp "$root/replay.slp" "$1/Replays/Game_20261124T120000.slp"; }
playback() {   # $1 case folder, $2 frames, $3 feature test (or empty)
  local test=""; [ -z "$3" ] || test="--feature-test $3"
  run "$1" "$2" 1 "--replay /out/Replays/Game_20261124T120000.slp $test"
}

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  record)
    fresh "$dir"
    python3 -I - "$dir/script.txt" <<'PY'
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
for i in range(8):                                          # forward smashes
    lines += [f"{t} sx=127 A", f"{t+3}", f"{t+60} sx=-127", f"{t+64}"]; t += 90
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    run "$dir" 3000 5 "--auto-vs"
    slp="$(ls -t "$dir"/Replays/*.slp 2>/dev/null | head -1 || true)"
    [ -n "$slp" ] && cp "$slp" "$root/replay.slp" && echo "recorded $(basename "$slp") ($(wc -c <"$slp") bytes) -> run-replays/replay.slp"
    ;;
  plain)    fresh "$dir"; need_replay "$dir"; playback "$dir" 2400 "" ;;
  controls) fresh "$dir"; need_replay "$dir"; playback "$dir" 2400 replay-controls ;;
  tips)     fresh "$dir"; need_replay "$dir"; playback "$dir" 2400 replay-tips ;;
  takeover) fresh "$dir"; need_replay "$dir"; playback "$dir" 2400 replay-takeover ;;
  practice) fresh "$dir"; need_replay "$dir"; printf '%s\n' "replay_try_length 0" > "$dir/settings.ini"; playback "$dir" 2400 replay-practice ;;
  analyze)  fresh "$dir"; need_replay "$dir"; playback "$dir" 2400 replay-analyze ;;
  library)
    # The app (--app, its folder /out/app as on the SD card): two copies of the replay, the menu opened on the Replays
    # page, the stats of both, the second deleted, then Watch it on the first (the app starts the game again with it).
    fresh "$dir"; app="$dir/app"; mkdir -p "$app/Replays" "$app/User/Slippi"
    cp "$root/replay.slp" "$app/Replays/Game_20261124T120000.slp"; cp "$root/replay.slp" "$app/Replays/Game_20261124T110000.slp"
    printf '%s\n' "iso=/iso/GALE01.iso" "frames=3000" > "$app/launcher.ini"
    run "$dir" 3000 1 "--app --base /out/app --feature-test replay-library"
    cp "$app/Logs/latest.log" "$dir/app-latest.log" 2>/dev/null || true
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  echo "== $case"
  grep -hE "^replay|^coach|^library|\[replay\]|^play: |exit code|STOPPED|rror" "$dir/console.log" | cut -c1-220 | tail -40
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
