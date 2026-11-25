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
#   legacy    replay.slp with its stored code list removed (../tools/slp_strip_codes.py), as replays from before Slippi stored one:
#             the playback must say it uses Slippi's legacy list and which codes that is (the post-frame check may differ: the
#             recording was made with the current codes)
#   analyze   --feature-test replay-analyze: the whole replay run once unseen from where it is, then back
#   music     musicpacks 1 and --feature-test replay-pause (6 s paused at frame 300), SDL's disk audio recording: the jukebox's song
#             stops while the viewer is paused and goes on after it (the recording's level per half second)
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
  name="${MU_DOCKER_NAME:-mnx}-replays-$(basename "$dir")-$$"
  timeout "${MU_TEST_LIMIT:-900}" docker run --rm --name "$name" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" \
    -e FRAMES="$frames" -e EVERY="$every" -e EXTRA="$extra" -e SDL_AUDIODRIVER="${MU_AUDIO_DRIVER:-dummy}" -e SDL_AUDIO_DRIVER="${MU_AUDIO_DRIVER:-dummy}" -e SDL_AUDIO_DISK_OUTPUT_FILE=/out/out.raw -e VOLUME="${MU_VOLUME:-0}" -e MU_ENV="${MU_ENV:-}" "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      [ -z "$MU_ENV" ] || export $MU_ENV
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      script=""; [ -f /out/script.txt ] && script="--play-script /out/script.txt"
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" --window 1280x720 --volume "$VOLUME" $script $EXTRA >/out/console.log 2>&1 &
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
  music)
    # musicpacks 1 (the jukebox plays the disc's songs in the sound output's callback) and the viewer paused for 6 s at frame 300
    # (--feature-test replay-pause), SDL's disk audio driver recording the output: the jukebox's song must stop with the picture
    # (audio.cpp music_hold) and go on after it, so the recording is silent while paused (the game's own sound stops with it).
    fresh "$dir"; need_replay "$dir"; printf '%s\n' "musicpacks 1" > "$dir/settings.ini"
    MU_AUDIO_DRIVER=disk MU_VOLUME=70 playback "$dir" 1400 replay-pause
    rate=$(sed -n 's/.*device buffer [0-9]* frames at \([0-9]*\) Hz.*/\1/p' "$dir/console.log" | head -1)
    [ -f "$dir/out.raw" ] && [ -n "$rate" ] && python3 -I - "$dir/out.raw" "$rate" <<'PY'
import math, struct, sys
data = open(sys.argv[1], "rb").read(); rate = int(sys.argv[2])
n = len(data) // 4; left = struct.unpack("<%dh" % (n * 2), data[:n * 4])[0::2]
half = rate // 2
def level(xs):   # the sound's level without its constant part (a starved output holds its last sample)
    m = sum(xs) / len(xs); return math.sqrt(sum((x - m) * (x - m) for x in xs) / len(xs))
levels = [level(left[i:i + half]) for i in range(0, n - half, half)]
print("recording: %.1f s; level per half second (RMS around the mean): %s" % (n / rate, " ".join("%d" % v for v in levels)))
quiet = [i for i, v in enumerate(levels) if v < 20]
runs = []; start = None
for i in range(len(levels) + 1):
    q = i < len(levels) and levels[i] < 20
    if q and start is None: start = i
    if not q and start is not None: runs.append((start, i)); start = None
print("silent stretches (RMS under 20): %s" % ", ".join("%.1f-%.1f s" % (a / 2, b / 2) for a, b in runs if b - a >= 2))
PY
    ;;
  legacy)   fresh "$dir"; need_replay "$dir"
            python3 -I "$here/../tools/slp_strip_codes.py" "$root/replay.slp" "$dir/Replays/Game_20261124T120000.slp"
            playback "$dir" 600 "" ;;
  ucf08)
    # A UCF 0.8 replay, as Slippi Dolphin recorded online matches until December 2023, made by this program (no third-party
    # replay): (1) a match recorded as the record case does, with dashbacks through partial stick positions and shield drops
    # added; (2) that replay played with UCF 0.8 (MU_TEST_REPLAY_UCF=8) while the playback's own events are written as a new file
    # (MU_TEST_RERECORD) whose code list carries Slippi's UCF 0.8 codes instead of 0.84's (scripts/ucf08_codes.txt); (3) the new
    # file played back untouched: the playback must say it was recorded with UCF 0.8 and the fidelity check 0 differ; (4) the same
    # file played with UCF 0.84 instead, which must differ (runs held just off the horizontal: 0.84's 1.0 cardinals, which 0.8 has not).
    base="$dir/record"; fresh "$base"
    python3 -I - "$base/script.txt" <<'PY'
import sys
lines = ["@scene 02 02"]
t = 200
lines += [f"{t} sx=127", f"{t+40}"]                         # walk towards the computer player
t += 60
for i in range(24):                                         # dash, then back through a partial stick position (UCF's dashback)
    mid = (-20, -45, -70, -95)[i % 4]; side = 1 if i % 2 == 0 else -1
    lines += [f"{t} sx={127 * side}", f"{t+2} sx={mid * side}", f"{t+3} sx={-127 * side}", f"{t+9}"]
    t += 30
for i in range(12):                                         # runs held just off the horizontal (0.84's 1.0 cardinals, not in 0.8)
    side = 1 if i % 2 == 0 else -1
    lines += [f"{t} sx={127 * side} sy={(12, -9, 15)[i % 3]}", f"{t+40}"]
    t += 50
for i in range(16):                                         # shield, then the stick down at different angles (shield drop)
    x = (0, 30, -30, 55)[i % 4]
    lines += [f"{t} R", f"{t+12} R sx={x} sy=-70", f"{t+13} R sx={x} sy=-127", f"{t+16}", f"{t+40} X", f"{t+42}"]
    t += 70
for i in range(12):                                         # jabs and short hops, as the record case
    lines += [f"{t} A", f"{t+3}"]; t += 9
for i in range(6):
    lines += [f"{t} X", f"{t+2}", f"{t+5} A", f"{t+8}", f"{t+29} L", f"{t+31}"]; t += 60
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
    run "$base" 3000 5 "--auto-vs"
    src="$(ls -t "$base"/Replays/*.slp 2>/dev/null | head -1 || true)"
    [ -n "$src" ] || { echo "ucf08: the match was not recorded"; continue; }
    echo "recorded (UCF 0.84): $(python3 -I "$here/../tools/slp_game_start.py" "$src")"
    make="$dir/make"; fresh "$make"; cp "$src" "$make/Replays/Game_20261124T120000.slp"; cp "$here/scripts/ucf08_codes.txt" "$make/"
    MU_ENV="MU_TEST_REPLAY_UCF=8 MU_TEST_RERECORD=/out/ucf08.slp MU_TEST_RERECORD_CODES=/out/ucf08_codes.txt" playback "$make" 2800 ""
    grep -h "replay: test" "$make/console.log" | cut -c1-200
    [ -f "$make/ucf08.slp" ] || { echo "ucf08: the UCF 0.8 replay was not written"; continue; }
    cp "$make/ucf08.slp" "$root/ucf08.slp"
    echo "made (UCF 0.8): $(python3 -I "$here/../tools/slp_game_start.py" "$root/ucf08.slp")"
    play="$dir/play"; fresh "$play"; cp "$root/ucf08.slp" "$play/Replays/Game_20261124T120000.slp"
    playback "$play" 2800 ""
    echo "UCF 0.8 replay played back: $(grep -h 'recorded with UCF\|playback finished' "$play/console.log" | tr '\n' ' ' | cut -c1-300)"
    wrong="$dir/as-084"; fresh "$wrong"; cp "$root/ucf08.slp" "$wrong/Replays/Game_20261124T120000.slp"
    MU_ENV="MU_TEST_REPLAY_UCF=84" playback "$wrong" 2800 ""
    echo "the same replay played with UCF 0.84: $(grep -h 'playback finished' "$wrong/console.log" | cut -c1-200)"
    dir="$play"
    ;;
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
  grep -hE "^replay|^coach|^library|jukebox.s music|\[replay\]|^play: |exit code|STOPPED|rror" "$dir/console.log" | cut -c1-220 | tail -40
  echo "shots: $(ls "$dir/shots" | wc -l | tr -d ' ')"
done
