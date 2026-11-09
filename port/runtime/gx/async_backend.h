// A frame queue between the simulation thread and the thread that owns the renderer.
//
// The renderer (AuroraBackend behind MetalFrontend on the Switch) must be driven from the
// thread that created it, and its submit blocks until the GPU-side threads have consumed
// the frame. Running that on the simulation thread cost a third of every frame on the
// Switch. AsyncBackend takes the simulation's finished frame (buffers moved, not copied,
// a recycled frame comes back) and the owner thread renders it from run().
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "frame_queue.h"
#include "gx_core.h"
#include <atomic>
#include <cstdint>
#include <functional>

namespace gx {
class AsyncBackend final : public Backend {
public:
  explicit AsyncBackend(Backend& renderer) : renderer_(renderer) {}
  // Simulation thread.
  void submit_frame(const Frame& frame) override;
  void submit_and_recycle(Frame& frame) override;
  bool wants_raw_vertices() const override { return renderer_.wants_raw_vertices(); }
  bool wants_state_deltas() const override { return renderer_.wants_state_deltas(); }
  // Owner thread: renders queued frames until `stop` is set and the queue is empty. Rendering
  // exceptions propagate to the caller, which owns the result of the run.
  void run(const std::atomic<bool>& stop);
  void finish() { queue_.finish(true); }
  // Called on the owner thread between frames and while waiting (window event pumping).
  void set_idle(std::function<void()> idle) { idle_ = std::move(idle); }
  uint64_t rendered() const { return rendered_.load(); }
  uint64_t dropped() const { return dropped_.load(); }
private:
  Backend& renderer_;
  FrameQueue queue_;
  std::atomic<uint64_t> rendered_{0}, dropped_{0};
  std::function<void()> idle_;
  unsigned stat_frames_ = 0;
  double stat_ms_ = 0.0, stat_worst_ms_ = 0.0;
};
}  // namespace gx
