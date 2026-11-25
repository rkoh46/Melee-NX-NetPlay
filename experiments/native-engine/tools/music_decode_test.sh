#!/usr/bin/env bash
# Use custom music's decoders (frontend/src/media_decode.cpp): test tones in WAV, MP3, OGG Vorbis (ffmpeg's own experimental encoder), FLAC and M4A made with
# ffmpeg on this computer (a 3 s 440 Hz sine at half scale), then tools/music_decode_test.cpp built and run in the aarch64
# Linux container with no network. Needs ../fetch.sh (the decoders and the fetched tree's linux_media.h) and ffmpeg.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
tones="$repo/build/native-engine/music-test"; rm -rf "$tones"; mkdir -p "$tones"
s='0.5*sin(2*PI*440*t)'
tone() { ffmpeg -loglevel error -f lavfi -i "aevalsrc=$s|$s:s=$1:d=3" "${@:2}"; }
tone 44100 "$tones/tone.wav"
ffmpeg -loglevel error -f lavfi -i "aevalsrc=$s:s=22050:d=3" "$tones/tone-mono.wav"
tone 44100 -c:a libmp3lame -b:a 192k "$tones/tone.mp3"
tone 48000 -c:a vorbis -strict experimental "$tones/tone.ogg"
tone 32000 -c:a flac "$tones/tone.flac"
tone 44100 -c:a aac "$tones/tone.m4a"
timeout 600 docker run --rm --network none --name "mnx-music-check-$$" -v "$repo:/r" -w /r "$IMG" bash -c '
  set -e
  g++ -std=c++17 -O1 -o /tmp/music_decode_test experiments/native-engine/tools/music_decode_test.cpp experiments/native-engine/frontend/src/media_decode.cpp \
    -Ibuild/native-engine/mu/port/linux -Ibuild/native-engine/audio-decoders -w
  /tmp/music_decode_test build/native-engine/music-test'
