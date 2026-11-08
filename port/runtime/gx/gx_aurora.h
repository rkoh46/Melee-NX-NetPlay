// Locked-rate decoded-Frame adapter for the pinned Aurora GX renderer.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"
#include <memory>

namespace gx {
struct AuroraOptions {
  uint32_t logical_width = 640, logical_height = 480;
  uint32_t reset_clear_color = 0, reset_clear_z = 0xffffff;
};
struct AuroraStats {
  uint64_t frames = 0, draws = 0, texture_copies = 0, epoch = 0;
};

// The owner initializes Aurora/SDL and configures VI to the logical extent.
// Construct, submit, reset, and destroy on that same thread, before Aurora
// shutdown. This adapter exclusively owns GX recording while it exists.
// No guest memory, PE interrupts, or gameplay state are changed here.
class AuroraBackend final : public Backend {
public:
  explicit AuroraBackend(AuroraOptions options = {});
  ~AuroraBackend() override;
  AuroraBackend(const AuroraBackend&) = delete;
  AuroraBackend& operator=(const AuroraBackend&) = delete;
  void submit_frame(const Frame& frame) override;
  void submit_frame(const Frame& frame, const DrawMatrices* overrides) override;
  void set_skip_present(bool skip) override;
  // Must be called by the rollback/load-state owner before a new timeline.
  // Drains both workers, destroys native texture/copy identities, and restores
  // the explicit initial clear. Epoch must increase; frame sequences may restart.
  void reset_epoch(uint64_t epoch);
  AuroraStats stats() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace gx
