// Checks src/media_decode.cpp (Use custom music's player-file decoders) on test tones made by music_decode_test.sh:
// each file decodes at its rate, to its stated length, as a 440 Hz tone at the level it was written with (the
// zero crossings give the pitch), and an M4A is refused (the disc's song plays then). Also the ten-minute limit.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "full/linux_media.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) ++failures; }

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  struct Case { const char* file; uint32_t rate; bool decodes; };
  const Case cases[] = {{"tone.wav", 44100, true}, {"tone.mp3", 44100, true}, {"tone.ogg", 48000, true}, {"tone.flac", 32000, true},
                        {"tone-mono.wav", 22050, true}, {"tone.m4a", 44100, false}};
  for (const Case& c : cases) {
    uint32_t rate = 0; size_t expected = 0, frames = 0; bool too_long = false;
    double sum = 0, peak = 0; size_t crossings = 0; float last = 0;
    const bool ok = linux_media::decode_audio(dir + "/" + c.file, &rate, 32000u * 600, [&](const float* pcm, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        const float l = pcm[i * 2];
        sum += double(l) * l; peak = std::max(peak, double(std::fabs(l)));
        if ((last < 0) != (l < 0) && frames + i > 0) ++crossings;
        last = l;
      }
      frames += n; return true;
    }, &expected, &too_long);
    if (!c.decodes) { check(!ok, std::string(c.file) + ": not decoded (the disc's song plays)"); continue; }
    const double seconds = rate ? double(frames) / rate : 0, hz = seconds > 0 ? crossings / 2.0 / seconds : 0, rms = frames ? std::sqrt(sum / frames) : 0;
    char line[256];
    std::snprintf(line, sizeof line, "%s: %u Hz, %zu frames (states %zu), %.2f s, %.0f Hz tone, RMS %.3f, peak %.3f", c.file, rate, frames, expected, seconds, hz, rms, peak);
    check(ok && rate == c.rate && std::fabs(seconds - 3.0) < 0.1 && std::fabs(hz - 440) < 5 && rms > 0.3 && rms < 0.4 && peak < 0.55, line);
  }
  uint32_t rate = 0; size_t expected = 0; bool too_long = false;
  const bool ok = linux_media::decode_audio(dir + "/tone.wav", &rate, 44100, [](const float*, size_t) { return true; }, &expected, &too_long);
  check(!ok && too_long, "tone.wav with a one-second limit: refused as too long");
  std::printf("%d failed\n", failures);
  return failures ? 1 : 0;
}
