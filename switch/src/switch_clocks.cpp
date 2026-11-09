// Clock control for the Erista Switch. The recompiled game and its rollback
// re-simulation are CPU-bound, so the port asks for the console's highest CPU
// clock while it runs, the way RetroArch and other homebrew do through the
// clkrst service (8.0.0+; pcv before that). Rates are player settings in
// launcher.ini (clock_cpu_mhz, clock_gpu_mhz, clock_mem_mhz; overclock=0 keeps
// the system's own clocks). sys-clk, when installed, overrides whatever is set
// here, which is fine: it is the same request made by a different hand.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch_clocks.h"

#include <switch.h>

#include <cstdio>

namespace {
bool g_clkrst = false, g_pcv = false;

bool set_rate(PcvModuleId module, uint32_t hz, uint32_t* actual_hz) {
  if (g_clkrst) {
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 3))) return false;
    const Result set = clkrstSetClockRate(&session, hz);
    uint32_t got = 0;
    clkrstGetClockRate(&session, &got);
    clkrstCloseSession(&session);
    if (actual_hz) *actual_hz = got;
    return R_SUCCEEDED(set);
  }
  if (g_pcv) {
    PcvModule legacy = module == PcvModuleId_CpuBus ? PcvModule_CpuBus : module == PcvModuleId_GPU ? PcvModule_GPU : PcvModule_EMC;
    const Result set = pcvSetClockRate(legacy, hz);
    uint32_t got = 0;
    pcvGetClockRate(legacy, &got);
    if (actual_hz) *actual_hz = got;
    return R_SUCCEEDED(set);
  }
  return false;
}
}  // namespace

bool switch_clocks_init() {
  if (hosversionAtLeast(8, 0, 0)) g_clkrst = R_SUCCEEDED(clkrstInitialize());
  else g_pcv = R_SUCCEEDED(pcvInitialize());
  return g_clkrst || g_pcv;
}

void switch_clocks_exit() {
  if (g_clkrst) clkrstExit();
  if (g_pcv) pcvExit();
  g_clkrst = g_pcv = false;
}

SwitchClockResult switch_clocks_apply(const SwitchClockRequest& request) {
  SwitchClockResult result{};
  if (!g_clkrst && !g_pcv) return result;
  result.cpu_ok = request.cpu_mhz && set_rate(PcvModuleId_CpuBus, request.cpu_mhz * 1000000u, &result.cpu_hz);
  result.gpu_ok = request.gpu_mhz && set_rate(PcvModuleId_GPU, request.gpu_mhz * 1000000u, &result.gpu_hz);
  result.mem_ok = request.mem_mhz && set_rate(PcvModuleId_EMC, request.mem_mhz * 1000000u, &result.mem_hz);
  return result;
}

SwitchClockResult switch_clocks_read() {
  SwitchClockResult result{};
  if (!g_clkrst && !g_pcv) return result;
  auto read = [](PcvModuleId module, uint32_t* hz) {
    if (g_clkrst) {
      ClkrstSession session;
      if (R_FAILED(clkrstOpenSession(&session, module, 3))) return;
      clkrstGetClockRate(&session, hz);
      clkrstCloseSession(&session);
    } else {
      PcvModule legacy = module == PcvModuleId_CpuBus ? PcvModule_CpuBus : module == PcvModuleId_GPU ? PcvModule_GPU : PcvModule_EMC;
      pcvGetClockRate(legacy, hz);
    }
  };
  read(PcvModuleId_CpuBus, &result.cpu_hz);
  read(PcvModuleId_GPU, &result.gpu_hz);
  read(PcvModuleId_EMC, &result.mem_hz);
  return result;
}

uint32_t switch_measure_cpu_mhz() {
  uint64_t x = 0;
  const uint64_t t0 = armGetSystemTick();
  for (int i = 0; i < 1 << 18; ++i) {
#define ADD8 "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
    asm volatile(ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 : "+r"(x));
#undef ADD8
  }
  const uint64_t ticks = armGetSystemTick() - t0;
  return ticks ? static_cast<uint32_t>(x * armGetSystemTickFreq() / ticks / 1000000u) : 0;
}
