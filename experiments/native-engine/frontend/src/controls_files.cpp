// Controller profiles and Dolphin profile import: see controls_files.h.
// Adapted from melee-unlocked v1.0.5 port/runtime/host/controller_profiles.cpp and dolphin_profile.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "controls_files.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/stat.h>

namespace fe::controls {
namespace {
constexpr const char* kHeader = "# Melee Unlocked controller profile";
constexpr const char* kExtension = ".profile";
constexpr int kFirstStickAction = SUp;

std::string file_for(const std::string& folder, const std::string& name) { return folder + "/" + name + kExtension; }
std::string read_file(const std::string& path, size_t limit) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.size() > limit) return {};
  return text;
}
bool device_of(const std::string& path, std::string& device) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key, value;
    if (!(words >> key) || key[0] == '#') continue;
    if (key == "device" && (words >> value)) { device = value; return true; }
  }
  return false;
}
bool less_nocase(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                      [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
}
std::vector<std::string> files_with(const std::string& folder, const char* extension) {
  std::vector<std::string> out;
  if (DIR* dir = opendir(folder.c_str())) {
    const size_t n = std::strlen(extension);
    while (dirent* e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name.size() <= n || name[0] == '.') continue;
      std::string tail = name.substr(name.size() - n);
      for (char& ch : tail) ch = char(std::tolower((unsigned char)ch));
      if (tail == extension) out.push_back(name);
    }
    closedir(dir);
  }
  std::sort(out.begin(), out.end(), less_nocase);
  return out;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}
}  // namespace

const char* kind_key(Kind kind) { return kind == Kind::Adapter ? "gcadapter" : kind == Kind::Keyboard ? "keyboard" : "sdlgamepad"; }   // keyboard: upstream's virtual-key profiles

std::string profile_clean_name(const std::string& name) {
  std::string out;
  for (unsigned char c : name)
    if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') out += char(c);
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() > 40) out.resize(40);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::vector<std::string> profile_list(const std::string& folder, Kind kind) {
  std::vector<std::string> names;
  for (const std::string& f : files_with(folder, kExtension)) {
    std::string device;
    if (device_of(folder + "/" + f, device) && device == kind_key(kind)) names.push_back(f.substr(0, f.size() - std::strlen(kExtension)));
  }
  return names;
}

bool profile_save(const std::string& folder, const std::string& name, const Profile& p) {
  const std::string clean = profile_clean_name(name);
  if (clean.empty() || clean != name) return false;
  mkdir(folder.c_str(), 0777);
  const std::string path = file_for(folder, clean), temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return false;
    out << kHeader << "\ndevice " << kind_key(p.config.kind) << "\n";
    for (int i = 0; i < kActions; ++i) out << kActionNames[i] << " " << p.config.mask[i] << "\n";
    // Beyond upstream's lines (its reader skips unknown names): the levels and the analog settings.
    for (int i = 0; i < kActions; ++i) if (p.config.level[i]) out << "opt.v" << i << " " << int(p.config.level[i]) << "\n";
    const Config& c = p.config;
    const std::pair<const char*, int> opts[] = {{"inv", c.invert}, {"mod", c.modifier_percent}, {"dz", c.dz_main}, {"dzc", c.dz_c},
        {"rad", c.radius_main}, {"radc", c.radius_c}, {"clkl", c.click_l}, {"clkr", c.click_r}, {"capl", c.cap_l}, {"capr", c.cap_r},
        {"snx", c.snap_x}, {"sny", c.snap_y}, {"snmx", c.snap_ms_x}, {"snmy", c.snap_ms_y}};
    for (const auto& [k, v] : opts) out << "opt." << k << " " << v << "\n";
    if (p.has_app)
      out << "app.stick_deadzone " << p.stick_deadzone << "\napp.cstick_deadzone " << p.cstick_deadzone << "\napp.trigger_press " << p.trigger_press
          << "\napp.swap_sticks " << (p.swap_sticks ? 1 : 0) << "\napp.rumble " << (p.rumble ? 1 : 0) << "\n";
    if (!out) return false;
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0) { std::remove(temporary.c_str()); return false; }
  return true;
}

bool profile_load(const std::string& folder, const std::string& name, Profile& p) {
  const std::string clean = profile_clean_name(name);
  if (clean.empty() || clean != name) return false;
  std::ifstream in(file_for(folder, clean));
  if (!in) return false;
  Profile read = p;
  for (int i = 0; i < kFirstStickAction; ++i) read.config.mask[i] = 0;   // upstream: an earlier action not mentioned is unbound
  bool device_ok = false;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key, value;
    if (!(words >> key) || key[0] == '#' || !(words >> value)) continue;
    if (key == "device") { device_ok = value == kind_key(p.config.kind); continue; }
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (!end || *end) continue;
    if (key.rfind("opt.", 0) == 0) { read.config.set(key.substr(4), v); continue; }
    if (key.rfind("app.", 0) == 0) {
      const std::string k = key.substr(4);
      read.has_app = true;
      if (k == "stick_deadzone") read.stick_deadzone = std::clamp(int(v), 0, 60);
      else if (k == "cstick_deadzone") read.cstick_deadzone = std::clamp(int(v), 0, 60);
      else if (k == "trigger_press") read.trigger_press = std::clamp(int(v), 5, 100);
      else if (k == "swap_sticks") read.swap_sticks = v != 0;
      else if (k == "rumble") read.rumble = v != 0;
      continue;
    }
    for (int i = 0; i < kActions; ++i)
      if (key == kActionNames[i]) read.config.mask[i] = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
  }
  if (!device_ok) return false;
  p = read;
  return true;
}

bool profile_delete(const std::string& folder, const std::string& name, Kind kind) {
  const std::string clean = profile_clean_name(name);
  if (clean.empty() || clean != name) return false;
  std::string device;
  const std::string path = file_for(folder, clean);
  if (!device_of(path, device) || device != kind_key(kind)) return false;   // never another kind's file
  return std::remove(path.c_str()) == 0;
}

std::string profile_free_name(const std::string& folder, const std::string& stem) {
  struct stat st{};
  for (int n = 1; n < 1000; ++n) {
    const std::string name = stem + " " + std::to_string(n);
    if (stat(file_for(folder, name).c_str(), &st) != 0) return name;
  }
  return stem;
}

// ---- Dolphin ----
namespace {
// Dolphin's setting for each action, in Action order. Here: also the stick modifier.
constexpr const char* kDolphinKeys[kActions] = {
    "Buttons/A", "Buttons/B", "Buttons/X", "Buttons/Y", "Buttons/Z", "Buttons/Start", "Triggers/L", "Triggers/R",
    "D-Pad/Up", "D-Pad/Down", "D-Pad/Left", "D-Pad/Right", "C-Stick/Up", "C-Stick/Down", "C-Stick/Left", "C-Stick/Right",
    "Main Stick/Up", "Main Stick/Down", "Main Stick/Left", "Main Stick/Right", "Triggers/L-Analog", "Triggers/R-Analog",
    nullptr, nullptr, nullptr, nullptr, "Main Stick/Modifier"};
struct Name { const char* name; uint32_t source; };
// SDL gamepad button numbers (SDL_GamepadButton): SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, GUIDE 5, START 6,
// LEFT_STICK 7, RIGHT_STICK 8, LEFT_SHOULDER 9, RIGHT_SHOULDER 10, DPAD_UP 11, DOWN 12, LEFT 13, RIGHT 14, MISC1 15,
// paddles 16..19, TOUCHPAD 20. Dolphin's SDL backend names them by position, its XInput backend by Xbox letter.
constexpr Name kSdlNames[] = {
    {"Button S", 1u << 0}, {"Button E", 1u << 1}, {"Button W", 1u << 2}, {"Button N", 1u << 3}, {"Back", 1u << 4}, {"Guide", 1u << 5},
    {"Start", 1u << 6}, {"Thumb L", 1u << 7}, {"Thumb R", 1u << 8}, {"Shoulder L", 1u << 9}, {"Shoulder R", 1u << 10},
    {"Pad N", 1u << 11}, {"Pad S", 1u << 12}, {"Pad W", 1u << 13}, {"Pad E", 1u << 14}, {"Misc 1", 1u << 15},
    {"Paddle 1", 1u << 16}, {"Paddle 2", 1u << 17}, {"Paddle 3", 1u << 18}, {"Paddle 4", 1u << 19}, {"Touchpad", 1u << 20},
    {"Trigger L", kSrcLT}, {"Trigger R", kSrcRT}, {"Trigger L+", kSrcLT}, {"Trigger R+", kSrcRT},
    {"Full Axis 4+", kSrcLT}, {"Full Axis 5+", kSrcRT}};
constexpr Name kXInputNames[] = {
    {"Button A", 1u << 0}, {"Button B", 1u << 1}, {"Button X", 1u << 2}, {"Button Y", 1u << 3}, {"Back", 1u << 4}, {"Guide", 1u << 5},
    {"Start", 1u << 6}, {"Thumb L", 1u << 7}, {"Thumb R", 1u << 8}, {"Shoulder L", 1u << 9}, {"Shoulder R", 1u << 10},
    {"Pad N", 1u << 11}, {"Pad S", 1u << 12}, {"Pad W", 1u << 13}, {"Pad E", 1u << 14},
    {"Trigger L", kSrcLT}, {"Trigger R", kSrcRT}};

// One plain control out of a Dolphin expression element: `Button A`, Start, `XInput/0/Gamepad:Button A`.
std::string single_control(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && value.front() == '`' && value.back() == '`') {
    value = value.substr(1, value.size() - 2);
    if (value.find('`') != std::string::npos) return {};
  } else {
    for (unsigned char c : value)
      if (!std::isalnum(c) && c != '_' && c != '+' && c != '-' && c != ' ') return {};
  }
  const size_t device = value.rfind(':');
  if (device != std::string::npos) value = value.substr(device + 1);
  return trim(value);
}
// `A | B | C` of plain controls; empty when any part is something else (a modifier, a function, `&`).
std::vector<std::string> controls_of(const std::string& value) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= value.size()) {
    size_t bar = value.find('|', at);
    if (bar == std::string::npos) bar = value.size();
    const std::string one = single_control(value.substr(at, bar - at));
    if (one.empty()) return {};
    out.push_back(one);
    at = bar + 1;
  }
  return out;
}
// "Left X+", "Right Y-": a stick axis. side 0 left, 1 right; axis 0 X, 1 Y; sign +1 / -1.
bool axis_of(const std::string& name, int& side, int& axis, int& sign) {
  std::string n = name;
  if (n.rfind("Left ", 0) == 0) { side = 0; n = n.substr(5); }
  else if (n.rfind("Right ", 0) == 0) { side = 1; n = n.substr(6); }
  else return false;
  if (n.size() != 2 || (n[0] != 'X' && n[0] != 'Y') || (n[1] != '+' && n[1] != '-')) return false;
  axis = n[0] == 'X' ? 0 : 1; sign = n[1] == '+' ? 1 : -1;
  return true;
}
}  // namespace

std::vector<DolphinEntry> dolphin_list(const std::string& folder) {
  std::vector<DolphinEntry> out;
  for (const std::string& f : files_with(folder, ".ini")) {
    const std::string path = folder + "/" + f, text = read_file(path, 256 * 1024);
    const std::string stem = f.substr(0, f.size() - 4);
    std::istringstream lines(text);
    std::string line, section;
    std::vector<std::string> pads;
    while (std::getline(lines, line)) {
      line = trim(line);
      if (line.size() > 2 && line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
      if (line.rfind("Device", 0) == 0 && line.find('=') != std::string::npos && section.rfind("GCPad", 0) == 0) pads.push_back(section);
    }
    if (!pads.empty()) for (const auto& s : pads) out.push_back({path, s, stem + " " + s});
    else if (text.find("Device") != std::string::npos) out.push_back({path, "", stem});
  }
  return out;
}

DolphinResult dolphin_read(const std::string& text, const std::string& section) {
  DolphinResult out;
  std::string values[kActions], device;
  std::string dz_main, dz_c, threshold, modifier_range, radius_main, radius_c;
  std::istringstream lines(text);
  std::string line, current;
  while (std::getline(lines, line)) {
    const std::string t = trim(line);
    if (t.size() > 2 && t.front() == '[' && t.back() == ']') { current = t.substr(1, t.size() - 2); continue; }
    if (!section.empty() && current != section) continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
    if (key == "Device") device = value;
    else if (key == "Main Stick/Dead Zone") dz_main = value;
    else if (key == "C-Stick/Dead Zone") dz_c = value;
    else if (key == "Triggers/Threshold") threshold = value;
    else if (key == "Main Stick/Modifier/Range") modifier_range = value;
    else if (key == "Main Stick/Radius") radius_main = value;
    else if (key == "C-Stick/Radius") radius_c = value;
    for (int i = 0; i < kActions; ++i)
      if (kDolphinKeys[i] && key == kDolphinKeys[i]) values[i] = value;
  }
  out.device = device;
  out.profile.config = Config::defaults(Kind::Sdl);
  out.profile.has_app = true;
  if (device.empty()) { out.message = "not a Dolphin controller profile (no Device line)"; return out; }
  const bool xinput = device.rfind("XInput/", 0) == 0, sdl = device.rfind("SDL/", 0) == 0;
  if (!xinput && !sdl) { out.message = "only SDL and XInput profiles can be read here; this one is for " + device; return out; }
  Config& c = out.profile.config;
  for (int i = 0; i < kActions; ++i) c.mask[i] = 0;   // only what the profile binds
  int stick_side[2] = {-1, -1};   // [main, c]: which physical stick feeds it
  for (int i = 0; i < kActions; ++i) {
    if (values[i].empty()) continue;
    const std::vector<std::string> names = controls_of(values[i]);
    if (names.empty()) { ++out.skipped; continue; }
    uint32_t mask = 0;
    bool understood = true, axis_seen = false;
    for (const std::string& n : names) {
      int side = 0, axis = 0, sign = 0;
      if (axis_of(n, side, axis, sign)) {
        // A stick direction on a stick axis: which stick, and whether it is reversed. The direction's own axis
        // must be the matching one (Up on Y, Right on X); anything rotated is not understood.
        const bool main = i >= SUp && i <= SRight, cst = i >= CUp && i <= CRight;
        const int dir = main ? i - SUp : cst ? i - CUp : -1;   // 0 up, 1 down, 2 left, 3 right
        if (dir < 0 || (dir < 2 ? axis != 1 : axis != 0)) { understood = false; break; }
        const int want = (dir == 0 || dir == 3) ? 1 : -1;
        const int which = main ? 0 : 1;
        stick_side[which] = side;
        if (sign != want) c.invert |= uint8_t(axis == 0 ? (main ? kInvertStickX : kInvertCX) : (main ? kInvertStickY : kInvertCY));
        axis_seen = true;
        continue;
      }
      bool known = false;
      for (const Name& k : (xinput ? std::vector<Name>(std::begin(kXInputNames), std::end(kXInputNames)) : std::vector<Name>(std::begin(kSdlNames), std::end(kSdlNames))))
        if (n == k.name) { mask |= k.source; known = true; break; }
      if (!known) { understood = false; break; }
    }
    if (!understood) { ++out.skipped; continue; }
    if (axis_seen && !mask) { ++out.bound; continue; }   // the stick itself: nothing to bind
    c.mask[i] = mask;
    ++out.bound;
  }
  // Both sticks named: the main stick on the right stick is the app's stick swap.
  // The inversion bits were set per game stick and stay with it (the swap happens before the inversion).
  if (stick_side[0] == 1 && stick_side[1] != 1) out.profile.swap_sticks = true;
  auto pct = [](const std::string& v, int fallback) { return v.empty() ? fallback : int(std::strtod(v.c_str(), nullptr) + 0.5); };
  out.profile.stick_deadzone = std::clamp(pct(dz_main, 24), 0, 60);
  out.profile.cstick_deadzone = std::clamp(pct(dz_c, 27), 0, 60);
  out.profile.trigger_press = std::clamp(pct(threshold, 78), 5, 100);
  c.modifier_percent = std::clamp(pct(modifier_range, 50), 0, 100);
  c.radius_main = std::clamp(pct(radius_main, 100), 50, 100);
  c.radius_c = std::clamp(pct(radius_c, 100), 50, 100);
  // A trigger whose travel the profile leaves unbound: the travel of the trigger its click is on (Dolphin's analog
  // value and its click come from the same trigger unless the profile says otherwise).
  if (!c.mask[LAnalog]) c.mask[LAnalog] = c.mask[L] & (kSrcLT | kSrcRT);
  if (!c.mask[RAnalog]) c.mask[RAnalog] = c.mask[R] & (kSrcLT | kSrcRT);
  out.ok = out.bound > 0;
  if (!out.ok) out.message = "no binding in this profile could be read";
  return out;
}

DolphinResult dolphin_read_entry(const DolphinEntry& e) {
  const std::string text = read_file(e.file, 256 * 1024);
  if (text.empty()) { DolphinResult r; r.message = "the file could not be read"; return r; }
  return dolphin_read(text, e.section);
}

}  // namespace fe::controls
