// melee-unlocked's PhobGCC section of the CONTROLS page (its gx/phob_menu.cpp) on the console: the settings of each GameCube
// adapter port with the firmware's names, ranges and defaults (host/phob.h), kept in <app folder>/PhobGCC/port<N>.ini, and the
// stick calibration and notch fix run with the controller's own buttons, the steps on the overlay (src/phob_adapter.cpp does
// the processing on the adapter's reader thread, frontend/patches/0123). Not here: upstream's PhobVision screens (the drawn
// stick maps, the scope plots, the reaction test), the named setups and the remap editor; the remap is read from the file.
// Inputs: a port's settings change only that controller's own input, as the firmware on a PhobGCC board would.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "phob_adapter.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace fe::features {
namespace {
namespace ph = host::phob;
std::atomic<int> g_port{0};
std::mutex g_line_mutex;
std::string g_last_line;
uint32_t g_last_until = 0;
std::atomic<uint32_t> g_now{0};

void tables(MuHostApi*, MuGameApi*) { if (options.play) phob_adapter::load(options.out_dir); }

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

void register_phob() {
  add_tables(tables);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"PhobGCC", "Adapter port", [] { return std::to_string(g_port.load() + 1); },
                  [](int d) { g_port.store((g_port.load() + (d < 0 ? 3 : 1)) % 4); }, nullptr, nullptr});
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
