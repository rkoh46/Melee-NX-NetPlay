// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_frontend.h"
#include "gx_aurora_bridge.h"

#include <algorithm>
#include <stdexcept>

namespace host {

MetalFrontend::MetalFrontend(std::unique_ptr<MetalNativeHost> native_host,
                             std::unique_ptr<gx::Backend> renderer)
    : native_host_(std::move(native_host)), renderer_(std::move(renderer)),
      owner_thread_(std::this_thread::get_id()) {
  if (!native_host_ || !renderer_)
    throw std::invalid_argument("MetalFrontend requires native host and renderer adapters");
}

MetalFrontend::~MetalFrontend() { shutdown(); }

void MetalFrontend::shutdown() noexcept {
  if (!native_host_) return;
  // AuroraBackend must release its GX objects before aurora_shutdown.
  renderer_.reset();
  native_host_->shutdown();
  native_host_.reset();
}

bool MetalFrontend::running() const noexcept { return native_host_ && renderer_; }

void MetalFrontend::require_active() {
  if (std::this_thread::get_id() != owner_thread_) {
    ++stats_.unsupported.wrong_thread;
    throw std::logic_error("MetalFrontend must be used on its owning thread");
  }
  if (!running()) {
    ++stats_.unsupported.after_shutdown;
    throw std::logic_error("MetalFrontend is shut down");
  }
}

void MetalFrontend::submit_frame(const gx::Frame& frame) {
  ++stats_.submit_attempts;
  require_active();
  if (last_sequence_ && frame.sequence <= *last_sequence_) {
    ++stats_.unsupported.frame_order;
    throw std::invalid_argument("MetalFrontend requires strictly increasing frame sequences");
  }
  try {
    renderer_->submit_frame(frame);
  } catch (const gx::aurora_bridge::CoverageError&) {
    ++stats_.unsupported.render_state;
    throw;
  }
  last_sequence_ = frame.sequence;
  ++stats_.presented_frames;
  stats_.decoded_draws += frame.draws.size();
  stats_.presented_xfb += std::count_if(frame.copies.begin(), frame.copies.end(),
                                       [](const gx::EfbCopy& copy) { return copy.to_xfb; });
}

void MetalFrontend::submit_frame(const gx::Frame& frame, const gx::DrawMatrices* overrides) {
  if (!overrides) {
    submit_frame(frame);
    return;
  }
  ++stats_.submit_attempts;
  require_active();
  ++stats_.unsupported.interpolation;
  throw gx::aurora_bridge::CoverageError(
      "Metal frontend coverage: unlocked interpolation is not fidelity-validated");
}

void MetalFrontend::set_skip_present(bool skip) {
  require_active();
  if (skip) {
    ++stats_.unsupported.drain_without_present;
    throw gx::aurora_bridge::CoverageError(
        "Metal frontend coverage: drain without presentation is not implemented");
  }
  renderer_->set_skip_present(false);
}

bool MetalFrontend::pump_events() {
  require_active();
  const auto events = native_host_->pump_events();
  stats_.native_events += events.events;
  stats_.resize_events += events.resize_events;
  close_requested_ = close_requested_ || events.close_requested;
  return !close_requested_;
}

bool MetalFrontend::close_requested() const noexcept { return close_requested_; }

MetalWindowSize MetalFrontend::window_size() const noexcept {
  return native_host_ ? native_host_->window_size() : MetalWindowSize{};
}

void MetalFrontend::resize(uint32_t width, uint32_t height) {
  require_active();
  if (!width || !height) throw std::invalid_argument("MetalFrontend resize requires non-zero dimensions");
  native_host_->resize(width, height);
  ++stats_.resize_requests;
}

MetalFrontendStats MetalFrontend::stats() const noexcept { return stats_; }

} // namespace host
