// The controller setup beyond the app's button remap, on the console's two kinds of controller: SDL gamepads
// (Pro Controller, Joy-Con, pads the system accepts) and GameCube controllers on the adapter. What it does is in
// controls_core.h (bindings for every GameCube action, box mode, inversion, radial deadzone, radius, snapback,
// trigger click point and cap) and controls_files.h (profiles, Dolphin import); this file reads the devices, keeps
// one setup per device (launcher.ini padx.<SDL GUID> or padx.gc<port>), draws three menu pages, captures bindings,
// watches for melee-unlocked's Start + D-pad + Z chords and measures the Pro Controller / Joy-Con report rate.
//
// What the game gets changes only for a device whose setup differs from its defaults, and only that device's pad:
// the same as the player pressing other buttons. Online play is unaffected beyond that (the game sends the pad).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host.h"                // shim: PadState, gcadapter_status, now_ns
#include "host_services.h"       // melee-unlocked: linux_host::state_snapshot
#include "mu_host.h"
#include "app/game_menu.h"       // MenuExtraPage, menu_toggle, menu_open_extra (patches/0104, 0111)
#include "input_config.h"
#include "controls_core.h"
#include "controls_files.h"
#include "settings_chord.h"      // melee-unlocked port/runtime/host/settings_chord.h (the fetched tree)
#include <SDL3/SDL.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
namespace {
using namespace controls;

struct Device { std::string id, name; Kind kind = Kind::Sdl; int port = -1; };

std::mutex mu;                                   // everything below
std::map<std::string, Config> configs;           // by device id; only setups that differ from the defaults
std::map<std::string, State> states[2];          // per reader lane
std::atomic<bool> any_active{false};
std::vector<Device> devices;
size_t selected = 0;
size_t profile_pick = 0, dolphin_pick = 0;
std::vector<std::string> profile_names;
std::vector<DolphinEntry> dolphin_entries;
std::string status = "";
bool delete_armed = false;
struct Capture { bool active = false, armed = false, done = false; std::string id; Kind kind = Kind::Sdl; int port = -1, action = 0; uint32_t frames = 0; } capture;

std::string profiles_folder() { return options.base_dir + "/ControllerProfiles"; }
std::string dolphin_folder() { return options.base_dir + "/Dolphin"; }

uint32_t test_adapter_ports() {
  static const uint32_t bits = [] { const char* v = std::getenv("MU_CONTROLS_TEST_ADAPTER"); return v ? uint32_t(std::strtoul(v, nullptr, 0)) & 15u : 0u; }();
  return bits;
}
uint32_t adapter_ports() {
  host::GcAdapterStatus st{};
  return (host::gcadapter_status(st) ? st.ports : 0u) | test_adapter_ports();
}
void update_active_locked() {
  bool any = false;
  for (const auto& [id, c] : configs) any = any || !c.is_default();
  any_active.store(any, std::memory_order_relaxed);
}
Config config_locked(const Device& d) {
  const auto it = configs.find(d.id);
  return it == configs.end() ? Config::defaults(d.kind) : it->second;
}
void store_locked(const Device& d, const Config& c) {
  if (c.is_default()) configs.erase(d.id); else configs[d.id] = c;
  update_active_locked();
}

SDL_Gamepad* sdl_by_guid(const std::string& guid) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  SDL_Gamepad* found = nullptr;
  for (int i = 0; ids && i < count && !found; ++i) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (!pad) continue;
    char text[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), text, sizeof text);
    if (guid == text) found = pad;
  }
  SDL_free(ids);
  return found;
}
uint32_t sdl_buttons(SDL_Gamepad* pad) {
  uint32_t bits = 0;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 28; ++b) if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) bits |= 1u << b;
  return bits;
}

// The devices the pages offer: SDL gamepads, then each adapter port with a controller. Simulation thread.
void refresh_devices_locked(bool files = true) {
  const std::string keep = selected < devices.size() ? devices[selected].id : std::string();
  const Kind kept_kind = selected < devices.size() ? devices[selected].kind : Kind::Sdl;
  devices.clear();
  for (const host::ControllerInfo& c : host::window_list_controllers()) devices.push_back({c.guid, c.name, Kind::Sdl, -1});
  const uint32_t ports = adapter_ports();
  for (int p = 0; p < 4; ++p)
    if (ports & (1u << p)) devices.push_back({"gc" + std::to_string(p + 1), "GameCube controller, adapter port " + std::to_string(p + 1), Kind::Adapter, p});
  selected = 0;
  for (size_t i = 0; i < devices.size(); ++i) if (devices[i].id == keep) selected = i;
  const Kind kind = devices.empty() ? Kind::Sdl : devices[selected].kind;
  if (!files && kind == kept_kind) return;
  profile_names = profile_list(profiles_folder(), kind);
  if (profile_pick >= profile_names.size()) profile_pick = 0;
  dolphin_entries = dolphin_list(dolphin_folder());
  if (dolphin_pick >= dolphin_entries.size()) dolphin_pick = 0;
}
const Device* device_locked() { return selected < devices.size() ? &devices[selected] : nullptr; }

// ---- names ----
const char* sdl_button_name(int b) {
  static const char* const names[] = {"A position (bottom face)", "Right face button", "Left face button", "Top face button", "Minus / Back",
      "Home", "Plus / Start", "Left stick press", "Right stick press", "L / left shoulder", "R / right shoulder", "D-pad up", "D-pad down",
      "D-pad left", "D-pad right", "Capture / Misc", "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2", "Touchpad"};
  return b >= 0 && b < int(sizeof names / sizeof *names) ? names[b] : "button";
}
std::string source_name(Kind kind, uint32_t mask, uint8_t level) {
  if (!mask) return "Not bound";
  std::string out;
  auto add = [&](const std::string& s) { out += out.empty() ? s : " + " + s; };
  if (kind == Kind::Sdl) {
    if (mask & kSrcLT) add("ZL / left trigger");
    if (mask & kSrcRT) add("ZR / right trigger");
    for (int b = 0; b < 28; ++b) if (mask & (1u << b)) add(sdl_button_name(b));
  } else {
    static const char* const gc[16] = {"D-left", "D-right", "D-down", "D-up", "Z", "R", "L", "?", "A", "B", "X", "Y", "Start", "?", "?", "?"};
    for (int b = 0; b < 16; ++b) if (mask & (1u << b)) add(gc[b]);
  }
  if (level) out += " (" + std::to_string(level) + ")";
  return out;
}

// ---- per-poll ----
double now_s() { return double(host::now_ns()) * 1e-9; }
}  // namespace

bool controls_active() { return any_active.load(std::memory_order_relaxed); }
uint32_t controls_test_adapter_ports() { return test_adapter_ports(); }

void controls_apply_adapter(int port, void* pad_v, int lane) {
  if (!any_active.load(std::memory_order_relaxed) || port < 0 || port > 3) return;
  auto& p = *static_cast<host::PadState*>(pad_v);
  std::lock_guard<std::mutex> lock(mu);
  const auto it = configs.find("gc" + std::to_string(port + 1));
  if (it == configs.end()) return;
  Raw raw;
  raw.buttons = p.button; raw.lt = p.trig_l; raw.rt = p.trig_r;
  Pad g{p.button, p.stick_x, p.stick_y, p.sub_x, p.sub_y, p.trig_l, p.trig_r};
  apply(it->second, raw, kDefaultTriggerClick, now_s(), states[lane & 1][it->first], g);
  p.button = g.button; p.stick_x = g.stick_x; p.stick_y = g.stick_y; p.sub_x = g.sub_x; p.sub_y = g.sub_y; p.trig_l = g.trig_l; p.trig_r = g.trig_r;
}

void controls_apply_sdl(const std::string& guid, void* sdl, const void* before_v, void* pad_v, int trigger_press, int lane) {
  if (!any_active.load(std::memory_order_relaxed)) return;
  auto& p = *static_cast<host::PadState*>(pad_v);
  const auto& before = *static_cast<const host::PadState*>(before_v);
  std::lock_guard<std::mutex> lock(mu);
  const auto it = configs.find(guid);
  if (it == configs.end()) return;
  Raw raw;
  raw.buttons = sdl_buttons(static_cast<SDL_Gamepad*>(sdl));
  raw.lt = before.trig_l; raw.rt = before.trig_r;   // the triggers as Aurora's PAD gave them, before the app's remap
  Pad g{p.button, p.stick_x, p.stick_y, p.sub_x, p.sub_y, p.trig_l, p.trig_r};
  apply(it->second, raw, std::max(1, trigger_press * 255 / 100), now_s(), states[lane & 1][guid], g);
  p.button = g.button; p.stick_x = g.stick_x; p.stick_y = g.stick_y; p.sub_x = g.sub_x; p.sub_y = g.sub_y; p.trig_l = g.trig_l; p.trig_r = g.trig_r;
}

// A binding row's A button starts a capture: the next control pressed on that device (after everything was let
// go) is bound; six seconds without one keep the binding. The menu gets neutral pads meanwhile, so the press that
// is being bound does not also move the menu.
bool controls_capture_frame(const void* raw_v) {
  const auto* raw = static_cast<const host::PadState*>(raw_v);
  std::lock_guard<std::mutex> lock(mu);
  static bool was_open = false;
  static uint32_t polls = 0;
  const bool open = host::menu_is_open();
  if (open && !was_open) refresh_devices_locked(true);                 // the menu opened: devices, profiles, Dolphin files
  else if (open && !capture.active && ++polls % 60 == 0) refresh_devices_locked(false);   // a controller plugged in meanwhile
  was_open = open;
  if (!capture.active) return false;
  if (!open) { capture.active = false; return false; }
  uint32_t sources = 0;
  if (capture.kind == Kind::Sdl) {
    SDL_Gamepad* pad = sdl_by_guid(capture.id);
    if (!pad) { capture.active = false; status = "The controller went away"; return true; }
    sources = sdl_buttons(pad);
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) sources |= kSrcLT;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) sources |= kSrcRT;
  } else if (capture.port >= 0 && raw[capture.port].err == 0) {
    const host::PadState& r = raw[capture.port];
    sources = r.button & 0x1F7Fu;
    if (is_analog_action(capture.action)) {   // the trigger's travel: half way in is enough to choose it
      if (r.trig_l > 100) sources |= kPadL;
      if (r.trig_r > 100) sources |= kPadR;
    }
  }
  ++capture.frames;
  // After a binding the menu waits until the control is let go: it would otherwise read it through the new binding.
  // The same after a change on the setup page (inverting the stick the menu is being driven with would flip it back on the
  // next poll): until every control and stick is let go.
  if (capture.done) {
    bool sticks = false;
    for (int i = 0; i < 4; ++i)
      if (raw[i].err == 0) sticks = sticks || std::abs(raw[i].stick_x) > 40 || std::abs(raw[i].stick_y) > 40 || std::abs(raw[i].sub_x) > 40 || std::abs(raw[i].sub_y) > 40;
    if ((!sources && !sticks) || capture.frames > 600) capture.active = false;
    return true;
  }
  if (!capture.armed) { if (!sources) capture.armed = true; }
  else if (sources) {
    const uint32_t pick = sources & (~sources + 1);   // the lowest one
    for (const Device& d : devices) {
      if (d.id != capture.id) continue;
      Config c = config_locked(d);
      c.mask[capture.action] = pick;
      c.level[capture.action] = 0;
      store_locked(d, c);
      status = std::string(kActionLabels[capture.action]) + ": " + source_name(d.kind, pick, 0);
      say("controls: %s: %s", d.name.c_str(), status.c_str());
    }
    capture.done = true;
    return true;
  }
  if (capture.frames > 360 && !capture.done) { capture.active = false; status = "Nothing pressed: binding kept"; }
  return true;
}

// melee-unlocked's chords (settings_chord.h, compiled from the fetched tree): Start + D-pad Down + Z opens or closes
// the menu, Start + D-pad Up + Z opens it on the Training page (upstream's Practice panel). While a chord is held its
// buttons are kept from the game. In offline Training a lone Start waits up to three polls for the rest of a chord
// (upstream hold_lone_start), so the pause menu does not open under the Training page.
void controls_chords(void* pads_v) {
  auto* pads = static_cast<host::PadState*>(pads_v);
  static bool settings_down[4] = {}, practice_down[4] = {};
  static int start_waited[4] = {};
  MuStatePod state{};
  linux_host::state_snapshot(&state);
  const bool training = state.scene_major == 0x1C && !session_online();
  for (int i = 0; i < 4; ++i) {
    const bool connected = pads[i].err == 0;
    const uint16_t raw = connected ? pads[i].button : 0;
    const bool settings = host::settings_shortcut::poll_z_start(connected, pads[i].button, settings_down[i]);
    const bool practice = host::settings_shortcut::poll_practice(connected, pads[i].button, practice_down[i]);
    if (settings) {
      static const char* test_page = std::getenv("MU_CONTROLS_TEST_PAGE");   // tests: the chord opens this page instead
      if (host::menu_is_open()) host::menu_close();
      else if (test_page && *test_page) host::menu_open_extra(test_page);
      else host::menu_toggle();
      say("controls: Start + D-pad Down + Z on port %d: the menu", i + 1);
    } else if (practice) {
      if (host::menu_is_open()) host::menu_close(); else host::menu_open_extra("Training");
      say("controls: Start + D-pad Up + Z on port %d: the Training page", i + 1);
    }
    if (connected) host::settings_shortcut::hold_lone_start(training && !host::menu_is_open(), settings_down[i] || practice_down[i], raw, pads[i].button, start_waited[i]);
  }
}

void controls_saved(void (*put)(const std::string&, const std::string&)) {
  std::lock_guard<std::mutex> lock(mu);
  for (const auto& [id, c] : configs) put(id, c.serialize());
}
void controls_load(const std::string& id, const std::string& text) {
  std::lock_guard<std::mutex> lock(mu);
  const Kind kind = id.size() == 3 && id[0] == 'g' && id[1] == 'c' ? Kind::Adapter : Kind::Sdl;
  const Config c = Config::parse(kind, text);
  if (!c.is_default()) configs[id] = c;
  update_active_locked();
}

// ---- profiles: the app's ControllerMap carries an SDL pad's twelve buttons ----
namespace {
uint32_t app_input_to_mask(int input) {
  if (input == host::kTriggerLeft) return kSrcLT;
  if (input == host::kTriggerRight) return kSrcRT;
  return input >= 0 && input < 28 ? 1u << input : 0u;
}
int mask_to_app_input(uint32_t mask) {
  if (mask & kSrcLT) return host::kTriggerLeft;
  if (mask & kSrcRT) return host::kTriggerRight;
  for (int b = 0; b < 28; ++b) if (mask & (1u << b)) return b;
  return host::kUnbound;
}
// The app map's control for each of the twelve button actions (Action order A..DRight).
constexpr host::GcControl kAppControl[12] = {host::GC_CTL_A, host::GC_CTL_B, host::GC_CTL_X, host::GC_CTL_Y, host::GC_CTL_Z, host::GC_CTL_START,
    host::GC_CTL_L, host::GC_CTL_R, host::GC_CTL_DUP, host::GC_CTL_DDOWN, host::GC_CTL_DLEFT, host::GC_CTL_DRIGHT};
host::ControllerConfig app_config(const std::string& guid) {
  host::ControllerConfig c;
  if (const host::ControllerConfig* e = host::controller_config_for(guid)) c = *e;
  c.guid = guid;
  return c;
}
Profile profile_of_locked(const Device& d) {
  Profile p;
  p.config = config_locked(d);
  if (d.kind == Kind::Sdl) {
    const host::ControllerConfig a = app_config(d.id);
    for (int i = 0; i < 12; ++i) p.config.mask[i] = app_input_to_mask(a.map.binding[kAppControl[i]]);
    p.has_app = true;
    p.stick_deadzone = a.map.stick_deadzone; p.cstick_deadzone = a.map.cstick_deadzone; p.trigger_press = a.map.trigger_press;
    p.swap_sticks = a.map.swap_sticks; p.rumble = a.map.rumble;
  }
  return p;
}
void apply_profile_locked(const Device& d, Profile p) {
  if (d.kind == Kind::Sdl) {
    host::ControllerConfig a = app_config(d.id);
    for (int i = 0; i < 12; ++i) a.map.binding[kAppControl[i]] = mask_to_app_input(p.config.mask[i]);
    if (p.has_app) {
      a.map.stick_deadzone = p.stick_deadzone; a.map.cstick_deadzone = p.cstick_deadzone; a.map.trigger_press = p.trigger_press;
      a.map.swap_sticks = p.swap_sticks; a.map.rumble = p.rumble;
    }
    host::upsert_controller_config(a);
    for (int i = 0; i < 12; ++i) p.config.mask[i] = 0;   // kept in the app's map, not here
  }
  p.config.kind = d.kind;
  store_locked(d, p.config);
}
}  // namespace

// ---- menu pages ----
namespace {
enum SetupRow { S_DEVICE, S_MODE, S_INV_SX, S_INV_SY, S_INV_CX, S_INV_CY, S_RADIUS, S_RADIUS_C, S_MODIFIER, S_DZ, S_DZ_C, S_SNAP_X, S_SNAP_Y,
                S_SNAP_MS, S_CLICK_L, S_CLICK_R, S_CAP_L, S_CAP_R, S_RESET_ANALOG, S_COUNT };
const char* const kSetupLabels[S_COUNT] = {"Controller", "Stick mode", "Invert stick left/right", "Invert stick up/down", "Invert C-stick left/right",
    "Invert C-stick up/down", "Stick radius", "C-stick radius", "Stick modifier push", "Stick deadzone (round)", "C-stick deadzone (round)",
    "Snapback filter, left/right", "Snapback filter, up/down", "Snapback filter window", "L click point", "R click point", "L trigger cap",
    "R trigger cap", "Reset these to defaults"};
bool setup_row_shown(int r, Kind kind) {
  if (kind == Kind::Adapter) return r != S_SNAP_X && r != S_SNAP_Y && r != S_SNAP_MS;   // upstream: no snapback filter on a GameCube controller
  return r != S_CLICK_L && r != S_CLICK_R;   // an SDL pad's press point is the Controls page's trigger row
}
std::vector<int> setup_rows_locked() {
  std::vector<int> rows;
  const Kind kind = device_locked() ? device_locked()->kind : Kind::Sdl;
  for (int r = 0; r < S_COUNT; ++r) if (setup_row_shown(r, kind)) rows.push_back(r);
  return rows;
}
std::string device_value_locked() {
  const Device* d = device_locked();
  if (!d) return "None connected";
  return d->name + (devices.size() > 1 ? "  (" + std::to_string(selected + 1) + "/" + std::to_string(devices.size()) + ")" : "");
}
std::string setup_value_locked(int r) {
  const Device* d = device_locked();
  if (r == S_DEVICE) return device_value_locked();
  if (!d) return "-";
  const Config c = config_locked(*d);
  char b[64];
  auto onoff = [](bool v) { return std::string(v ? "On" : "Off"); };
  switch (r) {
    case S_MODE: return c.box_mode() ? "Box (B0XX coordinates)" : "Standard";
    case S_INV_SX: return onoff(c.invert & kInvertStickX);
    case S_INV_SY: return onoff(c.invert & kInvertStickY);
    case S_INV_CX: return onoff(c.invert & kInvertCX);
    case S_INV_CY: return onoff(c.invert & kInvertCY);
    case S_RADIUS: std::snprintf(b, sizeof b, "%d%%", c.radius_main); return b;
    case S_RADIUS_C: std::snprintf(b, sizeof b, "%d%%", c.radius_c); return b;
    case S_MODIFIER: std::snprintf(b, sizeof b, "%d%%", c.modifier_percent); return b;
    case S_DZ: if (!c.dz_main) return "Off"; std::snprintf(b, sizeof b, "%d of 127", c.dz_main); return b;
    case S_DZ_C: if (!c.dz_c) return "Off"; std::snprintf(b, sizeof b, "%d of 127", c.dz_c); return b;
    case S_SNAP_X: if (!c.snap_x) return "Off"; std::snprintf(b, sizeof b, "%d of 127", c.snap_x); return b;
    case S_SNAP_Y: if (!c.snap_y) return "Off"; std::snprintf(b, sizeof b, "%d of 127", c.snap_y); return b;
    case S_SNAP_MS: std::snprintf(b, sizeof b, "%d ms", c.snap_ms_x); return b;
    case S_CLICK_L: if (c.click_l == kDefaultTriggerClick) return "The trigger's switch"; std::snprintf(b, sizeof b, "%d of 255", c.click_l); return b;
    case S_CLICK_R: if (c.click_r == kDefaultTriggerClick) return "The trigger's switch"; std::snprintf(b, sizeof b, "%d of 255", c.click_r); return b;
    case S_CAP_L: if (c.cap_l >= 255) return "Off"; std::snprintf(b, sizeof b, "%d (no click)", c.cap_l); return b;
    case S_CAP_R: if (c.cap_r >= 255) return "Off"; std::snprintf(b, sizeof b, "%d (no click)", c.cap_r); return b;
    default: return "";
  }
}
bool step_device_locked(int dir) {
  refresh_devices_locked();
  if (devices.empty()) return false;
  selected = (selected + devices.size() + size_t(dir < 0 ? -1 : 1)) % devices.size();
  profile_names = profile_list(profiles_folder(), devices[selected].kind);
  profile_pick = 0;
  delete_armed = false;
  return false;
}
bool setup_change(int item, int dir) {
  std::lock_guard<std::mutex> lock(mu);
  const std::vector<int> rows = setup_rows_locked();
  if (item < 0 || item >= int(rows.size())) return false;
  const int r = rows[size_t(item)];
  if (r == S_DEVICE) return step_device_locked(dir ? dir : 1);
  const Device* d = device_locked();
  if (!d || r == S_MODE) return false;
  Config c = config_locked(*d);
  const int s = dir ? dir : 1;
  auto stepv = [&](int& v, int by, int lo, int hi) { v = std::clamp(v + s * by, lo, hi); };
  switch (r) {
    case S_INV_SX: c.invert ^= kInvertStickX; break;
    case S_INV_SY: c.invert ^= kInvertStickY; break;
    case S_INV_CX: c.invert ^= kInvertCX; break;
    case S_INV_CY: c.invert ^= kInvertCY; break;
    case S_RADIUS: stepv(c.radius_main, 1, 50, 100); break;
    case S_RADIUS_C: stepv(c.radius_c, 1, 50, 100); break;
    case S_MODIFIER: stepv(c.modifier_percent, 5, 0, 100); break;
    case S_DZ: stepv(c.dz_main, 1, 0, 40); break;
    case S_DZ_C: stepv(c.dz_c, 1, 0, 40); break;
    case S_SNAP_X: stepv(c.snap_x, 2, 0, kSnapbackEdge); break;
    case S_SNAP_Y: stepv(c.snap_y, 2, 0, kSnapbackEdge); break;
    case S_SNAP_MS: stepv(c.snap_ms_x, 2, kSnapbackMinMs, kSnapbackMaxMs); c.snap_ms_y = c.snap_ms_x; break;
    case S_CLICK_L: stepv(c.click_l, 5, 40, 255); break;
    case S_CLICK_R: stepv(c.click_r, 5, 40, 255); break;
    case S_CAP_L: c.cap_l = c.cap_l >= 255 ? (s < 0 ? 140 : 80) : std::clamp(c.cap_l + s * 5, 43, 255); break;
    case S_CAP_R: c.cap_r = c.cap_r >= 255 ? (s < 0 ? 140 : 80) : std::clamp(c.cap_r + s * 5, 43, 255); break;
    case S_RESET_ANALOG: {
      if (dir < 0) return false;
      Config n = Config::defaults(d->kind);   // the bindings stay
      std::memcpy(n.mask, c.mask, sizeof n.mask); std::memcpy(n.level, c.level, sizeof n.level);
      c = n;
      status = "Analog settings back to defaults";
      break;
    }
    default: return false;
  }
  if (c.cap_l > 140 && c.cap_l < 255 && r == S_CAP_L && s > 0) c.cap_l = 255;   // past the game's full shield: off
  if (c.cap_r > 140 && c.cap_r < 255 && r == S_CAP_R && s > 0) c.cap_r = 255;
  store_locked(*d, c);
  say("controls: %s: %s -> %s", d->name.c_str(), kSetupLabels[r], setup_value_locked(r).c_str());
  if (r >= S_INV_SX && r <= S_INV_CY) {   // the menu waits until the stick is let go (see controls_capture_frame)
    capture = Capture{};
    capture.active = capture.armed = capture.done = true; capture.id = d->id; capture.kind = d->kind; capture.port = d->port;
  }
  return true;
}

// Bindings page: an SDL pad lists the actions beyond its twelve buttons (those are the Controls page's remap); an
// adapter port lists all of them. A: capture; Left: unbind; Right: the default.
std::vector<int> binding_actions_locked() {
  std::vector<int> v;
  const Device* d = device_locked();
  for (int i = (d && d->kind == Kind::Adapter) ? 0 : CUp; i < kActions; ++i) v.push_back(i);
  return v;
}
// Rows: the controller, a hint, the actions, "Reset all bindings". A or Right on an action: capture; Left: unbind.
constexpr int kFirstBindingRow = 2;
bool bindings_change(int item, int dir) {
  std::lock_guard<std::mutex> lock(mu);
  if (item == 0) return step_device_locked(dir ? dir : 1);
  const Device* d = device_locked();
  if (!d || item == 1) return false;
  const std::vector<int> actions = binding_actions_locked();
  if (item == int(actions.size()) + kFirstBindingRow) {   // reset all bindings
    if (dir < 0) return false;
    Config c = config_locked(*d);
    const Config def = Config::defaults(d->kind);
    std::memcpy(c.mask, def.mask, sizeof c.mask); std::memcpy(c.level, def.level, sizeof c.level);
    store_locked(*d, c);
    status = "Bindings back to defaults";
    say("controls: %s: bindings back to defaults", d->name.c_str());
    return true;
  }
  if (item < kFirstBindingRow || item >= int(actions.size()) + kFirstBindingRow) return false;
  const int a = actions[size_t(item - kFirstBindingRow)];
  Config c = config_locked(*d);
  if (dir >= 0) {   // A (the menu sends +1 for A on a row with a value) or Right
    capture = Capture{};
    capture.active = true; capture.id = d->id; capture.kind = d->kind; capture.port = d->port; capture.action = a;
    status = std::string("Press the control for ") + kActionLabels[a];
    return false;
  }
  c.mask[a] = 0;
  c.level[a] = 0;
  store_locked(*d, c);
  say("controls: %s: %s not bound", d->name.c_str(), kActionLabels[a]);
  return true;
}

enum ProfileRow { P_DEVICE, P_PICK, P_LOAD, P_SAVE, P_SAVE_NEW, P_DELETE, P_DEFAULTS, P_DOLPHIN, P_IMPORT, P_STATUS, P_COUNT };
const char* const kProfileLabels[P_COUNT] = {"Controller", "Profile", "Load this profile", "Save over this profile", "Save as a new profile",
    "Delete this profile", "Default setup for this controller", "Dolphin profile (Dolphin folder)", "Import it into this controller", "Last"};
std::string profile_value_locked(int r) {
  const Device* d = device_locked();
  switch (r) {
    case P_DEVICE: return device_value_locked();
    case P_PICK: return profile_names.empty() ? "None saved" : profile_names[profile_pick] + (profile_names.size() > 1 ? "  (" + std::to_string(profile_pick + 1) + "/" + std::to_string(profile_names.size()) + ")" : "");
    case P_DELETE: return delete_armed ? "Press A again to delete" : "";
    case P_DOLPHIN: return dolphin_entries.empty() ? "None found" : dolphin_entries[dolphin_pick].name;
    case P_IMPORT: return d && d->kind == Kind::Adapter ? "SDL controllers only" : "";
    case P_STATUS: return status.empty() ? "-" : status;
    default: return "";
  }
}
bool profiles_change(int item, int dir) {
  std::lock_guard<std::mutex> lock(mu);
  if (item < 0 || item >= P_COUNT) return false;
  if (item != P_DELETE) delete_armed = false;
  if (item == P_DEVICE) return step_device_locked(dir ? dir : 1);
  const Device* d = device_locked();
  if (!d) { status = "No controller connected"; return false; }
  const std::string folder = profiles_folder();
  switch (item) {
    case P_PICK:
      profile_names = profile_list(folder, d->kind);
      if (!profile_names.empty()) profile_pick = (profile_pick + profile_names.size() + size_t(dir < 0 ? -1 : 1)) % profile_names.size();
      return false;
    case P_LOAD: {
      if (dir < 0 || profile_names.empty()) return false;   // A (0, or +1 on a row showing a value)
      Profile p = profile_of_locked(*d);
      if (!profile_load(folder, profile_names[profile_pick], p)) { status = "Could not read " + profile_names[profile_pick]; return false; }
      apply_profile_locked(*d, p);
      status = "Loaded " + profile_names[profile_pick];
      say("controls: profile %s loaded onto %s", profile_names[profile_pick].c_str(), d->name.c_str());
      return true;
    }
    case P_SAVE: case P_SAVE_NEW: {
      if (dir < 0) return false;
      const std::string name = item == P_SAVE && !profile_names.empty() ? profile_names[profile_pick]
                             : profile_free_name(folder, d->kind == Kind::Adapter ? "GameCube" : "Controller");
      if (!profile_save(folder, name, profile_of_locked(*d))) { status = "Could not save " + name; return false; }
      profile_names = profile_list(folder, d->kind);
      for (size_t i = 0; i < profile_names.size(); ++i) if (profile_names[i] == name) profile_pick = i;
      status = "Saved " + name;
      say("controls: profile %s saved from %s (%s)", name.c_str(), d->name.c_str(), folder.c_str());
      return false;
    }
    case P_DELETE:
      if (dir < 0 || profile_names.empty()) return false;   // A (0, or +1 on a row showing a value)
      if (!delete_armed) { delete_armed = true; return false; }
      delete_armed = false;
      status = profile_delete(folder, profile_names[profile_pick], d->kind) ? "Deleted " + profile_names[profile_pick] : "Could not delete " + profile_names[profile_pick];
      profile_names = profile_list(folder, d->kind);
      profile_pick = 0;
      return false;
    case P_DEFAULTS: {
      if (dir < 0) return false;
      Profile p;
      p.config = Config::defaults(d->kind);
      if (d->kind == Kind::Sdl) {
        const host::ControllerMap m = host::ControllerMap::defaults();
        for (int i = 0; i < 12; ++i) p.config.mask[i] = app_input_to_mask(m.binding[kAppControl[i]]);
        p.has_app = true; p.stick_deadzone = m.stick_deadzone; p.cstick_deadzone = m.cstick_deadzone; p.trigger_press = m.trigger_press;
        p.swap_sticks = m.swap_sticks; p.rumble = m.rumble;
      }
      apply_profile_locked(*d, p);
      status = "Default setup";
      return true;
    }
    case P_DOLPHIN:
      dolphin_entries = dolphin_list(dolphin_folder());
      if (!dolphin_entries.empty()) dolphin_pick = (dolphin_pick + dolphin_entries.size() + size_t(dir < 0 ? -1 : 1)) % dolphin_entries.size();
      return false;
    case P_IMPORT: {
      if (dir < 0 || dolphin_entries.empty()) return false;
      if (d->kind != Kind::Sdl) { status = "A Dolphin pad profile is for a pad, not the adapter"; return false; }
      const DolphinEntry& e = dolphin_entries[dolphin_pick];
      const DolphinResult res = dolphin_read_entry(e);
      if (!res.ok) { status = "Not imported: " + res.message; say("controls: Dolphin %s not imported: %s", e.name.c_str(), res.message.c_str()); return false; }
      apply_profile_locked(*d, res.profile);
      const std::string name = profile_clean_name("Dolphin " + e.name);
      Profile saved = profile_of_locked(*d);
      if (!name.empty()) profile_save(folder, name, saved);
      profile_names = profile_list(folder, d->kind);
      status = "Imported " + e.name + ": " + std::to_string(res.bound) + " read, " + std::to_string(res.skipped) + " skipped";
      say("controls: Dolphin %s (%s) imported onto %s: %d bindings read, %d not understood; saved as profile %s", e.name.c_str(), res.device.c_str(),
          d->name.c_str(), res.bound, res.skipped, name.c_str());
      return true;
    }
    default: return false;
  }
}
}  // namespace

void controls_menu_pages(void* out_v) {
  auto& out = *static_cast<std::vector<host::MenuExtraPage>*>(out_v);
  {
    std::lock_guard<std::mutex> lock(mu);
    refresh_devices_locked();
  }
  host::MenuExtraPage setup;
  setup.title = "Controller setup";
  setup.items = [] {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<host::MenuExtraItem> items;
    for (int r : setup_rows_locked()) {
      std::string v = setup_value_locked(r);
      if (r == S_RESET_ANALOG) v.clear();
      items.push_back({kSetupLabels[r], r == S_RESET_ANALOG ? std::string() : (v.empty() ? std::string("-") : v)});
    }
    return items;
  };
  setup.change = setup_change;
  out.push_back(std::move(setup));

  host::MenuExtraPage bindings;
  bindings.title = "Controller bindings";
  bindings.items = [] {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<host::MenuExtraItem> items;
    items.push_back({"Controller", device_value_locked()});
    items.push_back({"A or Right: press the new control", "Left: unbind"});
    const Device* d = device_locked();
    const Config c = d ? config_locked(*d) : Config::defaults(Kind::Sdl);
    for (int a : binding_actions_locked()) {
      const bool waiting = capture.active && !capture.done && d && capture.id == d->id && capture.action == a;
      items.push_back({kActionLabels[a], waiting ? std::string("Press it now...") : source_name(c.kind, c.mask[a], c.level[a])});
    }
    items.push_back({"Reset all bindings", ""});
    return items;
  };
  bindings.change = bindings_change;
  out.push_back(std::move(bindings));

  host::MenuExtraPage profiles;
  profiles.title = "Controller profiles";
  profiles.items = [] {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<host::MenuExtraItem> items;
    for (int r = 0; r < P_COUNT; ++r) {
      const std::string v = profile_value_locked(r);
      const bool value_row = r == P_DEVICE || r == P_PICK || r == P_DOLPHIN || r == P_STATUS || !v.empty();
      items.push_back({kProfileLabels[r], value_row ? (v.empty() ? std::string("-") : v) : std::string()});
    }
    return items;
  };
  profiles.change = profiles_change;
  out.push_back(std::move(profiles));
}

// ---- report rates of the system's controllers (console) ----
// The GameCube adapter's rate is measured by the app's adapter code. Pro Controller, Joy-Con and the Switch Online
// GameCube controller report through the system's HID: each new state it writes for a controller carries a sampling
// number one higher than the last, so new numbers per second are the rate at which this program can see new input.
std::vector<std::string> controls_rate_lines() {
  std::vector<std::string> lines;
#ifdef __SWITCH__
  struct Seen { uint64_t number = 0; uint64_t at_ns = 0; double hz = 0; bool have = false; };
  static Seen seen[9];
  static const HidNpadIdType ids[9] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4, HidNpadIdType_No5,
                                       HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8, HidNpadIdType_Handheld};
  const uint64_t now = host::now_ns();
  for (int i = 0; i < 9; ++i) {
    const u32 style = hidGetNpadStyleSet(ids[i]);
    if (!style) { seen[i] = Seen{}; continue; }
    uint64_t number = 0;
    const char* what = "Controller";
    if (style & HidNpadStyleTag_NpadGc) { HidNpadGcState s{}; if (hidGetNpadStatesGc(ids[i], &s, 1)) number = s.sampling_number; what = "Switch Online GameCube controller"; }
    else if (style & HidNpadStyleTag_NpadFullKey) { HidNpadFullKeyState s{}; if (hidGetNpadStatesFullKey(ids[i], &s, 1)) number = s.sampling_number; what = "Pro Controller"; }
    else if (style & HidNpadStyleTag_NpadHandheld) { HidNpadHandheldState s{}; if (hidGetNpadStatesHandheld(ids[i], &s, 1)) number = s.sampling_number; what = "Joy-Con (handheld)"; }
    else if (style & HidNpadStyleTag_NpadJoyDual) { HidNpadJoyDualState s{}; if (hidGetNpadStatesJoyDual(ids[i], &s, 1)) number = s.sampling_number; what = "Joy-Con pair"; }
    else if (style & HidNpadStyleTag_NpadJoyLeft) { HidNpadJoyLeftState s{}; if (hidGetNpadStatesJoyLeft(ids[i], &s, 1)) number = s.sampling_number; what = "Joy-Con (L)"; }
    else if (style & HidNpadStyleTag_NpadJoyRight) { HidNpadJoyRightState s{}; if (hidGetNpadStatesJoyRight(ids[i], &s, 1)) number = s.sampling_number; what = "Joy-Con (R)"; }
    Seen& w = seen[i];
    if (!w.have) { w.have = true; w.number = number; w.at_ns = now; }
    else if (now - w.at_ns >= 1000000000ull) { w.hz = double(number - w.number) * 1e9 / double(now - w.at_ns); w.number = number; w.at_ns = now; }
    char line[128];
    if (w.hz > 0) std::snprintf(line, sizeof line, "%s (%s) %.0f reports/s", what, i == 8 ? "handheld" : ("controller " + std::to_string(i + 1)).c_str(), w.hz);
    else std::snprintf(line, sizeof line, "%s (%s): measuring", what, i == 8 ? "handheld" : ("controller " + std::to_string(i + 1)).c_str());
    lines.push_back(line);
  }
#else
  lines.push_back("Pro Controller / Joy-Con: console only");
#endif
  return lines;
}
}  // namespace fe
