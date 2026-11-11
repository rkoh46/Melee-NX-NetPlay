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
    const double t1 = host::now_seconds();
    const double ms = (t1 - t0) * 1000.0;
    stat_ms_ += ms; if (ms > stat_worst_ms_) stat_worst_ms_ = ms;
    // Age of the picture when the render thread finished handing it on: from the moment the game
    // finished the frame (frame.time; its inputs were read one simulation frame earlier) to here. Aurora's FIFO, render worker and the swap
    // chain come after this, so it is a floor on input-to-display latency, not the whole of it.
    static double age_sum = 0.0, age_worst = 0.0;
    if (frame.time > 0.0) { const double age = (t1 - frame.time) * 1000.0; age_sum += age; if (age > age_worst) age_worst = age; }
    ++rendered_;
    if (++stat_frames_ == 120) {
      host::log("renderer: %.1f ms/frame on the render thread (worst %.1f), %llu stale frames dropped so far, %llu given up while the renderer was stuck; "
                "frame age at hand-off %.1f ms (worst %.1f)",
                stat_ms_ / stat_frames_, stat_worst_ms_, (unsigned long long)dropped_.load(), (unsigned long long)queue_.overflow_dropped.load(), age_sum / stat_frames_, age_worst);
      stat_frames_ = 0; stat_ms_ = stat_worst_ms_ = 0.0; age_sum = age_worst = 0.0;
    }
    queue_.recycle(std::move(frame));
    frame = Frame{};
  }
}

}  // namespace gx
