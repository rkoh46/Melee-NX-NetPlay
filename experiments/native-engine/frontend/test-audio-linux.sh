#!/usr/bin/env bash
# melee-unlocked's Audio buffer and Use custom music without a console: play mode (mu-native-gfx-linux) in a container on a
# screenless X server, SDL's disk audio driver recording what the output plays (the game's sound and the jukebox's music).
#   ./test-audio-linux.sh /path/to/disc-folder CASE...
# Cases:
#   auto      --auto-vs and a match, `audio_mode 0` (Auto): the buffer it starts with, what gaps in the match add, the number kept
#             in audio-auto.txt; then a second start, which begins where the first settled
#   fixed     the same with `audio_mode 1`, `audio_buffer_ms 15` (Low latency): the ring's slack stays 15 ms
#   jukebox   `musicpacks 1` and no music pack: the game's own songs through the jukebox (decoded from the disc's HPS files)
#   te-music  20XX TE's "menu music from Sound Test" (MU_MODS_DIR with the TE save in te/; mod_profile te, musicpacks 1 so the
#             log names each song): Data > Sound Test, a song played with A (command 0xF7 keeps it in settings.ini.state); the
#             next start's menu then plays that song
#   packs     `musicpacks 1` with test tones (tools/music_decode_test.sh makes them with ffmpeg) as the menu's song
#             (MusicPacks/audio/menu01.mp3) and Onett's two songs (a playlist folder MusicPacks/audio/onetto+onetto2/ with an
#             OGG and a FLAC): the log names each pick, and the recording has a 440 Hz tone over the game's sound
# Output: build/native-engine-gfx/run-audio/<case>/ (console.log, out.raw, MusicPacks/). MU_TEST_LIMIT: time limit in s.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
work="$repo/build/native-engine-gfx"; root="$work/run-audio"
mkdir -p "$root"

run() {   # $1 folder, $2 frames, $3 container name suffix, $4 extra arguments
  local dir=$1 frames=$2 name=$3 extra=$4
  local mods=(); [ -n "${MODS:-}" ] && mods=(-v "$MODS:/out/mods:ro")
  mkdir -p "$dir/shots"
  timeout "${MU_TEST_LIMIT:-600}" docker run --rm --name "mnx-hw-audio-$name-$$" --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$dir:/out" ${mods[@]+"${mods[@]}"} \
    -e FRAMES="$frames" -e EXTRA="$extra" -e SDL_AUDIO_DRIVER=disk -e SDL_AUDIO_DISK_OUTPUT_FILE=/out/out.raw "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
      Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
      export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
      for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
      script=""; [ -f /out/script.txt ] && script="--play-script /out/script.txt"
      /b/mu-native-gfx-linux --play --iso /iso/GALE01.iso --out /out --settings /out/settings.ini --frames "$FRAMES" --volume 70 $script $EXTRA >>/out/console.log 2>&1 &
      pid=$!; n=0
      while kill -0 $pid 2>/dev/null; do
        sleep 2; n=$((n+1))
        xwd -root -silent 2>/dev/null | convert xwd:- "$(printf /out/shots/'"$name"'-%03d.png $n)" 2>/dev/null || true
      done
      wait $pid; echo "exit code $?" >>/out/console.log
      kill %1 2>/dev/null || true' || echo "test-audio-linux: container ended with $?"
}
fresh() { rm -rf "$1"; mkdir -p "$1"; }
# The recording's 440 Hz share: Goertzel power at 440 Hz against the whole signal, per second of the recording.
tone_report() {
  python3 -I - "$1" "$2" <<'PY'
import math, struct, sys
path, rate = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()
n = len(data) // 4
left = struct.unpack("<%dh" % (n * 2), data[:n * 4])[0::2]
k = 2 * math.cos(2 * math.pi * 440 / rate)
seconds = []
for start in range(0, n - rate, rate):
    s1 = s2 = 0.0; energy = 0.0
    for x in left[start:start + rate]:
        s = x + k * s1 - s2; s2 = s1; s1 = s; energy += x * x
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    seconds.append(power / (energy * rate / 2) if energy else 0.0)
loud = [i for i, v in enumerate(seconds) if v > 0.2]
print("recording: %.1f s at %d Hz; seconds where a 440 Hz tone is over 20%% of the sound: %d (%s)" % (n / rate, rate, len(loud),
      ", ".join(str(i) for i in loud[:40]) + (" ..." if len(loud) > 40 else "")))
PY
}

for case in "$@"; do
  dir="$root/$case"
  case "$case" in
  auto)
    fresh "$dir"
    printf '%s\n' "audio_mode 0" > "$dir/settings.ini"
    run "$dir" 2400 a "--auto-vs"
    echo "--- second start" >> "$dir/console.log"
    run "$dir" 900 b "--auto-vs"
    ;;
  fixed)
    fresh "$dir"
    printf '%s\n' "audio_mode 1" "audio_buffer_ms 15" > "$dir/settings.ini"
    run "$dir" 1800 a "--auto-vs"
    ;;
  jukebox)
    fresh "$dir"
    printf '%s\n' "musicpacks 1" > "$dir/settings.ini"
    run "$dir" 1800 a "--auto-vs"
    ;;
  te-music)
    [ -n "${MU_MODS_DIR:-}" ] || { echo "== $case: skipped: MU_MODS_DIR (with te/) not given"; continue; }
    fresh "$dir"
    printf '%s\n' "mod_profile te" "musicpacks 1" > "$dir/settings.ini"
    # From the start: the memory card message, the title, then the main menu (1-P Mode first): down four times to Data,
    # A, down twice to Sound Test, A; in Sound Test three songs to the right, A plays it.
    python3 -I - "$dir/script.txt" <<'PY'
import sys
L = ["130 A", "140", "400 START", "410"]
t = 560
def p(w, n=1, gap=20, hold=4):
    global t
    for _ in range(n): L.extend([f"{t} {w}", f"{t+hold}"]); t += gap
p("sy=-127", 4); t += 30; p("A", gap=90); p("sy=-127", 2); t += 30; p("A", gap=150); p("sx=127", 3); t += 30; p("A", gap=200)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
    MODS="$MU_MODS_DIR" run "$dir" 1600 a ""
    printf '%s\n' "130 A" "140" "400 START" "410" > "$dir/script.txt"; echo "--- second start" >> "$dir/console.log"
    MODS="$MU_MODS_DIR" run "$dir" 1000 b ""
    ;;
  packs)
    fresh "$dir"
    tones="$repo/build/native-engine/music-test"
    [ -f "$tones/tone.mp3" ] || bash "$here/../tools/music_decode_test.sh" >/dev/null
    mkdir -p "$dir/MusicPacks/audio/onetto+onetto2"
    cp "$tones/tone.mp3" "$dir/MusicPacks/audio/menu01.mp3"
    cp "$tones/tone.ogg" "$tones/tone.flac" "$dir/MusicPacks/audio/onetto+onetto2/"
    printf '%s\n' "musicpacks 1" > "$dir/settings.ini"
    run "$dir" 1800 a "--auto-vs"
    ;;
  *) echo "unknown case $case" >&2; exit 2 ;;
  esac
  echo "== $case"
  grep -hE "^audio:|jukebox|ring target|exit code|STOPPED|rror|second start" "$dir/console.log" | grep -v "audio: game made" | cut -c1-200 | tail -30
  rate=$(sed -n 's/.*device buffer [0-9]* frames at \([0-9]*\) Hz.*/\1/p' "$dir/console.log" | head -1)
  [ -f "$dir/out.raw" ] && [ -n "$rate" ] && tone_report "$dir/out.raw" "$rate"
  [ -f "$dir/settings.ini.state" ] && grep -h "te_menu_music" "$dir/settings.ini.state"
  [ -f "$dir/audio-auto.txt" ] && { echo "audio-auto.txt:"; cat "$dir/audio-auto.txt"; }
  [ -f "$dir/MusicPacks/Tracks.txt" ] && echo "MusicPacks/Tracks.txt: $(wc -l < "$dir/MusicPacks/Tracks.txt") lines"
done
