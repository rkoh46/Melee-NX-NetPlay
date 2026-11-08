// Aurora owns the Cocoa window; adapt it to the runtime's portable host hooks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_window.h"

#include "host.h"
#include "metal_frontend.h"
#include "window.h"

#include <cstdlib>
#include <stdexcept>

namespace host {
namespace {
MetalFrontend* g_frontend = nullptr;
ResizeCallback g_resize_callback;
MetalWindowSize g_last_size;
}

void metal_window_attach(MetalFrontend& frontend) {
  if (g_frontend) throw std::logic_error("a Metal frontend is already attached");
  if (!frontend.running()) throw std::invalid_argument("cannot attach a stopped Metal frontend");
  g_frontend = &frontend;
  g_last_size = frontend.window_size();
}

void metal_window_detach(MetalFrontend& frontend) noexcept {
  if (g_frontend == &frontend) {
    g_frontend = nullptr;
    g_last_size = {};
    g_resize_callback = {};
  }
}

void* window_create(int, int, const wchar_t*, bool) {
  die("Aurora owns window creation in the Metal executable");
}
void window_set_message_callback(MessageCallback) {}
void window_input_capture(bool) {}
bool window_ui_gamecube_pad(PadState& pad) { pad = {}; pad.err = -1; return false; }
void window_set_resize_callback(ResizeCallback callback) { g_resize_callback = std::move(callback); }

void window_pump() {
  if (!g_frontend) return;
  static const bool diag_no_pump = std::getenv("MELEE_DIAG_NO_PUMP") != nullptr;   // isolation aid
  if (diag_no_pump) return;
  if (!g_frontend->pump_events()) request_exit(0);
  const auto size = g_frontend->window_size();
  if ((size.width != g_last_size.width || size.height != g_last_size.height) &&
      size.width && size.height) {
    g_last_size = size;
    if (g_resize_callback) g_resize_callback(static_cast<int>(size.width), static_cast<int>(size.height));
  }
}

// The first Metal target intentionally has no fullscreen/input UI surface.
void window_set_fullscreen(bool) {}
void window_gamepad_rumble(int, bool) {}
bool window_take_fullscreen_toggle() { return false; }
double window_refresh_rate() { return 60.0; }
void window_destroy() {}
void window_set_title(const wchar_t*) {}
bool window_closed() { return g_frontend && g_frontend->close_requested(); }
void window_client_size(int* width, int* height) {
  const auto size = g_frontend ? g_frontend->window_size() : MetalWindowSize{};
  if (width) *width = static_cast<int>(size.width);
  if (height) *height = static_cast<int>(size.height);
}
} // namespace host
