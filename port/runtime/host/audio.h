// Host audio output: 32 kHz 16-bit stereo blocks from the emulated AI DMA, played through WinMM.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace host {
void audio_set_volume(int volume);
int audio_volume();
// volume_percent 0..100; 0 keeps the session muted (default for development).
bool audio_open(int volume_percent, const char* wav_dump_path = nullptr, bool open_device = true);
void audio_close();
// `bytes` of big-endian 16-bit samples ordered R, L, R, L ... (GameCube AI DMA format).
void audio_push(const uint8_t* be_samples, size_t bytes);
uint64_t audio_pushed_frames();
uint64_t audio_dropped_blocks();
uint64_t audio_underruns(uint64_t* silent_ms);   // output gaps (ring empty), and the total time they held the last sample
void audio_rate_range(double* low, double* high);  // resampling ratio extremes used to track the sound card's clock
uint32_t audio_buffered_ms();                      // how much audio is queued for the device right now
}  // namespace host
