// Slippi EXI device (slot B): the game-side Slippi codes talk to it with DMA writes (command
// buffers) and DMA reads (responses). Port of Dolphin's CEXISlippi, grown feature by feature.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include "slippi_record_events.h"

namespace slippi {

void init();
void shutdown();
// EXI transfers on the Slippi channel.
void dma_write(uint32_t addr, uint32_t size);
void dma_read(uint32_t addr, uint32_t size);
void imm_write(uint32_t data, uint32_t size);
uint32_t imm_read(uint32_t size);
// Diagnostics.
uint32_t gct_load_address();
uint64_t commands_seen();
uint64_t replays_written();
RecordingEvents recording_events(); // snapshot on the simulation thread or after it has stopped
const std::string& replay_directory();
const std::string& last_replay_path();   // the .slp most recently written (for the game report upload)
// Widescreen 16:9 (Slippi's optional code, compiled in both ways). request_* is thread-safe and
// takes effect on the simulation thread at the next retrace; the initial value comes from the
// command line / settings before the game loads the code table.
void request_widescreen(bool on);
bool widescreen();
void poll_options();

}  // namespace slippi
