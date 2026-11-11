// Hitch catcher. A small watcher thread checks every 4 ms how long the game thread has been inside
// its current frame. When a frame passes 40 ms (and again at 150 and 600 ms, for long stalls) it
// pauses the game thread for a few microseconds, reads where it is (pc, lr and the chain of frame
// records on its stack), resumes it and logs one line:
//   hitch: frame N at 41 ms: pc=... lr=... up=a,b,c,... (anchor switch_hitch_start=...)
// and, at 40 and 600 ms, one "hitch: frame N thread <name>: ..." line for every other thread, so a
// game thread that is waiting shows what it waits for and the others show what holds it up.
// tools/nx-hitch.sh names the addresses with the ELF of the run. Always on; it costs nothing until
// a frame is already late. hitch_watch=0 in launcher.ini turns it off.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch_hitch.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace host { void log(const char* fmt, ...); uint32_t retrace_count(); }
namespace {
Thread g_watcher;
Handle g_target = INVALID_HANDLE;
uint64_t g_stack_lo = 0, g_stack_hi = 0;
std::atomic<bool> g_run{false};
std::atomic<uint64_t> g_frame_begin{0};   // system tick at the start of the game thread's current frame
std::atomic<uint32_t> g_frame_number{0};
constexpr uint64_t kTicksPerMs = 19200;

extern "C" int switch_cpu_threads(Handle* handles, char (*names)[20], int cap);   // switch_threads.c

// Where one other thread is: pc, lr and up to 8 frame records found within 1 MB above its sp.
void capture_other(Handle h, const char* name, uint32_t frame) {
  if (h == g_target || h == threadGetCurHandle()) return;
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return;
  ThreadContext ctx{};
  Result rc = 0;
  for (int tries = 0; tries < 20; ++tries) { rc = svcGetThreadContext3(&ctx, h); if (R_SUCCEEDED(rc)) break; svcSleepThread(5000); }
  uint64_t up[8] = {}; int n = 0;
  if (R_SUCCEEDED(rc)) {
    const uint64_t lo = ctx.sp, hi = ctx.sp + (1u << 20);
    uint64_t fp = ctx.fp;
    while (n < 8 && fp >= lo && fp + 16 <= hi && !(fp & 15)) {
      const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
      if (rec[1]) up[n++] = rec[1];
      if (rec[0] <= fp) break;
      fp = rec[0];
    }
  }
  svcSetThreadActivity(h, ThreadActivity_Runnable);
  if (R_FAILED(rc)) return;
  char chain[8 * 18 + 1] = ""; size_t at = 0;
  for (int i = 0; i < n && at < sizeof chain - 18; ++i) at += std::snprintf(chain + at, sizeof chain - at, "%s%llx", i ? "," : "", (unsigned long long)up[i]);
  host::log("hitch: frame %u thread %s: pc=%llx lr=%llx up=%s (anchor switch_hitch_start=%llx)", frame, name,
            (unsigned long long)ctx.pc.x, (unsigned long long)ctx.lr, chain, (unsigned long long)reinterpret_cast<uintptr_t>(&switch_hitch_start));
}

// Every other registered thread at once (graphics, netplay, audio, disc...): a game thread that waits
// shows what it waits on; these show who is holding it up.
void capture_all(uint32_t frame) {
  Handle handles[24]; char names[24][20];
  const int n = switch_cpu_threads(handles, names, 24);
  for (int i = 0; i < n; ++i) capture_other(handles[i], names[i], frame);
}

void capture(uint32_t frame, uint64_t ms) {
  if (R_FAILED(svcSetThreadActivity(g_target, ThreadActivity_Paused))) return;
  ThreadContext ctx{};
  Result rc = 0;
  for (int tries = 0; tries < 50; ++tries) {
    rc = svcGetThreadContext3(&ctx, g_target);
    if (R_SUCCEEDED(rc)) break;
    svcSleepThread(5000);
  }
  uint64_t up[16] = {};
  int n = 0;
  if (R_SUCCEEDED(rc)) {
    uint64_t fp = ctx.fp;
    while (n < 16 && fp >= g_stack_lo && fp + 16 <= g_stack_hi && !(fp & 15)) {
      const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
      if (rec[1]) up[n++] = rec[1];
      if (rec[0] <= fp) break;
      fp = rec[0];
    }
  }
  svcSetThreadActivity(g_target, ThreadActivity_Runnable);
  if (R_FAILED(rc)) { host::log("hitch: frame %u at %llu ms: context unavailable (%x)", frame, (unsigned long long)ms, rc); return; }
  char chain[16 * 18 + 1] = "";
  size_t at = 0;
  for (int i = 0; i < n && at < sizeof chain - 18; ++i) at += std::snprintf(chain + at, sizeof chain - at, "%s%llx", i ? "," : "", (unsigned long long)up[i]);
  host::log("hitch: frame %u at %llu ms: pc=%llx lr=%llx up=%s (anchor switch_hitch_start=%llx)", frame, (unsigned long long)ms,
            (unsigned long long)ctx.pc.x, (unsigned long long)ctx.lr, chain,
            (unsigned long long)reinterpret_cast<uintptr_t>(&switch_hitch_start));
}

void watcher(void*) {
  uint32_t captured_frame = ~0u;
  int stage = 0;
  static const uint64_t marks[3] = {40, 150, 600};
  while (g_run.load(std::memory_order_relaxed)) {
    svcSleepThread(4'000'000);
    const uint64_t begin = g_frame_begin.load(std::memory_order_acquire);
    if (!begin) continue;
    const uint32_t frame = g_frame_number.load(std::memory_order_relaxed);
    if (frame != captured_frame) { captured_frame = frame; stage = 0; }
    if (stage >= 3) continue;
    const uint64_t ms = (armGetSystemTick() - begin) / kTicksPerMs;
    if (ms < marks[stage]) continue;
    ++stage;
    capture(frame, ms);
    if (stage == 1 || stage == 3) capture_all(frame);   // at 40 ms and again if it is still stuck at 600 ms
  }
}
}  // namespace

void switch_hitch_frame_begin(void) {
  g_frame_number.store(host::retrace_count(), std::memory_order_relaxed);
  g_frame_begin.store(armGetSystemTick(), std::memory_order_release);
}

bool switch_hitch_start(Thread* target, uint64_t stack_bytes) {
  if (g_run.load() || !target) return false;
  g_target = target->handle;
  // The stack libnx allocated for the thread: frame records are trusted only inside it.
  g_stack_lo = reinterpret_cast<uintptr_t>(target->stack_mirror ? target->stack_mirror : target->stack_mem);
  g_stack_hi = g_stack_lo + (target->stack_sz ? target->stack_sz : stack_bytes);
  g_run.store(true);
  // Core 0 and a priority above the graphics threads there, so a stuck frame is seen at once.
  Result rc = threadCreate(&g_watcher, watcher, nullptr, nullptr, 32 * 1024, 0x1E, 0);
  if (R_FAILED(rc)) rc = threadCreate(&g_watcher, watcher, nullptr, nullptr, 32 * 1024, 0x28, 0);
  if (R_FAILED(rc)) { g_run.store(false); return false; }
  threadStart(&g_watcher);
  return true;
}

void switch_hitch_stop(void) {
  if (!g_run.exchange(false)) return;
  threadWaitForExit(&g_watcher);
  threadClose(&g_watcher);
}
