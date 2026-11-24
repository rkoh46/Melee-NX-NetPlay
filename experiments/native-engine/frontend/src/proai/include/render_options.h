// The live settings the Practice menu's warm-up rows read (upstream gx/render_options.h, the same defaults). There is no
// warm-up room in this app; the values only keep those rows' code as upstream has it.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
namespace gx {
struct RenderOptions {
  static int& live_warmup_panel_style() { static int value = 0; return value; }
  static int& live_warmup_watch() { static int value = 1; return value; }
  static int& live_warmup_pip_size() { static int value = 1; return value; }
  static int& live_warmup_pip_corner() { static int value = 4; return value; }
  static std::atomic<float>& live_warmup_pip_cx() { static std::atomic<float> value{0.5f}; return value; }
  static std::atomic<float>& live_warmup_pip_cy() { static std::atomic<float> value{0.5f}; return value; }
  static std::atomic<float>& live_warmup_pip_cw() { static std::atomic<float> value{0.0f}; return value; }
  static int& live_spectate_view() { static int value = 0; return value; }
};
}  // namespace gx
