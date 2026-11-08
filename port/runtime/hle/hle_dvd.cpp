// DVD HLE: file reads served from the ISO, completions delivered at guest wait points.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle.h"
#include "dvd_read_worker.h"
#include "hle_dvd.h"
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace {
constexpr uint32_t DVD_STATE_END = 0, DVD_STATE_BUSY = 1;
constexpr uint32_t DVD_COMMAND_READ = 1;

// DVDFileInfo: cb (0x30 bytes) + startAddr(0x30) + length(0x34) + callback(0x38)
void finish_read(uint32_t block, uint32_t addr, uint32_t length, uint32_t disc_offset) {
  host::wr32(block + 0x08, DVD_COMMAND_READ);
  host::wr32(block + 0x0C, DVD_STATE_END);
  host::wr32(block + 0x10, disc_offset);
  host::wr32(block + 0x14, length);
  host::wr32(block + 0x18, addr);
  host::wr32(block + 0x1C, length);
  host::wr32(block + 0x20, length);
}
void do_read(uint32_t block, uint32_t addr, uint32_t length, uint32_t disc_offset) {
  host::SimCostScope cost(host::SIM_DVD);
  if (!host::disc_read(disc_offset, host::ptr(addr, length), length))
    host::die("disc read failed: offset %08X length %X to %08X", disc_offset, length, addr);
  finish_read(block, addr, length, disc_offset);
}

// Asynchronous reads run on a worker so a stage load (tens of MB) never stalls the simulation
// thread for long (which starves audio). Completion is delivered at a fixed *virtual* time after
// the request (a quarter frame), in request order, so the guest sees deterministic timing; if
// the worker has not finished by then the simulation waits for it, as it used to for every read.
struct AsyncRead {
  AsyncRead() = default;
  AsyncRead(uint32_t block_, uint32_t addr_, uint32_t length_, uint32_t disc_offset_,
            uint32_t callback_, bool file_info_)
      : block(block_), addr(addr_), length(length_), disc_offset(disc_offset_),
        callback(callback_), file_info(file_info_) {}

  uint32_t block = 0, addr = 0, length = 0, disc_offset = 0, callback = 0;
  bool file_info = false;
  uint64_t ready_tb = 0;
  uint64_t generation = 0;
  hle::dvd_detail::ReadWorker::Job job;
};
std::mutex g_dvd_lifecycle_mutex;
std::mutex g_dvd_pending_mutex;
std::recursive_mutex g_dvd_delivery_mutex;
std::deque<AsyncRead> g_dvd_pending;  // request order fixes guest-visible completion order
std::atomic<bool> g_dvd_active{false};
std::atomic<uint64_t> g_dvd_generation{0};
hle::dvd_detail::ReadWorker g_dvd_worker(
    [](uint32_t offset, void* dst, uint32_t length) {
      return host::disc_read(offset, dst, length);
    });

void post_dvd_callback(uint64_t generation, uint32_t callback, uint32_t result,
                       uint32_t block) {
  if (!callback || !g_dvd_active.load(std::memory_order_acquire) ||
      g_dvd_generation.load(std::memory_order_acquire) != generation)
    return;
  host::post_completion([generation, callback, result, block] {
    // A completion may already be in the host queue when shutdown starts. The generation also
    // prevents an old completion from reviving if another guest run initializes DVD later.
    std::lock_guard<std::recursive_mutex> delivery_lock(g_dvd_delivery_mutex);
    if (g_dvd_active.load(std::memory_order_acquire) &&
        g_dvd_generation.load(std::memory_order_acquire) == generation)
      host::call_guest(callback, result, block);
  });
}

bool start_read(AsyncRead r) {
  // The delivery barrier prevents shutdown from passing this guest-state commit, while the
  // pending lock is deliberately released before any checked guest-memory access can fatal.
  std::lock_guard<std::recursive_mutex> delivery_lock(g_dvd_delivery_mutex);
  {
    std::lock_guard<std::mutex> pending_lock(g_dvd_pending_mutex);
    if (!g_dvd_active.load(std::memory_order_acquire)) return false;
    // Preserve the existing lazy-start behavior: DVDInit opens a lifecycle generation, while the
    // worker thread is created only if that generation actually submits an asynchronous read.
    g_dvd_worker.initialize();
    r.ready_tb = host::cpu->tb + host::TB_PER_FRAME / 4;
    r.generation = g_dvd_generation.load(std::memory_order_relaxed);
    r.job = g_dvd_worker.submit(r.disc_offset, r.length);
    if (!r.job) return false;
    g_dvd_pending.push_back(r);
  }
  host::wr32(r.block + 0x08, DVD_COMMAND_READ);
  host::wr32(r.block + 0x0C, DVD_STATE_BUSY);
  host::wr32(r.block + (r.file_info ? 0x38 : 0x28), r.callback);
  return true;
}
}  // namespace

namespace hle {
void dvd_init() {
  std::lock_guard<std::recursive_mutex> delivery_lock(g_dvd_delivery_mutex);
  std::lock_guard<std::mutex> lifecycle_lock(g_dvd_lifecycle_mutex);
  if (g_dvd_active.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> pending_lock(g_dvd_pending_mutex);
  g_dvd_pending.clear();
  g_dvd_generation.fetch_add(1, std::memory_order_acq_rel);
  g_dvd_active.store(true, std::memory_order_release);
}

void dvd_shutdown() noexcept {
  // Serialize with a callback already executing guest code. DVD callbacks may nest at guest
  // wait points, so their delivery barrier is recursive on the simulation thread.
  std::lock_guard<std::recursive_mutex> delivery_lock(g_dvd_delivery_mutex);
  std::lock_guard<std::mutex> lifecycle_lock(g_dvd_lifecycle_mutex);
  {
    std::lock_guard<std::mutex> pending_lock(g_dvd_pending_mutex);
    // This is the delivery cutoff. Queued guest state/callbacks are abandoned, while the worker
    // is joined below after any already-started host read finishes into its private buffer.
    g_dvd_active.store(false, std::memory_order_release);
    g_dvd_pending.clear();
  }
  g_dvd_worker.shutdown();
}

// Called from the simulation thread at every wait point.
void dvd_poll() {
  for (;;) {
    AsyncRead r;
    {
      std::lock_guard<std::mutex> lock(g_dvd_pending_mutex);
      if (!g_dvd_active.load(std::memory_order_acquire) || g_dvd_pending.empty()) return;
      if (host::cpu->tb < g_dvd_pending.front().ready_tb) return;
      r = g_dvd_pending.front();
    }

    if (!dvd_detail::ReadWorker::finished(r.job->status.load(std::memory_order_acquire))) {
      host::SimCostScope cost(host::SIM_DVD);
      g_dvd_worker.wait(r.job);
    }

    bool read_failed = false;
    {
      // Serialize guest-state commit with shutdown. The non-recursive pending mutex is released
      // before checked guest access, and failed-read fatal dispatch happens after this barrier too.
      std::lock_guard<std::recursive_mutex> delivery_lock(g_dvd_delivery_mutex);
      bool deliver = false;
      {
        std::lock_guard<std::mutex> pending_lock(g_dvd_pending_mutex);
        if (!g_dvd_active.load(std::memory_order_acquire) ||
            g_dvd_generation.load(std::memory_order_acquire) != r.generation)
          return;
        if (g_dvd_pending.empty() || g_dvd_pending.front().job != r.job) continue;

        const dvd_detail::ReadStatus status = r.job->status.load(std::memory_order_acquire);
        if (status == dvd_detail::ReadStatus::failed) {
          read_failed = true;
          g_dvd_pending.pop_front();
        } else if (status == dvd_detail::ReadStatus::cancelled) {
          g_dvd_pending.pop_front();
        } else if (status == dvd_detail::ReadStatus::complete) {
          deliver = true;
          g_dvd_pending.pop_front();
        }
      }

      if (deliver) {
        // Only the simulation thread receives a guest pointer. Worker I/O always targets owned data.
        if (r.length) {
          host::SimCostScope cost(host::SIM_DVD);
          std::memcpy(host::ptr(r.addr, r.length), r.job->data.data(), r.length);
        }
        finish_read(r.block, r.addr, r.length, r.disc_offset);
        post_dvd_callback(r.generation, r.callback, r.length, r.block);
      }
    }
    if (read_failed)
      host::die("disc read failed: offset %08X length %X to %08X", r.disc_offset,
                r.length, r.addr);
  }
}
}  // namespace hle

HLE(DVDInit) {
  // Only the filesystem tables need initialising; everything else is host-side.
  hle::dvd_init();
  host::call_guest(gs::__DVDFSInit);
  host::wr32(0x80000000u + 0, host::rd32(0x80000000u));  // keep disc id (no-op, documents intent)
  TRACE("DVDInit");
}

// BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
HLE(DVDReadAsyncPrio) {
  uint32_t info = ARG0, addr = ARG1, length = ARG2, offset = ARG3, callback = ARG4;
  host::pump_completions();
  uint32_t start = host::rd32(info + 0x30);
  TRACE("DVDReadAsyncPrio info=%08X addr=%08X len=%X off=%X cb=%08X", info, addr, length, offset, callback);
  bool accepted = start_read(AsyncRead{info, addr, length, start + offset, callback, true});
  RET(accepted ? 1 : 0);
}

// s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio)
HLE(DVDReadPrio) {
  uint32_t info = ARG0, addr = ARG1, length = ARG2, offset = ARG3;
  uint32_t start = host::rd32(info + 0x30);
  TRACE("DVDReadPrio info=%08X addr=%08X len=%X off=%X", info, addr, length, offset);
  do_read(info, addr, length, start + offset);
  RET(length);
}

// BOOL DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, s32 length, s32 offset, DVDCBCallback cb, s32 prio)
HLE(DVDReadAbsAsyncPrio) {
  uint32_t block = ARG0, addr = ARG1, length = ARG2, offset = ARG3, callback = ARG4;
  host::pump_completions();
  TRACE("DVDReadAbsAsyncPrio block=%08X addr=%08X len=%X off=%X", block, addr, length, offset);
  bool accepted = start_read(AsyncRead{block, addr, length, offset, callback, false});
  RET(accepted ? 1 : 0);
}

HLE(DVDGetCommandBlockStatus) { host::pump_completions(); RET(host::rd32(ARG0 + 0x0C)); }
HLE(DVDCheckDisk) { host::pump_completions(); RET(1); }
HLE(DVDGetDriveStatus) { host::pump_completions(); RET(0); }
HLE(DVDGetCurrentDiskID) { RET(0x80000000u); }
HLE(DVDCancelAsync) {
  post_dvd_callback(g_dvd_generation.load(std::memory_order_acquire), ARG1, 0, ARG0);
  RET(1);
}
HLE(DVDCancel) { RET(0); }
HLE(DVDReset) {}
HLE(DVDPrepareStreamAsync) { RET(0); }
HLE(DVDPrepareStream) { RET(0); }
HLE(DVDCancelStreamAsync) { RET(0); }
HLE(DVDCancelStream) { RET(0); }
HLE(DVDStopStreamAtEndAsync) { RET(0); }
HLE(DVDGetStreamPlayAddrAsync) { RET(0); }
HLE(DVDGetStreamStartAddrAsync) { RET(0); }
HLE(DVDGetStreamLengthAsync) { RET(0); }
HLE(DVDGetStreamErrorStatusAsync) { RET(0); }
HLE(DVDSeekAsyncPrio) { RET(1); }
