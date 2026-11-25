// melee-unlocked's PhobGCC menu on the console (its gx/phob_menu.cpp, GPL-3.0-only, and host/phob_store.cpp, GPL-3.0-or-later,
// compiled from the fetched tree: build.sh sync copies them to build/native-engine-gfx/gen/phob): PhobVision's screens (stick
// calibration and notch fix, the snapback filter, waveshaping, smoothing, cardinal snapping, radius scaling, the remap editor,
// trigger modes, tourney lockouts, resets), PhobScope (input viewer, stickmaps, value-over-time plots with snapback, dashback,
// pivot and powershield odds, button timing, reaction test, continuous scope, gate visualizer, CSV export, coordinate viewer),
// Phob Check and the named Setups. Here: the host services those files call, answered by src/phob_adapter.cpp (the adapter's
// reports on the console, frontend/patches/0123; a scripted adapter in the Linux tests), and the render-thread call.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.h"            // src/phob_shim
#include "controller_profiles.h"
#include "phob_menu.h"       // build-time copy (gen/phob)
#include "phob_menu_state.h"
#include "controls_files.h"  // fe::controls::profile_clean_name

#include <imgui.h>

namespace host {
void gcadapter_phob_set(int port, const phob::Settings& s) { fe::phob_adapter::set_settings(port, s); }
phob::Settings gcadapter_phob_get(int port) { return fe::phob_adapter::settings(port); }
bool gcadapter_phob_raw(int port, phob::HardwareInput& in) { return fe::phob_adapter::raw(port, in); }
int gcadapter_scope(int port, GCScopeSample* out, int max) { return fe::phob_adapter::scope(port, out, max); }
void gcadapter_test_script_start() { fe::phob_adapter::test_script_start(); }
double gcadapter_poll_rate_hz() { return fe::phob_adapter::rate_hz(); }
std::string profile_clean_name(const std::string& name) { return fe::controls::profile_clean_name(name); }
}  // namespace host

namespace fe {
void phob_menu_draw(float display_w, float display_h, float game_x0, float game_y0, float game_x1, float game_y1) {
  (void)display_w; (void)display_h;
  const phob_menu::View v = phob_menu::view();
  if (!v.open) { if (gx::phob_native_open()) gx::phob_native_close(); return; }
  if (v.just_opened || !gx::phob_native_open()) gx::phob_native_request(v.port, v.scopes_only);
  gx::PhobNativeIn in;
  in.x0 = game_x0; in.y0 = game_y0; in.x1 = game_x1; in.y1 = game_y1;
  // Until the controllers were let go since it opened, nothing of theirs is passed: upstream's menu arms itself when the adapter
  // socket's own controller is at rest and would otherwise take the press that opened it from the panel as its first A.
  in.buttons = v.armed ? v.buttons : 0;
  in.stick_x = v.armed ? v.stick_x : 0;
  in.stick_y = v.armed ? v.stick_y : 0;
  in.armed = v.armed;
  in.time = ImGui::GetTime();
  const gx::PhobNativeOut out = gx::phob_native_frame(in);
  if (out.back_to_panel || out.close_all) gx::phob_native_close();
  phob_menu::result(out.back_to_panel, out.close_all, out.changed);
}
}  // namespace fe
