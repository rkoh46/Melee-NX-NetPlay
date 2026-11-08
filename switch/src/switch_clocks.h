// See switch_clocks.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

struct SwitchClockRequest {
  uint32_t cpu_mhz = 0, gpu_mhz = 0, mem_mhz = 0;   // 0 leaves that clock alone
};
struct SwitchClockResult {
  bool cpu_ok = false, gpu_ok = false, mem_ok = false;
  uint32_t cpu_hz = 0, gpu_hz = 0, mem_hz = 0;      // what the system reports afterwards
};
// Erista's highest documented rates, used as the defaults: CPU 1785 MHz, GPU 921 MHz
// (the docked GPU clock), memory 1600 MHz (the docked memory clock).
constexpr SwitchClockRequest kSwitchClocksErista{1785, 921, 1600};

bool switch_clocks_init();                  // false when neither clkrst nor pcv is reachable
void switch_clocks_exit();
SwitchClockResult switch_clocks_apply(const SwitchClockRequest& request);
SwitchClockResult switch_clocks_read();
