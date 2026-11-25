// Controller processing beyond the app's button remap: every GameCube action bindable (stick and C-stick
// directions, trigger travel, Mod X / Mod Y / light and mid shield / stick modifier), box mode with the B0XX
// coordinates, stick axis inversion, radial deadzone, stick radius, snapback filter, trigger click point and
// trigger cap. Pure functions, no SDL: controls.cpp feeds them, tools/controls_check.cpp tests them.
//
// Adapted from melee-unlocked v1.0.5 (https://github.com/Hero88go/melee-unlocked, GPL-2.0-or-later):
// port/runtime/host/input_binding_types.h (the action list and its order), input_bindings.h
// (apply_cstick_actions, apply_stick_actions, apply_box_actions, analog_binding_travel, gc_binding_pressed,
// snapback_axis, apply_deadzone, apply_stick_radius, apply_trigger_click, apply_trigger_cap, the defaults) and
// window.cpp (apply_family_options: the order snapback, deadzone, radius, click, cap). box_melee.h itself is
// compiled from the fetched tree, unchanged. Differences are marked "here:".
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "box_melee.h"   // melee-unlocked port/runtime/host/box_melee.h (the fetched tree)

namespace fe::controls {
namespace box = host::box;

// Upstream's BindAction order, so a profile's action names and numbers mean the same thing there and here.
enum Action : int {
  A, B, X, Y, Z, Start, L, R, DUp, DDown, DLeft, DRight,
  CUp, CDown, CLeft, CRight, SUp, SDown, SLeft, SRight,
  LAnalog, RAnalog, ModX, ModY, LightShield, MidShield, StickMod,
  kActions
};
inline constexpr const char* kActionNames[kActions] = {"A", "B", "X", "Y", "Z", "Start", "L", "R", "DUp", "DDown", "DLeft", "DRight",
    "CUp", "CDown", "CLeft", "CRight", "SUp", "SDown", "SLeft", "SRight", "LAnalog", "RAnalog", "ModX", "ModY", "LightShield", "MidShield", "StickMod"};
inline constexpr const char* kActionLabels[kActions] = {"A", "B", "X", "Y", "Z", "Start", "L (click)", "R (click)", "D-pad up", "D-pad down",
    "D-pad left", "D-pad right", "C-stick up", "C-stick down", "C-stick left", "C-stick right", "Stick up", "Stick down", "Stick left",
    "Stick right", "L travel (light shield)", "R travel (light shield)", "Mod X (box)", "Mod Y (box)", "Light shield button", "Mid shield button",
    "Stick modifier (walk)"};
// The GameCube pad bit of each button action (upstream kActionPadBit); 0 for the actions that are not buttons.
inline constexpr uint16_t kPadBit[kActions] = {0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x1000, 0x0040, 0x0020, 0x0008, 0x0004, 0x0001, 0x0002};
inline constexpr uint16_t kPadL = 0x0040, kPadR = 0x0020;
inline constexpr bool is_analog_action(int i) { return i == LAnalog || i == RAnalog; }

// The two kinds of device on the console. Their source numbers differ, so a profile names its kind.
//  Sdl:     bit b (0..27) is SDL gamepad button b (SDL_GamepadButton), kSrcLT / kSrcRT the analog triggers.
//  Adapter: a GameCube controller on the adapter; the bits are its own buttons in the pad layout (kPadBit),
//           where L and R are the clicks at the end of the triggers' travel (as upstream's gcadapter tables, so a
//           gcadapter profile saved by melee-unlocked on a PC loads here unchanged).
//  Keyboard: a USB keyboard (the console's HID keyboard, usb_keyboard.cpp); mask[i] is the Windows virtual-key code of the key
//           bound to action i (upstream's KeyBindings: one key per action, 0 unbound), so upstream's "keyboard" profiles and
//           its default layout (default_key_bindings) mean the same keys here.
enum class Kind : uint8_t { Sdl, Adapter, Keyboard };
inline constexpr uint32_t kSrcLT = 1u << 28, kSrcRT = 1u << 29;

inline constexpr int kDefaultTriggerClick = 200;   // upstream: the physical click is kept at this value
inline constexpr int kDefaultAnalogDepth = 100;    // a button on L / R travel with no level: a light shield
inline constexpr int kSnapbackEdge = 60, kSnapbackMinMs = 4, kSnapbackMaxMs = 50;
inline constexpr uint8_t kInvertStickX = 1, kInvertStickY = 2, kInvertCX = 4, kInvertCY = 8;

// One device's settings. Defaults change nothing: a device whose Config equals its defaults is passed through.
struct Config {
  Kind kind = Kind::Sdl;
  uint32_t mask[kActions] = {};       // 0 = unbound. Sdl: only actions CUp.. are used (the app's map has the twelve buttons)
  uint8_t level[kActions] = {};       // upstream's per-binding level (0 = the device default)
  uint8_t invert = 0;                 // kInvert* bits
  int modifier_percent = 50;          // the stick modifier's push (upstream g_stick_modifier_percent)
  int dz_main = 0, dz_c = 0;          // radial deadzone in the game's units (127 = a full push); 0 off
  int radius_main = 100, radius_c = 100;
  int click_l = kDefaultTriggerClick, click_r = kDefaultTriggerClick;   // Adapter: where L / R click (200 = the switch)
  int cap_l = 255, cap_r = 255;       // below 255: the trigger is analog only, capped there, no click
  int snap_x = 0, snap_y = 0;         // snapback filter: the largest bounce removed (0 off, up to 60)
  int snap_ms_x = 20, snap_ms_y = 20; // and how long after the stick leaves the edge
  static Config defaults(Kind kind) {
    Config c;
    c.kind = kind;
    if (kind == Kind::Sdl) {
      c.mask[LAnalog] = kSrcLT;   // the trigger's own travel, as Aurora's PAD gives it
      c.mask[RAnalog] = kSrcRT;
    } else if (kind == Kind::Keyboard) {   // upstream default_key_bindings, in its virtual-key codes
      static constexpr uint8_t keys[kActions] = {'Z', 'X', 'C', 'V', 'E', 0x0D /* Return */, 'Q', 'W', 'T', 'G', 'F', 'H',
                                                 'I', 'K', 'J', 'L', 0x26, 0x28, 0x25, 0x27 /* the arrows: up, down, left, right */,
                                                 0, 0, 0, 0, 0, 0, 0x10 /* Shift walks */};
      for (int i = 0; i < kActions; ++i) c.mask[i] = keys[i];
    } else {   // upstream default_gc_bindings: the identity, L / R travel from the triggers themselves
      for (int i = 0; i <= DRight; ++i) c.mask[i] = kPadBit[i];
      c.mask[LAnalog] = kPadL;
      c.mask[RAnalog] = kPadR;
    }
    return c;
  }
  bool same_as(const Config& o) const {
    return kind == o.kind && std::memcmp(mask, o.mask, sizeof mask) == 0 && std::memcmp(level, o.level, sizeof level) == 0 &&
           invert == o.invert && modifier_percent == o.modifier_percent && dz_main == o.dz_main && dz_c == o.dz_c &&
           radius_main == o.radius_main && radius_c == o.radius_c && click_l == o.click_l && click_r == o.click_r &&
           cap_l == o.cap_l && cap_r == o.cap_r && snap_x == o.snap_x && snap_y == o.snap_y && snap_ms_x == o.snap_ms_x &&
           snap_ms_y == o.snap_ms_y;
  }
  bool is_default() const { return same_as(defaults(kind)); }
  bool box_mode() const { return mask[ModX] || mask[ModY]; }   // upstream: binding Mod X or Mod Y turns box mode on
  bool bindings_default() const {
    const Config d = defaults(kind);
    return std::memcmp(mask, d.mask, sizeof mask) == 0 && std::memcmp(level, d.level, sizeof level) == 0;
  }

  // launcher.ini form: only what differs from the defaults, "k=v" separated by commas ("m22=512,dz=8,rad=90").
  std::string serialize() const {
    const Config d = defaults(kind);
    std::string out;
    char buf[48];
    auto put = [&](const char* k, long v) { std::snprintf(buf, sizeof buf, "%s%s=%ld", out.empty() ? "" : ",", k, v); out += buf; };
    for (int i = 0; i < kActions; ++i) {
      char k[8];
      if (mask[i] != d.mask[i]) { std::snprintf(k, sizeof k, "m%d", i); put(k, long(mask[i])); }
      if (level[i] != d.level[i]) { std::snprintf(k, sizeof k, "v%d", i); put(k, long(level[i])); }
    }
    if (invert != d.invert) put("inv", invert);
    if (modifier_percent != d.modifier_percent) put("mod", modifier_percent);
    if (dz_main != d.dz_main) put("dz", dz_main);
    if (dz_c != d.dz_c) put("dzc", dz_c);
    if (radius_main != d.radius_main) put("rad", radius_main);
    if (radius_c != d.radius_c) put("radc", radius_c);
    if (click_l != d.click_l) put("clkl", click_l);
    if (click_r != d.click_r) put("clkr", click_r);
    if (cap_l != d.cap_l) put("capl", cap_l);
    if (cap_r != d.cap_r) put("capr", cap_r);
    if (snap_x != d.snap_x) put("snx", snap_x);
    if (snap_y != d.snap_y) put("sny", snap_y);
    if (snap_ms_x != d.snap_ms_x) put("snmx", snap_ms_x);
    if (snap_ms_y != d.snap_ms_y) put("snmy", snap_ms_y);
    return out;
  }
  // Every value is brought into its range; unknown keys are ignored.
  static Config parse(Kind kind, const std::string& text) {
    Config c = defaults(kind);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = item.substr(0, eq);
      const long v = std::strtol(item.c_str() + eq + 1, nullptr, 10);
      c.set(k, v);
    }
    return c;
  }
  void set(const std::string& k, long v) {
    auto clampv = [](long x, long lo, long hi) { return int(x < lo ? lo : x > hi ? hi : x); };
    if ((k[0] == 'm' || k[0] == 'v') && k.size() > 1 && k[1] >= '0' && k[1] <= '9') {
      const int i = std::atoi(k.c_str() + 1);
      if (i < 0 || i >= kActions) return;
      if (k[0] == 'm') mask[i] = uint32_t(v); else level[i] = uint8_t(clampv(v, 0, 255));
      return;
    }
    if (k == "inv") invert = uint8_t(v & 15);
    else if (k == "mod") modifier_percent = clampv(v, 0, 100);
    else if (k == "dz") dz_main = clampv(v, 0, 100);
    else if (k == "dzc") dz_c = clampv(v, 0, 100);
    else if (k == "rad") radius_main = clampv(v, 50, 100);
    else if (k == "radc") radius_c = clampv(v, 50, 100);
    else if (k == "clkl") click_l = clampv(v, 1, 255);
    else if (k == "clkr") click_r = clampv(v, 1, 255);
    else if (k == "capl") cap_l = clampv(v, 0, 255);
    else if (k == "capr") cap_r = clampv(v, 0, 255);
    else if (k == "snx") snap_x = clampv(v, 0, kSnapbackEdge);
    else if (k == "sny") snap_y = clampv(v, 0, kSnapbackEdge);
    else if (k == "snmx") snap_ms_x = clampv(v, kSnapbackMinMs, kSnapbackMaxMs);
    else if (k == "snmy") snap_ms_y = clampv(v, kSnapbackMinMs, kSnapbackMaxMs);
  }
};

// The pad the game gets (same fields as host::PadState / MuPadStatus).
struct Pad { uint16_t button; int8_t stick_x, stick_y, sub_x, sub_y; uint8_t trig_l, trig_r; };

// What a device reports this poll, in its own source numbers.
struct Raw {
  uint32_t buttons = 0;     // Sdl: bit b = SDL button b held; Adapter: the pad bits as the adapter reported them
  uint8_t lt = 0, rt = 0;   // the triggers' travel (0..255)
  const uint8_t* keys = nullptr;   // Keyboard: 256 bits, bit v = virtual key v held
};
inline bool key_held(const uint8_t* keys, uint32_t vk) { return keys && vk && vk < 256 && (keys[vk >> 3] >> (vk & 7) & 1); }

// ---- upstream input_bindings.h, adapted ----

// Upstream snapback_axis, unchanged: after a hard release a reading on the other side smaller than `size` within
// `window_ms` is the stick bouncing past centre and reads as centre.
struct SnapbackAxis { int armed = 0; double left_edge = -1.0; };
inline int8_t snapback_axis(SnapbackAxis& a, int8_t value, double now, int size, int window_ms) {
  if (size <= 0) { a = SnapbackAxis{}; return value; }
  constexpr int kEdge = kSnapbackEdge;
  const int v = value, sign = v > 0 ? 1 : v < 0 ? -1 : 0;
  const double window = (window_ms < kSnapbackMinMs ? kSnapbackMinMs : window_ms > kSnapbackMaxMs ? kSnapbackMaxMs : window_ms) / 1000.0;
  const int threshold = size > kEdge ? kEdge : size;
  if (v >= kEdge || v <= -kEdge) { a.armed = sign; a.left_edge = -1.0; return value; }
  if (!a.armed) return value;
  if (a.left_edge < 0.0) a.left_edge = now;
  if (now - a.left_edge > window) { a.armed = 0; return value; }
  if (sign == -a.armed && (v < 0 ? -v : v) < threshold) return 0;
  return value;
}
inline void apply_deadzone(int r, int8_t& x, int8_t& y) {
  if (r > 0 && int(x) * x + int(y) * y < r * r) { x = 0; y = 0; }
}
// Rounds to nearest, away from zero on a tie, so both directions of an axis scale alike.
inline void apply_stick_radius(int r, int8_t& x, int8_t& y) {
  if (r >= 100 || r <= 0) return;
  auto scale = [r](int8_t v) { const int n = int(v) * r; return int8_t((n + (n >= 0 ? 50 : -50)) / 100); };
  x = scale(x); y = scale(y);
}
inline void apply_trigger_click(int threshold, uint8_t value, uint16_t& button, uint16_t click) {
  if (value > threshold) button |= click;
}
inline void apply_trigger_cap(int cap, uint8_t& value, uint16_t& button, uint16_t click) {
  if (cap >= 255) return;
  // A full press with the trigger at rest comes from a button bound to L or R: it stays a full press.
  if ((button & click) && value < 43) { value = 255; return; }
  if (value > cap) value = uint8_t(cap < 0 ? 0 : cap);
  button &= uint16_t(~click);
}
inline void apply_cstick_actions(uint32_t actions, int8_t& sub_x, int8_t& sub_y) {
  const bool up = actions & (1u << CUp), down = actions & (1u << CDown), left = actions & (1u << CLeft), right = actions & (1u << CRight);
  if (up != down) sub_y = up ? 127 : -127;
  if (left != right) sub_x = right ? 127 : -127;
}
inline bool apply_stick_actions(uint32_t actions, int8_t& stick_x, int8_t& stick_y, int modifier_percent) {
  const bool up = actions & (1u << SUp), down = actions & (1u << SDown), left = actions & (1u << SLeft), right = actions & (1u << SRight);
  if (!up && !down && !left && !right) return false;
  int push = 127;
  if (actions & (1u << StickMod)) {
    const int pct = modifier_percent < 0 ? 0 : modifier_percent > 100 ? 100 : modifier_percent;
    push = (127 * pct + 50) / 100;
  }
  if (up != down) stick_y = int8_t(up ? push : -push);
  if (left != right) stick_x = int8_t(right ? push : -push);
  return true;
}
// Upstream apply_box_actions. Here: the light and mid shield buttons press R that deep on any device (upstream only
// in box mode), so a Pro Controller's digital ZL can be a light shield without making the stick a box.
inline bool apply_box_actions(bool box_mode, uint32_t actions, Pad& pad, box::State& state) {
  if (!box_mode) return false;
  auto on = [&](int a) { return ((actions >> a) & 1u) != 0; };
  box::Buttons in;
  in.left = on(SLeft); in.right = on(SRight); in.down = on(SDown); in.up = on(SUp);
  in.c_left = on(CLeft); in.c_right = on(CRight); in.c_down = on(CDown); in.c_up = on(CUp);
  in.mod_x = on(ModX); in.mod_y = on(ModY);
  in.shield = on(L) || on(R) || on(LAnalog) || on(RAnalog) || on(LightShield) || on(MidShield);
  in.b = on(B);
  const box::Result r = box::melee(in, state);
  if (r.stick) { pad.stick_x = r.stick_x; pad.stick_y = r.stick_y; }
  if (r.c) { pad.sub_x = r.c_x; pad.sub_y = r.c_y; }
  if (r.dpad_up) pad.button |= kPadBit[DUp];
  if (r.dpad_down) pad.button |= kPadBit[DDown];
  if (r.dpad_left) pad.button |= kPadBit[DLeft];
  if (r.dpad_right) pad.button |= kPadBit[DRight];
  return true;
}
inline uint8_t analog_binding_travel(uint32_t mask, int level, uint32_t buttons, uint32_t analog_l, uint32_t analog_r, uint8_t left, uint8_t right) {
  int travel = 0;
  if (mask & analog_l) travel = left;
  if ((mask & analog_r) && right > travel) travel = right;
  if (mask & buttons & ~(analog_l | analog_r)) {
    const int depth = level ? level : kDefaultAnalogDepth;
    if (depth > travel) travel = depth;
  }
  return uint8_t(travel);
}
// Is action i's binding down? An analog source (an SDL trigger, an adapter trigger with a level) counts past its
// level, or past `trigger_point` without one; any other bit is a button.
inline bool binding_pressed(const Config& c, int i, const Raw& raw, int trigger_point) {
  const uint32_t m = c.mask[i];
  if (!m) return false;
  if (c.kind == Kind::Keyboard) return key_held(raw.keys, m);
  if (c.kind == Kind::Sdl) {
    const int t = c.level[i] ? c.level[i] : trigger_point;
    if ((m & kSrcLT) && raw.lt > t) return true;
    if ((m & kSrcRT) && raw.rt > t) return true;
    return (raw.buttons & m & ~(kSrcLT | kSrcRT)) != 0;
  }
  // Adapter (upstream gc_binding_pressed): with no level a trigger bit is its click; with one, its travel.
  if (!c.level[i]) return (raw.buttons & m) != 0;
  if ((m & kPadL) && raw.lt > c.level[i]) return true;
  if ((m & kPadR) && raw.rt > c.level[i]) return true;
  return (raw.buttons & m & ~uint32_t(kPadL | kPadR)) != 0;
}

// Per device and per reader (the game's poll, the input-suggestion reads): the box's opposing-button rule and the
// snapback filter keep state from poll to poll.
struct State { box::State box; SnapbackAxis snap[2]; };

// One poll. `pad` holds what the app's input path made of the device (for Sdl, after the app's own button remap and
// stick swap); `raw` the device's own sources; `trigger_point` (0..255) where an SDL trigger counts as pressed.
// Returns the actions held, as bits of Action.
inline uint32_t apply(const Config& c, const Raw& raw, int trigger_point, double now, State& st, Pad& pad) {
  uint32_t actions = 0;
  const bool sdl = c.kind == Kind::Sdl;
  if (c.kind == Kind::Keyboard) {   // upstream keyboard_apply_bindings: every action from the table, always
    pad = Pad{};
    for (int i = 0; i < kActions; ++i)
      if (binding_pressed(c, i, raw, 0)) { actions |= 1u << i; if (i <= DRight) pad.button |= kPadBit[i]; }
    // A key on L / R travel presses it to its level (upstream's level, a light shield without one); L or R itself bottoms out.
    if (actions & (1u << LAnalog)) pad.trig_l = uint8_t(c.level[LAnalog] ? c.level[LAnalog] : kDefaultAnalogDepth);
    if (actions & (1u << RAnalog)) pad.trig_r = uint8_t(c.level[RAnalog] ? c.level[RAnalog] : kDefaultAnalogDepth);
    if (pad.button & kPadL) pad.trig_l = 255;
    if (pad.button & kPadR) pad.trig_r = 255;
    apply_cstick_actions(actions, pad.sub_x, pad.sub_y);
    if (!apply_box_actions(c.box_mode(), actions, pad, st.box)) apply_stick_actions(actions, pad.stick_x, pad.stick_y, c.modifier_percent);
    const uint8_t shield = (actions & (1u << MidShield)) ? box::kMidShield : (actions & (1u << LightShield)) ? box::kLightShield : 0;
    if (shield > pad.trig_r) pad.trig_r = shield;
  } else if (!c.bindings_default()) {
    if (!sdl) {   // the adapter: every action from its own table (upstream gc_apply_bindings)
      pad.button = 0;
      for (int i = 0; i < kActions; ++i)
        if (!is_analog_action(i) && binding_pressed(c, i, raw, kDefaultTriggerClick)) { actions |= 1u << i; pad.button |= kPadBit[i]; }
      pad.trig_l = analog_binding_travel(c.mask[LAnalog], c.level[LAnalog], raw.buttons, kPadL, kPadR, raw.lt, raw.rt);
      pad.trig_r = analog_binding_travel(c.mask[RAnalog], c.level[RAnalog], raw.buttons, kPadL, kPadR, raw.lt, raw.rt);
      if ((pad.button & kPadL) && !pad.trig_l) pad.trig_l = 255;
      if ((pad.button & kPadR) && !pad.trig_r) pad.trig_r = 255;
    } else {      // an SDL pad: the twelve buttons are the app's; the rest from this table
      for (int i = CUp; i < kActions; ++i)
        if (!is_analog_action(i) && binding_pressed(c, i, raw, trigger_point)) actions |= 1u << i;
      for (int i = 0; i <= DRight; ++i) if (pad.button & kPadBit[i]) actions |= 1u << i;
      const Config d = Config::defaults(Kind::Sdl);
      if (c.mask[LAnalog] != d.mask[LAnalog] || c.level[LAnalog]) pad.trig_l = analog_binding_travel(c.mask[LAnalog], c.level[LAnalog], raw.buttons, kSrcLT, kSrcRT, raw.lt, raw.rt);
      if (c.mask[RAnalog] != d.mask[RAnalog] || c.level[RAnalog]) pad.trig_r = analog_binding_travel(c.mask[RAnalog], c.level[RAnalog], raw.buttons, kSrcLT, kSrcRT, raw.lt, raw.rt);
      if ((pad.button & kPadL) && !pad.trig_l) pad.trig_l = 255;   // as the app's remap: L / R on a button bottom the trigger out
      if ((pad.button & kPadR) && !pad.trig_r) pad.trig_r = 255;
    }
    if (pad.trig_l) actions |= 1u << LAnalog;
    if (pad.trig_r) actions |= 1u << RAnalog;
    apply_cstick_actions(actions, pad.sub_x, pad.sub_y);
    if (!apply_box_actions(c.box_mode(), actions, pad, st.box)) apply_stick_actions(actions, pad.stick_x, pad.stick_y, c.modifier_percent);
    const uint8_t shield = (actions & (1u << MidShield)) ? box::kMidShield : (actions & (1u << LightShield)) ? box::kLightShield : 0;
    if (shield > pad.trig_r) pad.trig_r = shield;
  }
  if (c.invert & kInvertStickX) pad.stick_x = int8_t(pad.stick_x == -128 ? 127 : -pad.stick_x);
  if (c.invert & kInvertStickY) pad.stick_y = int8_t(pad.stick_y == -128 ? 127 : -pad.stick_y);
  if (c.invert & kInvertCX) pad.sub_x = int8_t(pad.sub_x == -128 ? 127 : -pad.sub_x);
  if (c.invert & kInvertCY) pad.sub_y = int8_t(pad.sub_y == -128 ? 127 : -pad.sub_y);
  // Upstream apply_family_options: snapback (not on a GameCube controller), deadzone, radius, click, cap.
  if (sdl) {
    pad.stick_x = snapback_axis(st.snap[0], pad.stick_x, now, c.snap_x, c.snap_ms_x);
    pad.stick_y = snapback_axis(st.snap[1], pad.stick_y, now, c.snap_y, c.snap_ms_y);
  }
  apply_deadzone(c.dz_main, pad.stick_x, pad.stick_y);
  apply_deadzone(c.dz_c, pad.sub_x, pad.sub_y);
  apply_stick_radius(c.radius_main, pad.stick_x, pad.stick_y);
  apply_stick_radius(c.radius_c, pad.sub_x, pad.sub_y);
  if (!sdl && c.click_l != kDefaultTriggerClick) { pad.button &= uint16_t(~kPadL); apply_trigger_click(c.click_l, pad.trig_l, pad.button, kPadL); }
  if (!sdl && c.click_r != kDefaultTriggerClick) { pad.button &= uint16_t(~kPadR); apply_trigger_click(c.click_r, pad.trig_r, pad.button, kPadR); }
  apply_trigger_cap(c.cap_l, pad.trig_l, pad.button, kPadL);
  apply_trigger_cap(c.cap_r, pad.trig_r, pad.button, kPadR);
  return actions;
}

}  // namespace fe::controls
