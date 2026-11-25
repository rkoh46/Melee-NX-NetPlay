// melee-unlocked's in-game settings panel on the Switch, in place of the app's own menu (game_menu.cpp, no longer built).
//
// The look is melee-unlocked's, drawn by its own code: its ImGui panel in the "Clean side" appearance (menu style 0, its
// default), from the fetched tree at build time (scripts/mu_ui_extract.py -> gen/mu_ui): pc_settings_visuals.inl (the
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
#include <string>
#include <sys/stat.h>
#include <vector>

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
#include "motion.h"
namespace gx {
#include "mu_settings_state.inc"
std::atomic<float> g_frame_ui_scale{1.0f};
// The GD Melee kit's pictures (the GD look, menu style 2) are not drawn: its picture and font atlases stay unloaded.
struct ImTextureData { ImTextureID GetTexRef() const { return ImTextureID(0); } };
static std::string ui_source_asset_path(const char*, const char*) { return {}; }
static ImTextureData* cosmetic_preview(const std::string&) { return nullptr; }
static bool g_menu_style_gallery_open = false;
static void draw_settings_icon(int index, ImVec2 p, ImU32 color);   // mu_panel_parts.inc (pc_settings.cpp has it first)
#include "gd_text.inl"
#include "pc_settings_visuals.inl"
#include "mu_panel_parts.inc"
}  // namespace gx

// melee-unlocked's fonts and launcher art, embedded by build.sh sync (gen/embedded.S).
extern "C" const uint8_t mu_ui_font_black[], mu_ui_font_black_end[], mu_ui_font_bold[], mu_ui_font_bold_end[];
extern "C" const uint8_t mu_ui_bailey[], mu_ui_bailey_end[], mu_ui_mark[], mu_ui_mark_end[];

namespace fe::features { __attribute__((weak)) bool practice_menu_chord(int port); }   // features/proai.cpp: the Practice menu

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
enum class Act { Main, Extra, Controls, Remap, Quit, Ini, Setting, Practice, ModPrimary, ModRemove, ModProfile };
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
    {"Controller bindings", 3, "Controller bindings"}, {"Controller profiles", 3, "Controller profiles"}, {"Audio", 1, "Output"}};
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
    {"Automatic L-cancel (offline)", 2, "L-cancel"}, {"Offline input delay", 2, "Input delay"},
    {"Console low-pass on voices (next start)", 1, "Music and sound"}, {"Use custom music (next start)", 1, "Music and sound"},
    // melee-unlocked's VIDEO page has the Arcade look section
    {"Show hitboxes", 0, "Arcade look"}, {"Projectile hitboxes", 0, "Arcade look"}, {"Hitboxes behind fighters", 0, "Arcade look"},
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
  // CUSTOMIZE: the panel's own colour (melee-unlocked's palettes for this appearance) and the home screen's accent.
  {
    Section& c = section(cats[5], "Appearance");
    Action a{Act::Ini}; a.key = "mu_palette";
    c.rows.push_back(choice("Color palette for this appearance", std::clamp(std::atoi(fe::app_ini("mu_palette", "0").c_str()), 0, 3),
                            {"Pink (classic)", "Cyan", "Orange", "Purple"}, a));
    Action h{Act::Ini}; h.key = "mu_accent";
    c.rows.push_back(choice("Launcher color", std::clamp(std::atoi(fe::app_ini("mu_accent", "0").c_str()), 0, 5),
                            {"Purple (default)", "Blue", "Green", "Amber", "Red", "Pink"}, h));
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
  // melee-unlocked's rows the Switch does not have, greyed under each page, with the reason (FEATURES.md has each one).
  static const struct { int cat; const char* label; const char* why; } kAbsent[] = {
    {0, "Frame rate, VSync, Display, Window size", "The console's display runs at 60 Hz with its own sync; there is no window."},
    {0, "Upscaling (DLSS, XeSS, FSR), frame generation, NVIDIA Reflex, HDR, ray tracing", "PC graphics cards only."},
    {0, "Graphics backend", "One renderer on the Switch: Aurora on Vulkan."},
    {0, "Widescreen (true widescreen), Look, Visual effects, Lightweight", "Not wired on the Switch yet (the Picture shape and Aspect ratio rows above are)."},
    {0, "RetroArch shader presets, texture packs, animated menu backgrounds", "Not wired on the Switch yet (the built-in CRT look is)."},
    {1, "WASAPI exclusive, ASIO, output device", "The console's audio output has no driver choice: Auto and Low latency are above."},
    {1, "Menu sounds", "Not wired on the Switch yet."},
    {2, "Background input, Discord", "No other windows or Discord on the console."},
    {2, "YouGame", "Left out: melee-unlocked's own online service."},
    {3, "Keyboard and HID controller editors, PhobGCC menu", "The Switch's controllers come through the system and the GameCube adapter: the rows above."},
    {4, "Controller overlay placement (dragging)", "No pointer: the overlays have fixed places."},
    {5, "Menu styles (Dashboard, GD Melee, Radial, Wide, Simple, legacy), custom accent wheel, transparency", "The Clean side style only (melee-unlocked's default)."},
    {6, "Your codes (add, edit, import)", "Needs a keyboard, and the codes run on the Static Recomp: not on the Switch."},
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
      if (gx::settings_combo(r.label.c_str(), &v, items.data(), int(items.size())) && v != r.value) { Action a = r.action; a.value = v; queue(a); }
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

void draw_panel(int w, int h) {
  const bool model_open = g_open.load();
  if (model_open != g_ui_open || (model_open && g_jump.exchange(false))) {
    g_ui_open = model_open;
    g_state.open = model_open;
    if (!model_open) g_reopen_page = g_last_page;   // where it was left: the next open returns there (this session)
    if (model_open) {
      g_state.home_focus_reset = true;
      g_state.clean_detail_open = false;
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
      host::log("settings: opened on %s%s%s", kCategory[std::clamp(g_state.active_tab, 0, 7)], g_scroll_to.empty() ? "" : " > ", g_scroll_to.c_str());
      g_scroll_to.clear();
    }
  }
  // B goes back a page, then closes; Start closes from any page (settings_frame's own handling).
  if (g_state.open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !g_remap_on.load()) {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) g_state.open = false;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
      if (g_state.clean_detail_open) g_state.clean_detail_open = false;
      else g_state.open = false;
    }
  }
  if (g_state.open && g_enter_frames == 0) g_last_page = g_state.clean_detail_open ? g_state.active_tab : -1;
  gx::mu_settings_motion(g_state);
  const bool visible = g_state.open || g_state.panel_slide_x > -720.0f;
  if (!visible) { if (!g_state.open && g_ui_open) { g_close_request = true; } return; }

  const OptionsView options;
  gx::g_settings_palette = std::clamp(std::atoi(fe::app_ini("mu_palette", "0").c_str()), 0, 3);
  gx::g_settings_custom_color_enabled = false;
  const gx::SettingsAppearanceScope appearance_scope(0);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float motion_scale = std::max(0.1f, display.y / 480.0f);
  const float width = std::min(560.0f, std::max(390.0f, display.x * 0.38f));
  const float pw = std::max(300.0f, std::min(width, display.x - 12.0f));
  const float slide_scale = std::max(motion_scale, pw / 720.0f);
  ImGui::SetNextWindowPos(ImVec2(display.x - pw + std::abs(g_state.panel_slide_x) * slide_scale, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(pw, display.y), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::Begin("PC settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
               ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  if (g_state.open && (g_state.home_focus_reset || ImGui::IsWindowAppearing())) {
    ImGui::SetWindowFocus();
    ImGui::SetNavCursorVisible(true);   // a controller-only panel: the focus frame shows, and the first A acts
  }
  {   // Back from a category page: the focus returns to that category's row (the page's controls are gone)
    static bool was_detail = false;
    if (was_detail != g_state.clean_detail_open) host::log("settings: %s", g_state.clean_detail_open ? kCategory[std::clamp(g_state.active_tab, 0, 7)] : "categories");
    if (g_enter_frames > 0 && !g_state.clean_detail_open) {
      if (g_enter_frames == 3) was_detail = true;          // focus the row as Back does
      else if (g_enter_frames == 1) { g_state.clean_detail_open = true; }
      --g_enter_frames;
    }
    if (!was_detail && g_state.clean_detail_open) {   // a page entered: its first row gets the focus (set below, by its ID)
      g_state.page_navigation.tab = g_state.active_tab; g_state.page_navigation.style = 0; g_state.page_navigation.logged_focus = 0;
      g_focus_first = true;
    }
    if (was_detail && !g_state.clean_detail_open) {
      ImGui::PushID(g_state.active_tab);
      const ImGuiID id = ImGui::GetID("##clean_category");
      ImGui::PopID();
      ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
      ImGui::SetNavCursorVisible(true);
    }
    was_detail = g_state.clean_detail_open;
  }
  const bool clean_home = !g_state.clean_detail_open && g_state.clean_detail_frames <= 0.0f;
  if (!gx::g_menu_style_gallery_open)
    gx::settings_clean_chrome(g_state.clean_detail_open ? kCategory[std::clamp(g_state.active_tab, 0, 7)] : "SETTINGS", g_state.panel_slide_x);
  if (clean_home) {
    gx::settings_clean_home(g_state);
    if (g_in_game_feed) gx::settings_home_quit(g_state, 0);
    confirm_quit_popup();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.06f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.12f, 0.48f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.24f, 0.58f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.07f, 0.14f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    gx::mu_settings_clean_detail_header(g_state);
    ImGui::BeginDisabled(!g_state.open);
    gx::mu_settings_page_entrance(g_state, options);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.025f, 0.055f, 0.78f));
    gx::g_settings_form_style = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
    const ImVec2 content_origin = ImGui::GetCursorPos();
    ImGui::BeginChild("##settings_content", ImVec2(0, -84.0f), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollWithMouse);
    gx::mu_settings_smooth_scroll();
    ImGui::PushTextWrapPos(0.0f);
    if (g_state.open && g_state.clean_detail_open) {
      g_state.page_navigation.begin(g_state.active_tab, 0);
      gx::settings_page::observer = &g_state.page_navigation;
    }
    // melee-unlocked has no keyboard-free way into its search box: the box is left out (FEATURES.md).
    const std::array<Category, 8> cats = build_rows(g_in_game_feed);
    for (int page = 0; page < 8; ++page) {
      if (!gx::settings_page_shown(g_state.active_tab, page)) continue;
      const Category& c = cats[size_t(page)];
      if (c.sections.empty()) ImGui::TextDisabled("Nothing on this page on the Switch.");
      for (const Section& s : c.sections) {
        if (s.rows.empty()) continue;
        gx::settings_section(s.title.c_str());
        for (const Row& r : s.rows) {
          // A row's ID: its "###key" when it has one (a label that changes keeps its focus), else its place in the section.
          const size_t key = r.label.find("###");
          if (key != std::string::npos) ImGui::PushID(r.label.c_str() + key + 3); else ImGui::PushID(int(&r - s.rows.data()));
          ImGui::PushID(s.title.c_str()); draw_row(r); ImGui::PopID(); ImGui::PopID();
          if (g_focus_first && g_row_id && g_state.clean_detail_frames >= 14.0f) {
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
    ImGui::PopTextWrapPos();
    const ImVec2 content_size = ImGui::GetWindowSize();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(content_origin.x, content_origin.y + content_size.y));
    ImGui::PopStyleVar();    // the page entrance's alpha
    gx::settings_page_footer(g_state, 0);
    confirm_quit_popup();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
  }
  ImGui::End();
  if (g_remap_on.load()) draw_remap(int(display.x), int(display.y));
  if (!g_state.open && g_ui_open) g_close_request = true;   // the panel asked to close: the model closes at its next poll
  (void)w; (void)h;
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
}  // namespace

namespace mu_ui {
void fonts_init() {
  if (!ImGui::GetCurrentContext() || g_heading) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();   // stays the default: every other overlay keeps its look
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;                        // the embedded files stay where they are
  config.OversampleH = 2;
  std::snprintf(config.Name, sizeof config.Name, "Source Sans 3 Black");
  g_heading = io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(mu_ui_font_black), int(mu_ui_font_black_end - mu_ui_font_black), 28.0f, &config);
  std::snprintf(config.Name, sizeof config.Name, "Source Sans 3 Bold");
  g_body = io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(mu_ui_font_bold), int(mu_ui_font_bold_end - mu_ui_font_bold), 16.0f, &config);
  gx::g_settings_heading_font = g_heading;
  gx::g_settings_body_font = g_body;
  aurora_imgui_host_gamepad(true);          // patches/0115: the panel's gamepad keys come from the app's controllers only
  io.ConfigInputTrickleEventQueue = true;   // settings_input::configure: press and release pairs kept between frames
  say("ui: melee-unlocked's fonts loaded (Source Sans 3 Black and Bold)");
}
ImFont* heading_font() { return g_heading; }
ImFont* body_font() { return g_body; }
const Art& bailey() { static const Art a = decode_bmp(mu_ui_bailey, mu_ui_bailey_end); return a; }
const Art& mark() { static const Art a = decode_bmp(mu_ui_mark, mu_ui_mark_end); return a; }

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
    if (a.act == Act::Ini) { fe::app_ini_set(a.key, std::to_string(a.value)); say("menu: %s %d", a.key.c_str(), a.value); }
    if (a.act == Act::ModPrimary) mu_mods::primary(a.key);
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

