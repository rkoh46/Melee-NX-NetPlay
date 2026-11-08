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
namespace gx {
class FrameQueue {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Frame> frames;
  std::deque<Frame> recycled;   // buffers returned by the renderer, capacity preserved
  bool finished = false;
public:
  static constexpr size_t capacity = 64;   // > 1 s at 60 Hz: longer than the longest drawable wait the system allows
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
    if (recycled.size() >= 4) return;
    frame.clear();
    recycled.push_back(std::move(frame));
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
