// Slippi-aware PADControlMotor routing shared by the HLE and its tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace hle::pad {

struct RumbleDecision {
  bool deliver = false;
  uint8_t physical_port = 0;
  bool on = false;
};

// Mirrors Online/Core/HandleRumble.asm for scene 0x0208. Invalid guest state
// fails closed instead of reading outside the supplied RAM view or addressing
// a nonexistent adapter port.
RumbleDecision decide_rumble(uint32_t logical_port, uint32_t command,
                             uint8_t mode, uint8_t state_id,
                             uint32_t r13, const uint8_t* ram,
                             size_t ram_size);

}  // namespace hle::pad
