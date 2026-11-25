// PhobGCC settings on a stock GameCube controller: see phob_adapter.h. The order of work is melee-unlocked's gc_adapter.cpp
// (phob_process: every report through the firmware's loop on the reader thread, steps = milliseconds since the last report,
// the poll hands the game the newest result) and its phob_store.cpp (PhobGCC/port<N>.ini). Adapted from melee-unlocked v1.0.5
// (GPL-2.0-or-later and GPL-3.0-only parts; phob.cpp itself is compiled from the fetched tree under its GPL-3.0-only notice).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phob_adapter.h"
#include "phob_adapter_input.h"   // melee-unlocked port/runtime/host (the fetched tree): a report's port block as the firmware's input

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>

namespace fe::phob_adapter {
namespace {
namespace ph = host::phob;
struct Origin { bool set = false; uint8_t sx = 128, sy = 128, cx = 128, cy = 128, tl = 0, tr = 0; };
struct Port {
  ph::Prepared prep;
  ph::PortState st;
  Origin origin;
  ph::Output out;
  bool live = false;
};
std::mutex g_mutex;
Port g_port[4];
std::string g_folder;
uint64_t g_last_us = 0;

// The calibration session (one port, one stick at a time).
struct Cal {
  bool active = false;
  int port = -1;
  ph::CalSession session;
  ph::StickCal saved;
  float ring_x[50] = {}, ring_y[50] = {};
  int ring_n = 0, ring_at = 0;
  uint16_t last_buttons = 0xFFFF;   // the first report only sets the edge base
  std::string line;
} g_cal;

constexpr uint16_t kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kZ = 0x0010, kR = 0x0020, kL = 0x0040, kStart = 0x1000;
const char* const kNotchName[16] = {"right", "right, a little up", "up-right", "up, a little right", "up", "up, a little left", "up-left",
                                    "left, a little up", "left", "left, a little down", "down-left", "down, a little left", "down",
                                    "down, a little right", "down-right", "right, a little down"};

std::string port_file(int port) { return g_folder + "/PhobGCC/port" + std::to_string(port + 1) + ".ini"; }

void cal_describe() {   // with g_mutex held
  const int step = g_cal.session.step;
  char t[256];
  const char* stick = g_cal.session.which == ph::ASTICK ? "control stick" : "C-stick";
  if (step < 0) std::snprintf(t, sizeof t, "PhobGCC: port %d %s calibrated and saved", g_cal.port + 1, stick);
  else if (step < ph::kCalPoints) {
    const ph::CalPrompt p = ph::cal_prompt(step);
    if (p.centre) std::snprintf(t, sizeof t, "PhobGCC port %d, %s, step %d of %d: let the stick rest, then A (Z back, Start stops)", g_cal.port + 1, stick, step + 1, ph::kCalSteps);
    else std::snprintf(t, sizeof t, "PhobGCC port %d, %s, step %d of %d: hold it %s%s, then A (Z back)", g_cal.port + 1, stick, step + 1, ph::kCalSteps,
                       kNotchName[p.notch & 15], p.optional ? " (no notch there on a stock gate: where it rests)" : " in the notch");
  } else {
    const int n = ph::cal_adjust_notch(step);
    std::snprintf(t, sizeof t, "PhobGCC port %d, %s, step %d of %d: notch %s: hold X / Y to turn it, B puts it back, A next (Z back)", g_cal.port + 1,
                  stick, step + 1, ph::kCalSteps, kNotchName[n & 15]);
  }
  g_cal.line = t;
}

void cal_report(const ph::HardwareInput& in, int steps) {   // with g_mutex held, the calibrating port's input
  const float rx = (g_cal.session.which == ph::ASTICK ? in.ax : in.cx) / 255.0f, ry = (g_cal.session.which == ph::ASTICK ? in.ay : in.cy) / 255.0f;
  g_cal.ring_x[g_cal.ring_at] = rx; g_cal.ring_y[g_cal.ring_at] = ry;
  g_cal.ring_at = (g_cal.ring_at + 1) % 50; g_cal.ring_n = std::min(g_cal.ring_n + 1, 50);
  if (g_cal.last_buttons == 0xFFFF) { g_cal.last_buttons = in.buttons; return; }
  const uint16_t pressed = in.buttons & uint16_t(~g_cal.last_buttons);
  g_cal.last_buttons = in.buttons;
  if (pressed & kStart) { g_cal.active = false; g_cal.line = "PhobGCC: calibration stopped, nothing kept"; return; }
  if (g_cal.session.step >= ph::kCalPoints)
    ph::cal_adjust(g_cal.session, (in.buttons & kX) != 0, (in.buttons & kY) != 0, (pressed & kB) != 0, float(steps));
  if (pressed & kZ) { ph::cal_undo(g_cal.session); cal_describe(); return; }
  if (pressed & (kA | kL | kR)) {
    float ax = 0, ay = 0;
    for (int i = 0; i < g_cal.ring_n; ++i) { ax += g_cal.ring_x[i]; ay += g_cal.ring_y[i]; }
    if (g_cal.ring_n) { ax /= float(g_cal.ring_n); ay /= float(g_cal.ring_n); }
    if (ph::cal_advance(g_cal.session, ax, ay, g_cal.saved)) {
      Port& p = g_port[g_cal.port];
      ph::Settings s = p.prep.s;
      (g_cal.session.which == ph::ASTICK ? s.a_cal : s.c_cal) = g_cal.saved;
      ph::prepare(s, p.prep);
      p.st = ph::PortState{};
      g_cal.active = false;
      cal_describe();
      const std::string text = ph::serialize(s);
      const std::string path = port_file(g_cal.port);
      mkdir((g_folder + "/PhobGCC").c_str(), 0777);
      if (FILE* f = std::fopen(path.c_str(), "w")) { std::fputs(text.c_str(), f); std::fclose(f); }
      return;
    }
    cal_describe();
  }
}
}  // namespace

void load(const std::string& app_folder) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_folder = app_folder;
  for (int port = 0; port < 4; ++port) {
    ph::Settings s;
    std::ifstream in(port_file(port));
    if (in) { std::ostringstream text; text << in.rdbuf(); ph::parse(text.str(), s); ph::clamp(s); }
    ph::prepare(s, g_port[port].prep);
    g_port[port].st = ph::PortState{};
  }
}
ph::Settings settings(int port) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return port >= 0 && port < 4 ? g_port[port].prep.s : ph::Settings{};
}
void set_settings(int port, const ph::Settings& in) {
  if (port < 0 || port > 3) return;
  ph::Settings s = in;
  ph::clamp(s);
  std::lock_guard<std::mutex> lock(g_mutex);
  ph::prepare(s, g_port[port].prep);
  g_port[port].st.primed = false;
}
bool save(int port) {
  if (port < 0 || port > 3) return false;
  std::string text, path;
  { std::lock_guard<std::mutex> lock(g_mutex); text = ph::serialize(g_port[port].prep.s); path = port_file(port); if (g_folder.empty()) return false; }
  mkdir((g_folder + "/PhobGCC").c_str(), 0777);
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fputs(text.c_str(), f);
  return std::fclose(f) == 0;
}

void report(const uint8_t r[37], uint64_t time_us) {
  std::lock_guard<std::mutex> lock(g_mutex);
  int steps = 1;
  if (g_last_us) steps = std::clamp(int((time_us - g_last_us + 500) / 1000), 1, 50);
  g_last_us = time_us;
  uint8_t copy[37];
  std::copy(r, r + 37, copy);
  for (int port = 0; port < 4; ++port) {
    Port& p = g_port[port];
    ph::HardwareInput in;
    if (!ph::mirror_input(copy, port, p.origin, in)) { p.live = false; p.st.primed = false; continue; }
    if (g_cal.active && g_cal.port == port) cal_report(in, steps);
    if (p.prep.s.enabled && !p.prep.s.phob_hardware) { p.out = ph::step(p.prep, p.st, in, steps); p.live = true; }
    else { p.live = false; p.st.primed = false; }
  }
}

bool pad(int port, Pad* out) {
  if (port < 0 || port > 3) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_cal.active && g_cal.port == port) { *out = Pad{0, 0, 0, 0, 0, 0, 0}; return true; }   // the game sees a resting controller
  const Port& p = g_port[port];
  if (!p.live) return false;
  *out = Pad{p.out.buttons, p.out.stick_x, p.out.stick_y, p.out.sub_x, p.out.sub_y, p.out.trig_l, p.out.trig_r};
  return true;
}

bool cal_start(int port, ph::Stick which, bool notch_fix) {
  if (port < 0 || port > 3) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const ph::Settings& s = g_port[port].prep.s;
  const ph::StickCal& saved = which == ph::ASTICK ? s.a_cal : s.c_cal;
  const int scaler = which == ph::ASTICK ? s.a_scaler : s.c_scaler;
  g_cal = Cal{};
  if (notch_fix) { if (!ph::cal_begin_notch_fix(g_cal.session, which, saved, scaler)) return false; }
  else ph::cal_begin(g_cal.session, which, saved, scaler);
  g_cal.saved = saved;
  g_cal.port = port;
  g_cal.active = true;
  cal_describe();
  return true;
}
bool cal_active(int* port) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (port) *port = g_cal.port;
  return g_cal.active;
}
std::string cal_line() { std::lock_guard<std::mutex> lock(g_mutex); return g_cal.line; }
}  // namespace fe::phob_adapter

#ifndef MNX_PHOB_CHECK
// frontend/patches/0123: the app's adapter reader and poll (gc_adapter_usbhs.cpp, the console only).
extern "C" void mu_frontend_adapter_report(const uint8_t* report37, uint64_t time_us) { fe::phob_adapter::report(report37, time_us); }
extern "C" int mu_frontend_adapter_pad(int port, uint16_t* buttons, int8_t sticks[4], uint8_t triggers[2]) {
  fe::phob_adapter::Pad p;
  if (!fe::phob_adapter::pad(port, &p)) return 0;
  *buttons = p.buttons; sticks[0] = p.stick_x; sticks[1] = p.stick_y; sticks[2] = p.sub_x; sticks[3] = p.sub_y;
  triggers[0] = p.trig_l; triggers[1] = p.trig_r;
  return 1;
}
#endif
