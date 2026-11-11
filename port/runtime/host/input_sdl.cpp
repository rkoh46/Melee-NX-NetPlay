// Physical input for the Metal executable: keyboard through SDL3 and gamepads
// through Aurora's PAD mapping (SDL3 gamepads with GameCube-style bindings).
// A loaded input script replaces every physical device, as on the other hosts.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "game_menu.h"
#include "input_script.h"
#include "window.h"

#include <SDL3/SDL.h>
#include <dolphin/pad.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace host {
namespace {
enum : uint16_t {
  GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008, GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
  GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000,
};
InputScript g_script;
bool g_scripted = false;
std::atomic<uint32_t> g_match_start{0};
bool g_pad_initialized = false;

void keyboard(PadState& p) {
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  if (!keys) return;
  auto key = [&](SDL_Scancode code) { return code < count && keys[code]; };
  int sx = 0, sy = 0, cx = 0, cy = 0;
  // Same layout as the Windows build: arrows = stick, IJKL = C-stick, Z=A X=B C=X V=Y,
  // Enter = Start, Q=L W=R E=Z, TFGH = D-pad.
  if (key(SDL_SCANCODE_LEFT)) sx -= 127; if (key(SDL_SCANCODE_RIGHT)) sx += 127;
  if (key(SDL_SCANCODE_UP)) sy += 127; if (key(SDL_SCANCODE_DOWN)) sy -= 127;
  if (key(SDL_SCANCODE_J)) cx -= 127; if (key(SDL_SCANCODE_L)) cx += 127;
  if (key(SDL_SCANCODE_I)) cy += 127; if (key(SDL_SCANCODE_K)) cy -= 127;
  if (key(SDL_SCANCODE_Z)) p.button |= GC_A; if (key(SDL_SCANCODE_X)) p.button |= GC_B;
  if (key(SDL_SCANCODE_C)) p.button |= GC_X; if (key(SDL_SCANCODE_V)) p.button |= GC_Y;
  if (key(SDL_SCANCODE_RETURN) || key(SDL_SCANCODE_KP_ENTER)) p.button |= GC_START;
  if (key(SDL_SCANCODE_Q)) { p.button |= GC_L; p.trig_l = 255; }
  if (key(SDL_SCANCODE_W)) { p.button |= GC_R; p.trig_r = 255; }
  if (key(SDL_SCANCODE_E)) p.button |= GC_Z;
  if (key(SDL_SCANCODE_T)) p.button |= GC_UP; if (key(SDL_SCANCODE_G)) p.button |= GC_DOWN;
  if (key(SDL_SCANCODE_F)) p.button |= GC_LEFT; if (key(SDL_SCANCODE_H)) p.button |= GC_RIGHT;
  if (sx || sy) { p.stick_x = int8_t(sx); p.stick_y = int8_t(sy); }
  if (cx || cy) { p.sub_x = int8_t(cx); p.sub_y = int8_t(cy); }
}

#if defined(__SWITCH__)
extern "C" int melee_nx_gc_trigger_travel[8];   // SDL's Switch joystick backend
extern "C" int melee_nx_gc_stick_min[4];
extern "C" int melee_nx_switch_gc_sticks(SDL_JoystickID id, signed char out[4]);
#endif
// Aurora maps every connected SDL gamepad to a GameCube layout per port.
uint32_t gamepads(PadState out[4]) {
  if (!g_pad_initialized) { PADInit(); g_pad_initialized = true; }
#if defined(__SWITCH__)
  // Read the controllers now, on the game thread. SDL otherwise refreshes them only when the
  // render thread pumps events between its frames, so PADRead saw a state up to a frame old (and
  // a frozen one while the renderer stalled). SDL_UpdateJoysticks takes SDL's joystick lock.
  SDL_UpdateJoysticks();
#endif
  PADStatus status[4];
  std::memset(status, 0, sizeof status);
  const uint32_t mask = PADRead(status);
  uint32_t connected = 0;
  for (int port = 0; port < 4; ++port) {
    if (status[port].err != 0) continue;   // PAD_ERR_NO_CONTROLLER or transfer error
    if (!(mask & (0x80000000u >> port))) continue;
    connected |= 1u << port;
    PadState& p = out[port];
    p.err = 0;
    p.button |= status[port].button & 0x1F7F;
    if (status[port].stickX || status[port].stickY) { p.stick_x = status[port].stickX; p.stick_y = status[port].stickY; }
    if (status[port].substickX || status[port].substickY) { p.sub_x = status[port].substickX; p.sub_y = status[port].substickY; }
    if (status[port].triggerLeft > p.trig_l) p.trig_l = status[port].triggerLeft;
    if (status[port].triggerRight > p.trig_r) p.trig_r = status[port].triggerRight;
    p.analog_a = status[port].analogA;
    p.analog_b = status[port].analogB;
#if defined(__SWITCH__)
    // A GameCube controller's sticks in GameCube units, without the generic gamepad dead zone
    // (sdl-dusklight-gc-sticks.patch): Melee applies its own, as on the console.
    {
      signed char gc[4];
      const s32 index = PADGetIndexForPort(uint32_t(port));
      SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(uint32_t(index)) : nullptr;
      if (gamepad && melee_nx_switch_gc_sticks(SDL_GetGamepadID(gamepad), gc)) {
        p.stick_x = gc[0]; p.stick_y = gc[1]; p.sub_x = gc[2]; p.sub_y = gc[3];
        static bool logged[4];
        if (!logged[port]) {
          logged[port] = true;
          log("pad %d: GameCube controller, sticks read directly in GameCube units", port + 1);
        }
      }
      static int logged_min[4];
      for (int slot = 0; slot < 4; ++slot)
        if (melee_nx_gc_stick_min[slot] && melee_nx_gc_stick_min[slot] != logged_min[slot] && (logged_min[slot] == 0 || melee_nx_gc_stick_min[slot] < logged_min[slot] / 2)) {
          logged_min[slot] = melee_nx_gc_stick_min[slot];
          log("pad: controller slot %d smallest GameCube stick movement the system reports: %d of 32767", slot + 1, logged_min[slot]);
        }
    }
    // Bring-up diagnostic: what the console's controller mapping delivers for the shoulders
    // (digital L/R/Z bits and the analog pulls), at most twice a second and only on change.
    static uint32_t last_key[4]; static double next_log = 0.0;
    const uint32_t key = (status[port].button & (GC_L | GC_R | GC_Z)) | (uint32_t(status[port].triggerLeft >> 4) << 8) | (uint32_t(status[port].triggerRight >> 4) << 12);
    const double t = now_seconds();
    if (key != last_key[port] && t >= next_log) {
      last_key[port] = key; next_log = t + 0.5;
      log("pad %d: L=%u R=%u Z=%u analog L=%u R=%u", port + 1, (status[port].button & GC_L) ? 1 : 0, (status[port].button & GC_R) ? 1 : 0,
          (status[port].button & GC_Z) ? 1 : 0, status[port].triggerLeft, status[port].triggerRight);
    }
    // The furthest analog trigger travel the system has reported without the click (SDL backend,
    // sdl-dusklight-gc-trigger-range.patch), logged when it grows by a step: the scale is set from it.
    static int logged_travel[8];
    for (int slot = 0; slot < 8; ++slot)
      if (melee_nx_gc_trigger_travel[slot] > logged_travel[slot] + 0x200) {
        logged_travel[slot] = melee_nx_gc_trigger_travel[slot];
        log("pad: controller slot %d trigger travel reaches 0x%X of 0x7FFF without the click", slot + 1, logged_travel[slot]);
      }
#endif
  }
  return connected;
}
}  // namespace

bool input_load_script(const char* path) {
  std::ifstream file(path);
  if (!file) return false;
  std::string error;
  if (!g_script.load(file, &error)) { std::fprintf(stderr, "input script: %s\n", error.c_str()); return false; }
  g_scripted = true;
  return true;
}
void input_mark_match_start() { g_match_start.store(retrace_count()); }

// Development aid: MELEE_PAD_FILE names a text file re-read every poll. Each line is
// "[p=N] BUTTON+BUTTON [sx=N] [sy=N] [cx=N] [cy=N]" (script token syntax); the state
// holds until the file changes. Lets a test harness drive menus without window focus.
bool pad_file(PadState out[4]) {
  static const char* path = std::getenv("MELEE_PAD_FILE");
  if (!path) return false;
  std::ifstream file(path);
  if (!file) return false;
  bool any = false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    int port = 0;
    PadState p{};
    size_t pos = 0;
    while (pos < line.size()) {
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '+')) ++pos;
      size_t end = pos;
      while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '+') ++end;
      if (end == pos) break;
      const std::string tok = line.substr(pos, end - pos);
      pos = end;
      if (tok == "A") p.button |= GC_A; else if (tok == "B") p.button |= GC_B;
      else if (tok == "X") p.button |= GC_X; else if (tok == "Y") p.button |= GC_Y;
      else if (tok == "Z") p.button |= GC_Z; else if (tok == "L") { p.button |= GC_L; p.trig_l = 255; }
      else if (tok == "R") { p.button |= GC_R; p.trig_r = 255; } else if (tok == "START") p.button |= GC_START;
      else if (tok == "DU") p.button |= GC_UP; else if (tok == "DD") p.button |= GC_DOWN;
      else if (tok == "DL") p.button |= GC_LEFT; else if (tok == "DR") p.button |= GC_RIGHT;
      else if (tok.rfind("sx=", 0) == 0) p.stick_x = int8_t(std::atoi(tok.c_str() + 3));
      else if (tok.rfind("sy=", 0) == 0) p.stick_y = int8_t(std::atoi(tok.c_str() + 3));
      else if (tok.rfind("cx=", 0) == 0) p.sub_x = int8_t(std::atoi(tok.c_str() + 3));
      else if (tok.rfind("cy=", 0) == 0) p.sub_y = int8_t(std::atoi(tok.c_str() + 3));
      else if (tok.rfind("p=", 0) == 0) { port = std::atoi(tok.c_str() + 2) - 1; if (port < 0 || port > 3) port = 0; }
    }
    p.err = 0;
    out[port] = p;
    any = true;
  }
  if (!any) out[0].err = 0;   // an empty file still keeps port 1 plugged in
  return true;
}

void input_poll(PadState out[4]) {
  for (int i = 0; i < 4; ++i) { out[i] = {}; out[i].err = -1; }
  // The in-game menu sees every input source and neutralises the pads while open.
  struct MenuPass { PadState* pads; ~MenuPass() { menu_frame(pads); } } menu_pass{out};
  if (pad_file(out)) return;
  if (g_scripted) {
    auto pads = g_script.sample(retrace_count(), g_match_start.load(),
                                rd8(0x80479D30), rd8(0x80479D33));
    for (unsigned port = 0; port < pads.size(); ++port) {
      out[port].err = pads[port].connected ? 0 : -1;
      out[port].button = pads[port].buttons;
      out[port].stick_x = pads[port].sx; out[port].stick_y = pads[port].sy;
      out[port].sub_x = pads[port].cx; out[port].sub_y = pads[port].cy;
    }
#if defined(__SWITCH__)
    // Real pads add to a scripted run (buttons OR, sticks when deflected): a person or the
    // console's remote control can step in when the script's menu path goes wrong.
    {
      PadState live[4];
      for (int i = 0; i < 4; ++i) { live[i] = {}; live[i].err = -1; }
      const uint32_t adapter = gcadapter_poll(live);   // the GameCube adapter too (and its report-rate log)
      PadState sdl[4];
      for (int i = 0; i < 4; ++i) { sdl[i] = {}; sdl[i].err = -1; }
      gamepads(sdl);
      for (int i = 0; i < 4; ++i) if (!(adapter & (1u << i))) live[i] = sdl[i];
      for (int port = 0; port < 4; ++port) {
        if (live[port].err != 0) continue;
        if (out[port].err != 0) out[port].err = 0;
        out[port].button |= live[port].button;
        if (live[port].stick_x || live[port].stick_y) { out[port].stick_x = live[port].stick_x; out[port].stick_y = live[port].stick_y; }
        if (live[port].sub_x || live[port].sub_y) { out[port].sub_x = live[port].sub_x; out[port].sub_y = live[port].sub_y; }
        if (live[port].trig_l > out[port].trig_l) out[port].trig_l = live[port].trig_l;
        if (live[port].trig_r > out[port].trig_r) out[port].trig_r = live[port].trig_r;
      }
    }
#endif
    return;
  }
  // GameCube adapter ports take precedence. Port 1 is otherwise always present so the
  // keyboard can drive menus; SDL gamepads add to any port the adapter does not own.
  const uint32_t adapter_mask = gcadapter_poll(out);
  PadState pads[4];
  for (int i = 0; i < 4; ++i) { pads[i] = {}; pads[i].err = -1; }
  pads[0].err = 0;
  gamepads(pads);
  keyboard(pads[0]);
  for (int port = 0; port < 4; ++port)
    if (!(adapter_mask & (1u << port))) out[port] = pads[port];
}

void gamepad_rumble(int port, bool on) {
  if (!g_pad_initialized) return;
  PADControlMotor(uint32_t(port), on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
}

// The controller that feeds `game_port`, and the whole local side: implemented in host.cpp.

}  // namespace host
