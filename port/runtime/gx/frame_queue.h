// Ordered transfer from the simulation to the renderer. Every frame is executed (EFB copies
// made in one frame feed later ones), never dropped; a backlog is drained by the renderer
// without presenting (threaded_backend.cpp), so the simulation only waits in the pathological
// case of a renderer that has stopped consuming altogether. The cap is just over a second: Core
// Animation can hold the render thread in nextDrawable for up to its 1 s timeout (measured 525 ms
// while a window entered full screen on an external display), and with the old half-second cap
// that stall froze the game itself, which in an online match means rollback past its limit.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
// Console: lowers the calling thread's priority (switch_threads.c); absent elsewhere.
extern "C" __attribute__((weak)) void switch_set_current_thread_priority(int priority);
namespace gx {
class FrameQueue {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Frame> frames;
  std::deque<Frame> recycled;   // buffers returned by the renderer, capacity preserved
  std::deque<Frame> returned;   // returned but not cleared yet: the cleaner thread empties them
  std::condition_variable clean_wake;
  std::thread cleaner;
  bool stopping = false;
  bool finished = false;
  // Clearing a frame destroys ~1,500 draw records (their texture and vertex-array references); on
  // the render thread that was 15% of a 4-player frame. A low-priority thread does it instead.
  void cleaner_main() {
    if (switch_set_current_thread_priority) switch_set_current_thread_priority(0x2E);
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      clean_wake.wait(lock, [&] { return stopping || !returned.empty(); });
      if (returned.empty()) return;   // stopping, nothing left
      Frame frame = std::move(returned.front()); returned.pop_front();
      lock.unlock();
      frame.clear();
      lock.lock();
      if (recycled.size() < 4) recycled.push_back(std::move(frame));
      else { lock.unlock(); { Frame drop = std::move(frame); } lock.lock(); }   // freed outside the lock
    }
  }
public:
  FrameQueue() = default;
  FrameQueue(const FrameQueue&) = delete;
  FrameQueue& operator=(const FrameQueue&) = delete;
  ~FrameQueue() {
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
    clean_wake.notify_all();
    if (cleaner.joinable()) cleaner.join();
  }
#if defined(__SWITCH__)
  // On the Switch a match frame is about 8 MB of draw state (1,500 draws of ~5 KB), and the render
  // thread can stop for a second at a time while NVK compiles a shader variant it has not seen
  // (the first match with new characters). 64 queued frames was half a gigabyte and ended the first
  // Fox-against-Marth match in std::bad_alloc; 12 bounds the backlog near 100 MB and the
  // simulation waits for the compile instead. Compiled pipelines are persisted, so this is once per
  // variant per install.
  static constexpr size_t capacity = 12;
#else
  static constexpr size_t capacity = 64;   // > 1 s at 60 Hz: longer than the longest drawable wait the system allows
#endif
  bool push(Frame frame) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return finished || frames.size() < capacity; });   // the renderer drains backlogs; this only trips if it is stuck
    if (finished) return false;
    frames.push_back(std::move(frame));
    changed.notify_all();
    return true;
  }
  // Hands the simulation's frame to the renderer without copying it, and hands back a cleared
  // frame that still owns its buffers (a few MB of vertices and draw state), so the simulation
  // thread refills them instead of reallocating every frame.
  bool push_and_recycle(Frame& frame) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return finished || frames.size() < capacity; });
    if (finished) return false;
    frames.push_back(std::move(frame));
    if (!recycled.empty()) { frame = std::move(recycled.back()); recycled.pop_back(); }
    frame.clear();
    changed.notify_all();
    return true;
  }
  // The renderer returns buffers it is about to overwrite.
  void recycle(Frame&& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (recycled.size() + returned.size() >= 6) return;   // the caller frees it (rare: a long backlog)
    if (!cleaner.joinable()) cleaner = std::thread([this] { cleaner_main(); });
    returned.push_back(std::move(frame));
    clean_wake.notify_one();
  }
  bool pop(Frame& frame) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(2), [&] { return finished || !frames.empty(); });
    if (frames.empty()) return false;
    frame = std::move(frames.front()); frames.pop_front();
    changed.notify_all();
    return true;
  }
  size_t size() { std::lock_guard<std::mutex> lock(mutex); return frames.size(); }
  // Non-blocking variant for a presenter that renders between frames.
  bool try_pop(Frame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frames.empty()) return false;
    frame = std::move(frames.front()); frames.pop_front();
    changed.notify_all();
    return true;
  }
  // Blocks up to `wait` for a frame to become available (or finish).
  template <class Rep, class Period>
  bool wait_available(std::chrono::duration<Rep, Period> wait) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, wait, [&] { return finished || !frames.empty(); }) && !frames.empty();
  }
  void finish(bool discard = false) {
    std::lock_guard<std::mutex> lock(mutex);
    finished = true;
    if (discard) frames.clear();
    changed.notify_all();
  }
  bool drained() {
    std::lock_guard<std::mutex> lock(mutex);
    return finished && frames.empty();
  }
};
}
