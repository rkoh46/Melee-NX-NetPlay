// The app: the native engine started with no arguments, as the player starts Melee-NX-NetPlay from the
// Homebrew Menu or a HOME-menu forwarder. Play mode with the current app's folders and launcher.ini
// (port/app/main_switch.cpp): the disc, the Slippi user folder, the memory card, replays, logs and the
// pipeline cache all live under sdmc:/switch/melee-nx-netplay, so 0.4.0 takes over from the earlier app
// with the player's sign-in, save and settings. This file: the folders, launcher.ini, the memory card's
// one-time backup, the controller configuration (remapping, deadzones, rumble: the app's input_config),
// the HUD's numbers and the window queries of the app's menu code.
//
// Memory card: the app's User/GC/CardA. Both engines keep slot A as a Dolphin-style folder of GCI files
// (64-byte directory entry, then the 8 KiB blocks) and both read every .gci in the folder; the native host
// keeps a loaded file's name when it saves. The game's save data is the same game's. So the player's save
// (names, rules, records, unlocks) carries over; the first session copies the folder once to
// User/GC/CardA-before-0.4 so going back to 0.3.x is always possible.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host.h"                // shim: PadState, the HUD queries
#include "fixedmap.h"
#include "netplay_bridge.h"      // the netplay device's round trip (ping_ms)
#include "feature_calls.h"       // the feature modules' HUD lines
#if MU_FE_AURORA
#include "app/game_menu.h"       // menu_is_open (the build-time copy with patches/0104)
#include "input_config.h"
#include "window.h"
#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__SWITCH__) && MU_FE_AURORA
extern "C" int melee_nx_gc_stick_range;     // SDL's Switch joystick backend (sdl-dusklight-gc-sticks.patch)
extern "C" int melee_nx_gc_trigger_scale;   // SDL's Switch joystick backend (sdl-dusklight-gc-trigger-range.patch)
#endif

namespace fe {
namespace {
std::mutex ini_mutex;
std::map<std::string, std::string> ini;     // launcher.ini, as the app keeps it
std::string ini_path;

std::map<std::string, std::string> read_ini(const std::string& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    out[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return out;
}
// The app's own format (main_switch.cpp write_ini): one header line, key=value sorted.
void write_ini_locked() {
  if (ini_path.empty()) return;
  const std::string temporary = ini_path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;
    out << "# Melee-NX-NetPlay settings. Edit while the app is closed; the in-game menu (L+R+Start) saves here too.\n";
    for (const auto& [key, value] : ini) out << key << '=' << value << '\n';
  }
  std::remove(ini_path.c_str());
  if (std::rename(temporary.c_str(), ini_path.c_str()) != 0) std::remove(temporary.c_str());
}
void ensure_dir(const std::string& path) { mkdir(path.c_str(), 0777); }
bool exists(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0; }
int clamp_int(const std::string& text, int fallback, int low, int high) {
  if (text.empty()) return fallback;
  return std::clamp(int(std::strtol(text.c_str(), nullptr, 10)), low, high);
}

// ---- HUD ----
std::atomic<int> hud_delay{0};
std::atomic<uint64_t> hud_late{0};
std::atomic<uint32_t> hud_work_x100{0};   // ms * 100
[[maybe_unused]] bool menu_was_open = false;
}  // namespace

// No arguments but the ones a forwarder or a restart may add: the app.
bool app_wanted(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--app" || a == "--relaunch") continue;
    // --replay FILE: the app started again by its Replays page ("Watch it"; play_main.cpp restarts in-process).
    if ((a == "--simulate-bad-layout" || a == "--base" || a == "--replay") && i + 1 < argc) { ++i; continue; }
    return false;
  }
  return true;
}

std::string app_ini(const std::string& key, const std::string& fallback) {
  std::lock_guard<std::mutex> lock(ini_mutex);
  const auto it = ini.find(key);
  return it == ini.end() || it->second.empty() ? fallback : it->second;
}
void app_ini_set(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(ini_mutex);
  ini[key] = value;
  write_ini_locked();
}

void app_configure() {
  auto& o = options;
#ifdef __SWITCH__
  if (o.base_dir.empty()) o.base_dir = "sdmc:/switch/melee-nx-netplay";
#else
  if (o.base_dir.empty()) o.base_dir = "./melee-nx-netplay";
#endif
  const std::string base = o.base_dir;
  for (const char* sub : {"", "/User", "/User/GC", "/User/GC/CardA", "/User/Slippi", "/Replays", "/Cache", "/Logs"}) ensure_dir(base + sub);
  // Logs/latest.log is this session's; the one before is kept as previous.log (a restart for the layout appends).
  if (fixedmap_relaunch_count() == 0) { std::remove((base + "/Logs/previous.log").c_str()); std::rename((base + "/Logs/latest.log").c_str(), (base + "/Logs/previous.log").c_str()); }
  ini_path = base + "/launcher.ini";
  ini = read_ini(ini_path);
  const auto get = [&](const char* key, const std::string& fallback) { const auto it = ini.find(key); return it == ini.end() || it->second.empty() ? fallback : it->second; };
  o.out_dir = base;
  if (o.iso.empty()) o.iso = get("iso", base + "/GALE01.iso");
  o.log_path = base + "/Logs/latest.log";
  o.host_log_path = base + "/Logs/host.log";
  o.net_dir = base + "/Logs";
  o.card_dir = base + "/User/GC/CardA";
  o.user_dir = base + "/User/Slippi";
  if (o.cache_dir.empty()) o.cache_dir = base + "/Cache";            // shared with the earlier app: the same renderer's pipelines
  if (o.settings.empty()) o.settings = base + "/settings.ini";
  if (o.self_path.size() < 4 || o.self_path.compare(o.self_path.size() - 4, 4, ".nro") != 0) o.self_path = base + "/melee-nx-netplay.nro";
  if (o.volume < 0) o.volume = clamp_int(get("volume", ""), 100, 0, 100);
  o.offline = get("offline", "0") == "1";
  const std::string delay = get("online_delay", "");
  o.online_delay = delay.empty() ? -1 : delay == "auto" ? 0 : clamp_int(delay, 2, 1, 9);
  if (get("overclock", "1") == "0") o.overclock = false;
  if (get("shader_warmup", "1") == "0") setenv("AURORA_LAUNCH_WARM_SECONDS", "0", 1);
  if (get("shader_warmup_all", "0") == "1") o.warm_all = true;
  // Antialiasing and texture filtering are fixed when the graphics device is created (frontend.cpp reads these).
  setenv("MELEE_MSAA", get("msaa", "1") == "4" ? "4" : "1", 1);
  setenv("MELEE_ANISOTROPY", std::to_string(clamp_int(get("anisotropy", ""), 16, 1, 16)).c_str(), 1);
  setenv("MU_APP_FILTER", std::to_string(clamp_int(get("filter", ""), 0, 0, 3)).c_str(), 1);
  // The app's test aids: a controller script, a frame limit, the menu open at a retrace (screenshots).
  if (o.play_script.empty()) o.play_script = get("script", "");
  if (!o.frames) o.frames = unsigned(std::strtoul(get("frames", "0").c_str(), nullptr, 10));
  if (const std::string v = get("menu_open", ""); !v.empty()) setenv("MELEE_MENU_OPEN", v.c_str(), 1);
  if (const std::string v = get("menu_page", ""); !v.empty()) setenv("MELEE_MENU_PAGE", v.c_str(), 1);
  // feature_test=NAME[:VALUE]: the feature modules' --feature-test (verification runs; play mode takes it on the command line).
  if (const std::string v = get("feature_test", ""); !v.empty()) { o.rest.push_back("--feature-test"); o.rest.push_back(v); }
#if MU_FE_AURORA
  // Controllers: pad.<SDL GUID>=<port>;<map> (input_config.h ControllerMap::serialize), keyboard=<map>.
  std::vector<host::ControllerConfig> configs;
  for (const auto& [key, value] : ini) {
    if (key.rfind("pad.", 0) != 0 || key.size() <= 4) continue;
    host::ControllerConfig c;
    c.guid = key.substr(4);
    const size_t semi = value.find(';');
    c.port = semi == std::string::npos ? 0 : std::clamp(std::atoi(value.substr(0, semi).c_str()), 0, 4);
    c.map = host::ControllerMap::parse(semi == std::string::npos ? value : value.substr(semi + 1));
    configs.push_back(c);
  }
  host::set_controller_configs(configs);
  if (const std::string k = get("keyboard", ""); !k.empty()) host::set_keyboard_map(host::KeyboardMap::parse(k));
  // The rest of each controller's setup (controls.cpp): padx.<SDL GUID> or padx.gc<adapter port>.
  for (const auto& [key, value] : ini)
    if (key.rfind("padx.", 0) == 0 && key.size() > 5) controls_load(key.substr(5), value);
#ifdef __SWITCH__
  melee_nx_gc_trigger_scale = clamp_int(get("trigger_range", ""), 4, 2, 8);
  melee_nx_gc_stick_range = clamp_int(get("stick_range", ""), 100, 80, 120);
#endif
#endif
}

// launcher.ini values that settle what settings.ini leaves open (after settings_load).
void app_after_settings() {
  // The earlier app drew 16:9 with Slippi's widescreen code unless widescreen=0; settings.ini's own line wins.
  if (settings_get("widescreen").empty() || !settings_given("widescreen")) settings_set_quiet("widescreen", app_ini("widescreen", "1") != "0" ? "1" : "0");
}

void app_report() {
  std::string keys;
  { std::lock_guard<std::mutex> lock(ini_mutex); for (const auto& [key, value] : ini) { if (!keys.empty()) keys += ", "; keys += key + "=" + value; } }
  say("app: Melee-NX-NetPlay %s (native engine); folder %s; launcher.ini: %s", MU_APP_VERSION, options.base_dir.c_str(), keys.empty() ? "(none yet)" : keys.c_str());
  say("app: disc %s, memory card %s, user %s, settings %s, logs %s, pipeline cache %s, volume %d, online delay %s%s",
      options.iso.c_str(), options.card_dir.c_str(), options.user_dir.c_str(), options.settings.c_str(), options.log_path.c_str(), options.cache_dir.c_str(),
      options.volume, options.online_delay < 0 ? "from settings.ini" : options.online_delay == 0 ? "automatic" : std::to_string(options.online_delay).c_str(),
      options.offline ? ", offline=1 (no online services)" : "");
}

// The memory card's .gci files (names, sorted).
std::vector<std::string> gci_names(const std::string& folder) {
  std::vector<std::string> names;
  if (DIR* dir = opendir(folder.c_str())) {
    while (dirent* e = readdir(dir)) { const std::string n = e->d_name; if (n.size() > 4 && n.compare(n.size() - 4, 4, ".gci") == 0) names.push_back(n); }
    closedir(dir);
  }
  std::sort(names.begin(), names.end());
  return names;
}
std::string file_bytes(const std::string& path) { std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); }

// Automatic save backups (upstream host/card_backup.h): at each start, when the card differs from the newest backup, a
// copy in User/GC/Backups/CardA-YYYYMMDD-HHMMSS; the 10 newest are kept, and of the older ones the newest of each day
// for 30 days. Only that folder is ever pruned.
void backup_card_rotating() {
  const std::string root = options.base_dir + "/User/GC/Backups";
  const std::vector<std::string> names = gci_names(options.card_dir);
  if (names.empty()) return;
  std::vector<std::string> backups;   // CardA-YYYYMMDD-HHMMSS, oldest first
  if (DIR* dir = opendir(root.c_str())) {
    while (dirent* e = readdir(dir)) { const std::string n = e->d_name; if (n.size() == 21 && n.rfind("CardA-", 0) == 0) backups.push_back(n); }
    closedir(dir);
  }
  std::sort(backups.begin(), backups.end());
  bool same = false;
  if (!backups.empty()) {
    const std::string last = root + "/" + backups.back();
    same = gci_names(last) == names;
    for (size_t i = 0; same && i < names.size(); ++i) same = file_bytes(last + "/" + names[i]) == file_bytes(options.card_dir + "/" + names[i]);
  }
  if (same) { say("app: memory card unchanged since the backup %s", backups.back().c_str()); return; }
  const time_t now = time(nullptr);
  struct tm local{};
  localtime_r(&now, &local);
  char name[32];
  std::strftime(name, sizeof name, "CardA-%Y%m%d-%H%M%S", &local);
  if (!backups.empty() && backups.back() >= name) { say("app: memory card backup skipped: the clock (%s) is behind the newest backup %s", name, backups.back().c_str()); return; }
  ensure_dir(root); ensure_dir(root + "/" + name);
  size_t copied = 0;
  for (const auto& n : names) {
    std::ofstream out(root + "/" + name + "/" + n, std::ios::binary | std::ios::trunc);
    const std::string bytes = file_bytes(options.card_dir + "/" + n);
    if (out && out.write(bytes.data(), std::streamsize(bytes.size()))) ++copied;
  }
  backups.push_back(name);
  // Keep the 10 newest; older: the newest of each day, for days in the last 30.
  char oldest_day[16];
  const time_t month_ago = now - 30 * 86400;
  struct tm then{};
  localtime_r(&month_ago, &then);
  std::strftime(oldest_day, sizeof oldest_day, "%Y%m%d", &then);
  size_t removed = 0;
  std::string kept_day;                       // newest first, so the first backup seen of a day is that day's newest
  for (size_t i = backups.size(), rank = 0; i-- > 0; ++rank) {
    if (rank < 10) continue;
    const std::string day = backups[i].substr(6, 8);
    if (day >= oldest_day && day != kept_day) { kept_day = day; continue; }
    const std::string dir = root + "/" + backups[i];
    for (const auto& n : gci_names(dir)) std::remove((dir + "/" + n).c_str());
    if (rmdir(dir.c_str()) == 0) ++removed;
  }
  say("app: memory card backed up to %s (%zu save file%s); %zu older backup%s removed", (root + "/" + name).c_str(), copied, copied == 1 ? "" : "s", removed, removed == 1 ? "" : "s");
}

// The first session of the native engine copies the player's memory card once, before anything writes to it; every
// start then makes a rotating backup when the card changed.
void app_backup_card() {
  backup_card_rotating();
  const std::string backup = options.base_dir + "/User/GC/CardA-before-0.4";
  if (exists(backup)) return;
  const std::vector<std::string> names = gci_names(options.card_dir);
  ensure_dir(backup);
  size_t copied = 0;
  for (const auto& n : names) {
    std::ifstream in(options.card_dir + "/" + n, std::ios::binary);
    std::ofstream out(backup + "/" + n, std::ios::binary | std::ios::trunc);
    if (in && out) { out << in.rdbuf(); ++copied; }
  }
  say("app: memory card %s: %zu save file%s copied once to %s (the earlier app's card, kept as it was)", options.card_dir.c_str(), copied, copied == 1 ? "" : "s", backup.c_str());
}

// ---- screens before the game ----
bool app_message_screen(const std::string& headline, const std::vector<std::string>& lines) {
  if (!app_notice_run || !screens_start()) return false;
  app_notice_run(headline, lines);
  return true;
}
// One launch in about seventy gets an address layout that cannot hold the game (fixedmap.h). Started from a
// forwarder with --relaunch the app starts itself again before anything is shown; from the Homebrew Menu it cannot.
bool app_layout_screen(const char* reason) {
  say("app: the game cannot start in this launch: %s", reason);
  return app_message_screen("PLEASE START THE APP AGAIN", {
      "This launch could not reserve the memory the game needs. It happens about once in seventy launches",
      "and has nothing to do with your disc, your saves or your settings.", "",
      "Close Melee-NX-NetPlay and open it again.", "",
      "Started from a HOME Menu icon (a forwarder that passes --relaunch), the app starts itself again instead."});
}

// ---- HUD ----
namespace {
// The performance graph's history (overlay_draw.cpp): the newest 240 frame times, written by the simulation thread.
std::atomic<uint32_t> perf_ring[240];
std::atomic<uint32_t> perf_count{0};
}
int perf_history(float* out, int capacity) {
  const uint32_t n = perf_count.load(std::memory_order_acquire);
  const int have = int(std::min<uint32_t>(n, 240u)), take = std::min(have, capacity);
  for (int i = 0; i < take; ++i) out[i] = perf_ring[(n - uint32_t(take) + uint32_t(i)) % 240u].load(std::memory_order_relaxed) / 100.0f;
  return take;
}
void hud_frame(double work_ms) {
  hud_work_x100.store(uint32_t(std::max(0.0, work_ms) * 100.0), std::memory_order_relaxed);
  const uint32_t n = perf_count.load(std::memory_order_relaxed);
  perf_ring[n % 240u].store(uint32_t(std::max(0.0, work_ms) * 100.0), std::memory_order_relaxed);
  perf_count.store(n + 1, std::memory_order_release);
  if (work_ms > 1000.0 / 60) hud_late.fetch_add(1, std::memory_order_relaxed);
}
// "slippi: automatic input delay 3 frames (...)": the delay the netplay device chose for this game.
void hud_log_line(const char* line) {
  const char* at = std::strstr(line, "slippi: automatic input delay ");
  if (at) hud_delay.store(std::atoi(at + 30), std::memory_order_relaxed);
}
uint32_t online_delay_frames() {
  if (options.online_delay > 0) return uint32_t(options.online_delay);
  const int frames = settings_online_delay();
  return frames > 0 ? uint32_t(frames) : uint32_t(hud_delay.load(std::memory_order_relaxed));
}

// Once per retrace (simulation thread): what the menu changed in the controller pages is saved when it closes.
void app_retrace(uint32_t, bool, bool) {
#if MU_FE_AURORA
  if (!options.app) return;
  const bool open = host::menu_is_open();
  if (menu_was_open && !open) {
    std::lock_guard<std::mutex> lock(ini_mutex);
    for (auto it = ini.begin(); it != ini.end();) it = it->first.rfind("pad.", 0) == 0 ? ini.erase(it) : std::next(it);
    for (const host::ControllerConfig& c : host::controller_configs()) ini["pad." + c.guid] = std::to_string(c.port) + ";" + c.map.serialize();
    ini["keyboard"] = host::keyboard_map().serialize();
    for (auto it = ini.begin(); it != ini.end();) it = it->first.rfind("padx.", 0) == 0 ? ini.erase(it) : std::next(it);
    controls_saved([](const std::string& id, const std::string& text) { ini["padx." + id] = text; });
    write_ini_locked();
  }
  menu_was_open = open;
#endif
}

#if MU_FE_AURORA
// The feature modules' lines (Training's frame advantage, frame advance, the practice CPU) on screen whenever a module
// has something to say, centred near the top, each in its own colour on a dark band. Render thread.
void feature_overlay(void* frame, int ww, int wh) {
  const auto lines = features::call_hud_lines();
  if (lines.empty()) return;
  auto& out = *static_cast<host::OverlayFrame*>(frame);
  out.alpha = 1.0f;
  const float unit = std::max(16.0f, float(wh) / 30.0f);
  float y = float(wh) * 0.12f;
  for (const auto& line : lines) {
    if (line.text.empty()) continue;
    const float w = std::min(float(ww) * 0.9f, unit * 0.55f * float(line.text.size()) + unit);
    out.shapes.push_back({float(ww) * 0.5f - w * 0.5f, y - unit * 0.15f, float(ww) * 0.5f + w * 0.5f, y + unit * 1.15f, 0.0f, 0.0f, 0.0f, 0.55f, unit * 0.3f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    const uint32_t c = line.rgba;
    out.texts.push_back({float(ww) * 0.5f, y, unit, ((c >> 24) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, std::max(0.4f, (c & 0xFF) / 255.0f), 1, line.text, float(ww) * 0.88f});
    y += unit * 1.45f;
  }
}
namespace {
// SDL gamepad and GUID of the controller Aurora's PAD put on `port` (nullptr: none, or the GameCube adapter).
SDL_Gamepad* gamepad_on_port(int port) {
  const s32 index = PADGetIndexForPort(uint32_t(port));
  return index >= 0 ? PADGetSDLGamepadForIndex(uint32_t(index)) : nullptr;
}
std::string guid_of(SDL_Gamepad* pad) {
  char text[64] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(SDL_GetGamepadID(pad)), text, sizeof text);
  return text;
}
bool same_bindings(const host::ControllerMap& a, const host::ControllerMap& b) { return std::equal(a.binding, a.binding + host::GC_CTL_COUNT, b.binding); }
}  // namespace

// Before the in-game menu sees the pads (play_io.cpp's menu_frame): a controller with a saved configuration gets its
// remapped buttons, swapped sticks, deadzones and trigger press point, as the app's desktop builds apply them
// (window_sdl.cpp), then the rest of its setup (controls.cpp: bindings beyond the buttons, box mode, inversion, radius,
// snapback, trigger click and cap). GameCube adapter ports get only the latter. Controllers without a configuration
// are left exactly as Aurora's GameCube layout or the adapter gives them (the earlier app's behaviour).
void app_pad_postprocess(void* pads4, int lane) {
  auto* pads = static_cast<host::PadState*>(pads4);
  const bool extended = controls_active();
  if (host::controller_configs().empty() && !extended) return;
  host::GcAdapterStatus adapter{};
  const uint32_t adapter_ports = (host::gcadapter_status(adapter) ? adapter.ports : 0) | controls_test_adapter_ports();
  static const uint16_t bits[host::GC_CTL_COUNT] = {0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0040, 0x0020, 0x1000, 0x0008, 0x0004, 0x0001, 0x0002};
  const host::ControllerMap defaults = host::ControllerMap::defaults();
  for (int port = 0; port < 4; ++port) {
    host::PadState& p = pads[port];
    if (p.err != 0) continue;
    if (adapter_ports & (1u << port)) { if (extended) controls_apply_adapter(port, &p, lane); continue; }
    SDL_Gamepad* pad = gamepad_on_port(port);
    if (!pad) continue;
    const std::string guid = guid_of(pad);
    const host::PadState before = p;
    const host::ControllerConfig* config = host::controller_config_for(guid);
    if (config) {
      const host::ControllerMap& m = config->map;
      if (!same_bindings(m, defaults) || m.trigger_press != defaults.trigger_press) {
        const int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128;
        uint16_t button = 0;
        for (int i = 0; i < host::GC_CTL_COUNT; ++i) {
          const int input = m.binding[i];
          bool down = false;
          if (input == host::kTriggerLeft || input == host::kTriggerRight) down = (input == host::kTriggerLeft ? lt : rt) >= std::max(1, m.trigger_press * 255 / 100);
          else if (input >= 0) down = SDL_GetGamepadButton(pad, SDL_GamepadButton(input));
          if (!down) continue;
          button |= bits[i];
          if (i == host::GC_CTL_L && input != host::kTriggerLeft && input != host::kTriggerRight) p.trig_l = 255;
          if (i == host::GC_CTL_R && input != host::kTriggerLeft && input != host::kTriggerRight) p.trig_r = 255;
        }
        p.button = button;
      }
      if (m.swap_sticks) { std::swap(p.stick_x, p.sub_x); std::swap(p.stick_y, p.sub_y); }
      if (m.stick_deadzone != defaults.stick_deadzone && std::abs(int(p.stick_x)) * 100 < m.stick_deadzone * 127 && std::abs(int(p.stick_y)) * 100 < m.stick_deadzone * 127) { p.stick_x = 0; p.stick_y = 0; }
      if (m.cstick_deadzone != defaults.cstick_deadzone && std::abs(int(p.sub_x)) * 100 < m.cstick_deadzone * 127 && std::abs(int(p.sub_y)) * 100 < m.cstick_deadzone * 127) { p.sub_x = 0; p.sub_y = 0; }
    }
    if (extended) controls_apply_sdl(guid, pad, &before, &p, config ? config->map.trigger_press : defaults.trigger_press, lane);
  }
}
bool app_rumble_allowed(int port) {
  if (port < 0 || port > 3) return false;
  SDL_Gamepad* pad = gamepad_on_port(port);
  if (!pad) return true;
  const host::ControllerConfig* config = host::controller_config_for(guid_of(pad));
  return !config || config->map.rumble;
}
#else
void app_pad_postprocess(void*, int) {}
bool app_rumble_allowed(int) { return true; }
void app_menu_setup() {}   // no menu without a picture
void feature_overlay(void*, int, int) {}
#endif
}  // namespace fe

// ---- what the app's menu and HUD code asks the host (shim host.h) ----
namespace host {
double last_sim_frame_ms() { return fe::hud_work_x100.load(std::memory_order_relaxed) / 100.0; }
uint64_t late_frame_count() { return fe::hud_late.load(std::memory_order_relaxed); }
// The netplay device's own measurement, updated with every acknowledged frame of an online game (both sides).
int online_ping_ms() { return fe::session_online() ? netplay_bridge::ping_ms() : -1; }
const char* latency_warning() { return ""; }
#if MU_FE_AURORA
double window_refresh_rate() { return fe::display_refresh_hz(); }
void window_client_size(int* w, int* h) { fe::window_size(w, h); }
// The Controls page: the SDL gamepads Aurora has opened (the GameCube adapter is configured by the game itself).
std::vector<ControllerInfo> window_list_controllers() {
  std::vector<ControllerInfo> list;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (!pad) continue;
    ControllerInfo info;
    info.name = SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "Controller";
    info.guid = fe::guid_of(pad);
    info.instance_id = ids[i];
    if (const ControllerConfig* c = controller_config_for(info.guid)) info.assigned_port = c->port;
    list.push_back(info);
  }
  SDL_free(ids);
  return list;
}
int window_capture_input(const std::string& guid) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  int input = kUnbound;
  for (int i = 0; ids && i < count && input == kUnbound; ++i) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (!pad || fe::guid_of(pad) != guid) continue;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) { input = b; break; }
    if (input == kUnbound && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) input = kTriggerLeft;
    if (input == kUnbound && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) input = kTriggerRight;
  }
  SDL_free(ids);
  return input;
}
void window_test_rumble(const std::string& guid) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i)
    if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]); pad && fe::guid_of(pad) == guid) { SDL_RumbleGamepad(pad, 0xC000, 0xC000, 350); break; }
  SDL_free(ids);
}
#endif
}  // namespace host

// ---- RVZ and WIA discs (melee-unlocked port/app/disc_rvz.cpp, compiled from the fetched tree: MU_FE_RVZ) ----
// Upstream's launcher turns a dropped .rvz or .wia into the plain disc image once; here the app does it at start: `iso` itself
// when it is one, else GALE01.rvz / GALE01.wia beside the expected GALE01.iso, else the only .rvz or .wia in that folder. The
// image is checked (a GameCube disc, GALE01) before anything is written; the result is GALE01.iso beside it (written as .part
// and renamed when complete, upstream's rule), and the compressed file is left as it was. Every later check (the vanilla
// MD5 of the mod installer, a vanilla-only session's header check) reads the plain image as usual.
#if MU_FE_RVZ
#include "disc_rvz.h"
#endif
namespace fe {
bool app_disc_convert(std::string* iso) {
  const auto exists = [](const std::string& p) { if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; } return false; };
#if MU_FE_RVZ
  const auto lower_ext = [](const std::string& p) {
    const size_t dot = p.rfind('.');
    std::string e = dot == std::string::npos ? std::string() : p.substr(dot);
    for (auto& c : e) c = char(std::tolower((unsigned char)c));
    return e;
  };
  const std::string given = *iso;
  const size_t slash = given.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string(".") : given.substr(0, slash);
  std::string source, target;
  if (exists(given) && disc_rvz::detect(given) != disc_rvz::Format::NotCompressed) { source = given; target = folder + "/GALE01.iso"; }
  else if (exists(given)) return true;   // a plain image under another name
  else {
    target = given;
    const std::string stem = given.size() > 4 && lower_ext(given) == ".iso" ? given.substr(0, given.size() - 4) : given;
    for (const char* ext : {".rvz", ".wia", ".RVZ", ".WIA"}) if (exists(stem + ext)) { source = stem + ext; break; }
    if (source.empty()) {
      std::vector<std::string> found;
      if (DIR* d = opendir(folder.c_str())) {
        while (dirent* e = readdir(d)) {
          const std::string n = e->d_name, x = lower_ext(n);
          if (n[0] != '.' && (x == ".rvz" || x == ".wia")) found.push_back(folder + "/" + n);
        }
        closedir(d);
      }
      if (found.size() == 1) source = found[0];
      else if (found.size() > 1) say("app: %zu RVZ or WIA images in %s: name the one to use with iso= in launcher.ini", found.size(), folder.c_str());
    }
  }
  if (source.empty()) return exists(*iso);
  if (exists(target)) { *iso = target; return true; }   // converted before
  disc_rvz::Info info;
  std::string error;
  if (!disc_rvz::probe(source, &info, &error)) {
    say("app: %s cannot be read as a disc image: %s", source.c_str(), error.c_str());
    app_message_screen("DISC IMAGE NOT USABLE", {source, error, "", "Use a clean Melee NTSC 1.02 image (ISO, or RVZ/WIA made by Dolphin), then start the app again."});
    return false;
  }
  if (!info.gamecube || info.game_id != "GALE01") {
    say("app: %s is %s (%s), not Super Smash Bros. Melee NTSC", source.c_str(), info.game_id.c_str(), info.gamecube ? "GameCube" : "not a GameCube disc");
    app_message_screen("NOT A MELEE DISC", {source, "is " + info.game_id + ", not Super Smash Bros. Melee (GALE01).", "", "Put your Melee NTSC 1.02 image there, then start the app again."});
    return false;
  }
  say("app: converting %s (%s, %s compression, revision %d) into %s (%.2f GB)", source.c_str(), info.format == disc_rvz::Format::Rvz ? "RVZ" : "WIA",
      info.compression.c_str(), info.revision, target.c_str(), double(info.iso_size) / 1e9);
  app_message_screen("CONVERTING YOUR DISC", {"Your disc image is a Dolphin " + std::string(info.format == disc_rvz::Format::Rvz ? "RVZ" : "WIA") +
                     " file. The app now writes the plain image it plays from:", target, "",
                     "This happens once and takes a few minutes on the SD card; the screen stays dark meanwhile.", "The compressed file is left as it was."});
  const auto t0 = std::chrono::steady_clock::now();
  int last_tenth = -1;
  const bool ok = disc_rvz::convert(source, target, [&](uint64_t done, uint64_t total) {
    const int tenth = total ? int(done * 10 / total) : 0;
    if (tenth != last_tenth) { last_tenth = tenth; say("app: converting the disc: %d%%", tenth * 10); }
    return true;
  }, &error);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!ok) {
    say("app: the conversion failed after %.0f s: %s", seconds, error.c_str());
    app_message_screen("THE DISC COULD NOT BE CONVERTED", {source, error, "", "Check the free space on the SD card (about 1.4 GB), then start the app again."});
    return false;
  }
  say("app: %s written in %.0f s", target.c_str(), seconds);
  *iso = target;
  return true;
#else
  return exists(*iso);
#endif
}
}  // namespace fe
