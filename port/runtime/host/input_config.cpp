// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_config.h"
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
#include "darwin_keycodes.h"
#endif

namespace host {
const char* const kGcControlNames[GC_CTL_COUNT] = {"A", "B", "X", "Y", "Z", "L", "R", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
const char* const kKeyboardControlNames[KB_COUNT] = {"A", "B", "X", "Y", "Z", "L", "R", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                                     "Stick up", "Stick down", "Stick left", "Stick right",
                                                     "C-stick up", "C-stick down", "C-stick left", "C-stick right", "Walk / tilt modifier"};
namespace {
std::vector<ControllerConfig> g_configs;
KeyboardMap g_keyboard = KeyboardMap::defaults();
// SDL_GamepadButton ids: 0 south, 1 east, 2 west, 3 north, 4 back, 5 guide, 6 start, 7 lstick, 8 rstick,
// 9 lshoulder, 10 rshoulder, 11..14 dpad up/down/left/right.
bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
}  // namespace

ControllerMap ControllerMap::defaults() {
  ControllerMap m;
  const int d[GC_CTL_COUNT] = {0, 2, 1, 3, 10, kTriggerLeft, kTriggerRight, 6, 11, 12, 13, 14};
  std::memcpy(m.binding, d, sizeof d);
  return m;
}
std::string ControllerMap::serialize() const {
  std::ostringstream out;
  for (int i = 0; i < GC_CTL_COUNT; ++i) out << (i ? "," : "") << binding[i];
  out << (swap_sticks ? ",swap" : "") << ",dz=" << stick_deadzone << ",cdz=" << cstick_deadzone << ",tp=" << trigger_press << (rumble ? "" : ",norumble");
  return out.str();
}
ControllerMap ControllerMap::parse(const std::string& text) {
  ControllerMap m = defaults();
  std::istringstream in(text);
  std::string token; int i = 0;
  while (std::getline(in, token, ',')) {
    if (token == "swap") m.swap_sticks = true;
    else if (token == "norumble") m.rumble = false;
    else if (starts_with(token, "dz=")) m.stick_deadzone = std::clamp(std::atoi(token.c_str() + 3), 0, 90);
    else if (starts_with(token, "cdz=")) m.cstick_deadzone = std::clamp(std::atoi(token.c_str() + 4), 0, 90);
    else if (starts_with(token, "tp=")) m.trigger_press = std::clamp(std::atoi(token.c_str() + 3), 5, 100);
    else if (i < GC_CTL_COUNT) m.binding[i++] = std::atoi(token.c_str());
  }
  return m;
}
const std::vector<ControllerConfig>& controller_configs() { return g_configs; }
void set_controller_configs(std::vector<ControllerConfig> configs) { g_configs = std::move(configs); }
const ControllerConfig* controller_config_for(const std::string& guid) {
  for (const ControllerConfig& c : g_configs) if (c.guid == guid) return &c;
  return nullptr;
}
void upsert_controller_config(const ControllerConfig& config) {
  for (ControllerConfig& c : g_configs) if (c.guid == config.guid) { c = config; return; }
  g_configs.push_back(config);
}
std::string physical_input_name(int input) {
  static const char* const names[] = {"A / Cross", "B / Circle", "X / Square", "Y / Triangle", "Back", "Guide", "Start / Options", "Left stick click", "Right stick click",
                                      "Left shoulder", "Right shoulder", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc 1", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
  if (input == kTriggerLeft) return "Left trigger";
  if (input == kTriggerRight) return "Right trigger";
  if (input >= 0 && input < (int)(sizeof names / sizeof names[0])) return names[input];
  if (input == kUnbound) return "Unbound";
  char buf[32]; std::snprintf(buf, sizeof buf, "Button %d", input); return buf;
}

KeyboardMap KeyboardMap::defaults() {
  KeyboardMap m;
  const int d[KB_COUNT] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_RETURN,
                           SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H,
                           SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                           SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_LSHIFT};
  std::memcpy(m.key, d, sizeof d);
  return m;
}
std::string KeyboardMap::serialize() const {
  std::ostringstream out;
  for (int i = 0; i < KB_COUNT; ++i) out << (i ? "," : "") << key[i];
  out << ",m=" << modifier_percent;
  return out.str();
}
KeyboardMap KeyboardMap::parse(const std::string& text) {
  KeyboardMap m = defaults();
  std::istringstream in(text);
  std::string token; int i = 0;
  while (std::getline(in, token, ',')) {
    if (starts_with(token, "m=")) m.modifier_percent = std::clamp(std::atoi(token.c_str() + 2), 10, 100);
    else if (i < KB_COUNT) m.key[i++] = std::max(0, std::atoi(token.c_str()));
  }
  return m;
}
const KeyboardMap& keyboard_map() { return g_keyboard; }
void set_keyboard_map(const KeyboardMap& map) { g_keyboard = map; }
std::string key_name(int scancode) {
  if (scancode <= 0) return "Unbound";
  const char* name = SDL_GetScancodeName((SDL_Scancode)scancode);
  return name && *name ? name : "Key " + std::to_string(scancode);
}
int scancode_from_mac_keycode(int keycode) {
#if defined(__APPLE__) && TARGET_OS_OSX
  if (keycode >= 0 && keycode < (int)(sizeof kDarwinKeycodeToScancode / sizeof kDarwinKeycodeToScancode[0])) return (int)kDarwinKeycodeToScancode[keycode];
#endif
  (void)keycode;
  return 0;
}
}  // namespace host
