// Deterministic AI DMA sink for the headless gate. WAV contains raw game DMA,
// matching the existing WAV path: 32 kHz, L/R PCM, before host volume or jukebox.
// No audio device, resampler, clock feedback, or guest event suppression.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.h"
#include "audio_headless.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>

namespace host {
namespace {
constexpr size_t BLOCK_BYTES = 640;
constexpr uint32_t SAMPLE_RATE = 32000;
std::atomic<int> volume{0};
std::mutex mutex;
FILE* wav = nullptr;
uint32_t wav_bytes = 0;
uint64_t frames = 0;
bool open = false, output_ok = true;

bool write_header(FILE* file, uint32_t size) {
  uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                        0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
  auto put32 = [&](unsigned at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) header[at + i] = uint8_t(value >> (i * 8));
  };
  put32(4, 36 + size); put32(24, SAMPLE_RATE); put32(28, SAMPLE_RATE * 4); put32(40, size);
  return std::fwrite(header, 1, sizeof header, file) == sizeof header;
}
}
void audio_set_volume(int value) { volume.store(std::clamp(value, 0, 100)); }
int audio_volume() { return volume.load(); }
bool audio_open(int value, const char* path, bool open_device) {
  std::lock_guard<std::mutex> lock(mutex);
  if (open || open_device) return false;
  audio_set_volume(value);
  frames = 0; wav_bytes = 0; output_ok = true;
  if (path && *path) {
    wav = std::fopen(path, "wbx");
    if (!wav) return false;
    if (!write_header(wav, 0)) { std::fclose(wav); wav = nullptr; output_ok = false; return false; }
  }
  open = true;
  return true;
}
void audio_push(const uint8_t* samples, size_t bytes) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open) return;
  // The guest's AI path submits whole five-millisecond blocks. Preserve the
  // original output path's treatment of an incomplete final block.
  for (size_t offset = 0; offset + BLOCK_BYTES <= bytes; offset += BLOCK_BYTES) {
    uint8_t converted[BLOCK_BYTES];
    for (size_t i = 0; i < BLOCK_BYTES; i += 4) {
      const uint8_t* in = samples + offset + i;
      converted[i] = in[3]; converted[i + 1] = in[2];
      converted[i + 2] = in[1]; converted[i + 3] = in[0];
    }
    if (wav && output_ok) {
      if (wav_bytes > std::numeric_limits<uint32_t>::max() - 36 - BLOCK_BYTES ||
          std::fwrite(converted, 1, sizeof converted, wav) != sizeof converted) output_ok = false;
      else wav_bytes += BLOCK_BYTES;
    }
    frames += BLOCK_BYTES / 4;
  }
}
void audio_close() {
  std::lock_guard<std::mutex> lock(mutex);
  if (wav) {
    if (std::fseek(wav, 0, SEEK_SET) != 0 || !write_header(wav, wav_bytes)) output_ok = false;
    if (std::fclose(wav) != 0) output_ok = false;
    wav = nullptr;
  }
  open = false;
}
bool headless_audio_output_ok() { std::lock_guard<std::mutex> lock(mutex); return output_ok; }
uint64_t audio_pushed_frames() { std::lock_guard<std::mutex> lock(mutex); return frames; }
uint64_t audio_dropped_blocks() { return 0; }
uint64_t audio_underruns(uint64_t* silent_ms) { if (silent_ms) *silent_ms = 0; return 0; }
void audio_rate_range(double* low, double* high) { if (low) *low = 1.0; if (high) *high = 1.0; }
uint32_t audio_buffered_ms() { return 0; }
}  // namespace host
