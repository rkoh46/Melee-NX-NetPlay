// melee-unlocked's in-game settings panel on the Switch, in place of the app's own menu (game_menu.cpp, no longer built).
//
// The look is melee-unlocked's, drawn by its own code: its ImGui panel in each of its menu styles (Clean side, its default;
// Icon tiles, GD Melee, Radial, Wide tabs, Simple; the legacy Classic and Old menus), from the fetched tree at build time (scripts/mu_ui_extract.py -> gen/mu_ui): pc_settings_visuals.inl (the
// rows, toggles, combos, sliders, section headers, footer chips, Quit game button), the Clean category list and title
// shards, the slide-in motion, the page entrance and smooth scroll of pc_settings.cpp, its ImGui style, the controller
// feed of settings_imgui_input.h and the focus bookkeeping of settings_page_navigation.h. The same eight categories
// (VIDEO, AUDIO, GAME, CONTROLS, OVERLAYS, CUSTOMIZE, GECKO CODES, MODS), A to select, B to go back, Start to close.
//
// What the rows do is the Switch app's: this file implements the app's menu interface (game_menu.h: the main rows of
// RuntimeSettings, the pages app_menu.cpp, controls.cpp and the feature modules add, the Controls remap, Quit, the HUD
// line) so everything that fed the old menu feeds this one unchanged. The panel is drawn on the render thread; every
// change it makes is queued and applied on the simulation thread at the next controller poll (panel_service), as the
// old menu applied its changes, so the game's state is never touched from the render thread. While the panel is open
// the game gets neutral controllers (play_io.cpp keeps the hand-over: what is held when it closes is kept from the
// game until let go).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "mu_ui.h"
#include "mu_mods.h"

#include "app/game_menu.h"       // the menu interface this file implements (build-time copy, patches/0104 and 0111)
#include "host.h"
#include "input_config.h"
#include "window.h"
#include "overlay.h"

#include <aurora/imgui.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <zlib.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

// ---- melee-unlocked's panel code (gen/mu_ui), with the few names it uses from the rest of its program ----
namespace host::test_env { inline const char* getenv(const char*) { return nullptr; } }   // its test hooks: none here
namespace gx {
namespace yougame {            // YouGame (melee-unlocked's own service) is not part of this app
// No F1 key on a controller: the footer's chip reads RETURN, as melee-unlocked labels it where the menu key is not F1.
inline bool stdio_bridge() { return true; }
namespace parity { constexpr const char* kMenuHint = ""; }
}  // namespace yougame
}  // namespace gx
namespace host { enum class CaptureDevice { None, Keyboard, Pad, Adapter }; }   // SettingsState's rebind fields (unused here)
#include "settings_page_navigation.h"
#include "settings_imgui_input.h"
#include "settings_search.h"
#include "radial_navigation.h"
#include "motion.h"
namespace fe::mu_ui { void* gd_texture(const std::string& name); }   // below: the GD Melee kit's pictures, decoded on first use
namespace gx {
#include "mu_settings_state.inc"
std::atomic<float> g_frame_ui_scale{1.0f};
// The GD Melee kit's pictures and font pages (menu style 2): the embedded PNGs of its assets/kit folder (build.sh sync).
struct ImTextureData { void* texture = nullptr; ImTextureID GetTexRef() const { return ImTextureID(reinterpret_cast<uintptr_t>(texture)); } };
static std::string ui_source_asset_path(const char* source, const char* name) { return std::string(source) + "/" + name; }
static ImTextureData* cosmetic_preview(const std::string& path) {
  static std::map<std::string, ImTextureData> cache;
  static int gen = -1;
  if (gen != fe::mu_ui::ui_generation()) { gen = fe::mu_ui::ui_generation(); cache.clear(); }   // textures of an earlier Aurora
  auto it = cache.find(path);
  if (it == cache.end()) it = cache.emplace(path, ImTextureData{fe::mu_ui::gd_texture(path)}).first;
  return it->second.texture ? &it->second : nullptr;
}
static bool g_menu_style_gallery_open = false;
static void draw_settings_icon(int index, ImVec2 p, ImU32 color);   // mu_panel_parts.inc (pc_settings.cpp has it first)
#include "gd_text.inl"
#include "pc_settings_visuals.inl"
#include "mu_panel_parts.inc"
}  // namespace gx

// melee-unlocked's fonts and launcher art, embedded by build.sh sync (gen/embedded.S).
extern "C" const uint8_t mu_ui_font_black[], mu_ui_font_black_end[], mu_ui_font_bold[], mu_ui_font_bold_end[];
extern "C" const uint8_t mu_ui_bailey[], mu_ui_bailey_end[], mu_ui_mark[], mu_ui_mark_end[];
#define MU_GD_ASSET(n) extern "C" const uint8_t mu_gd_##n[], mu_gd_##n##_end[];
#include "gd_assets.inc"
#undef MU_GD_ASSET

namespace fe {
void egg_watch_pads(const host::PadState pads[4]);   // egg.cpp: melee-unlocked's character select easter egg
void egg_draw(float x0, float y0, float x1, float y1, double time);
}
namespace fe::features {
__attribute__((weak)) bool practice_menu_chord(int port);   // features/proai.cpp: the Practice menu
__attribute__((weak)) uint32_t cosmetics_off_online(uint32_t* stages);   // features/cosmetics.cpp: skins served standard online
}
#include "feature_hooks.h"
namespace fe::features {   // features/gecko.cpp: the player's Gecko codes (GECKO CODES > Your codes)
__attribute__((weak)) std::vector<GeckoRow> gecko_rows();
__attribute__((weak)) std::string gecko_file();
__attribute__((weak)) void gecko_toggle(const std::string& name);
__attribute__((weak)) std::vector<GeckoRow> gecko_online_rows();   // GECKO CODES > Online (patches/net-0014)
}

namespace fe {
namespace {
constexpr uint16_t BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008, BTN_R = 0x0020, BTN_L = 0x0040,
                   BTN_A = 0x0100, BTN_START = 0x1000;
constexpr int kSkipFrames = 6 * 60;   // remap: a control left untouched for 6 s keeps its binding
constexpr const char* kCategory[8] = {"VIDEO", "AUDIO", "GAME", "CONTROLS", "OVERLAYS", "CUSTOMIZE", "GECKO CODES", "MODS"};

// ---- the model: simulation thread, guarded by g_mutex (the atomics are read anywhere) ----
std::mutex g_mutex;
host::RuntimeSettings g_settings;
std::function<void(const host::RuntimeSettings&, host::MenuChange)> g_apply;
std::vector<host::MenuExtraPage> g_extra;
std::set<int> g_hidden;
std::map<int, std::string> g_labels;
std::function<void()> g_quit_hook;
std::function<std::string()> g_hud_extra;
bool g_controls_live = true;
std::string g_notice;
std::atomic<bool> g_open{false}, g_toggle_request{false}, g_close_request{false}, g_changed{false}, g_remap_on{false};
std::atomic<bool> g_jump{false};          // menu_open_extra while open: the render thread goes to that page
std::atomic<bool> g_home_quit{false};     // Quit chosen before the game (the home screen closes the app)
std::string g_open_title;            // a page the next open shows (menu_open_extra), consumed by the render thread
int g_combo_frames = 0;
bool g_combo_blocked = false;
int g_nav_port = 0;                  // the pad that drives the panel (settings_nav.h's rule)
bool g_nav_rest[4] = {true, true, true, true}, g_nav_was_open = false;
int g_nav_closed_frames = 60;
int g_open_at_frame = -1;
// The navigation pads for the panel (render thread reads them).
host::PadState g_nav[4] = {};
uint64_t g_nav_seq = 0;

// Controllers on the CONTROLS page (the app's remap, deadzones, rumble: the rows game_menu.cpp had).
struct Device { std::string guid, name; };   // an empty guid is the keyboard
std::vector<Device> g_devices;
int g_device = 0;
int g_step = 0, g_step_frames = 0;
bool g_step_armed = false;
enum ControlsRow { C_DEVICE, C_REMAP, C_STICK_DZ, C_CSTICK_DZ, C_TRIGGER, C_SWAP, C_RUMBLE, C_TEST_RUMBLE, C_MODIFIER, C_RESET };

// What the panel asked for, applied on the simulation thread (panel_service).
enum class Act { Main, Extra, Controls, Remap, Quit, Ini, Setting, Practice, ModPrimary, ModRemove, ModProfile, Gecko };
struct Action { Act act; int a = 0, b = 0, value = 0; std::string key, text; };
std::vector<Action> g_actions;

const host::ControllerConfig pad_config(const std::string& guid) {
  host::ControllerConfig c;
  if (const host::ControllerConfig* e = host::controller_config_for(guid)) c = *e;
  c.guid = guid;
  return c;
}
const Device* device_locked() { return g_devices.empty() ? nullptr : &g_devices[size_t(std::clamp(g_device, 0, int(g_devices.size()) - 1))]; }
bool is_keyboard(const Device* d) { return d && d->guid.empty(); }
void refresh_devices_locked() {
  const std::string keep = device_locked() ? device_locked()->guid : std::string("\x01");
  g_devices.clear();
#ifndef __SWITCH__
  g_devices.push_back({"", "Keyboard"});
#endif
  if (g_controls_live)
    for (const host::ControllerInfo& c : host::window_list_controllers()) if (!c.is_gamecube_adapter) g_devices.push_back({c.guid, c.name});
  g_device = 0;
  for (size_t i = 0; i < g_devices.size(); ++i) if (g_devices[i].guid == keep) g_device = int(i);
}
void set_open_locked(bool open) {
  if (open != g_open.load()) host::log("menu: %s", open ? "opened" : "closed");
  if (open && !g_open.load()) refresh_devices_locked();
  g_open.store(open);
  g_combo_frames = 0;
  if (!open) { g_remap_on.store(false); }
}

// ---- main rows (RuntimeSettings): set to a value; returns the change class or -1 ----
int set_main_locked(host::MenuChange what, int v) {
  host::RuntimeSettings& s = g_settings;
  switch (what) {
    case host::MenuChange::Volume: s.volume = std::clamp(v, 0, 100); break;
    case host::MenuChange::Hud: s.hud = v != 0; break;
    case host::MenuChange::OnlineDelay: s.online_delay = std::clamp(v, 0, 9); break;
    case host::MenuChange::Widescreen: s.widescreen = v != 0; break;
    case host::MenuChange::Filter: s.filter = std::clamp(v, 0, 3); break;
    case host::MenuChange::Msaa: s.msaa = v ? 4 : 1; break;
    case host::MenuChange::Anisotropy: s.anisotropy = v >= 2 ? 16 : v == 1 ? 4 : 1; break;
    case host::MenuChange::Trigger: s.trigger_range = std::clamp(v, 2, 8); break;
    case host::MenuChange::Stick: s.stick_range = std::clamp(v, 80, 120); break;
    case host::MenuChange::FullSpeed: s.full_speed = v != 0; break;
    default: return -1;
  }
  return int(what);
}

// Acts on a CONTROLS row (the logic of game_menu.cpp's controls_act); true when a saved setting changed.
bool controls_act_locked(int r, int dir) {
  const Device* d = device_locked();
  switch (r) {
    case C_DEVICE: if (g_devices.size() > 1) g_device = (g_device + dir + int(g_devices.size())) % int(g_devices.size()); return false;
    case C_TEST_RUMBLE: if (d && !is_keyboard(d)) host::window_test_rumble(d->guid); return false;
    case C_MODIFIER: { host::KeyboardMap k = host::keyboard_map(); k.modifier_percent = std::clamp(dir, 20, 90); host::set_keyboard_map(k); return true; }
    case C_RESET:
      if (!d) return false;
      if (is_keyboard(d)) host::set_keyboard_map(host::KeyboardMap::defaults());
      else { host::ControllerConfig c = pad_config(d->guid); c.map = host::ControllerMap::defaults(); host::upsert_controller_config(c); }
      return true;
  }
  if (!d || is_keyboard(d)) return false;
  host::ControllerConfig c = pad_config(d->guid);
  switch (r) {
    case C_STICK_DZ: c.map.stick_deadzone = std::clamp(dir, 0, 60); break;     // the slider's value
    case C_CSTICK_DZ: c.map.cstick_deadzone = std::clamp(dir, 0, 60); break;
    case C_TRIGGER: c.map.trigger_press = std::clamp(dir, 20, 100); break;
    case C_SWAP: c.map.swap_sticks = !c.map.swap_sticks; break;
    case C_RUMBLE: c.map.rumble = !c.map.rumble; break;
    default: return false;
  }
  host::upsert_controller_config(c);
  return true;
}
int remap_count_locked() { return is_keyboard(device_locked()) ? int(host::KB_COUNT) : int(host::GC_CTL_COUNT); }
void remap_assign_locked(int input) {   // the same input on another control is cleared, so one press never does two things
  const Device* d = device_locked();
  if (!d) return;
  if (is_keyboard(d)) {
    host::KeyboardMap k = host::keyboard_map();
    for (int j = 0; j < host::KB_COUNT; ++j) if (j != g_step && k.key[j] == input) k.key[j] = 0;
    k.key[g_step] = input; host::set_keyboard_map(k);
  } else {
    host::ControllerConfig c = pad_config(d->guid);
    for (int j = 0; j < host::GC_CTL_COUNT; ++j) if (j != g_step && c.map.binding[j] == input) c.map.binding[j] = host::kUnbound;
    c.map.binding[g_step] = input; host::upsert_controller_config(c);
  }
  g_changed.store(true);
}
void remap_next_locked() { ++g_step; g_step_frames = 0; g_step_armed = false; if (g_step >= remap_count_locked()) g_remap_on.store(false); }

void queue(Action a) { std::lock_guard<std::mutex> lock(g_mutex); g_actions.push_back(std::move(a)); }

// ---- render thread: the rows of each category, read from the model every frame ----
enum class Kind { Toggle, Choice, Slider, Stepper, Action, Info };
struct Row {
  std::string label, text, hint;
  Kind kind = Kind::Info;
  int value = 0, low = 0, high = 1, step = 1;
  std::vector<std::string> names;
  const char* format = "%d";
  bool enabled = true, next_start = false;
  Action action{Act::Main};
};
struct Section { std::string title; std::vector<Row> rows; };
struct Category { std::vector<Section> sections; };

Section& section(Category& c, const std::string& title) {
  for (auto& s : c.sections) if (s.title == title) return s;
  c.sections.push_back({title, {}});
  return c.sections.back();
}
// Where a page of the app goes among melee-unlocked's categories, and under which section header. melee-unlocked's own
// GAME page has Input, Language, Input delay, Online, Stages, L-cancel, HUD and Community codes sections; its OVERLAYS page
// the overlays; its GECKO CODES page the built-in codes.
void place(const std::string& page, int* cat, std::string* title) {
  struct P { const char* page; int cat; const char* title; };
  static const P table[] = {
    {"Picture", 0, "Picture"}, {"Game options", 2, "Game"}, {"Picture and HUD", 2, "HUD"}, {"Community codes", 2, "Community codes"},
    {"Computer players", 2, "Computer players"}, {"Game", 2, "Input"}, {"Online", 2, "Online"}, {"Training", 2, "Training"},
    {"Replays", 2, "Replays"}, {"Overlays", 4, "Overlays"}, {"Mods", 7, "Game mods"}, {"Controller setup", 3, "Controller setup"},
    {"Controller bindings", 3, "Controller bindings"}, {"Controller profiles", 3, "Controller profiles"}, {"Audio", 1, "Output"}, {"PhobGCC", 3, "PhobGCC (GameCube adapter)"},
    {"Skins", 7, "Choose skins"}, {"Texture packs", 0, "Texture packs"}, {"Diagnostics", 0, "Advanced"},
    {"Looks", 0, "Looks"}, {"Lab view", 0, "Lab view"}, {"Menu videos", 0, "Menu video backgrounds"}, {"Shader presets", 0, "Shader presets"}};
  for (const P& p : table) if (page == p.page) { *cat = p.cat; *title = p.title; return; }
  *cat = 2; *title = page;
}
// Single rows of a page that melee-unlocked keeps on another page (its AUDIO page has the music and voices, its GECKO CODES
// page the built-in codes).
bool place_row(const std::string& label, int* cat, std::string* title) {
  struct P { const char* label; int cat; const char* title; };
  static const P table[] = {
    {"Music volume", 1, "Music and sound"}, {"Mute voices", 1, "Music and sound"}, {"Faster sound effects", 1, "Music and sound"},
    {"Retail game (no built-in codes)", 6, "Built-in codes"}, {"Slippi menus (Online Play)", 6, "Built-in codes"},
    {"Language", 2, "Language"}, {"Everything unlocked", 2, "Language"},
    {"Frozen Pokemon Stadium", 2, "Stages"}, {"Frozen Final Destination", 2, "Stages"},
    {"Automatic L-cancel (offline; Direct when both agree)", 2, "L-cancel"}, {"Offline input delay", 2, "Input delay"},
    {"Console low-pass on voices (next start)", 1, "Music and sound"}, {"Use custom music (next start)", 1, "Music and sound"},
    // melee-unlocked's VIDEO page has the Arcade look section
    {"Show hitboxes", 0, "Arcade look"}, {"Projectile hitboxes", 0, "Arcade look"}, {"Hitboxes behind fighters", 0, "Arcade look"},
    {"Lossless adapter lag test (next start only)", 0, "Advanced"},
    {"2D camera", 0, "Arcade look"}, {"Grid behind the stage", 0, "Arcade look"}, {"Combo counter (offline)", 0, "Arcade look"}};
  for (const P& p : table) if (label == p.label) { *cat = p.cat; *title = p.title; return true; }
  return false;
}

std::string mods_folder() { return options.out_dir + "/mods"; }
std::vector<std::string> mod_profiles() {
  std::vector<std::string> out;
  const std::string base = mods_folder();
  if (DIR* d = opendir((base + "/Profiles").c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 4 && n.substr(n.size() - 4) == ".ini") out.push_back(n.substr(0, n.size() - 4));
    }
    closedir(d);
  }
  if (DIR* d = opendir(base.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.empty() || n[0] == '.' || n == "Profiles") continue;
      struct stat st{};
      if (stat((base + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

Row toggle(const std::string& label, bool on, Action a) { Row r; r.label = label; r.kind = Kind::Toggle; r.value = on; r.action = std::move(a); return r; }
Row choice(const std::string& label, int value, std::vector<std::string> names, Action a) {
  Row r; r.label = label; r.kind = Kind::Choice; r.value = value; r.names = std::move(names); r.action = std::move(a); return r;
}
Row slider(const std::string& label, int value, int low, int high, int step, const char* format, Action a) {
  Row r; r.label = label; r.kind = Kind::Slider; r.value = value; r.low = low; r.high = high; r.step = step; r.format = format; r.action = std::move(a); return r;
}

std::array<Category, 8> build_rows(bool in_game) {
  std::array<Category, 8> cats;
  std::lock_guard<std::mutex> lock(g_mutex);
  const host::RuntimeSettings s = g_settings;
  const auto shown = [&](host::MenuChange w) { return !g_hidden.count(int(w)); };
  const auto label = [&](host::MenuChange w, const char* fallback) {
    const auto it = g_labels.find(int(w));
    std::string l = it != g_labels.end() ? it->second : fallback;
    if (l.size() > 2 && l.substr(l.size() - 2) == " *") l.resize(l.size() - 2);   // the next-start mark is a hint here
    return l;
  };
  const auto main_act = [](host::MenuChange w) { Action a{Act::Main}; a.a = int(w); return a; };
  // VIDEO
  {
    Section& v = section(cats[0], "Display");
    if (shown(host::MenuChange::Widescreen)) { v.rows.push_back(choice(label(host::MenuChange::Widescreen, "Aspect ratio"), s.widescreen, {"4:3 (original)", "16:9 widescreen"}, main_act(host::MenuChange::Widescreen))); v.rows.back().next_start = true; }
    if (shown(host::MenuChange::Filter)) v.rows.push_back(choice("Screen filter", s.filter, {"Smooth", "Area", "CRT", "Vibrant"}, main_act(host::MenuChange::Filter)));
    if (shown(host::MenuChange::Msaa)) { v.rows.push_back(choice("Anti-aliasing", s.msaa > 1, {"Off", "4x MSAA"}, main_act(host::MenuChange::Msaa))); v.rows.back().next_start = true; }
    if (shown(host::MenuChange::Anisotropy)) { v.rows.push_back(choice("Anisotropic filtering", s.anisotropy >= 16 ? 2 : s.anisotropy >= 4 ? 1 : 0, {"Off", "4x", "16x"}, main_act(host::MenuChange::Anisotropy))); v.rows.back().next_start = true; }
  }
  // AUDIO
  if (shown(host::MenuChange::Volume)) section(cats[1], "Volume").rows.push_back(slider("Volume", s.volume, 0, 100, 10, "%d%%", main_act(host::MenuChange::Volume)));
  // GAME: the online input delay first (melee-unlocked's "Input delay" section)
  if (shown(host::MenuChange::OnlineDelay)) {
    std::vector<std::string> names = {"Auto (from ping)", "1 frame (lowest)"};
    for (int i = 2; i <= 9; ++i) names.push_back(std::to_string(i) + " frames");
    Row r = choice("Online input delay (next match)", s.online_delay, names, main_act(host::MenuChange::OnlineDelay));
    section(cats[2], "Input delay").rows.push_back(r);
  }
  // CONTROLS: the controller, its remap, deadzones and rumble, then the console's stick and trigger ranges
  {
    Section& c = section(cats[3], "Controller");
    const Device* d = device_locked();
    Row dev; dev.label = "Controller"; dev.kind = Kind::Stepper; dev.text = d ? d->name : std::string("No controller connected");
    dev.action = Action{Act::Controls}; dev.action.a = C_DEVICE; c.rows.push_back(dev);
    if (d) {
      Row remap; remap.label = is_keyboard(d) ? "Remap keys" : "Remap buttons"; remap.kind = Kind::Action; remap.action = Action{Act::Remap};
      remap.hint = "One control at a time: press the new button, or wait 6 s to keep it."; c.rows.push_back(remap);
      if (is_keyboard(d)) {
        Action a{Act::Controls}; a.a = C_MODIFIER;
        c.rows.push_back(slider("Stick amount with modifier", host::keyboard_map().modifier_percent, 20, 90, 5, "%d%%", a));
      } else {
        const host::ControllerMap m = pad_config(d->guid).map;
        Action a{Act::Controls};
        a.a = C_STICK_DZ; c.rows.push_back(slider("Stick deadzone", m.stick_deadzone, 0, 60, 2, "%d%%", a));
        a.a = C_CSTICK_DZ; c.rows.push_back(slider("C-stick deadzone", m.cstick_deadzone, 0, 60, 2, "%d%%", a));
        a.a = C_TRIGGER; c.rows.push_back(slider("Trigger press point", m.trigger_press, 20, 100, 5, "%d%%", a));
        a.a = C_SWAP; c.rows.push_back(toggle("Swap sticks", m.swap_sticks, a));
        a.a = C_RUMBLE; c.rows.push_back(toggle("Rumble", m.rumble, a));
        Row t; t.label = "Test rumble"; t.kind = Kind::Action; t.action = Action{Act::Controls}; t.action.a = C_TEST_RUMBLE; c.rows.push_back(t);
      }
      Row reset; reset.label = "Reset to defaults"; reset.kind = Kind::Action; reset.action = Action{Act::Controls}; reset.action.a = C_RESET; c.rows.push_back(reset);
    }
    Section& g = section(cats[3], "GameCube controllers");
    if (shown(host::MenuChange::Stick)) g.rows.push_back(slider("GameCube stick range", s.stick_range, 80, 120, 4, "%d", main_act(host::MenuChange::Stick)));
    if (shown(host::MenuChange::Trigger)) g.rows.push_back(slider("Light shield range", s.trigger_range, 2, 8, 1, "%dx", main_act(host::MenuChange::Trigger)));
  }
  // OVERLAYS: the performance HUD
  if (shown(host::MenuChange::Hud)) section(cats[4], "Performance").rows.push_back(toggle("Performance HUD", s.hud, main_act(host::MenuChange::Hud)));
  // The pages the app adds (settings.ini pages, the feature modules, the controller setup).
  for (size_t p = 0; p < g_extra.size(); ++p) {
    const host::MenuExtraPage& page = g_extra[p];
    if (!page.items) continue;
    int cat = 2; std::string title;
    place(page.title, &cat, &title);
    const std::vector<host::MenuExtraItem> items = page.items();
    for (size_t i = 0; i < items.size(); ++i) {
      const host::MenuExtraItem& it = items[i];
      int rc = cat; std::string rt = title;
      place_row(it.label, &rc, &rt);
      Row r; r.label = it.label; r.enabled = it.enabled; r.next_start = it.next_start;
      while (!r.label.empty() && r.label[0] == ' ') r.label.erase(0, 1);   // indented sub-rows: the panel's own rhythm
      r.action = Action{Act::Extra}; r.action.a = int(p); r.action.b = int(i);
      if (it.value == "On" || it.value == "Off") { r.kind = Kind::Toggle; r.value = it.value == "On"; }
      else if (it.value.empty()) r.kind = Kind::Action;
      else { r.kind = Kind::Stepper; r.text = it.value; }
      if (!it.enabled) r.hint = "Locked during online play.";
      section(cats[size_t(rc)], rt).rows.push_back(r);
    }
  }
  // GAME, Training: the Practice menu (branch feat/proai), when it is linked.
  if (&fe::features::practice_menu_chord) {
    Row p; p.label = "Practice menu"; p.kind = Kind::Action; p.action = Action{Act::Practice};
    p.hint = "Also Start + D-pad Up + Z.";
    Section& t = section(cats[2], "Training");
    t.rows.insert(t.rows.begin(), p);
  }
  // CUSTOMIZE (upstream's page 5): the menu style, its palette, the window's transparency, the custom accent (its colour
  // wheel as hue, saturation and brightness: no pointer), the legacy menu; and the home screen's accent.
  {
    const auto ini = [](const char* key, int fallback, int low, int high) {
      const std::string v = fe::app_ini(key, "");
      return v.empty() ? fallback : std::clamp(std::atoi(v.c_str()), low, high);
    };
    const auto ini_act = [](const std::string& key) { Action a{Act::Ini}; a.key = key; return a; };
    Section& c = section(cats[5], "Appearance");
    const int style = ini("overlaystyle", 0, 0, 5);
    Row ms = choice("Menu style", style, {"Clean side", "Icon tiles", "GD Melee", "Radial", "Wide tabs", "Simple"}, ini_act("overlaystyle"));
    static const char* const kHints[6] = {"Pink side panel; settings slide in beside the live game", "Rounded, dimensional settings tiles",
                                          "GD Melee's slanted navigation and matching setting rows", "Controller-first radial category navigation",
                                          "Wide category tabs with live settings pages", "Compact settings layout"};
    ms.hint = kHints[style];
    c.rows.push_back(ms);
    static const char* const kPalettes[7][4] = {
      {"Original pink", "Electric cyan", "Sunset amber", "Arcade violet"}, {"Original candy", "Arcade brights", "Soft pastels", "Night lights"},
      {"Classic gold", "Cobalt", "Emerald", "Crimson"}, {"Original blue", "Plasma violet", "Emerald", "Amber"},
      {"Original blue", "Magenta", "Mint", "Amber"}, {"Simple blue", "Graphite", "Forest", "Warm gray"}, {"Windows blue", "Steel", "Pine", "Sepia"}};
    const std::string pkey = "overlaypalette" + std::to_string(style);
    Row pal = choice("Color palette for this appearance", ini(pkey.c_str(), style == 0 ? ini("mu_palette", 0, 0, 3) : 0, 0, 3),
                     {kPalettes[style][0], kPalettes[style][1], kPalettes[style][2], kPalettes[style][3]}, ini_act(pkey));
    pal.hint = style == 2 ? "Classic Gold is GD Melee's original palette." : "Each layout remembers its own color choice.";
    c.rows.push_back(pal);
    c.rows.push_back(slider("Window transparency", ini("settingstransparency", 0, 0, 65), 0, 65, 5, "%d%%", ini_act("settingstransparency")));
    const bool custom = ini("menucustom", 0, 0, 1) != 0;
    c.rows.push_back(toggle("Use custom accent", custom, ini_act("menucustom")));
    if (custom) {
      c.rows.push_back(slider("Accent hue", ini("menuhue", 333, 0, 359), 0, 359, 5, "%d", ini_act("menuhue")));
      c.rows.push_back(slider("Accent saturation", ini("menusat", 84, 0, 100), 0, 100, 5, "%d%%", ini_act("menusat")));
      c.rows.push_back(slider("Accent brightness", ini("menuval", 96, 0, 100), 0, 100, 5, "%d%%", ini_act("menuval")));
    }
    Section& l = section(cats[5], "Legacy menu");
    Row lg = toggle("Enable Legacy Menu", ini("legacymenu", 0, 0, 1) != 0, ini_act("legacymenu"));
    lg.hint = "The home screen's Settings opens the selected legacy menu (F11 on a keyboard). Both menus edit the same settings.";
    l.rows.push_back(lg);
    Action la = ini_act("legacymenustyle"); la.text = "7,6";
    l.rows.push_back(choice("Legacy menu", ini("legacymenustyle", 7, 6, 7) == 6 ? 1 : 0, {"Legacy Old (v0.6.6)", "Legacy New"}, la));
    Section& h = section(cats[5], "Launcher");
    Action ha{Act::Ini}; ha.key = "mu_accent";
    h.rows.push_back(choice("Launcher color", std::clamp(std::atoi(fe::app_ini("mu_accent", "0").c_str()), 0, 5),
                            {"Purple (default)", "Blue", "Green", "Amber", "Red", "Pink"}, ha));
  }
  // AUDIO: the panel's menu sounds; OVERLAYS: the reminder and the note about skins online (upstream's rows on those pages).
  {
    const auto ini_on = [](const char* key, int fallback) { const std::string v = fe::app_ini(key, ""); return v.empty() ? fallback != 0 : v != "0"; };
    Action m{Act::Ini}; m.key = "menusounds";
    Row ms = toggle("Menu sounds", ini_on("menusounds", 0), m);
    ms.hint = "A short tick when you move through the settings, and when a page opens or closes.";
    section(cats[1], "Menu").rows.push_back(ms);
    Action r{Act::Ini}; r.key = "settingsreminder";
    Row rr = toggle("Show the \"Settings\" reminder in menus", ini_on("settingsreminder", 0), r);
    rr.hint = "L + R + Start, or Start + D-pad Down + Z on a GameCube controller, opens settings.";
    section(cats[4], "Notices").rows.push_back(rr);
    Action n{Act::Ini}; n.key = "skin_online_notice";
    Row nn = toggle("Show the note when skins are off online", ini_on("skin_online_notice", 1), n);
    nn.hint = "In an online mode, skins that could change more than looks show as the standard ones, and a note says so for 15 seconds.";
    section(cats[4], "Notices").rows.push_back(nn);
  }
  // MODS: melee-unlocked's catalog through the app's mod installer (mu_mods.h), then the profile the next start loads.
  {
    std::string error;
    const std::vector<mu_mods::Card> list = mu_mods::cards(&error);
    Section& m = section(cats[7], "Game mods");
    if (!error.empty()) { Row e; e.kind = Kind::Info; e.label = "Mod catalog"; e.text = error; m.rows.push_back(e); }
    for (const auto& c : list) {
      Row info; info.kind = Kind::Info; info.label = c.status.mod.name;
      info.text = c.state + "\n" + c.detail + (c.get_it.empty() ? "" : "\n" + c.get_it);
      m.rows.push_back(info);
      Row p; p.kind = Kind::Action; p.label = c.primary + "###mod-main-" + c.status.mod.id; p.enabled = c.primary_enabled;
      p.action = Action{Act::ModPrimary}; p.action.key = c.status.mod.id;
      if (c.status.percent >= 0) p.hint = c.status.line;
      m.rows.push_back(p);
      if (c.can_remove) {
        Row rm; rm.kind = Kind::Action; rm.label = std::string(c.remove_armed ? "Remove: press A again" : "Remove") + "###mod-remove-" + c.status.mod.id;
        rm.action = Action{Act::ModRemove}; rm.action.key = c.status.mod.id;
        m.rows.push_back(rm);
      }
    }
    Section& n = section(cats[7], "Next start");
    std::vector<std::string> names;
    const std::vector<std::string> choices = mu_mods::profile_choices();
    const std::string next = mu_mods::profile_next();
    int value = 0;
    for (size_t i = 0; i < choices.size(); ++i) { names.push_back(choices[i].empty() ? "None (the retail game)" : choices[i]); if (choices[i] == next) value = int(i); }
    if (names.empty()) names.push_back("None (the retail game)");
    Row pr = choice("Mod profile", value, names, Action{Act::ModProfile});
    pr.next_start = true;
    pr.enabled = !session_online();
    n.rows.push_back(pr);
    Row where; where.kind = Kind::Info; where.label = "Mods folder";
    where.text = "switch/melee-nx-netplay/mods: a mod per folder, Profiles/<name>.ini, and Downloads/ for files you fetch yourself.";
    n.rows.push_back(where);
  }
  // GECKO CODES > Your codes (features/gecko.cpp): each code of GeckoCodes.ini, switched here; one the Source Port cannot run is
  // greyed with upstream's reason.
  if (&fe::features::gecko_rows) {
    Section& g = section(cats[6], "Your codes");
    const std::vector<fe::features::GeckoRow> rows = fe::features::gecko_rows();
    for (const auto& c : rows) {
      Row r = toggle(c.name + "###gecko-" + c.name, c.enabled, Action{Act::Gecko});
      r.action.key = c.name; r.enabled = c.supported && !session_online();
      r.hint = !c.reason.empty() ? c.reason : session_online() ? "Your codes run offline only." : "Offline only.";
      g.rows.push_back(r);
    }
    Row where; where.kind = Kind::Info; where.label = rows.empty() ? "No codes yet" : "Codes file";
    where.text = "switch/melee-nx-netplay/GeckoCodes.ini, in Dolphin's format ([Gecko], $Name, then XXXXXXXX YYYYYYYY lines). Read at each start.";
    g.rows.push_back(where);
  }
  // GECKO CODES > Online (features/gecko.cpp, patches/net-0014): codes agreed in Direct with another Melee-NX-NetPlay player.
  if (&fe::features::gecko_online_rows) {
    Section& g = section(cats[6], "Online (Direct, Melee-NX-NetPlay players)");
    for (const auto& c : fe::features::gecko_online_rows()) {
      if (c.info) { Row r; r.kind = Kind::Info; r.label = c.name; r.text = c.reason; g.rows.push_back(r); continue; }
      Row r = toggle(c.name + "###geckoon-" + c.key, c.enabled, Action{Act::Gecko});
      if (c.action) r.kind = Kind::Action;
      r.action.key = c.key; r.enabled = c.supported; r.hint = c.reason;
      g.rows.push_back(r);
    }
  }
  // melee-unlocked's rows the Switch does not have, greyed under each page, with the reason (FEATURES.md has each one).
  static const struct { int cat; const char* label; const char* why; } kAbsent[] = {
    {0, "Frame rate, VSync, Display, Window size", "The console's display runs at 60 Hz with its own sync; there is no window."},
    {0, "Upscaling (DLSS, XeSS, FSR), frame generation, NVIDIA Reflex, HDR, ray tracing", "PC graphics cards only."},
    {0, "Graphics backend", "One renderer on the Switch: Aurora on Vulkan."},
    {1, "WASAPI exclusive, ASIO, output device", "The console's audio output has no driver choice: Auto and Low latency are above."},
    {2, "Background input, Discord", "No other windows or Discord on the console."},
    {2, "YouGame", "Left out: melee-unlocked's own online service."},
    {3, "Keyboard and HID controller editors", "The Switch's controllers come through the system and the GameCube adapter: the rows above."},
    {4, "Controller overlay placement (dragging)", "No pointer: the overlays have fixed places."},
    {5, "Menu style gallery (screenshots), the colour wheel, menu artwork", "No pointer: the style is a list, the accent three sliders."},
    {6, "Add, edit or import a code here", "Needs a keyboard: put codes in GeckoCodes.ini on the SD card (above)."},
    {6, "Codes that inject PowerPC (C2) or rewrite game functions", "The Source Port runs no PowerPC, and the console does not let a program rewrite its own code."},
    {7, "Cosmetic mods (costumes, stages, effects), the mods catalog", "Mod discs load as profiles (above), built on the PC."},
  };
  for (const auto& a : kAbsent) {
    Row r; r.kind = Kind::Info; r.label = a.label; r.text = a.why;
    section(cats[size_t(a.cat)], "Not on the Switch").rows.push_back(r);
  }
  return cats;
}

// ---- render thread: the panel ----
gx::SettingsState g_state;
bool g_ui_open = false;              // what the panel shows as open (follows g_open)
bool g_armed = false;                // the controller was let go once since the panel opened
bool g_quit_pending = false;
std::string g_scroll_to;             // the page a menu_open_extra named (logged)
bool g_focus_first = false;          // the page just entered: its first row takes the focus
ImGuiID g_row_id = 0;                // the control of the row drawn last
int g_enter_frames = 0;              // menu_open_extra: frames until the category is entered
int g_last_page = -1, g_reopen_page = -1;   // the category page shown last (-1: the category list), for the next open
host::PadState g_fed[4] = {};
uint64_t g_fed_seq = ~0ull;
bool g_in_game_feed = true;          // the pads come from the simulation thread (false: panel_feed from the home screen)

struct OptionsView { int overlay_style = 0; };   // the panel's options as melee-unlocked's code reads them

void feed_imgui(const host::PadState pads[4]) {
  struct Merged { uint16_t button = 0; int stick_x = 0, stick_y = 0; } m;
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0) continue;
    m.button |= pads[i].button;
    if (std::abs(pads[i].stick_x) > std::abs(m.stick_x)) m.stick_x = pads[i].stick_x;
    if (std::abs(pads[i].stick_y) > std::abs(m.stick_y)) m.stick_y = pads[i].stick_y;
  }
  // Until everything held when the panel opened (L + R + Start, the chord) is let go, the panel sees nothing: the
  // Start of the combo would otherwise close it at once (melee-unlocked's practice_pad_armed does the same).
  if (!g_armed) {
    if (m.button == 0 && std::abs(m.stick_x) < 30 && std::abs(m.stick_y) < 30) g_armed = true;
    else m = Merged{};
  }
  g_state.settings_stick_x = int8_t(std::clamp(m.stick_x, -128, 127));   // the radial wheel's stick
  g_state.settings_stick_y = int8_t(std::clamp(m.stick_y, -128, 127));
  gx::settings_input::gamepad(ImGui::GetIO(), m);
}

// A row whose value steps left and right (a value the app's pages show as text): drawn with the panel's field label
// and a frame like its combo, < value >. Left/Right or A steps it.
int stepper(const Row& r) {
  int dir = 0;
  ImGui::PushID(r.label.c_str());
  gx::settings_field_label(r.label.c_str());
  const float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight();
  const bool pressed = gx::settings_hit_button("##stepper", ImVec2(width, height));
  gx::settings_page::label_item(r.label.c_str());
  const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(a, b, ImGui::GetColorU32(hovered || focused ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
  const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
  const float mid = (a.y + b.y) * 0.5f, s = ImGui::GetFontSize() * 0.32f;
  draw->AddTriangleFilled(ImVec2(a.x + 10, mid), ImVec2(a.x + 10 + 1.6f * s, mid - s), ImVec2(a.x + 10 + 1.6f * s, mid + s), ink);
  draw->AddTriangleFilled(ImVec2(b.x - 10, mid), ImVec2(b.x - 10 - 1.6f * s, mid - s), ImVec2(b.x - 10 - 1.6f * s, mid + s), ink);
  const ImVec2 ts = ImGui::CalcTextSize(r.text.c_str());
  const float tx = std::max(a.x + 26.0f, (a.x + b.x - ts.x) * 0.5f);
  draw->PushClipRect(ImVec2(a.x + 24, a.y), ImVec2(b.x - 24, b.y), true);
  draw->AddText(ImVec2(tx, mid - ts.y * 0.5f), ink, r.text.c_str());
  draw->PopClipRect();
  if (pressed) dir = 1;
  if (focused && r.enabled) {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) dir = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dir = 1;
  }
  ImGui::PopID();
  return dir;
}

void draw_row(const Row& r) {
  ImGui::BeginDisabled(!r.enabled);
  g_row_id = 0;
  switch (r.kind) {
    case Kind::Toggle: {
      bool v = r.value != 0;
      if (gx::settings_toggle(r.label.c_str(), &v)) { Action a = r.action; a.value = v; a.b = r.action.b; queue(a); }
      break;
    }
    case Kind::Choice: {
      std::vector<const char*> items;
      for (const auto& n : r.names) items.push_back(n.c_str());
      int v = std::clamp(r.value, 0, int(items.size()) - 1);
      // GD Melee's combo shows its own short names for a row called "Aspect ratio" (upstream's five aspects): the app's
      // two-value row keeps its own names there under another label.
      const char* label = gx::g_settings_form_style == 2 && r.label == "Aspect ratio" ? "Picture aspect" : r.label.c_str();
      if (gx::settings_combo(label, &v, items.data(), int(items.size())) && v != r.value) { Action a = r.action; a.value = v; queue(a); }
      break;
    }
    case Kind::Slider: {
      int v = r.value;
      if (gx::settings_slider(r.label.c_str(), &v, r.low, r.high, r.format)) {
        // Controller tweaks move one unit; the row's own step is kept (Volume in tens, as before).
        if (r.step > 1) v = r.low + int(std::lround(double(v - r.low) / r.step)) * r.step;
        if (v == r.value && ImGui::IsItemFocused()) {
          if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight)) v = std::min(r.high, r.value + r.step);
          if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft)) v = std::max(r.low, r.value - r.step);
        }
        v = std::clamp(v, r.low, r.high);
        if (v != r.value) { Action a = r.action; a.value = v; queue(a); }
      }
      break;
    }
    case Kind::Stepper: {
      const gx::SettingsRow row = gx::settings_row_begin(r.label.c_str());
      if (!row.shown) break;
      if (const int dir = stepper(r)) { Action a = r.action; a.value = dir; queue(a); }
      gx::settings_row_end(row);
      break;
    }
    case Kind::Action:
      if (gx::settings_widgets::Button(r.label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        Action a = r.action; a.value = 0; queue(a);
      }
      break;
    case Kind::Info: {
      // A read-only row: its label and a dim line. The controller can rest on it, so a long page scrolls to its end.
      const gx::SettingsRow row = gx::settings_row_begin(r.label.c_str());
      if (!row.shown) break;
      ImGui::PushID(r.label.c_str());
      const ImVec2 start = ImGui::GetCursorScreenPos();
      gx::settings_field_label(r.label.c_str());
      ImGui::NewLine();
      ImGui::TextDisabled("%s", r.text.c_str());
      const ImVec2 end = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(start);
      gx::settings_shaped_hit_button("##info", ImVec2(ImGui::GetContentRegionAvail().x, std::max(1.0f, end.y - start.y - ImGui::GetStyle().ItemSpacing.y)));
      gx::settings_page::label_item(r.label.c_str());
      if (ImGui::IsItemFocused()) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_NavCursor), 4.0f, 0, 1.5f);
      ImGui::PopID();
      gx::settings_row_end(row);
      break;
    }
  }
  g_row_id = ImGui::GetItemID();
  ImGui::EndDisabled();
  if (!r.hint.empty()) gx::settings_hint("%s", r.hint.c_str());
  else if (r.next_start) gx::settings_hint("Applies at the next start.");
}

void confirm_quit_popup() {
  // melee-unlocked's Quit confirmation (settings_frame's confirm_popup), with the Switch app's way out.
  if (g_state.confirm == gx::SettingsState::Confirm::None) return;
  ImGui::OpenPopup("Quit game?");
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Quit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted("Quit now? The current match will end.");
    ImGui::Separator();
    if (gx::settings_widgets::Button("Quit", ImVec2(110, 0))) {
      g_state.confirm = gx::SettingsState::Confirm::None;
      ImGui::CloseCurrentPopup();
      queue(Action{Act::Quit});
    }
    ImGui::SameLine();
    if (gx::settings_widgets::Button("Cancel", ImVec2(110, 0))) { g_state.confirm = gx::SettingsState::Confirm::None; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

void draw_remap(int w, int h) {
  int step = 0, count = 0; std::string control, current, device;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Device* d = device_locked();
    if (!d) return;
    step = g_step; count = remap_count_locked();
    if (step >= count) return;
    control = is_keyboard(d) ? host::kKeyboardControlNames[step] : host::kGcControlNames[step];
    current = is_keyboard(d) ? host::key_name(host::keyboard_map().key[step]) : host::physical_input_name(pad_config(d->guid).map.binding[step]);
    device = d->name;
  }
  ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(520.0f, w - 40.0f), 0));
  ImGui::Begin("##mu_remap", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PushFont(gx::settings_heading_font());
  ImGui::Text("REMAP %d / %d", step + 1, count);
  ImGui::PopFont();
  ImGui::TextDisabled("%s", device.c_str());
  ImGui::Separator();
  ImGui::Text("Press the control for: %s", control.c_str());
  ImGui::TextDisabled("Now: %s. Nothing pressed for 6 s keeps it.", current.c_str());
  ImGui::End();
}

// ---- the panel's appearance (melee-unlocked's CUSTOMIZE page: menu style, palette, transparency, custom accent, legacy menu) ----
// launcher.ini keys with upstream's names (port-settings.ini there): overlaystyle 0..5, overlaypalette0..6, settingstransparency
// 0..65, menucustom and menuhue/menusat/menuval (its colour wheel as three sliders: no pointer on a controller), legacymenu and
// legacymenustyle (6 Legacy New, 7 Legacy Old), menusounds, settingsreminder, skin_online_notice.
struct Appearance { int style = 0, palette = 0, transparency = 0; bool custom = false; ImVec4 accent{0.96f, 0.15f, 0.52f, 1}; };
int ini_int(const char* key, int fallback, int low, int high) {
  const std::string v = fe::app_ini(key, "");
  return v.empty() ? fallback : std::clamp(std::atoi(v.c_str()), low, high);
}
bool g_legacy = false;               // the legacy presentation (the home screen's Settings with Legacy Menu on, or F11)
Appearance read_appearance() {
  Appearance a;
  a.style = ini_int("overlaystyle", 0, 0, 5);
  if (g_legacy) a.style = ini_int("legacymenustyle", 7, 6, 7);
  // Style 0 kept the app's earlier key (mu_palette) before every style had its own.
  a.palette = ini_int(("overlaypalette" + std::to_string(std::min(a.style, 6))).c_str(), a.style == 0 ? ini_int("mu_palette", 0, 0, 3) : 0, 0, 3);
  a.transparency = ini_int("settingstransparency", 0, 0, 65);
  a.custom = ini_int("menucustom", 0, 0, 1) != 0;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(ini_int("menuhue", 333, 0, 359) / 360.0f, ini_int("menusat", 84, 0, 100) / 100.0f, ini_int("menuval", 96, 0, 100) / 100.0f, r, g, b);
  a.accent = ImVec4(r, g, b, 1);
  return a;
}
constexpr const char* kStyleNames[8] = {"Clean side", "Icon tiles", "GD Melee", "Radial", "Wide tabs", "Simple", "Classic", "Legacy Old"};
bool& detail_flag(int style) {
  static bool always = true;
  switch (style) {
    case 0: return g_state.clean_detail_open;
    case 1: return g_state.dashboard_detail_open;
    case 2: return g_state.gd_detail_open;
    case 3: return g_state.radial_detail_open;
    case 4: return g_state.wide_detail_open;
    default: always = true; return always;   // Simple and the legacy styles: the categories and a page side by side
  }
}
bool has_home(int style) { return style <= 3; }
const char* home_item_id(int style) { return style == 0 ? "##clean_category" : style == 1 ? "##toy_tile" : style == 2 ? "##gd_category" : nullptr; }

// Menu sounds (upstream audio.cpp's mix_ui_sound): a short decaying sine, mixed into the game's own output by play_io.cpp.
std::atomic<int> g_ui_sound{0};
std::atomic<bool> g_in_match{false}, g_online_now{false};
uint32_t g_menu_since = 0;           // the retrace the menus came on (the reminder's first 20 s)

// The panel's windows (itself and its children) get the transparency after they are built, as upstream scales the
// draw data's alpha (pc_settings.cpp, after ImGui::Render): here only the panel's own lists.
void apply_transparency(int percent, int foreground_from) {
  if (percent <= 0) return;
  const float k = 1.0f - percent / 100.0f;
  auto scale = [&](ImDrawList* list, int from) {
    for (int i = from; i < list->VtxBuffer.Size; ++i) {
      ImDrawVert& v = list->VtxBuffer[i];
      const uint32_t alpha = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
      v.col = (v.col & ~IM_COL32_A_MASK) | (uint32_t(std::lround(alpha * k)) << IM_COL32_A_SHIFT);
    }
  };
  ImGuiWindow* panel = ImGui::FindWindowByName("PC settings");
  ImGuiWindow* legacy = ImGui::FindWindowByName("PC settings###legacy_old_settings");
  for (ImGuiWindow* w : GImGui->Windows)
    if (w->Active && (w->RootWindow == panel || (legacy && w->RootWindow == legacy))) scale(w->DrawList, 0);
  scale(ImGui::GetForegroundDrawList(), foreground_from);
}

// The search box (settings_search_box): the Switch's software keyboard instead of a keyboard. A asks for the words on a
// worker thread (the keyboard is a system applet that takes the screen); they arrive at a later frame and search every page.
std::atomic<bool> g_search_busy{false};
std::mutex g_search_mutex;
std::string g_search_result; bool g_search_ready = false;
void search_row() {
  const bool on = gx::g_search_text[0] != 0;
  std::string label = on ? std::string("Search: ") + gx::g_search_text : std::string("Search all settings");
  label += "###settings_search";
  if (gx::settings_widgets::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !g_search_busy.exchange(true)) {
    const std::string initial = gx::g_search_text;
    std::thread([initial] {
      std::string text = initial;
      const bool ok = fe::software_keyboard("Search all settings", "Words from a setting's name; a typo is fine.", text);
      std::lock_guard<std::mutex> lock(g_search_mutex);
      if (ok) { g_search_result = text; g_search_ready = true; }
      g_search_busy = false;
    }).detach();
  }
  gx::settings_page::label_item("Search all settings");
  {
    std::lock_guard<std::mutex> lock(g_search_mutex);
    if (g_search_ready) {
      g_search_ready = false;
      std::snprintf(gx::g_search_text, sizeof gx::g_search_text, "%s", g_search_result.c_str());
      host::log("settings: search \"%s\"", gx::g_search_text);
    }
  }
  if (on) {
    if (gx::settings_widgets::Button("Clear the search###settings_search_clear", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
      gx::g_search_text[0] = 0;
      host::log("settings: search cleared");
    }
  }
  // settings_search_box's own bookkeeping, without its text box
  gx::g_search = gx::g_search_text[0] != 0;
  gx::g_search_hits = 0;
  if (!gx::g_goto_label.empty() && ImGui::GetTime() - gx::g_goto_flash > 1.0) gx::g_goto_label.clear();
  gx::g_hidden = 0;
  gx::g_search_page = -1;
  ImGui::Spacing();
  if (gx::g_search) gx::settings_hide_begin();
}

// The radial style's home (settings_frame's radial_home branch, upstream pc_settings.cpp).
void radial_home() {
  gx::settings_home_quit(g_state, 3);
  confirm_quit_popup();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 window_size = ImGui::GetWindowSize();
  const ImVec2 center(window_pos.x + window_size.x * 0.5f, window_pos.y + window_size.y * 0.51f);
  const float radius = std::min(224.0f, std::min(window_size.x, window_size.y) * 0.315f);
  draw->AddText(ImVec2(window_pos.x + 24.0f, window_pos.y + 18.0f), IM_COL32(240, 248, 255, 255), "MELEE UNLOCKED  /  SETTINGS");
  const ImVec2 hint_min(window_pos.x + window_size.x - 238.0f, window_pos.y + 46.0f);
  const ImVec2 hint_max(window_pos.x + window_size.x - 18.0f, window_pos.y + 96.0f);
  draw->AddRectFilled(ImVec2(hint_min.x + 2, hint_min.y + 3), ImVec2(hint_max.x + 2, hint_max.y + 3), IM_COL32(0, 3, 10, 96), 12.0f);
  draw->AddRectFilled(hint_min, hint_max, IM_COL32(15, 27, 42, 235), 12.0f);
  draw->AddRect(hint_min, hint_max, IM_COL32(95, 145, 184, 205), 12.0f, 0, 1.2f);
  draw->AddText(ImVec2(hint_min.x + 12.0f, hint_min.y + 6.0f), IM_COL32(186, 204, 222, 255), "MOVE  /  D-PAD OR STICK");
  draw->AddText(ImVec2(hint_min.x + 12.0f, hint_min.y + 25.0f), IM_COL32(239, 246, 252, 255), "A  SELECT     B  BACK");
  gx::settings_radial_nav(g_state, center, radius, true);
  g_state.home_focus_reset = false;
}

// The categories beside a page, for Simple and the legacy styles (settings_frame's own steps for appearances 5, 6 and 7).
void side_categories(int style) {
  if (style == 7) {
    static constexpr const char* tabs[] = {"Video", "Audio", "Game", "Controls", "Overlays", "Customize", "Gecko Codes", "Mods"};
    if (ImGui::BeginTabBar("settings_tabs")) {
      for (int i = 0; i < 8; ++i) {
        if (gx::settings_widgets::BeginTabItem(tabs[i], nullptr, g_state.active_tab == i && g_enter_frames > 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
          g_state.active_tab = i;
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
  } else if (style == 6) {
    static constexpr const char* tabs[] = {"Video", "Audio", "Game", "Controls", "Overlays", "Customize", "Gecko Codes", "Mods"};
    ImGui::SetCursorPos(ImVec2(8, 12));
    for (int i = 0; i < 8; ++i) {
      const bool selected = g_state.active_tab == i;
      ImGui::PushID(i);
      ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(.34f, .23f, .58f, 1.0f) : ImVec4(.10f, .18f, .29f, 1.0f));
      const ImVec2 label_size = ImGui::CalcTextSize(tabs[i]);
      if (gx::settings_widgets::Button(tabs[i], ImVec2(label_size.x + 18.0f, 23.0f))) g_state.active_tab = i;
      ImGui::PopStyleColor();
      ImGui::PopID();
      if (i != 7) ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::Separator();
    ImGui::Spacing();
  } else {   // Simple: a column of tiles
    ImGui::BeginChild("##settings_navigation", ImVec2(151.0f, -80), false);
    for (int i = 0; i < 8; ++i)
      if (gx::settings_nav_tile(i, g_state.active_tab == i, style, ImVec2(151.0f - 8.0f, 48.0f))) g_state.active_tab = i;
    ImGui::EndChild();
    ImGui::SameLine();
  }
}

// One page's rows (the category's sections), inside the content child.
void draw_rows(int style) {
  if (g_state.open) {
    g_state.page_navigation.begin(g_state.active_tab, style);
    gx::settings_page::observer = &g_state.page_navigation;
  }
  search_row();
  if (style == 4) {   // Wide tabs: the page's icon and title at the top of its card
    static constexpr const char* wide_titles[] = {"Video", "Audio", "Game", "Controls", "Overlays", "Customize", "Gecko Codes", "Mods"};
    const ImVec2 header = ImGui::GetCursorScreenPos();
    gx::settings_symbol_icon(g_state.active_tab, ImVec2(header.x + 19.0f, header.y + 17.0f), 0.52f, gx::settings_palette_tint(4, IM_COL32(226, 238, 249, 255)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40.0f);
    ImGui::PushFont(gx::settings_heading_font());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(gx::settings_accent(4)), "%s", wide_titles[g_state.active_tab]);
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    ImGui::Spacing();
  }
  const std::array<Category, 8> cats = build_rows(g_in_game_feed);
  for (int page = 0; page < 8; ++page) {
    if (!gx::settings_page_shown(g_state.active_tab, page)) continue;
    const Category& c = cats[size_t(page)];
    if (c.sections.empty() && !gx::g_search) ImGui::TextDisabled("Nothing on this page on the Switch.");
    for (const Section& s : c.sections) {
      if (s.rows.empty()) continue;
      gx::settings_section(s.title.c_str());
      for (const Row& r : s.rows) {
        // A row's ID: its "###key" when it has one (a label that changes keeps its focus), else its place in the section.
        const size_t key = r.label.find("###");
        if (key != std::string::npos) ImGui::PushID(r.label.c_str() + key + 3); else ImGui::PushID(int(&r - s.rows.data()));
        ImGui::PushID(s.title.c_str()); draw_row(r); ImGui::PopID(); ImGui::PopID();
        if (g_focus_first && g_row_id && (style != 0 || g_state.clean_detail_frames >= 14.0f)) {
          ImGui::SetFocusID(g_row_id, ImGui::GetCurrentWindow());
          ImGui::SetNavCursorVisible(true);
          ImGui::SetScrollY(0.0f);
          g_focus_first = false;
        }
      }
    }
  }
  gx::settings_pages_end();
  gx::settings_page::observer = nullptr;
  if (g_state.open) if (const char* focus = g_state.page_navigation.changed_focus()) host::log("settings: focus %s", focus);
}

// A category page: each style's chrome around the rows (settings_frame's detail branch, upstream pc_settings.cpp).
void draw_page(int style, const Appearance& look) {
  const bool gd = style == 2, clean = style == 0, dashboard = style == 1, wide = style == 4, classic = style >= 6, old_menu = style == 7;
  int colours = 0, vars = 0;
  if (dashboard) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    draw->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y), IM_COL32(25, 34, 49, 246), 13.0f);
    draw->AddRectFilled(p, ImVec2(p.x + s.x, p.y + 5.0f), IM_COL32(246, 195, 55, 255), 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.31f, 0.55f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.49f, 0.82f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.60f, 0.94f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.18f, 0.34f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    colours += 4; vars += 1;
  }
  if (clean) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.06f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.12f, 0.48f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.24f, 0.58f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.07f, 0.14f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    colours += 4; vars += 1;
  }
  if (gd) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.21f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.69f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.79f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    colours += 4; vars += 1;
  }
  static constexpr const char* page_names[] = {"VIDEO", "AUDIO", "GAME", "CONTROLS", "OVERLAYS", "CUSTOMIZE", "GECKO CODES", "MODS"};
  const float gd_scale = ImGui::GetWindowWidth() / 640.0f;
  if (gd) {
    const ImVec2 origin = ImGui::GetWindowPos();
    gx::settings_gd_chrome(origin, gd_scale, page_names[g_state.active_tab]);
    gx::gd_kit_text(ImGui::GetWindowDrawList(), "body", origin, gd_scale, 104, 414.0f,
                    gx::g_settings_gd_help.empty() ? "Choose a setting. Changes save automatically." : gx::g_settings_gd_help.c_str(), IM_COL32(242, 239, 228, 255), 472);
    ImGui::SetCursorPos(ImVec2(72.0f * gd_scale, 84.0f * gd_scale));
  } else if (clean) {
    gx::mu_settings_clean_detail_header(g_state);
  } else if (dashboard) {
    gx::settings_detail_header(g_state, 1);
    ImGui::SetCursorPos(ImVec2(24, 112));
  } else if (style == 3) {
    gx::settings_detail_header(g_state, 3);
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float nav_width = std::min(320.0f, std::max(248.0f, (ws.x - 52.0f) * 0.31f));
    const ImVec2 center(wp.x + 26.0f + nav_width * 0.5f, wp.y + 112.0f + (ws.y - 160.0f) * 0.45f);
    const float radius = std::min(122.0f, std::min(nav_width * 0.43f, (ws.y - 200.0f) * 0.34f));
    gx::settings_radial_nav(g_state, center, radius, false);
  } else if (old_menu) {
    ImGui::TextUnformatted("Start or B: return to game");
  } else if (classic) {
    ImGui::SetCursorPos(ImVec2(8, 12));
  } else if (wide) {
    ImGui::SetCursorPos(ImVec2(20, 56));
  } else {   // Simple
    ImGui::SetCursorPos(ImVec2(20, 14));
    ImGui::PushFont(gx::settings_heading_font());
    ImGui::TextUnformatted("SETTINGS");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(20, 56));
  }
  ImGui::BeginDisabled(!g_state.open);
  if (!gd && !clean && style != 3 && !classic) ImGui::Separator();
  if (wide) ImGui::SetCursorPos(ImVec2(18.0f, 120.0f));
  else if (style >= 5) side_categories(style);
  const OptionsView options{style};
  if (clean) gx::mu_settings_page_entrance(g_state, options);
  else {   // the other styles' entrance (the same function; its Clean branch is not taken)
    gx::mu_settings_page_entrance(g_state, options);
  }
  if (wide) {
    const ImVec2 window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 a(window.x + 24.0f, window.y + 108.0f), b(window.x + size.x - 24.0f, window.y + size.y - 50.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(a.x, a.y + 4.0f), ImVec2(b.x, b.y + 4.0f), IM_COL32(0, 0, 0, 55), 14.0f);
    draw->AddRectFilled(a, b, IM_COL32(12, 21, 32, 246), 14.0f);
    draw->AddRect(a, b, IM_COL32(76, 105, 132, 150), 14.0f, 0, 1.0f);
  }
  int child_colours = 0;
  if (style <= 4) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, gd ? ImVec4(0, 0, 0, 0) : clean ? ImVec4(0.035f, 0.025f, 0.055f, 0.78f) :
                                            dashboard ? ImVec4(0.055f, 0.12f, 0.22f, 0.96f) : ImVec4(0, 0, 0, 0));
    child_colours = 1;
  }
  gx::g_settings_form_style = gd ? 2 : style;
  gx::g_settings_gd_origin = ImGui::GetWindowPos();
  gx::g_settings_gd_scale = gd_scale;
  if (style == 3 || wide) {
    const ImVec2 window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + ImGui::GetContentRegionAvail().x, window.y + size.y - 54.0f);
    if (style == 3) {
      const float nav_width = std::min(320.0f, std::max(248.0f, (size.x - 52.0f) * 0.31f));
      a = ImVec2(window.x + 26.0f + nav_width + 36.0f, window.y + 112.0f);
      b = ImVec2(window.x + size.x - 64.0f, window.y + size.y - 54.0f);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(a.x - 5, a.y - 7), ImVec2(b.x + 5, b.y + 4), style == 3 ? IM_COL32(13, 22, 35, 250) : IM_COL32(7, 19, 35, 204), 18.0f);
    draw->AddRect(ImVec2(a.x - 5, a.y - 7), ImVec2(b.x + 5, b.y + 4),
                  style == 3 ? IM_COL32(99, 119, 139, 112) : gx::settings_palette_tint(style, IM_COL32(85, 150, 208, 142)), 18.0f, 0, 1.5f);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, gd ? ImVec2(0, 0) : classic ? ImVec2(8.0f, 5.0f) : style == 3 ? ImVec2(24.0f, 20.0f) :
                                                   wide ? ImVec2(26.0f, 22.0f) : ImVec2(clean ? 18.0f : 24.0f, 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, old_menu ? ImVec2(5.0f, 3.75f) : classic ? ImVec2(4.0f, 2.0f) : style == 3 ? ImVec2(14.0f, 9.0f) : ImVec2(12.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, old_menu ? ImVec2(10.0f, 5.0f) : classic ? ImVec2(5.0f, 4.0f) : gd ? ImVec2(4.0f * gd_scale, 4.0f * gd_scale) : ImVec2(12.0f, 12.0f));
  constexpr float kGdListTop = 84.0f, kGdHintTop = 398.0f, kGdHintGap = 8.0f, kGdRowHeight = 30.0f, kGdRowSpacing = 4.0f, kGdRowPitch = kGdRowHeight + kGdRowSpacing;
  const float gd_list_height = std::floor((kGdHintTop - kGdHintGap - kGdListTop + kGdRowSpacing) / kGdRowPitch) * kGdRowPitch - kGdRowSpacing;
  ImVec2 content_size = gd ? ImVec2(532.0f * gd_scale, gd_list_height * gd_scale) : ImVec2(0, -48);
  if (clean) content_size.y = -84.0f;
  if (style == 3) {
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float nav_width = std::min(320.0f, std::max(248.0f, (ws.x - 52.0f) * 0.31f));
    const float content_x = 26.0f + nav_width + 36.0f;
    ImGui::SetCursorScreenPos(ImVec2(wp.x + content_x, wp.y + 112.0f));
    content_size.x = std::max(300.0f, ws.x - content_x - 64.0f);
  }
  const ImVec2 content_origin = ImGui::GetCursorPos();
  ImGui::BeginChild("##settings_content", content_size, ImGuiChildFlags_NavFlattened | ((style == 3 || wide) ? ImGuiChildFlags_AlwaysUseWindowPadding : 0),
                    ImGuiWindowFlags_NoScrollWithMouse);
  gx::mu_settings_smooth_scroll(gd);
  ImGui::PushTextWrapPos(0.0f);
  draw_rows(style);
  ImGui::PopTextWrapPos();
  const ImVec2 child_size = ImGui::GetWindowSize();
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  if (child_colours) ImGui::PopStyleColor(child_colours);
  ImGui::EndDisabled();
  ImGui::SetCursorPos(ImVec2(content_origin.x, content_origin.y + child_size.y));
  ImGui::PopStyleVar();    // the page entrance's alpha
  gx::settings_page_footer(g_state, style);
  if (!has_home(style)) g_state.home_focus_reset = false;
  confirm_quit_popup();
  if (vars) ImGui::PopStyleVar(vars);
  if (colours) ImGui::PopStyleColor(colours);
  (void)look;
}

void draw_panel(int w, int h) {
  const bool model_open = g_open.load();
  const Appearance look = read_appearance();
  const int style = look.style;
  static int last_style = -1;
  if (model_open != g_ui_open || (model_open && g_jump.exchange(false))) {
    g_ui_open = model_open;
    g_state.open = model_open;
    if (!model_open) { g_reopen_page = g_last_page; g_legacy = false; }   // where it was left: the next open returns there (this session)
    if (model_open) {
      g_state.home_focus_reset = true;
      g_state.clean_detail_open = g_state.dashboard_detail_open = g_state.gd_detail_open = g_state.radial_detail_open = g_state.wide_detail_open = false;
      g_state.dashboard_home_frames = 0; g_state.gd_screen_frames = 0;
      if (g_reopen_page >= 0) { g_state.active_tab = g_reopen_page; g_state.home_focus_reset = false; g_enter_frames = 3; }
      std::string title;
      { std::lock_guard<std::mutex> lock(g_mutex); title = g_open_title; g_open_title.clear(); }
      if (!title.empty()) {
        int cat = 2; std::string sec;
        place(title, &cat, &sec);
        // As a player gets there: the category's row is focused on the category list, then entered (A) a frame later.
        g_state.active_tab = cat; g_state.home_focus_reset = false; g_enter_frames = 3;
        g_scroll_to = sec;
      }
      g_state.radial_selection = g_state.active_tab;
      host::log("settings: opened on %s%s%s (style %s)", kCategory[std::clamp(g_state.active_tab, 0, 7)], g_scroll_to.empty() ? "" : " > ", g_scroll_to.c_str(), kStyleNames[style]);
      g_scroll_to.clear();
    }
  }
  if (last_style != style) {   // a new style starts on its own first page, the category kept
    if (last_style >= 0 && g_state.open) { host::log("settings: menu style %s", kStyleNames[style]); g_enter_frames = 3; g_state.home_focus_reset = false; }
    last_style = style;
  }
  bool& detail = detail_flag(style);
  // B goes back a page, then closes; Start closes from any page (settings_frame's own handling).
  if (g_state.open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !g_remap_on.load()) {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) g_state.open = false;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
      if (has_home(style) && detail) detail = false;
      else if (style == 4 && g_state.wide_detail_open) g_state.wide_detail_open = false;
      else g_state.open = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && ini_int("legacymenu", 0, 0, 1)) { g_legacy = !g_legacy; host::log("settings: legacy menu %s", g_legacy ? "on" : "off"); }
  }
  if (g_state.open && g_enter_frames == 0) g_last_page = (!has_home(style) || detail) ? g_state.active_tab : -1;
  gx::mu_settings_motion(g_state);
  const bool visible = g_state.open || g_state.panel_slide_x > -720.0f;
  if (!visible) { if (!g_state.open && g_ui_open) { g_close_request = true; } return; }

  gx::g_settings_palette = look.palette;
  gx::g_settings_custom_color_enabled = look.custom;
  gx::g_settings_custom_color = look.accent;
  const int foreground_from = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
  const gx::SettingsAppearanceScope appearance_scope(style);
  const bool classic = style == 6, old_menu = style == 7;
  int classic_colours = 0, classic_vars = 0;
  ImFont* plain = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];   // ImGui's own (the legacy menus' fixed font)
  if (classic) {
    // The compact v0.6.6 screen (settings_frame's appearance 6): black canvas, navy controls, purple selected tab. Its Lucida
    // Console is a Windows font: ImGui's own fixed font here.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.045f, .048f, .055f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.055f, .062f, .075f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.88f, .90f, .93f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(.48f, .51f, .57f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.105f, .16f, .24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.24f, .20f, .38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.10f, .18f, .29f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.30f, .22f, .47f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.34f, .23f, .58f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.20f, .22f, .26f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    classic_colours = 10; classic_vars = 3;
  }
  if ((classic || old_menu) && plain) ImGui::PushFont(plain);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float motion_scale = std::max(0.1f, display.y / 480.0f);
  const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (old_menu) {
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, display.x - 20.0f), std::min(620.0f, display.y - 20.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(10 + g_state.panel_slide_x * motion_scale * 0.0f, 10), ImGuiCond_Always);
    ImGui::Begin("PC settings###legacy_old_settings", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  } else {
    const float width = style == 0 ? std::min(560.0f, std::max(390.0f, display.x * 0.38f)) : style == 1 ? std::min(1040.0f, display.x - 24.0f) :
                        style == 2 ? std::min(display.x - 12.0f, (display.y - 12.0f) * 4.0f / 3.0f) : style == 3 ? display.x - 24.0f :
                        style == 4 ? std::min(960.0f, display.x - 24.0f) : style == 6 ? std::min(980.0f, display.x - 40.0f) : std::min(860.0f, display.x - 24.0f);
    const float pw = std::max(300.0f, std::min(width, display.x - 12.0f));
    const float ph = style == 3 ? std::max(320.0f, display.y - 12.0f) : style == 4 || style == 6 ? std::max(360.0f, std::min(710.0f, display.y - 40.0f)) :
                     style == 2 ? pw * 3.0f / 4.0f : std::max(320.0f, std::min(790.0f, display.y - 12.0f));
    const ImVec2 base = style == 0 ? ImVec2(display.x - pw, 0) : ImVec2((display.x - pw) * 0.5f, (display.y - ph) * 0.5f);
    const float slide_x = style == 0 ? std::abs(g_state.panel_slide_x) : g_state.panel_slide_x;
    const float slide_scale = std::max(motion_scale, pw / 720.0f);
    ImGui::SetNextWindowPos(ImVec2(base.x + slide_x * slide_scale, base.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pw, style == 0 ? display.y : ph), ImGuiCond_Always);
    const bool scene_chrome = style <= 4;
    ImGui::SetNextWindowBgAlpha(scene_chrome ? 0.0f : classic ? 1.0f : 0.97f);
    ImGui::Begin("PC settings", nullptr, fixed | (scene_chrome ? ImGuiWindowFlags_NoBackground : 0));
  }
  if (g_state.open && (g_state.home_focus_reset || ImGui::IsWindowAppearing())) {
    ImGui::SetWindowFocus();
    ImGui::SetNavCursorVisible(true);   // a controller-only panel: the focus frame shows, and the first A acts
  }
  {   // Entering and leaving a category page: the log line, the first row's focus, the category's own focus on the way back
    static bool was_detail = false;
    static int was_style = -1;
    if (was_style != style) { was_detail = !has_home(style); was_style = style; if (!has_home(style)) { g_focus_first = true; } }
    if (was_detail != detail && has_home(style)) host::log("settings: %s", detail ? kCategory[std::clamp(g_state.active_tab, 0, 7)] : "categories");
    if (g_enter_frames > 0) {
      if (has_home(style) && !detail) {
        if (g_enter_frames == 3) was_detail = true;          // focus the row as Back does
        else if (g_enter_frames == 1) { detail = true; g_state.radial_selection = g_state.active_tab; }
      } else if (!has_home(style) && g_enter_frames == 1) {
        g_focus_first = true;
        host::log("settings: %s", kCategory[std::clamp(g_state.active_tab, 0, 7)]);
        if (style == 4) g_state.wide_detail_open = true;
      }
      --g_enter_frames;
    }
    static int was_tab = -1;
    if (!has_home(style) && g_state.open && was_tab != g_state.active_tab && was_tab >= 0 && g_enter_frames == 0) {
      host::log("settings: %s", kCategory[std::clamp(g_state.active_tab, 0, 7)]);
      g_state.page_navigation.tab = g_state.active_tab; g_state.page_navigation.style = style; g_state.page_navigation.logged_focus = 0;
    }
    was_tab = g_state.active_tab;
    if (!was_detail && detail) {   // a page entered: its first row gets the focus (set below, by its ID)
      g_state.page_navigation.tab = g_state.active_tab; g_state.page_navigation.style = style; g_state.page_navigation.logged_focus = 0;
      g_focus_first = true;
    }
    if (was_detail && !detail && home_item_id(style)) {
      ImGui::PushID(g_state.active_tab);
      const ImGuiID id = ImGui::GetID(home_item_id(style));
      ImGui::PopID();
      ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
      ImGui::SetNavCursorVisible(true);
    }
    was_detail = detail;
  }
  if (style == 0 && !gx::g_menu_style_gallery_open)
    gx::settings_clean_chrome(g_state.clean_detail_open ? kCategory[std::clamp(g_state.active_tab, 0, 7)] : "SETTINGS", g_state.panel_slide_x);
  if (style == 3) gx::settings_radial_scene();
  if (style == 4) gx::settings_wide_home(g_state);
  const bool clean_home = style == 0 && !g_state.clean_detail_open && g_state.clean_detail_frames <= 0.0f;
  if (clean_home) {
    gx::settings_clean_home(g_state);
    if (g_in_game_feed) gx::settings_home_quit(g_state, 0);
    confirm_quit_popup();
  } else if (style == 1 && !detail) {
    gx::settings_dashboard_home(g_state);
    if (g_in_game_feed) gx::settings_home_quit(g_state, 1);
    confirm_quit_popup();
  } else if (style == 2 && !detail) {
    gx::settings_gd_home(g_state);
    if (g_in_game_feed) gx::settings_home_quit(g_state, 2);
    confirm_quit_popup();
  } else if (style == 3 && !detail) {
    radial_home();
  } else {
    draw_page(style, look);
  }
  ImGui::End();
  if ((classic || old_menu) && plain) ImGui::PopFont();
  if (classic_vars) ImGui::PopStyleVar(classic_vars);
  if (classic_colours) ImGui::PopStyleColor(classic_colours);
  apply_transparency(look.transparency, foreground_from);
  if (g_remap_on.load()) draw_remap(int(display.x), int(display.y));
  // Menu sounds (settings_frame): a tick when the category or the focused control changes, a brighter one when a page or the
  // panel opens, a lower one when it closes.
  {
    static bool snd_init = false, snd_open = false;
    static int snd_tab = 0, snd_detail = 0;
    static ImGuiID snd_focus = 0;
    const int detail_now = (g_state.clean_detail_open ? 1 : 0) | (g_state.dashboard_detail_open ? 2 : 0) | (g_state.gd_detail_open ? 4 : 0) |
                           (g_state.radial_detail_open ? 8 : 0) | (g_state.wide_detail_open ? 16 : 0);
    const ImGuiID focus_now = g_state.open ? ImGui::GetFocusID() : 0;
    int sound = 0;
    if (snd_init && ini_int("menusounds", 0, 0, 1) && g_in_game_feed) {
      if (g_state.open != snd_open) sound = g_state.open ? 2 : 3;
      else if (g_state.open && detail_now != snd_detail) sound = detail_now ? 2 : 3;
      else if (g_state.open && (g_state.active_tab != snd_tab || (focus_now && focus_now != snd_focus))) sound = 1;
    }
    if (sound) { g_ui_sound.store(sound); static int logged = 0; if (logged++ < 8) host::log("settings: menu sound %d", sound); }
    snd_init = true; snd_open = g_state.open; snd_tab = g_state.active_tab; snd_detail = detail_now; snd_focus = focus_now;
  }
  if (!g_state.open && g_ui_open) g_close_request = true;   // the panel asked to close: the model closes at its next poll
  (void)w; (void)h;
}

// The note when skins are off online (upstream's draw_skin_online_notice, ini skin_online_notice, on by default): once per
// visit to the online screens, for 15 seconds, when a picked skin is served as the standard costume there.
void draw_skin_online_notice() {
  static bool shown_this_visit = false;
  static double show_until = 0.0;
  const bool online = g_online_now.load();
  if (!online || !&fe::features::cosmetics_off_online || !ini_int("skin_online_notice", 1, 0, 1)) { if (!online) { shown_this_visit = false; show_until = 0.0; } return; }
  uint32_t stages = 0;
  const uint32_t skins = fe::features::cosmetics_off_online(&stages);
  if (!shown_this_visit && skins + stages > 0) {
    shown_this_visit = true;
    show_until = ImGui::GetTime() + 15.0;
    host::log("settings: skins off online note (%u skins, %u stages)", skins, stages);
  }
  if (ImGui::GetTime() >= show_until || skins + stages == 0) return;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, 64), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::Begin("SkinOnlineNotice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  const ImVec4 yellow(1, .75f, .25f, 1);
  if (skins == 1) ImGui::TextColored(yellow, "1 skin is off for this online mode and shows as the standard costume.");
  else if (skins > 1) ImGui::TextColored(yellow, "%u skins are off for this online mode and show as the standard costume.", skins);
  if (stages == 1) ImGui::TextColored(yellow, "1 stage mod is off for this online mode and shows as the standard stage.");
  else if (stages > 1) ImGui::TextColored(yellow, "%u stage mods are off for this online mode and show as the standard stage.", stages);
  ImGui::TextUnformatted("Settings > Mods lists them.");
  ImGui::End();
}

// The "Settings: F1" reminder (upstream's settingsreminder, in its controller form): in menus only, top right, brighter for its
// first 20 seconds; never in a match or a replay.
void draw_reminder() {
  if (g_open.load() || !g_in_game_feed || g_in_match.load() || !ini_int("settingsreminder", 0, 0, 1)) return;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(ImGui::GetTime() < 20.0 ? 0.8f : 0.35f);
  ImGui::Begin("SettingsButton", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextUnformatted("Settings: L + R + START  /  START + DOWN + Z");
  ImGui::End();
  static bool logged = false;
  if (!logged) { logged = true; host::log("settings: reminder shown"); }
}

// ---- fonts and artwork ----
ImFont* g_heading = nullptr;
ImFont* g_body = nullptr;
// A Windows BMP as melee-unlocked ships its launcher art: 24-bit bottom-up, or 32-bit premultiplied top-down cutouts.
mu_ui::Art decode_bmp(const uint8_t* b, const uint8_t* e) {
  mu_ui::Art art;
  const size_t n = size_t(e - b);
  if (n < 54 || b[0] != 'B' || b[1] != 'M') return art;
  auto u32 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; };
  const uint32_t off = u32(10);
  const int w = int(int32_t(u32(18)));
  int h = int(int32_t(u32(22)));
  const int bpp = b[28] | b[29] << 8;
  const bool top_down = h < 0;
  h = std::abs(h);
  if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32)) return art;
  const size_t stride = (size_t(w) * (bpp / 8) + 3) & ~size_t(3);
  if (off + stride * size_t(h) > n) return art;
  std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = b + off + stride * size_t(top_down ? y : h - 1 - y);
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = row + size_t(x) * (bpp / 8);
      uint8_t* o = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      const uint8_t a = bpp == 32 ? p[3] : 255;
      // Premultiplied in the file; ImGui blends straight alpha.
      auto un = [&](uint8_t c) { return a == 0 ? uint8_t(0) : uint8_t(std::min(255, (c * 255 + a / 2) / a)); };
      o[0] = bpp == 32 ? un(p[2]) : p[2]; o[1] = bpp == 32 ? un(p[1]) : p[1]; o[2] = bpp == 32 ? un(p[0]) : p[0]; o[3] = a;
    }
  }
  art.texture = reinterpret_cast<void*>(aurora_imgui_add_texture(uint32_t(w), uint32_t(h), rgba.data()));
  art.w = w; art.h = h;
  return art;
}
// The menu sound being played (audio thread of play_io.cpp's mixer: the simulation thread).
struct UiTick { int remaining = 0, total = 0; double step = 0, phase = 0, level = 0; } g_tick;

// An 8-bit RGBA or RGB PNG, not interlaced (the GD kit's files): zlib's inflate and the five row filters.
bool decode_png(const uint8_t* p, size_t n, int* w_out, int* h_out, std::vector<uint8_t>& rgba) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (n < 8 || std::memcmp(p, sig, 8) != 0) return false;
  auto be32 = [](const uint8_t* q) { return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]; };
  uint32_t w = 0, h = 0; int channels = 0;
  std::vector<uint8_t> idat;
  for (size_t at = 8; at + 12 <= n;) {
    const uint32_t len = be32(p + at); const uint8_t* type = p + at + 4; const uint8_t* data = p + at + 8;
    if (at + 12 + len > n) return false;
    if (!std::memcmp(type, "IHDR", 4)) {
      w = be32(data); h = be32(data + 4);
      if (data[8] != 8 || data[12] != 0 || (data[9] != 6 && data[9] != 2)) return false;
      channels = data[9] == 6 ? 4 : 3;
    } else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), data, data + len);
    else if (!std::memcmp(type, "IEND", 4)) break;
    at += 12 + len;
  }
  if (!w || !h || !channels || w > 4096 || h > 4096) return false;
  const size_t stride = size_t(w) * channels;
  std::vector<uint8_t> raw((stride + 1) * h);
  uLongf out = uLongf(raw.size());
  if (uncompress(raw.data(), &out, idat.data(), uLong(idat.size())) != Z_OK || out != raw.size()) return false;
  std::vector<uint8_t> cur(stride), prev(stride, 0);
  rgba.assign(size_t(w) * h * 4, 255);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t filter = raw[y * (stride + 1)];
    const uint8_t* src = &raw[y * (stride + 1) + 1];
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= size_t(channels) ? cur[x - channels] : 0, b = prev[x], c = x >= size_t(channels) ? prev[x - channels] : 0;
      int v = src[x];
      switch (filter) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: { const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                  v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; break; }
        default: break;
      }
      cur[x] = uint8_t(v);
    }
    for (uint32_t x = 0; x < w; ++x)
      for (int k = 0; k < channels; ++k) rgba[(size_t(y) * w + x) * 4 + k] = cur[size_t(x) * channels + k];
    std::swap(cur, prev);
  }
  *w_out = int(w); *h_out = int(h);
  return true;
}
}  // namespace

namespace mu_ui {
int g_ui_generation = 0;
ImGuiContext* g_font_context = nullptr;
int ui_generation() { return g_ui_generation; }
void fonts_init() {
  if (!ImGui::GetCurrentContext()) return;
  if (ImGui::GetCurrentContext() != g_font_context) {   // a new context: its atlas has none of the fonts loaded before
    g_font_context = ImGui::GetCurrentContext(); ++g_ui_generation; g_heading = g_body = nullptr;
  }
  if (g_heading) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();   // stays the default: every other overlay keeps its look
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;                        // the embedded files stay where they are
  config.OversampleH = 2;
  // The launcher's Japanese, Chinese and Korean (mu_lang.cpp): Source Sans 3 has no CJK, so the console's own shared fonts are
  // merged into both faces (upstream picks a CJK face per script on Windows). Off the console none is found: those show as boxes.
  const auto merge_cjk = [&](float size) {
#ifdef __SWITCH__
    static bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (!pl) return;
    for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified, PlSharedFontType_KO}) {
      PlFontData font{};
      if (R_FAILED(plGetSharedFontByType(&font, type)) || !font.address) continue;
      ImFontConfig merge;
      merge.MergeMode = true;
      merge.FontDataOwnedByAtlas = false;
      io.Fonts->AddFontFromMemoryTTF(font.address, int(font.size), size, &merge);
    }
#else
    (void)size;
#endif
  };
  std::snprintf(config.Name, sizeof config.Name, "Source Sans 3 Black");
  g_heading = io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(mu_ui_font_black), int(mu_ui_font_black_end - mu_ui_font_black), 28.0f, &config);
  merge_cjk(28.0f);
  std::snprintf(config.Name, sizeof config.Name, "Source Sans 3 Bold");
  g_body = io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(mu_ui_font_bold), int(mu_ui_font_bold_end - mu_ui_font_bold), 16.0f, &config);
  merge_cjk(16.0f);
  gx::g_settings_heading_font = g_heading;
  gx::g_settings_body_font = g_body;
  aurora_imgui_host_gamepad(true);          // patches/0115: the panel's gamepad keys come from the app's controllers only
  io.ConfigInputTrickleEventQueue = true;   // settings_input::configure: press and release pairs kept between frames
  say("ui: melee-unlocked's fonts loaded (Source Sans 3 Black and Bold)");
}
ImFont* heading_font() { return g_heading; }
ImFont* body_font() { return g_body; }
const Art& bailey() { static Art a; static int gen = -1; if (gen != g_ui_generation) { gen = g_ui_generation; a = decode_bmp(mu_ui_bailey, mu_ui_bailey_end); } return a; }
const Art& mark() { static Art a; static int gen = -1; if (gen != g_ui_generation) { gen = g_ui_generation; a = decode_bmp(mu_ui_mark, mu_ui_mark_end); } return a; }

void panel_feed(const host::PadState pads[4]) {
  if (ImGui::GetCurrentContext()) feed_imgui(pads);
}

void panel_draw(int window_w, int window_h) {
  if (!ImGui::GetCurrentContext()) return;
  ImGuiIO& io = ImGui::GetIO();
  {   // a panel that has just opened waits for the controller to be let go before it reads it
    static bool was_open = false;
    const bool now_open = g_open.load();
    if (now_open && !was_open) g_armed = false;
    was_open = now_open;
  }
  if (g_in_game_feed) {   // the game's controllers, as the menu's poll left them
    host::PadState pads[4]; uint64_t seq;
    { std::lock_guard<std::mutex> lock(g_mutex); std::memcpy(pads, g_nav, sizeof pads); seq = g_nav_seq; }
    if (seq != g_fed_seq) { g_fed_seq = seq; feed_imgui(pads); }
  }
  const bool open = g_open.load();
  gx::settings_input::navigation(io, open);
  // The panel is driven by the controller (a touch or a mouse pointer resting over it must not pick categories).
  // Its focus frame always shows (there is no pointer to hide it for).
  const bool shown = open || g_state.panel_slide_x > -720.0f;
  if (shown) io.ConfigFlags |= ImGuiConfigFlags_NoMouse; else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
  io.ConfigNavCursorVisibleAlways = shown;
  if (!open) {
    draw_reminder(); draw_skin_online_notice();
    // The easter egg over the game picture (4:3 in the middle of the screen, or the whole width with widescreen).
    if (g_in_game_feed && !g_in_match.load()) {
      const ImVec2 d = io.DisplaySize;
      const float gw = std::min(d.x, d.y * 4.0f / 3.0f);
      egg_draw((d.x - gw) * 0.5f, 0, (d.x + gw) * 0.5f, d.y, ImGui::GetTime());
    }
  }
  if (!open && g_state.panel_slide_x <= -720.0f && !g_ui_open && g_state.panel_anim_initialized) return;
  // melee-unlocked's style and fonts for the panel only; ImGui's own default for everything drawn after it.
  const ImGuiStyle saved = ImGui::GetStyle();
  gx::mu_settings_style();
  if (g_body) ImGui::PushFont(g_body);
  draw_panel(window_w, window_h);
  if (g_body) ImGui::PopFont();
  ImGui::GetStyle() = saved;
}

void panel_service(bool in_game) {
  std::vector<Action> actions;
  bool quit = false;
  std::vector<std::pair<host::RuntimeSettings, host::MenuChange>> applies;
  std::vector<std::pair<std::function<bool(int, int)>, std::pair<int, int>>> extra;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_toggle_request.exchange(false)) set_open_locked(!g_open.load());
    if (g_close_request.exchange(false)) set_open_locked(false);
    actions.swap(g_actions);
    for (const Action& a : actions) {
      switch (a.act) {
        case Act::Main: {
          const int what = set_main_locked(host::MenuChange(a.a), a.value);
          if (what >= 0) { g_changed = true; applies.push_back({g_settings, host::MenuChange(what)}); }
          break;
        }
        case Act::Extra:
          if (a.a >= 0 && a.a < int(g_extra.size()) && g_extra[size_t(a.a)].change) {
            // Toggles and actions are the row's A (the old menu's press); a stepped value its direction.
            extra.push_back({g_extra[size_t(a.a)].change, {a.b, a.value <= 0 ? (a.value < 0 ? -1 : 0) : 1}});
          }
          break;
        case Act::Controls: if (controls_act_locked(a.a, a.a == C_DEVICE || a.a == C_SWAP || a.a == C_RUMBLE ? (a.value ? a.value : 1) : a.value)) g_changed = true; break;
        case Act::Remap:
          if (device_locked()) { g_step = 0; g_step_frames = 0; g_step_armed = false; g_remap_on = true; host::window_take_key_press(); }
          break;
        case Act::Quit: if (in_game) quit = true; else g_home_quit = true; break;
        case Act::Ini: break;
        case Act::Setting: break;
        case Act::Practice: set_open_locked(false); break;
        default: break;   // the mod actions, below (outside the lock)
      }
    }
  }
  for (const auto& [s, what] : applies) if (g_apply) g_apply(s, what);
  for (auto& [change, args] : extra) {
    // A toggle row of an app page flips with the A press (dir 0 for action rows, 1 for a value row).
    if (change && change(args.first, args.second == 0 ? 0 : args.second)) g_changed = true;
  }
  for (const Action& a : actions) {
    if (a.act == Act::Ini) {
      int v = a.value;
      if (!a.text.empty()) {   // a choice whose saved values differ from its places ("7,6")
        std::vector<int> values; std::stringstream ss(a.text); std::string part;
        while (std::getline(ss, part, ',')) values.push_back(std::atoi(part.c_str()));
        if (v >= 0 && v < int(values.size())) v = values[size_t(v)];
      }
      fe::app_ini_set(a.key, std::to_string(v));
      say("menu: %s %d", a.key.c_str(), v);
      if (a.key == "menusounds" && v) g_ui_sound.store(2);   // a sample, so the change is heard
    }
    if (a.act == Act::ModPrimary) mu_mods::primary(a.key);
    if (a.act == Act::Gecko && &fe::features::gecko_toggle) fe::features::gecko_toggle(a.key);
    if (a.act == Act::ModRemove) mu_mods::remove(a.key);
    if (a.act == Act::ModProfile) {
      const std::vector<std::string> choices = mu_mods::profile_choices();
      if (a.value >= 0 && a.value < int(choices.size())) mu_mods::profile_set(choices[size_t(a.value)]);
    }
    if (a.act == Act::Setting && a.key == "mod_profile") {
      const std::vector<std::string> profiles = mod_profiles();
      const std::string v = a.value > 0 && a.value <= int(profiles.size()) ? profiles[size_t(a.value - 1)] : std::string();
      if (v.empty()) settings_write_key("mod_profile", ""), settings_set_quiet("mod_profile", "");
      else settings_change("mod_profile", v);
      say("menu: mod profile %s from the next start", v.empty() ? "none" : v.c_str());
    }
  }
  for (const Action& a : actions) if (a.act == Act::Practice && in_game && &fe::features::practice_menu_chord) fe::features::practice_menu_chord(0);
  if (quit) {
    host::log("menu: quit");
    if (g_quit_hook) g_quit_hook(); else std::exit(0);
  }
  (void)in_game;
}

bool panel_open() { return g_open.load(); }
void panel_home_mode(bool on) { g_in_game_feed = !on; if (on) g_armed = false; }
void panel_open_from_launcher() {
  // The launcher's Settings: the legacy menu when it is enabled (upstream: "F11 and launcher Settings open the selected legacy menu").
  g_legacy = ini_int("legacymenu", 0, 0, 1) != 0;
  host::menu_open_extra("");
}
void note_session(bool fighting, bool online) { g_in_match.store(fighting); g_online_now.store(online); }
bool ui_sound_pending() { return g_ui_sound.load(std::memory_order_relaxed) != 0 || g_tick.remaining > 0; }
void ui_sound_mix(int16_t* rl, uint32_t frames, uint32_t rate) {
  // upstream audio.cpp mix_ui_sound: 740 Hz for 30 ms (a move), 990 Hz (open) or 590 Hz (close) for 55 ms, quiet, decaying.
  if (const int kind = g_ui_sound.exchange(0)) {
    const double freq = kind == 1 ? 740.0 : kind == 2 ? 990.0 : 590.0;
    const double seconds = kind == 1 ? 0.030 : 0.055;
    g_tick.total = g_tick.remaining = int(rate * seconds);
    g_tick.step = 6.283185307179586 * freq / rate;
    g_tick.phase = 0.0;
    g_tick.level = (kind == 1 ? 0.045 : 0.065) * 32767.0;
  }
  for (uint32_t i = 0; i < frames && g_tick.remaining > 0; ++i, --g_tick.remaining) {
    const double env = double(g_tick.remaining) / g_tick.total;
    const double v = std::sin(g_tick.phase) * env * env * g_tick.level;
    g_tick.phase += g_tick.step;
    for (int c = 0; c < 2; ++c) rl[i * 2 + c] = int16_t(std::clamp(rl[i * 2 + c] + v, -32768.0, 32767.0));
  }
}
Art picture_file(const std::string& path) {
  Art art;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return art;
  std::vector<uint8_t> bytes;
  uint8_t buf[65536]; size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() < (32u << 20)) bytes.insert(bytes.end(), buf, buf + n);
  std::fclose(f);
  if (bytes.size() > 2 && bytes[0] == 'B' && bytes[1] == 'M') return decode_bmp(bytes.data(), bytes.data() + bytes.size());
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  if (!decode_png(bytes.data(), bytes.size(), &w, &h, rgba)) return art;
  art.texture = reinterpret_cast<void*>(aurora_imgui_add_texture(uint32_t(w), uint32_t(h), rgba.data()));
  art.w = w; art.h = h;
  return art;
}
void* gd_texture(const std::string& name) {
  // gd_melee/kit/<file>.png -> the embedded copy (gen/mu_ui/gd_assets.inc), decoded once and uploaded.
  struct Asset { const char* name; const uint8_t* begin; const uint8_t* end; };
#define MU_GD_ASSET(n) {#n, mu_gd_##n, mu_gd_##n##_end},
  static const Asset assets[] = {
#include "gd_assets.inc"
  };
#undef MU_GD_ASSET
  const size_t slash = name.find_last_of('/');
  std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".png") == 0) base.resize(base.size() - 4);
  for (const Asset& a : assets) {
    if (base != a.name) continue;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!decode_png(a.begin, size_t(a.end - a.begin), &w, &h, rgba)) { host::log("ui: GD Melee picture %s could not be read", a.name); return nullptr; }
    return reinterpret_cast<void*>(aurora_imgui_add_texture(uint32_t(w), uint32_t(h), rgba.data()));
  }
  return nullptr;
}
bool panel_take_quit() { return g_home_quit.exchange(false); }
void panel_open_on(const std::string& title) { host::menu_open_extra(title); }
void panel_close() { host::menu_close(); }
bool practice_available() { return &fe::features::practice_menu_chord != nullptr; }
}  // namespace mu_ui
}  // namespace fe

// ---- the menu interface (game_menu.h) ----
namespace host {
void menu_init(const RuntimeSettings& initial, std::function<void(const RuntimeSettings&, MenuChange)> apply) {
  std::lock_guard<std::mutex> lock(fe::g_mutex);
  fe::g_settings = initial; fe::g_apply = std::move(apply);
  if (const char* e = std::getenv("MELEE_HUD")) fe::g_settings.hud = *e && *e != '0';
  if (const char* open = std::getenv("MELEE_MENU_OPEN")) fe::g_open_at_frame = std::atoi(open);   // screenshot aid: open at this retrace
}
bool menu_is_open() { return fe::g_open.load(); }
bool menu_capturing() { return fe::g_open.load() && fe::g_remap_on.load(); }
bool menu_changed() { return fe::g_changed.load(); }
RuntimeSettings menu_settings() { std::lock_guard<std::mutex> lock(fe::g_mutex); return fe::g_settings; }
void menu_toggle() { fe::g_toggle_request.store(true); }
void menu_close() { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::set_open_locked(false); }
void menu_open_extra(const std::string& title) {
  std::lock_guard<std::mutex> lock(fe::g_mutex);
  fe::g_open_title = title;
  if (fe::g_open.load()) fe::g_jump = true;   // already open: the render thread jumps there
  fe::set_open_locked(true);
}
void menu_open_replays() { menu_open_extra("Replays"); }
void menu_set_notice(const std::string& text) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_notice = text; }
void menu_set_replays(std::function<std::vector<std::string>()>, std::function<void(const std::string&)>) {}   // the Replays page of the feature modules lists them
void menu_set_extra_pages(std::vector<MenuExtraPage> pages) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_extra = std::move(pages); }
void menu_hide(MenuChange what) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_hidden.insert(int(what)); }
void menu_set_label(MenuChange what, const std::string& label) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_labels[int(what)] = label; }
void menu_set_quit(std::function<void()> quit) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_quit_hook = std::move(quit); }
void menu_set_hud_extra(std::function<std::string()> text) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_hud_extra = std::move(text); }
void menu_set_controls_live(bool live) { std::lock_guard<std::mutex> lock(fe::g_mutex); fe::g_controls_live = live; }
bool menu_touch(float, float) { return false; }

// play_io.cpp's menu_frame calls this with the game's controllers (simulation thread): the open combo, the panel's
// queued changes, the remap's capture, and neutral controllers for the game while the panel is open.
void app_menu_frame(PadState pads[4]) {
  fe::egg_watch_pads(pads);   // the easter egg's code: read only, before anything here changes the pads
  {
    std::lock_guard<std::mutex> lock(fe::g_mutex);
    if (fe::g_open_at_frame >= 0 && int(retrace_count()) >= fe::g_open_at_frame) {
      fe::g_open_at_frame = -1; fe::set_open_locked(true);
      if (const char* page = std::getenv("MELEE_MENU_PAGE")) fe::g_open_title = page;
    }
    // L + R + Start held on any connected controller for half a second toggles the panel. Melee's own L + R + A + Start
    // (quit the match) contains it, so a hold that includes A never counts, and pressing A during the hold cancels it.
    bool combo = false, any_held = false;
    int combo_port = -1;
    for (int i = 0; i < 4; ++i) {
      if (pads[i].err != 0) continue;
      const uint32_t b = pads[i].button;
      if ((b & (fe::BTN_L | fe::BTN_R | fe::BTN_START)) == (fe::BTN_L | fe::BTN_R | fe::BTN_START)) {
        any_held = true;
        if (b & fe::BTN_A) fe::g_combo_blocked = true; else { combo = true; if (combo_port < 0) combo_port = i; }
      }
    }
    if (!any_held) fe::g_combo_blocked = false;
    if (fe::g_combo_blocked) combo = false;
    fe::g_combo_frames = combo ? fe::g_combo_frames + 1 : 0;
    if (fe::g_combo_frames == 1) log("menu: L+R+Start held");
    if (fe::g_combo_frames == 30) { fe::set_open_locked(!fe::g_open.load()); fe::g_combo_blocked = true; if (combo_port >= 0) fe::g_nav_port = combo_port; }
    // Which controller drives the panel, and when the game gets its controllers back (as patches/0114 did for the old menu,
    // after melee-unlocked's host/settings_nav.h, GPL-2.0-or-later): the panel follows the pad that went from rest to active
    // last, so a drifting second pad cannot take it; after it closes the game reads neutral pads until every button is up and
    // the driving pad is at rest, at most a second.
    {
      const auto active = [&](int i) {
        return pads[i].err == 0 && (pads[i].button || std::abs(pads[i].stick_x) >= 30 || std::abs(pads[i].stick_y) >= 30);
      };
      bool moved = false;
      for (int i = 0; i < 4; ++i) {
        const bool now = active(i);
        if (fe::g_open.load() && !moved && now && fe::g_nav_rest[i]) { fe::g_nav_port = i; moved = true; }
        fe::g_nav_rest[i] = pads[i].err == 0 && !now;
      }
      if (fe::g_nav_port < 0 || fe::g_nav_port > 3 || pads[fe::g_nav_port].err != 0)
        for (int i = 0; i < 4; ++i) if (pads[i].err == 0) { fe::g_nav_port = i; break; }
      if (!fe::g_open.load()) {
        if (fe::g_nav_was_open) { fe::g_nav_closed_frames = 0; fe::g_nav_was_open = false; }
        if (fe::g_nav_closed_frames < 60) {
          ++fe::g_nav_closed_frames;
          bool hold = false;
          for (int i = 0; i < 4; ++i) if (pads[i].err == 0 && (pads[i].button || (i == fe::g_nav_port && active(i)))) hold = true;
          if (hold) for (int i = 0; i < 4; ++i) { const int8_t err = pads[i].err; pads[i] = {}; pads[i].err = err; }
          else fe::g_nav_closed_frames = 60;
        }
      } else fe::g_nav_was_open = true;
    }
    if (fe::g_open.load()) {
      if (fe::g_remap_on.load()) {
        const fe::Device* d = fe::device_locked();
        if (!d) fe::g_remap_on = false;
        else {
          int input = kUnbound;
          if (fe::is_keyboard(d)) { const int key = window_take_key_press(); if (key == 41) fe::g_remap_on = false; else if (key > 0) input = key; }
          else {
            const int held = window_capture_input(d->guid);
            if (!fe::g_step_armed) { if (held == kUnbound) fe::g_step_armed = true; }   // the previous press let go first
            else input = held;
          }
          if (fe::g_remap_on.load()) {
            if (input != kUnbound) { fe::remap_assign_locked(input); fe::remap_next_locked(); }
            else if (++fe::g_step_frames >= fe::kSkipFrames) fe::remap_next_locked();
          }
        }
        for (int i = 0; i < 4; ++i) { fe::g_nav[i] = {}; fe::g_nav[i].err = pads[i].err; }   // the remap's presses are not the panel's
      } else {   // only the driving pad moves the panel
        for (int i = 0; i < 4; ++i) { fe::g_nav[i] = {}; fe::g_nav[i].err = pads[i].err; }
        if (fe::g_nav_port >= 0 && fe::g_nav_port < 4) fe::g_nav[fe::g_nav_port] = pads[fe::g_nav_port];
      }
      ++fe::g_nav_seq;
    }
  }
  fe::mu_ui::panel_service(true);
  if (fe::g_open.load()) for (int i = 0; i < 4; ++i) { const int8_t err = pads[i].err; pads[i] = {}; pads[i].err = err; }
}

// The performance HUD line (top left), as the old menu drew it; the panel itself is drawn by panel_draw.
void menu_overlay(OverlayFrame& out, int ww, int wh, bool) {
  RuntimeSettings s; std::function<std::string()> extra;
  { std::lock_guard<std::mutex> lock(fe::g_mutex); s = fe::g_settings; extra = fe::g_hud_extra; }
  if (!s.hud) return;
  const float unit = std::max(18.0f, wh / 24.0f);
  GcAdapterStatus adapter; const bool have_adapter = gcadapter_status(adapter);
  char line[200], rate[24] = "";
  if (have_adapter && adapter.report_hz > 0) std::snprintf(rate, sizeof rate, "%.0f Hz", adapter.report_hz);
  std::snprintf(line, sizeof line, "sim %.1f ms   display %.0f Hz   late %llu%s%s", last_sim_frame_ms(), window_refresh_rate(), (unsigned long long)late_frame_count(),
                have_adapter ? "   GC adapter " : "", rate);
  std::string hud = line;
  if (const int ping = online_ping_ms(); ping >= 0) { hud += "   ping "; hud += std::to_string(ping); hud += " ms"; }
  if (extra) { const std::string e = extra(); if (!e.empty()) { hud += "   "; hud += e; } }
  const float h = std::min(unit * 0.7f, 20.0f), pad = h * 0.4f, w = h * 0.55f * float(hud.size()) + pad * 2;
  out.shapes.push_back({pad, pad, pad + w, pad + h + pad * 1.5f, 0.0f, 0.0f, 0.0f, 0.55f, h * 0.35f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
  out.texts.push_back({pad + pad, pad + pad * 0.6f, h, 1, 1, 1, 0.92f, 0, hud, float(ww) - 4 * pad});
}
}  // namespace host

