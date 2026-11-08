// Native Metal presentation seam for decoded GX frames.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gx_core.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace host {

struct MetalNativeEvents {
  uint64_t events = 0;
  uint64_t resize_events = 0;
  bool close_requested = false;
};

struct MetalWindowSize {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t framebuffer_width = 0;
  uint32_t framebuffer_height = 0;
};

// Platform lifecycle owned by MetalFrontend. The production adapter owns
// Aurora's Cocoa/Metal session; tests provide a non-game adapter here.
class MetalNativeHost {
public:
  virtual ~MetalNativeHost() = default;
  virtual MetalNativeEvents pump_events() = 0;
  virtual void resize(uint32_t width, uint32_t height) = 0;
  virtual MetalWindowSize window_size() const noexcept = 0;
  virtual void shutdown() noexcept = 0;
};

struct MetalUnsupportedStats {
  uint64_t frame_order = 0;
  uint64_t render_state = 0;
  uint64_t interpolation = 0;
  uint64_t drain_without_present = 0;
  uint64_t after_shutdown = 0;
  uint64_t wrong_thread = 0;
};

struct MetalFrontendStats {
  uint64_t submit_attempts = 0;
  uint64_t presented_frames = 0;
  uint64_t decoded_draws = 0;
  uint64_t presented_xfb = 0;
  uint64_t native_events = 0;
  uint64_t resize_events = 0;
  uint64_t resize_requests = 0;
  MetalUnsupportedStats unsupported;

  bool has_real_output() const noexcept {
    return presented_frames != 0 && decoded_draws != 0 && presented_xfb != 0;
  }
};

class MetalFrontend final : public gx::Backend {
public:
  MetalFrontend(std::unique_ptr<MetalNativeHost> native_host,
                std::unique_ptr<gx::Backend> renderer);
  ~MetalFrontend() override;

  MetalFrontend(const MetalFrontend&) = delete;
  MetalFrontend& operator=(const MetalFrontend&) = delete;

  void submit_frame(const gx::Frame& frame) override;
  void submit_frame(const gx::Frame& frame, const gx::DrawMatrices* overrides) override;
  void set_skip_present(bool skip) override;
  bool pump_events();
  bool close_requested() const noexcept;
  MetalWindowSize window_size() const noexcept;
  void resize(uint32_t width, uint32_t height);
  void shutdown() noexcept;
  bool running() const noexcept;
  MetalFrontendStats stats() const noexcept;

private:
  void require_active();
  std::unique_ptr<MetalNativeHost> native_host_;
  std::unique_ptr<gx::Backend> renderer_;
  const std::thread::id owner_thread_;
  std::optional<uint64_t> last_sequence_;
  MetalFrontendStats stats_;
  bool close_requested_ = false;
};

struct AuroraMetalConfig {
  std::string app_name = "Melee Unlocked Metal";
  std::string user_path;
  std::string cache_path;
  std::string resources_path;
  uint32_t window_width = 960;
  uint32_t window_height = 720;
  bool vsync = true;
};

// Initializes Aurora against explicit paths, verifies its SDL Cocoa window and
// CAMetalLayer plus the selected Metal backend, then constructs AuroraBackend.
// All lifecycle and frame methods must be used on the creating thread.
std::unique_ptr<MetalFrontend> create_aurora_metal_frontend(const AuroraMetalConfig& config);

} // namespace host
