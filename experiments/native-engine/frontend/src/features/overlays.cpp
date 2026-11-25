// On-screen overlays and picture options: the settings and the simulation-thread half. The drawing is
// src/overlay_draw.cpp (render thread, Aurora's ImGui pass); this module collects what it draws from what the game
// already hands its host and never writes anything into the game:
//   - the pads as the game got them this retrace (options.cpp's pad wrapper) for the input display;
//   - MuHostApi.hud_player and hud_combo (the game reports each drawn frame, display only, read-only on its side:
//     melee-unlocked sourceport/game/shim/mu_hud_scale.c mu_hud_report) for nicknames, the combo counter and the
//     L-cancel flash's place;
//   - MuGameApi.lcancel_view (a read of the fighters' landing state; upstream port/runtime/host/lcancel.cpp, the
//     indicator half) for the L-cancel flash;
//   - the state snapshot play mode passes each retrace (actions, frames, percent, speed) for the Training panel, and
//     Training's frame advantage (command 0xF4, training.cpp);
//   - the netplay host's answer to 0xB3 (the online match state) for the players' names;
//   - the option word the game reads (game_options2) for 20XX TE's input display bit.
// Upstream equivalents (melee-unlocked v1.0.5, read for behaviour; nothing copied): port/runtime/gx/training_overlay.cpp
// (input display, Training panel, TE input display), pc_settings.cpp (controller overlay, combo counter's counting,
// nicknames, performance graph, display latency test: gx_core.cpp latency_test_fill), host/lcancel.cpp.
// settings.ini keys: inputoverlay, inputoverlayvalues, inputoverlayports, inputoverlaystick, inputoverlayhideborder,
// showfps, performance, playernicknames, arcade_combo_counter, lcancel_flash_mode, lcancel_success_flash, latencytest,
// adapterrate; picture: aspect, brightness, contrast, vibrance, sharpness, crt_filter, crt_curve, borderart, borderedge,
// scale (upstream's names).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "overlay_state.h"
#include "looks_live.h"
#include "mu_lcancel_flash.h"   // melee-unlocked port/runtime/abi (the fetched tree): the flash modes and TE's word 2 bits

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>

namespace fe::features {
namespace {
bool lc_mode_given = false;              // lcancel_flash_mode was in settings.ini or changed on the menu
constexpr uint32_t kTrainingMajor = 0x1C;
std::mutex config_mutex;
overlay::Config cfg;          // under config_mutex
std::mutex snap_mutex;
overlay::Snapshot snap;       // under snap_mutex
std::atomic<bool> drawn{false};   // a drawer has asked for the snapshot (the Training panel is on screen)

// ---- settings ----
float to_float(const std::string& v, float lo, float hi, float fallback) {
  char* end = nullptr;
  const float f = std::strtof(v.c_str(), &end);
  return end && end != v.c_str() && std::isfinite(f) ? std::clamp(f, lo, hi) : fallback;
}
bool setting(const std::string& key, const std::string& value) {
  const int n = std::atoi(value.c_str());
  const bool on = value == "1";
  std::lock_guard<std::mutex> lock(config_mutex);
  overlay::Config& c = cfg;
  const uint32_t before = c.generation;
  if (key == "inputoverlay") c.input = on;
  else if (key == "inputoverlayvalues") c.input_values = on;
  else if (key == "inputoverlayhideborder") c.input_hide_border = on;
  else if (key == "inputoverlayports") c.input_ports = std::clamp(n, 0, 15);
  else if (key == "inputoverlayport") { if (n >= 0 && n < 4) c.input_ports = 1 << n; }
  else if (key == "inputoverlaystick") c.input_size = std::clamp(n, 1, 10);
  else if (key == "showfps") c.fps = on;
  else if (key == "performance") c.graph = on;
  else if (key == "playernicknames") c.nicknames = on;
  else if (key == "arcade_combo_counter") c.combo = on;
  else if (key == "lcancel_flash_mode") { c.lcancel = std::clamp(n, 0, 6); lc_mode_given = true; }
  else if (key == "lcancel_success_flash") c.lcancel_success = std::clamp(n, 0, 2);
  else if (key == "latencytest") c.latency_test = on;
  else if (key == "adapterrate") c.adapter_rate = on;
  else if (key == "aspect") { c.aspect = std::clamp(n, 0, 4); ++c.generation; }
  else if (key == "brightness") { c.brightness = to_float(value, 0.5f, 1.5f, 1.0f); ++c.generation; }
  else if (key == "contrast") { c.contrast = to_float(value, 0.5f, 1.5f, 1.0f); ++c.generation; }
  else if (key == "vibrance") { c.vibrance = to_float(value, 0.0f, 2.0f, 1.0f); ++c.generation; }
  else if (key == "sharpness") { c.sharpness = to_float(value, 0.0f, 1.0f, 0.0f); ++c.generation; }
  else if (key == "crt_filter") { c.crt = std::clamp(n, 0, 2); ++c.generation; }
  else if (key == "crt_curve") { c.crt_curve = to_float(value, 0.0f, 3.0f, 1.0f); ++c.generation; }
  else if (key == "borderart") { c.border = value == "-" || value == "0" ? std::string() : value; ++c.generation; }
  else if (key == "borderedge") { c.border_edge = on ? 1 : 0; ++c.generation; }
  else if (key == "scale") { c.scale = std::clamp(n, 0, 4); ++c.generation; }
  else if (key == "ssaa") { c.ssaa = std::clamp(n, 1, 4); ++c.generation; }
  else return false;
  (void)before;
  return true;
}

// ---- what the game reports (simulation thread) ----
struct Hud { bool present = false; int damage = 0, stocks = 0; float x = NAN, y = NAN; bool visible = false;
             int link = -1; float percent = 0; bool stunned = false; };
Hud hud[4];
void (*orig_hud_player)(int32_t, int32_t, int32_t, int32_t, float, float, int32_t) = nullptr;
void (*orig_hud_combo)(int32_t, int32_t, float, int32_t) = nullptr;
void hud_player(int32_t slot, int32_t present, int32_t damage, int32_t stocks, float x, float y, int32_t visible) {
  if (orig_hud_player) orig_hud_player(slot, present, damage, stocks, x, y, visible);
  if (slot < 0 || slot > 3) return;
  Hud& h = hud[slot];
  h.present = present && damage >= 0 && damage <= 999 && stocks >= 0 && stocks <= 99;
  h.damage = h.present ? damage : 0; h.stocks = h.present ? stocks : 0;
  h.x = x; h.y = y;
  h.visible = h.present && visible && std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 640 && y >= 0 && y <= 480;
}
void hud_combo(int32_t slot, int32_t link, float percent, int32_t stunned) {
  if (orig_hud_combo) orig_hud_combo(slot, link, percent, stunned);
  if (slot < 0 || slot > 3) return;
  hud[slot].link = link >= 0 && link < 4 && link != slot ? link : -1;
  hud[slot].percent = std::isfinite(percent) ? percent : 0.0f;
  hud[slot].stunned = stunned != 0;
}

// Players' names and this console's player from the netplay host's 0xB3 answer (prepare_online_match_state in
// port/runtime/hle/slippi_online.cpp): state, ready, ready, local index, remote index, rng (4), delay, three chat
// bytes, two ranks, then the local name and the four players' names, 31 bytes each in the game's Shift-JIS.
std::mutex names_mutex;
std::string names[4];
int local_player = -1;
std::string from_game_text(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i < n && p[i]; ++i) {
    const uint8_t b = p[i];
    if (b < 0x80) { out += char(b); continue; }
    if (i + 1 >= n) break;
    const unsigned code = unsigned(b) << 8 | p[i + 1];
    ++i;
    if (code >= 0x824F && code <= 0x8258) out += char('0' + (code - 0x824F));
    else if (code >= 0x8260 && code <= 0x8279) out += char('A' + (code - 0x8260));
    else if (code >= 0x8281 && code <= 0x829A) out += char('a' + (code - 0x8281));
    else if (code == 0x8140) out += ' ';
    else if (code == 0x8194) out += '#';
    else out += '?';
  }
  return out;
}
void reply(uint8_t command, const uint8_t* r, uint32_t size) {
  if (command != 0xB3 || size < 1) return;
  std::lock_guard<std::mutex> lock(names_mutex);
  if (r[0] == 4 && size >= 46 + 4 * 31) {          // CONNECTION_SUCCESS
    local_player = r[3] < 4 ? r[3] : -1;
    for (int i = 0; i < 4; ++i) names[i] = from_game_text(r + 46 + 31 * i, 31);
  } else if (r[0] == 0 || r[0] == 5) {             // idle, or an error: the match is over
    local_player = -1;
    for (auto& n : names) n.clear();
  }
}

void (*orig_tables_hud_player)() = nullptr;
void tables(MuHostApi* host, MuGameApi*) {
  if (!host) return;
  orig_hud_player = host->hud_player; host->hud_player = hud_player;
  orig_hud_combo = host->hud_combo; host->hud_combo = hud_combo;
}

// The combo counter's counting (upstream pc_settings.cpp combo_track, done here once per game frame): a hit is a rise
// of a player's percent, credited to the player whose combo link holds them (or the only opponent); the combo lives
// while the player hit keeps getting hit before they have been able to act for 45 frames, and ends at once when
// their percent drops (a KO).
struct Track { int attacker = -1; float percent = 0; int hits = 0, free_frames = 0; bool live = false; uint32_t changed = 0, ended = 0; };
Track tracks[4];
uint32_t last_match_frame = 0;
void count_combos(bool match, uint32_t match_frame, uint32_t now) {
  if (match_frame == last_match_frame) return;    // paused or between frames: nothing new
  last_match_frame = match_frame;
  for (int slot = 0; slot < 4; ++slot) {
    const Hud& p = hud[slot];
    Track& c = tracks[slot];
    if (!match || !p.present) { if (c.live) { c.live = false; c.ended = now; } c.percent = 0; continue; }
    if (p.percent > c.percent + 0.01f) {
      int attacker = -1, others = 0, other = -1;
      for (int a = 0; a < 4; ++a) {
        if (a == slot || !hud[a].present) continue;
        ++others; other = a;
        if (hud[a].link == slot) attacker = a;
      }
      if (attacker < 0) attacker = others == 1 ? other : c.attacker;
      if (c.live && attacker == c.attacker) ++c.hits; else { c.hits = 1; c.attacker = attacker; }
      c.live = attacker >= 0; c.free_frames = 0; c.changed = now;
    } else if (c.live) {
      c.free_frames = p.stunned ? 0 : c.free_frames + 1;
      if (c.free_frames >= 45 || p.percent + 0.01f < c.percent) { c.live = false; c.ended = now; }
    }
    c.percent = p.percent;
  }
}

// The L-cancel flash (upstream host/lcancel.cpp, the indicator): the retrace a human fighter enters a LandingAir state is
// the one the game tested frames-since-trigger against the window, and that byte has not moved since.
constexpr int32_t kLandingAirN = 70, kLandingAirLw = 74;
int32_t last_motion[4] = {-1, -1, -1, -1};
overlay::Flash flashes[4];
uint64_t flashes_made = 0, flashes_missed = 0;
void watch_lcancel(int mode, bool online, int local, uint32_t now) {
  MuGameApi* g = game();
  if (!mode || !g || !g->lcancel_view) { for (auto& m : last_motion) m = -1; return; }
  MuLcancelView view{};
  g->lcancel_view(&view);
  const int window = view.have_common && view.lcancel_window > 0 && view.lcancel_window <= 60 ? view.lcancel_window : 7;
  for (int port = 0; port < 4; ++port) {
    const MuLcancelFighter& f = view.port[port];
    if (!f.present || f.slot < 0 || f.slot > 3 || (online && f.slot != local)) { last_motion[port] = -1; continue; }
    if (f.motion_id >= kLandingAirN && f.motion_id <= kLandingAirLw && f.motion_id != last_motion[port]) {
      const bool success = f.frames_since_trigger < uint32_t(window);
      const bool report = success ? (mode == 5 || mode == 6) : (mode == 1 || mode == 6);
      (success ? flashes_made : flashes_missed) += 1;
      if (report) flashes[f.slot] = {true, success, now};
    }
    last_motion[port] = f.motion_id;
  }
}

// The flash on the fighter's model (upstream publish_tints: the slot's skinned model of the world camera tinted, not its
// shadow or effects): through the looks' renderer hook (looks_live.h flash, the bridge's tint in patches/0120). Upstream blends
// 85 % of the colour in for 6 frames and fades it over 4 more; the bridge's tint is solid (light and shade kept), so the model
// takes the colour for the 6 solid frames and 2 of the fade. Display only: the game is asked to mark draw owners while a
// renderer mode is chosen.
void lcancel_tints(const overlay::Config& c, uint32_t now) {
  auto& l = fe::looks::live();
  l.flash_on.store(mu_lcancel_renderer_mode(c.lcancel) != 0, std::memory_order_relaxed);
  for (int slot = 0; slot < 4; ++slot) {
    const overlay::Flash& f = flashes[slot];
    uint32_t rgb = 0;
    static const uint32_t frames = std::getenv("MU_TEST_FLASH_FRAMES") ? uint32_t(std::atoi(std::getenv("MU_TEST_FLASH_FRAMES"))) : 8u;   // tests: long enough for a screenshot
    if (f.on && now - f.start < frames && !(f.success && c.lcancel_success == 0))
      rgb = !f.success ? 0xFF1F1A : c.lcancel_success == 1 ? 0xFFFFFF : 0x1FFF2E;
    l.flash[slot].store(rgb ? 0x1000000u | rgb : 0u, std::memory_order_relaxed);
  }
}
// 20XX TE's own flash (modes 2..4, upstream mu_lcancel_with_flash_mode): the game draws it from TE's word 2 bits, offline with
// the TE save; the success colour from lcancel_success_flash. Only when lcancel_flash_mode was given or changed here, so TE's
// own menu keeps its choice otherwise. Picking a TE effect also turns TE's features on (word 1 bit 0x10), as upstream does.
void lcancel_te_word(const overlay::Config& c) {
  if (!lc_mode_given || !mods_te_owned()) return;
  uint32_t& w2 = settings_te_word(2);
  const bool te = c.lcancel >= MU_LCFLASH_TE_MISSED && c.lcancel <= MU_LCFLASH_TE_BOTH;
  uint32_t want = mu_lcancel_with_flash_mode(w2, c.lcancel);
  if (te) want = mu_lcancel_with_success_color(want, c.lcancel_success);
  if (want == w2 && (!te || (settings_te_word(1) & 0x10))) return;
  w2 = want;
  if (te) settings_te_word(1) |= 0x10;
  settings_state_changed();
  say("overlays: 20XX TE L-cancel flash %s (te_options2 %X)", te ? "on" : "off", w2);
}

// Display latency test (upstream gx_core.cpp latency_test_fill): from the pads the game read for this frame.
int fill = -1;
uint16_t held_before = 0;
void latency_test(bool on, const MuPadStatus* pads) {
  if (!on) { fill = -1; held_before = 0; return; }
  uint16_t held = 0, x_held = 0;
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err) continue;
    held |= pads[i].button;
    if (pads[i].button & 0x0400) x_held |= pads[i].button;   // X
  }
  const uint16_t pressed = held & uint16_t(~held_before);
  held_before = held;
  const int before = fill;
  if ((x_held & 0x0004) && (pressed & 0x0004)) fill = fill == 0 ? 1 : 0;   // D-pad Down: black, then flips
  else if ((x_held & 0x0008) && (pressed & 0x0008)) fill = -1;            // D-pad Up: the picture again
  if (fill != before) say("overlays: latency test: %s from retrace %u", fill < 0 ? "the picture" : fill ? "white" : "black", retrace());
}

// Retraces per second.
using Clock = std::chrono::steady_clock;
Clock::time_point fps_start = Clock::now();
uint32_t fps_count = 0;
float fps_value = 0;

void on_retrace(const Retrace& info) {
  const overlay::Config c = overlay_config();
  ++fps_count;
  const auto now = Clock::now();
  const double span = std::chrono::duration<double>(now - fps_start).count();
  if (span >= 0.5) { fps_value = float(fps_count / span); fps_count = 0; fps_start = now; }

  const MuStatePod* state = info.state;
  const bool match = state && state->match_frame != 0;
  const bool training = info.scene_major == kTrainingMajor;
  int local = -1;
  std::string player_names[4];
  {
    std::lock_guard<std::mutex> lock(names_mutex);
    local = local_player;
    for (int i = 0; i < 4; ++i) player_names[i] = info.online ? names[i] : std::string();
  }
  count_combos(match && !info.online, state ? state->match_frame : 0, info.retrace);
  watch_lcancel(match && mu_lcancel_renderer_mode(c.lcancel) ? c.lcancel : 0, info.online, local, info.retrace);
  for (auto& f : flashes) if (f.on && info.retrace - f.start > std::max<uint32_t>(10, std::getenv("MU_TEST_FLASH_FRAMES") ? uint32_t(std::atoi(std::getenv("MU_TEST_FLASH_FRAMES"))) : 0u)) f.on = false;
  lcancel_tints(c, info.retrace);
  lcancel_te_word(c);
  const MuPadStatus* pads = pads_out();
  latency_test(c.latency_test, pads);

  overlay::Snapshot s;
  s.valid = true;
  s.retrace = info.retrace;
  s.match = match;
  s.training = training;
  s.online = info.online;
  s.replay = replay_playing();
  s.local_player = local;
  for (int i = 0; i < 4; ++i) {
    overlay::Pad& p = s.pads[i];
    p.used = pads[i].err == 0;
    p.button = pads[i].button; p.sx = pads[i].stick_x; p.sy = pads[i].stick_y; p.cx = pads[i].sub_x; p.cy = pads[i].sub_y;
    p.l = pads[i].trigger_l; p.r = pads[i].trigger_r;
    overlay::Player& pl = s.players[i];
    const Hud& h = hud[i];
    pl.present = match && h.present;
    pl.damage = h.damage; pl.stocks = h.stocks; pl.tag_x = h.x; pl.tag_y = h.y; pl.tag_visible = match && h.visible;
    pl.name = player_names[i];
    if (state && state->player[i].present) {
      const MuFighterState& f = state->player[i];
      pl.fighter = true; pl.action = f.action;
      std::memcpy(&pl.anim_frame, &f.anim_frame, 4); std::memcpy(&pl.percent, &f.percent, 4);
      std::memcpy(&pl.vel_x, &f.vel_x, 4); std::memcpy(&pl.vel_y, &f.vel_y, 4);
    }
    const Track& t = tracks[i];
    s.combos[i] = {t.attacker, t.hits, t.live, t.changed, t.ended};
    s.flashes[i] = flashes[i];
  }
  s.lab_panel = training && !info.online && training_lab_overlay();
  s.practice_combo = training && !info.online && training_practice_combo();
  s.advantage = training_advantage(&s.advantage_frames, &s.advantage_kind, &s.advantage_retrace);
  const MuHostApi* h = host();
  s.te_inputs = !info.online && h && h->game_options2 && (h->game_options2() & MU_GAME_OPTION2_TE_INPUT_DISPLAY);
  s.latency_fill = fill;
  s.game_fps = fps_value;
  std::lock_guard<std::mutex> lock(snap_mutex);
  snap = std::move(s);
}

void scene_reset() {}
void run_end() {
  const overlay::Config c = overlay_config();
  if (c.lcancel) say("overlays: L-cancel flash saw %llu made and %llu missed L-cancels", (unsigned long long)flashes_made, (unsigned long long)flashes_missed);
}

// ---- menu rows (the Overlays and Picture pages) ----
using Write = MenuEntry::Write;
std::string on_off(bool v) { return v ? "On" : "Off"; }
template <typename F> void edit(F f) { std::lock_guard<std::mutex> lock(config_mutex); f(cfg); }
std::string fmt(float v) { char b[16]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
std::string percent(float v) { return std::to_string(int(std::lround(v * 100))) + "%"; }
std::string ports_text(int ports) {
  if (!ports) return "None";
  std::string out;
  for (int i = 0; i < 4; ++i) if (ports & (1 << i)) { if (!out.empty()) out += " "; out += "P" + std::to_string(i + 1); }
  return out;
}
// The quality presets (upstream's Video > Quality, the parts this renderer has): internal resolution, antialiasing and
// texture filtering (launcher.ini, at the next start), sharpening.
struct Preset { const char* name; int scale; int msaa; int aniso; float sharp; };
constexpr Preset kPresets[] = {
  {"Low", 1, 1, 1, 0.0f},        // the console's 640x480, no antialiasing, no anisotropic filtering
  {"Medium", 2, 1, 4, 0.0f},     // twice the console's resolution, 4x filtering
  {"High", 0, 1, 16, 0.0f},      // the display's resolution (Auto), 16x filtering: the app's default
  {"Ultra", 0, 4, 16, 0.3f},     // High with 4x MSAA and a little sharpening
};
int current_preset() {
  const overlay::Config c = overlay_config();
  const int msaa = std::atoi(app_ini("msaa", "1").c_str()), aniso = std::atoi(app_ini("anisotropy", "16").c_str());
  for (int i = 0; i < int(sizeof kPresets / sizeof kPresets[0]); ++i) {
    const Preset& p = kPresets[i];
    if (c.scale == p.scale && msaa == p.msaa && aniso == p.aniso && std::fabs(c.sharpness - p.sharp) < 0.01f) return i;
  }
  return -1;
}
}  // namespace

overlay::Config overlay_config() { std::lock_guard<std::mutex> lock(config_mutex); return cfg; }
void overlay_snapshot(overlay::Snapshot& out) {
  drawn.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(snap_mutex);
  out = snap;
}
bool overlay_draws_training() { return drawn.load(std::memory_order_relaxed); }
void overlay_names(std::string out[4]) {
  std::lock_guard<std::mutex> lock(names_mutex);
  for (int i = 0; i < 4; ++i) out[i] = online() ? names[i] : std::string();
}
// The Borders folder's pictures and videos (upstream's border_files: PNG, JPG, BMP, WebP and MP4), sorted by name.
static std::vector<std::string> border_files() {
  std::vector<std::string> out;
  const std::string folder = (options.app && !options.base_dir.empty() ? options.base_dir : options.out_dir) + "/Borders";
  if (DIR* dir = opendir(folder.c_str())) {
    while (const dirent* e = readdir(dir)) {
      std::string name = e->d_name, ext;
      const size_t dot = name.rfind('.');
      if (name.empty() || name[0] == '.' || dot == std::string::npos) continue;
      for (char ch : name.substr(dot + 1)) ext += char(std::tolower(static_cast<unsigned char>(ch)));
      if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "webp" || ext == "mp4") out.push_back(name);
    }
    closedir(dir);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void register_overlays() {
  add_setting(setting);
  add_tables(tables);
  add_retrace(on_retrace);
  add_reply(reply);
  add_run_end(run_end);
  (void)scene_reset; (void)orig_tables_hud_player;
  // Overlays page.
  add_menu_entry({"Overlays", "Controller input display", [] { return on_off(overlay_config().input); }, [](int) { edit([](overlay::Config& c) { c.input = !c.input; }); }, nullptr,
                  [](Write w) { w("inputoverlay", overlay_config().input ? "1" : "0"); }});
  add_menu_entry({"Overlays", "  Ports shown", [] { return ports_text(overlay_config().input_ports); },
                  [](int d) { edit([d](overlay::Config& c) { c.input_ports = ((c.input_ports + (d < 0 ? 15 : 1)) % 16); }); }, nullptr,
                  [](Write w) { w("inputoverlayports", std::to_string(overlay_config().input_ports)); }});
  add_menu_entry({"Overlays", "  Show values", [] { return on_off(overlay_config().input_values); }, [](int) { edit([](overlay::Config& c) { c.input_values = !c.input_values; }); }, nullptr,
                  [](Write w) { w("inputoverlayvalues", overlay_config().input_values ? "1" : "0"); }});
  add_menu_entry({"Overlays", "  Size", [] { return std::to_string(overlay_config().input_size); },
                  [](int d) { edit([d](overlay::Config& c) { c.input_size = std::clamp(c.input_size + d, 1, 10); }); }, nullptr,
                  [](Write w) { w("inputoverlaystick", std::to_string(overlay_config().input_size)); }});
  add_menu_entry({"Overlays", "  No border", [] { return on_off(overlay_config().input_hide_border); }, [](int) { edit([](overlay::Config& c) { c.input_hide_border = !c.input_hide_border; }); }, nullptr,
                  [](Write w) { w("inputoverlayhideborder", overlay_config().input_hide_border ? "1" : "0"); }});
  add_menu_entry({"Overlays", "Frame counter", [] { return on_off(overlay_config().fps); }, [](int) { edit([](overlay::Config& c) { c.fps = !c.fps; }); }, nullptr,
                  [](Write w) { w("showfps", overlay_config().fps ? "1" : "0"); }});
  add_menu_entry({"Overlays", "Performance graph", [] { return on_off(overlay_config().graph); }, [](int) { edit([](overlay::Config& c) { c.graph = !c.graph; }); }, nullptr,
                  [](Write w) { w("performance", overlay_config().graph ? "1" : "0"); }});
  add_menu_entry({"Overlays", "Player names above fighters (online)", [] { return on_off(overlay_config().nicknames); }, [](int) { edit([](overlay::Config& c) { c.nicknames = !c.nicknames; }); }, nullptr,
                  [](Write w) { w("playernicknames", overlay_config().nicknames ? "1" : "0"); }});
  add_menu_entry({"Overlays", "Combo counter (offline)", [] { return on_off(overlay_config().combo); }, [](int) { edit([](overlay::Config& c) { c.combo = !c.combo; }); }, nullptr,
                  [](Write w) { w("arcade_combo_counter", overlay_config().combo ? "1" : "0"); }});
  add_menu_entry({"Overlays", "L-cancel flash",
                  [] { static const char* names[] = {"Off", "MU: missed (red)", "TE: missed (red)", "TE: success", "TE: both", "MU: success (green)", "MU: both (red / green)"};
                       return std::string(names[std::clamp(overlay_config().lcancel, 0, 6)]); },
                  // upstream's order; the TE effects only with the 20XX TE save loaded
                  [](int d) { edit([d](overlay::Config& c) {
                    std::vector<int> order = {0, 1, 5, 6};
                    if (mods_te_owned()) order.insert(order.end(), {2, 3, 4});
                    size_t i = 0; while (i < order.size() && order[i] != c.lcancel) ++i;
                    const size_t n = order.size(); c.lcancel = order[((i % n) + (d < 0 ? n - 1 : 1)) % n]; lc_mode_given = true; }); }, nullptr,
                  [](Write w) { w("lcancel_flash_mode", std::to_string(overlay_config().lcancel)); }});
  add_menu_entry({"Overlays", "  Made L-cancel colour",
                  [] { const int m = overlay_config().lcancel_success; return std::string(m == 1 ? "White" : m == 2 ? "Green" : "None"); },
                  [](int d) { edit([d](overlay::Config& c) { c.lcancel_success = (c.lcancel_success + (d < 0 ? 2 : 1)) % 3; }); }, nullptr,
                  [](Write w) { w("lcancel_success_flash", std::to_string(overlay_config().lcancel_success)); }});
  add_menu_entry({"Overlays", "Display latency test (X + D-pad Down)", [] { return on_off(overlay_config().latency_test); }, [](int) { edit([](overlay::Config& c) { c.latency_test = !c.latency_test; }); }, nullptr,
                  [](Write w) { w("latencytest", overlay_config().latency_test ? "1" : "0"); }});
  add_menu_entry({"Overlays", "GameCube adapter report rate", [] { return on_off(overlay_config().adapter_rate); }, [](int) { edit([](overlay::Config& c) { c.adapter_rate = !c.adapter_rate; }); }, nullptr,
                  [](Write w) { w("adapterrate", overlay_config().adapter_rate ? "1" : "0"); }});
  // Picture page.
  add_menu_entry({"Picture", "Quality preset",
                  [] { const int p = current_preset(); return std::string(p >= 0 ? kPresets[p].name : "Custom"); },
                  [](int d) {
                    const int count = int(sizeof kPresets / sizeof kPresets[0]);
                    const int now = current_preset();
                    const int next = now < 0 ? (d < 0 ? count - 1 : 0) : ((now + (d < 0 ? count - 1 : 1)) % count);
                    const Preset& p = kPresets[next];
                    edit([&](overlay::Config& c) { c.scale = p.scale; c.sharpness = p.sharp; ++c.generation; });
                    app_ini_set("msaa", std::to_string(p.msaa));
                    app_ini_set("anisotropy", std::to_string(p.aniso));
                  }, nullptr,
                  [](Write w) { const overlay::Config c = overlay_config(); w("scale", std::to_string(c.scale)); w("sharpness", fmt(c.sharpness)); }});
  add_menu_entry({"Picture", "Picture shape",
                  [] { static const char* const n[] = {"Auto", "73:60 (console pixels)", "4:3", "16:9", "Stretched"}; return std::string(n[std::clamp(overlay_config().aspect, 0, 4)]); },
                  [](int d) { edit([d](overlay::Config& c) { c.aspect = (c.aspect + (d < 0 ? 4 : 1)) % 5; ++c.generation; }); }, nullptr,
                  [](Write w) { w("aspect", std::to_string(overlay_config().aspect)); }});
  add_menu_entry({"Picture", "Internal resolution",
                  [] { const int s = overlay_config().scale; return s == 0 ? std::string("Auto (the display)") : std::to_string(640 * s) + "x" + std::to_string(480 * s); },
                  [](int d) { edit([d](overlay::Config& c) { c.scale = (c.scale + (d < 0 ? 4 : 1)) % 5; ++c.generation; }); }, nullptr,
                  [](Write w) { w("scale", std::to_string(overlay_config().scale)); }});
  add_menu_entry({"Picture", "Supersampling (SSAA)",
                  [] { const int a = overlay_config().ssaa; return a <= 1 ? std::string("Off") : std::to_string(a * a) + "x (heavy on the console)"; },
                  [](int d) { edit([d](overlay::Config& c) { c.ssaa = std::clamp(c.ssaa + (d < 0 ? -1 : 1), 1, 4); ++c.generation; }); }, nullptr,
                  [](Write w) { w("ssaa", std::to_string(overlay_config().ssaa)); }});
  add_menu_entry({"Picture", "Brightness", [] { return percent(overlay_config().brightness); },
                  [](int d) { edit([d](overlay::Config& c) { c.brightness = std::clamp(std::round((c.brightness + 0.05f * d) * 20.0f) / 20.0f, 0.5f, 1.5f); ++c.generation; }); }, nullptr,
                  [](Write w) { w("brightness", fmt(overlay_config().brightness)); }});
  add_menu_entry({"Picture", "Contrast", [] { return percent(overlay_config().contrast); },
                  [](int d) { edit([d](overlay::Config& c) { c.contrast = std::clamp(std::round((c.contrast + 0.05f * d) * 20.0f) / 20.0f, 0.5f, 1.5f); ++c.generation; }); }, nullptr,
                  [](Write w) { w("contrast", fmt(overlay_config().contrast)); }});
  add_menu_entry({"Picture", "Vibrance", [] { return percent(overlay_config().vibrance); },
                  [](int d) { edit([d](overlay::Config& c) { c.vibrance = std::clamp(std::round((c.vibrance + 0.1f * d) * 10.0f) / 10.0f, 0.0f, 2.0f); ++c.generation; }); }, nullptr,
                  [](Write w) { w("vibrance", fmt(overlay_config().vibrance)); }});
  add_menu_entry({"Picture", "Sharpening", [] { return percent(overlay_config().sharpness); },
                  [](int d) { edit([d](overlay::Config& c) { c.sharpness = std::clamp(std::round((c.sharpness + 0.1f * d) * 10.0f) / 10.0f, 0.0f, 1.0f); ++c.generation; }); }, nullptr,
                  [](Write w) { w("sharpness", fmt(overlay_config().sharpness)); }});
  add_menu_entry({"Picture", "CRT look",
                  [] { static const char* const n[] = {"Off", "Studio monitor", "Home television"}; return std::string(n[std::clamp(overlay_config().crt, 0, 2)]); },
                  [](int d) { edit([d](overlay::Config& c) { c.crt = (c.crt + (d < 0 ? 2 : 1)) % 3; ++c.generation; }); }, nullptr,
                  [](Write w) { w("crt_filter", std::to_string(overlay_config().crt)); }});
  add_menu_entry({"Picture", "  Screen curve (television)", [] { return percent(overlay_config().crt_curve); },
                  [](int d) { edit([d](overlay::Config& c) { c.crt_curve = std::clamp(std::round((c.crt_curve + 0.25f * d) * 4.0f) / 4.0f, 0.0f, 3.0f); ++c.generation; }); }, nullptr,
                  [](Write w) { w("crt_curve", fmt(overlay_config().crt_curve)); }});
  add_menu_entry({"Picture", "Border art",
                  [] { const std::string b = overlay_config().border; return b.empty() ? std::string("None") : b == "grid" ? std::string("Grid") : b; },
                  [](int d) {
                    // None, Grid, then each picture or video in the Borders folder (upstream's border_files), in name order.
                    std::vector<std::string> choices{std::string(), "grid"};
                    for (const std::string& f : border_files()) choices.push_back(f);
                    edit([&](overlay::Config& c) {
                      size_t at = 0;
                      for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == c.border) at = i;
                      c.border = choices[(at + choices.size() + (d < 0 ? -1 : 1)) % choices.size()]; ++c.generation;
                    });
                  }, nullptr,
                  [](Write w) { const std::string b = overlay_config().border; w("borderart", b.empty() ? "-" : b); }});
  add_menu_entry({"Picture", "  Shadow at the picture's edge", [] { return on_off(overlay_config().border_edge != 0); },
                  [](int) { edit([](overlay::Config& c) { c.border_edge = c.border_edge ? 0 : 1; ++c.generation; }); }, nullptr,
                  [](Write w) { w("borderedge", overlay_config().border_edge ? "1" : "0"); }});
}
}  // namespace fe::features
