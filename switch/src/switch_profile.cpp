// Sampling profiler for the simulation thread. A sampler thread on another core pauses the
// target about a thousand times a second, reads its program counter and link register with
// svcGetThreadContext3, resumes it and counts the addresses. At stop, the hottest addresses
// are written as raw runtime values next to the runtime address of one known symbol, so a
// host script can turn them into ELF offsets and names (tools/nx-profile.sh). Pausing costs the
// target a few microseconds per sample; the numbers are for finding where frames go, not for
// shipping. profile=1 in launcher.ini turns it on.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch_profile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {
Thread g_sampler;
Handle g_target[2] = {INVALID_HANDLE, INVALID_HANDLE};   // sampled alternately
std::atomic<bool> g_run{false};
bool g_started = false;
unsigned g_hz = 1000;
uint64_t g_samples[2] = {0, 0}, g_failed = 0;
std::unordered_map<uint64_t, uint32_t> g_pc[2], g_lr[2];
std::unordered_map<uint64_t, uint32_t> g_svc_lr[2];   // callers of syscalls: lr when pc sits on an SVC instruction
std::unordered_map<uint64_t, uint32_t> g_range_lr[2]; // callers of a watched code range (switch_profile_watch_range)
// For syscall samples, the return addresses one and two frame records up the stack (x29 chain): the
// syscall's lr names the libnx wrapper (mutexLock, condvarWait...), these name who called the wrapper.
std::unordered_map<uint64_t, uint32_t> g_svc_up[2], g_svc_up2[2];
uint64_t g_range_lo = 0, g_range_hi = 0;

void sampler(void*) {
  const uint64_t period_ns = 1000000000ull / (g_hz ? g_hz : 1000);
  for (auto& m : g_pc) m.reserve(1 << 16);
  for (auto& m : g_lr) m.reserve(1 << 16);
  unsigned which = 0;
  while (g_run.load(std::memory_order_relaxed)) {
    svcSleepThread(period_ns);
    if (g_target[1] != INVALID_HANDLE) which ^= 1;
    const Handle target = g_target[which];
    if (R_FAILED(svcSetThreadActivity(target, ThreadActivity_Paused))) { ++g_failed; continue; }
    ThreadContext ctx{};
    Result rc = 0;
    for (int tries = 0; tries < 50; ++tries) {      // the pause takes effect at the target's next scheduling point
      rc = svcGetThreadContext3(&ctx, target);
      if (R_SUCCEEDED(rc)) break;
      svcSleepThread(5000);
    }
    // Frame records are read while the target is still paused; fp is trusted only inside its stack.
    uint64_t up = 0, up2 = 0;
    bool on_svc = false;
    if (R_SUCCEEDED(rc) && ctx.pc.x >= 0x1000 && (ctx.pc.x & 3) == 0) {
      const uint32_t insn = *reinterpret_cast<const uint32_t*>(ctx.pc.x);
      const uint32_t prev = *reinterpret_cast<const uint32_t*>(ctx.pc.x - 4);
      on_svc = (insn & 0xFFE0001Fu) == 0xD4000001u || (prev & 0xFFE0001Fu) == 0xD4000001u;
      const auto in_stack = [&](uint64_t fp) { return fp >= ctx.sp && fp < ctx.sp + (8u << 20) && !(fp & 15); };
      if (on_svc && which == 0 && in_stack(ctx.fp)) {   // the guest thread only: its 32 MB stack contains the whole 8 MB window
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(ctx.fp);
        up = rec[1];
        if (in_stack(rec[0]) && rec[0] > ctx.fp) up2 = reinterpret_cast<const uint64_t*>(rec[0])[1];
      }
    }
    svcSetThreadActivity(target, ThreadActivity_Runnable);
    if (R_FAILED(rc)) { ++g_failed; continue; }
    if (up) ++g_svc_up[which][up];
    if (up2) ++g_svc_up2[which][up2];
    ++g_samples[which];
    ++g_pc[which][ctx.pc.x];
    ++g_lr[which][ctx.lr];
    // A thread paused inside a syscall reports the pc of the SVC (or the instruction after it);
    // its lr then names the libnx wrapper's caller, which is what tells one IPC user from another.
    if (ctx.pc.x >= g_range_lo && ctx.pc.x < g_range_hi) ++g_range_lr[which][ctx.lr];
    if (ctx.pc.x >= 0x1000 && (ctx.pc.x & 3) == 0) {
      const uint32_t insn = *reinterpret_cast<const uint32_t*>(ctx.pc.x);
      const uint32_t prev = *reinterpret_cast<const uint32_t*>(ctx.pc.x - 4);
      if ((insn & 0xFFE0001Fu) == 0xD4000001u || (prev & 0xFFE0001Fu) == 0xD4000001u) ++g_svc_lr[which][ctx.lr];
    }
  }
}

void write_table(FILE* f, const char* tag, const std::unordered_map<uint64_t, uint32_t>& table) {
  std::vector<std::pair<uint64_t, uint32_t>> rows(table.begin(), table.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  const size_t n = std::min<size_t>(rows.size(), 3000);
  for (size_t i = 0; i < n; ++i) std::fprintf(f, "%s %016llx %u\n", tag, (unsigned long long)rows[i].first, rows[i].second);
}
}  // namespace

void switch_profile_watch_range(uint64_t elf_lo, uint64_t elf_hi, uint64_t elf_anchor) {
  const uint64_t slide = reinterpret_cast<uintptr_t>(&switch_profile_start) - elf_anchor;
  g_range_lo = elf_lo + slide; g_range_hi = elf_hi + slide;
}

bool switch_profile_start(Thread* target, unsigned hz, Thread* second) {
  if (g_started || !target) return false;
  g_target[0] = target->handle;
  g_target[1] = second ? second->handle : INVALID_HANDLE;
  g_hz = hz ? hz : 1000;
  g_run.store(true);
  Result rc = threadCreate(&g_sampler, sampler, nullptr, nullptr, 64 * 1024, 0x1C, 0);
  if (R_FAILED(rc)) rc = threadCreate(&g_sampler, sampler, nullptr, nullptr, 64 * 1024, 0x28, 0);
  if (R_FAILED(rc)) { g_run.store(false); return false; }
  threadStart(&g_sampler);
  g_started = true;
  return true;
}

void switch_profile_stop(const char* path) {
  if (!g_started) return;
  g_run.store(false);
  threadWaitForExit(&g_sampler);
  threadClose(&g_sampler);
  g_started = false;
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "# melee-nx-netplay profile: samples=%llu samples1=%llu failed=%llu hz=%u anchor_symbol=switch_profile_start anchor_runtime=%016llx\n",
               (unsigned long long)g_samples[0], (unsigned long long)g_samples[1], (unsigned long long)g_failed, g_hz,
               (unsigned long long)reinterpret_cast<uintptr_t>(&switch_profile_start));
  write_table(f, "pc", g_pc[0]);     // thread 0 (the guest)
  write_table(f, "lr", g_lr[0]);
  write_table(f, "pc1", g_pc[1]);    // thread 1 (the renderer), when given
  write_table(f, "lr1", g_lr[1]);
  write_table(f, "svclr", g_svc_lr[0]);
  write_table(f, "svclr1", g_svc_lr[1]);
  write_table(f, "svcup", g_svc_up[0]);
  write_table(f, "svcup1", g_svc_up[1]);
  write_table(f, "svcup2", g_svc_up2[0]);
  write_table(f, "svcup21", g_svc_up2[1]);
  write_table(f, "rangelr", g_range_lr[0]);
  write_table(f, "rangelr1", g_range_lr[1]);
  std::fclose(f);
}
