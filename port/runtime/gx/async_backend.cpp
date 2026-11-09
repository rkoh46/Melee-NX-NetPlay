// See async_backend.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "async_backend.h"

#include "host.h"
#include <chrono>
#include <utility>

namespace gx {

void AsyncBackend::submit_frame(const Frame& frame) {
  Frame copy = frame;   // the recycling path below is the one the simulation uses; this one is rare
  queue_.push(std::move(copy));
}

void AsyncBackend::submit_and_recycle(Frame& frame) { queue_.push_and_recycle(frame); }

void AsyncBackend::run(const std::atomic<bool>& stop) {
  Frame frame;
  for (;;) {
    if (idle_) idle_();
    if (!queue_.pop(frame)) {               // waits up to 2 ms
      if (stop.load(std::memory_order_acquire) && queue_.drained()) break;
      if (stop.load(std::memory_order_acquire) && queue_.size() == 0) break;
      continue;
    }
    // Behind the game: a frame that already has a newer one behind it is stale, and showing
    // it would put the picture behind the sound. Drop it (its buffers go back to the game)
    // and render the newest instead. The display can only show one frame per refresh anyway.
    while (queue_.size() > 0) {
      Frame newer;
      if (!queue_.try_pop(newer)) break;
      queue_.recycle(std::move(frame));
      frame = std::move(newer);
      ++dropped_;
    }
    const double t0 = host::now_seconds();
    renderer_.submit_frame(frame);
    const double ms = (host::now_seconds() - t0) * 1000.0;
    stat_ms_ += ms; if (ms > stat_worst_ms_) stat_worst_ms_ = ms;
    ++rendered_;
    if (++stat_frames_ == 120) {
      host::log("renderer: %.1f ms/frame on the render thread (worst %.1f), %llu stale frames dropped so far",
                stat_ms_ / stat_frames_, stat_worst_ms_, (unsigned long long)dropped_.load());
      stat_frames_ = 0; stat_ms_ = stat_worst_ms_ = 0.0;
    }
    queue_.recycle(std::move(frame));
    frame = Frame{};
  }
}

}  // namespace gx
