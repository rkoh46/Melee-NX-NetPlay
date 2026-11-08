// High-level emulation of the GameCube AX DSP ucode (port of Dolphin's AXUCode / AXVoice.h),
// operating directly on guest RAM and the host ARAM buffer. The recompiled AX library builds
// command lists and parameter blocks exactly as on hardware; this runs them synchronously when
// the command-list mail arrives, so audio state in guest RAM evolves like it does under Dolphin.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace ax {

// Memory access used by the mixer; production binds it to host RAM/ARAM, tests to buffers.
struct Memory {
  uint16_t (*rd16)(uint32_t addr);
  uint32_t (*rd32)(uint32_t addr);
  void (*wr16)(uint32_t addr, uint16_t v);
  void (*wr32)(uint32_t addr, uint32_t v);
  const uint8_t* aram;      // 16 MB
  uint32_t aram_size;
};

void set_memory(const Memory& mem);
void reset();
// CPU -> DSP mailbox (DSPSendMailToDSP). Runs a command list when its address arrives.
void handle_mail(uint32_t mail);
// Exposed for tests.
uint32_t convert_mixer_control(uint16_t mixer_control);
void process_pb_list(uint32_t pb_addr);
void output_samples(uint32_t lr_addr, uint32_t surround_addr);
void setup_processing(uint32_t init_addr);
uint64_t frames_processed();

}  // namespace ax
