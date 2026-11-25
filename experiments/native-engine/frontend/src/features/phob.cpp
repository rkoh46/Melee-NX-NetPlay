// melee-unlocked's PhobGCC section of the CONTROLS page (its gx/phob_menu.cpp) on the console: the settings of each GameCube
// adapter port with the firmware's names, ranges and defaults (host/phob.h), kept in <app folder>/PhobGCC/port<N>.ini, and the
// stick calibration and notch fix run with the controller's own buttons, the steps on the overlay (src/phob_adapter.cpp does
// the processing on the adapter's reader thread, frontend/patches/0123). "Open the PhobGCC menu" and "Controller test" open
// upstream's full-screen PhobGCC menu (its gx/phob_menu.cpp compiled from the fetched tree, src/phob_menu_glue.cpp): PhobVision's
// screens, the remap editor, PhobScope, Phob Check and the named setups (PhobGCC/Setups/<name>.ini); while it is open the
// controllers drive it and the game gets resting controllers (phob_menu_pads, from controls.cpp).
// Inputs: a port's settings change only that controller's own input, as the firmware on a PhobGCC board would.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "phob_adapter.h"
#include "phob_menu_state.h"
#include "mu_host.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

// The panel (mu_panel.cpp) and the setups store (melee-unlocked's phob_store.cpp, picture builds only). Weak: absent in a build
// without them.
namespace host {
__attribute__((weak)) void menu_close();
__attribute__((weak)) void menu_open_extra(const std::string& title);
__attribute__((weak)) void phob_store_load(const std::string& settings_path);
}

namespace fe::features {
namespace {
namespace ph = host::phob;
std::atomic<int> g_port{0};
std::mutex g_line_mutex;
std::string g_last_line;
uint32_t g_last_until = 0;
std::atomic<uint32_t> g_now{0};

// The full-screen PhobGCC menu's simulation-thread half.
struct Menu {
  bool open = false, just_opened = false, scopes_only = false, armed = false;
  int port = 0;
  int quiet = 0;   // polls in a row with every controller let go (the panel's A that opened it may still be on its way)
  uint16_t buttons = 0;
  int8_t sx = 0, sy = 0;
  bool back_asked = false, close_asked = false;
  uint16_t swallow[4] = {};
} g_menu;
std::mutex g_menu_mutex;

void menu_request(bool scopes_only) {
  {
    std::lock_guard<std::mutex> lock(g_menu_mutex);
    g_menu.open = true; g_menu.just_opened = true; g_menu.scopes_only = scopes_only; g_menu.armed = false; g_menu.quiet = 0;
    g_menu.port = g_port.load(); g_menu.buttons = 0; g_menu.sx = g_menu.sy = 0; g_menu.back_asked = g_menu.close_asked = false;
  }
  if (&host::menu_close) host::menu_close();
  say("phob: the PhobGCC menu opened on adapter socket %d%s", g_port.load() + 1, scopes_only ? " (Controller test: PhobScope)" : "");
}

void tables(MuHostApi*, MuGameApi*) {
  if (!options.play) return;
  phob_adapter::load(options.out_dir);
  if (&host::phob_store_load) host::phob_store_load(options.out_dir + "/settings.ini");   // the setups' folder (PhobGCC/Setups)
  if (std::getenv("MU_TEST_ADAPTER_AT_START")) phob_adapter::test_script_start();   // tests: the scripted adapter from the start
}

// One integer field of the selected port's settings: a row stepping it within the firmware's range, saved at once.
struct Field { const char* label; int ph::Settings::*member; int low, high, shown_offset; };
const Field kFields[] = {
  {"  Snapback filter, X", &ph::Settings::x_snapback, ph::kSnapbackMin, ph::kSnapbackMax, 0},
  {"  Snapback filter, Y", &ph::Settings::y_snapback, ph::kSnapbackMin, ph::kSnapbackMax, 0},
  {"  Stick smoothing, X", &ph::Settings::ax_smoothing, ph::kSmoothingMin, ph::kSmoothingMax, 0},
  {"  Stick smoothing, Y", &ph::Settings::ay_smoothing, ph::kSmoothingMin, ph::kSmoothingMax, 0},
  {"  C-stick snapback, X", &ph::Settings::cx_smoothing, ph::kSmoothingMin, ph::kSmoothingMax, 0},
  {"  C-stick snapback, Y", &ph::Settings::cy_smoothing, ph::kSmoothingMin, ph::kSmoothingMax, 0},
  {"  Waveshaping, stick X", &ph::Settings::ax_waveshaping, ph::kWaveshapingMin, ph::kWaveshapingMax, 0},
  {"  Waveshaping, stick Y", &ph::Settings::ay_waveshaping, ph::kWaveshapingMin, ph::kWaveshapingMax, 0},
  {"  Cardinal snapping, stick", &ph::Settings::a_cardinal, ph::kCardinalMin, ph::kCardinalMax, 0},
  {"  Cardinal snapping, C-stick", &ph::Settings::c_cardinal, ph::kCardinalMin, ph::kCardinalMax, 0},
  {"  Radius scaling, stick", &ph::Settings::a_scaler, ph::kScalerMin, ph::kScalerMax, 0},
  {"  Radius scaling, C-stick", &ph::Settings::c_scaler, ph::kScalerMin, ph::kScalerMax, 0},
  {"  L trigger mode", &ph::Settings::l_mode, ph::kTriggerModeMin, ph::kTriggerModeMax, 1},
  {"  R trigger mode", &ph::Settings::r_mode, ph::kTriggerModeMin, ph::kTriggerModeMax, 1},
  {"  L trigger offset", &ph::Settings::l_offset, ph::kTriggerOffsetMin, ph::kTriggerOffsetMax, 0},
  {"  R trigger offset", &ph::Settings::r_offset, ph::kTriggerOffsetMin, ph::kTriggerOffsetMax, 0},
  {"  Tournament lockouts", &ph::Settings::tourney, ph::kTourneyMin, ph::kTourneyMax, 0},
};
constexpr int kFieldCount = int(sizeof kFields / sizeof kFields[0]);

void change_field(int index, int direction) {
  const int port = g_port.load();
  ph::Settings s = phob_adapter::settings(port);
  const Field& f = kFields[index];
  s.*f.member = std::clamp(s.*f.member + (direction < 0 ? -1 : 1), f.low, f.high);
  phob_adapter::set_settings(port, s);
  phob_adapter::save(port);
}
std::string field_value(int index) {
  const ph::Settings s = phob_adapter::settings(g_port.load());
  return std::to_string(s.*kFields[index].member + kFields[index].shown_offset);
}

// The rows: MenuEntry takes plain functions, so each field gets its own pair through a template on its index.
template <int I> std::string value_of() { return field_value(I); }
template <int I> void change_of(int d) { change_field(I, d); }
template <int... I> void add_fields(std::integer_sequence<int, I...>) {
  (add_menu_entry({"PhobGCC", kFields[I].label, value_of<I>, change_of<I>, nullptr, nullptr}), ...);
}

void toggle_enabled(int) {
  const int port = g_port.load();
  ph::Settings s = phob_adapter::settings(port);
  s.enabled = !s.enabled;
  phob_adapter::set_settings(port, s); phob_adapter::save(port);
  say("phob: adapter port %d PhobGCC settings %s", port + 1, s.enabled ? "on" : "off");
}
void toggle_hardware(int) {
  const int port = g_port.load();
  ph::Settings s = phob_adapter::settings(port);
  s.phob_hardware = !s.phob_hardware;
  phob_adapter::set_settings(port, s); phob_adapter::save(port);
}
void calibrate(ph::Stick which, bool notch_fix) {
  const int port = g_port.load();
  if (!phob_adapter::cal_start(port, which, notch_fix)) { std::lock_guard<std::mutex> l(g_line_mutex); g_last_line = "PhobGCC: Notch fix needs a stick that was calibrated once"; g_last_until = g_now + 240; return; }
  say("phob: %s on adapter port %d (%s)", notch_fix ? "notch fix" : "stick calibration", port + 1, which == ph::ASTICK ? "control stick" : "C-stick");
}

void on_retrace(const Retrace& info) {
  g_now = info.retrace;
  {
    bool back = false, closed = false;
    {
      std::lock_guard<std::mutex> lock(g_menu_mutex);
      if (g_menu.back_asked || g_menu.close_asked) {
        back = g_menu.back_asked; closed = true;
        g_menu.open = false; g_menu.back_asked = g_menu.close_asked = false;
      }
    }
    if (closed) say("phob: the PhobGCC menu closed%s", back ? " (back to the panel)" : "");
    if (back && &host::menu_open_extra) host::menu_open_extra("PhobGCC");
  }
  static bool was = false;
  const bool now = phob_adapter::cal_active();
  if (was && !now) { std::lock_guard<std::mutex> l(g_line_mutex); g_last_line = phob_adapter::cal_line(); g_last_until = info.retrace + 240; say("%s", g_last_line.c_str()); }
  was = now;
}
void hud(std::vector<HudLine>& out) {
  if (phob_adapter::cal_active()) { out.push_back({phob_adapter::cal_line(), 0xFFE070FFu}); return; }
  std::lock_guard<std::mutex> l(g_line_mutex);
  if (!g_last_line.empty() && g_now < g_last_until) out.push_back({g_last_line, 0xFFE070FFu});
}
}  // namespace

// controls.cpp, after the chords, every poll: while the PhobGCC menu is open the controllers are the menu's (every connected
// port's buttons together, the first stick pushed) and every port gets a resting controller; what is held when it closes is kept
// from the game until let go.
void phob_menu_pads(MuPadStatus pads[4]) {
  std::lock_guard<std::mutex> lock(g_menu_mutex);
  if (g_menu.open) {
    uint16_t b = 0; int8_t sx = 0, sy = 0;
    for (int i = 0; i < 4; ++i) {
      if (pads[i].err != 0) continue;
      b |= pads[i].button;
      if (!sx && !sy && (std::abs(int(pads[i].stick_x)) >= 30 || std::abs(int(pads[i].stick_y)) >= 30)) { sx = int8_t(pads[i].stick_x); sy = int8_t(pads[i].stick_y); }
    }
    g_menu.buttons = b; g_menu.sx = sx; g_menu.sy = sy;
    // Armed after 12 polls (0.2 s) with everything let go: the press that opened it from the panel can reach the pads a poll
    // after the panel let them go, and must not also act in the menu.
    if (!g_menu.armed) { g_menu.quiet = (b & 0x1F7F) == 0 && sx == 0 && sy == 0 ? g_menu.quiet + 1 : 0; g_menu.armed = g_menu.quiet >= 12; }
    for (int i = 0; i < 4; ++i) {
      const int8_t err = pads[i].err;
      pads[i] = MuPadStatus{};
      pads[i].err = err;
      g_menu.swallow[i] = err == 0 ? 0x1F7F : 0;
    }
    return;
  }
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0) { g_menu.swallow[i] = 0; continue; }
    g_menu.swallow[i] &= pads[i].button;
    pads[i].button &= uint16_t(~g_menu.swallow[i]);
  }
}
bool phob_menu_open() { std::lock_guard<std::mutex> lock(g_menu_mutex); return g_menu.open; }

namespace {
void register_rows_menu() {
  add_menu_entry({"PhobGCC", "Open the PhobGCC menu (PhobVision, setups, remap)", nullptr, nullptr, [] { menu_request(false); }, nullptr});
  add_menu_entry({"PhobGCC", "Controller test (PhobScope, Phob Check)", nullptr, nullptr, [] { menu_request(true); }, nullptr});
}
}  // namespace

void register_phob() {
  add_tables(tables);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"PhobGCC", "Adapter port", [] { return std::to_string(g_port.load() + 1); },
                  [](int d) { g_port.store((g_port.load() + (d < 0 ? 3 : 1)) % 4); }, nullptr, nullptr});
  register_rows_menu();
  add_run_end([] { phob_adapter::test_script_stop(); { std::lock_guard<std::mutex> lock(g_menu_mutex); g_menu.open = false; } });
  add_menu_entry({"PhobGCC", "PhobGCC settings on this port", [] { return std::string(phob_adapter::settings(g_port.load()).enabled ? "On" : "Off"); },
                  toggle_enabled, [] { toggle_enabled(0); }, nullptr});
  add_menu_entry({"PhobGCC", "  This controller is a PhobGCC (its own firmware)", [] { return std::string(phob_adapter::settings(g_port.load()).phob_hardware ? "On" : "Off"); },
                  toggle_hardware, [] { toggle_hardware(0); }, nullptr});
  add_fields(std::make_integer_sequence<int, kFieldCount>{});
  add_menu_entry({"PhobGCC", "  Calibrate the control stick", nullptr, nullptr, [] { calibrate(ph::ASTICK, false); }, nullptr});
  add_menu_entry({"PhobGCC", "  Calibrate the C-stick", nullptr, nullptr, [] { calibrate(ph::CSTICK, false); }, nullptr});
  add_menu_entry({"PhobGCC", "  Notch fix, control stick", nullptr, nullptr, [] { calibrate(ph::ASTICK, true); }, nullptr});
  add_menu_entry({"PhobGCC", "  Soft reset (keeps the calibration)", nullptr, nullptr, [] {
    const int port = g_port.load(); ph::Settings s = phob_adapter::settings(port); ph::soft_reset(s);
    phob_adapter::set_settings(port, s); phob_adapter::save(port); }, nullptr});
}
}  // namespace fe::features

namespace fe::phob_menu {
View view() {
  using namespace fe::features;
  std::lock_guard<std::mutex> lock(g_menu_mutex);
  View v;
  v.open = g_menu.open; v.just_opened = g_menu.just_opened; g_menu.just_opened = false;
  v.scopes_only = g_menu.scopes_only; v.port = g_menu.port;
  v.buttons = g_menu.buttons; v.stick_x = g_menu.sx; v.stick_y = g_menu.sy; v.armed = g_menu.armed;
  return v;
}
bool is_open() { std::lock_guard<std::mutex> lock(fe::features::g_menu_mutex); return fe::features::g_menu.open; }
void result(bool back_to_panel, bool close_all, bool changed) {
  using namespace fe::features;
  std::lock_guard<std::mutex> lock(g_menu_mutex);
  if (!g_menu.open) return;
  if (back_to_panel) g_menu.back_asked = true;
  else if (close_all) g_menu.close_asked = true;
  (void)changed;   // the menu saved the port's file itself (phob_store_save)
}
}  // namespace fe::phob_menu
