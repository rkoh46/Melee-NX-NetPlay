// PhobGCC settings on a stock GameCube controller: see phob_adapter.h. The order of work is melee-unlocked's gc_adapter.cpp
// (phob_process: every report through the firmware's loop on the reader thread, steps = milliseconds since the last report,
// the poll hands the game the newest result) and its phob_store.cpp (PhobGCC/port<N>.ini). Adapted from melee-unlocked v1.0.5
// (GPL-2.0-or-later and GPL-3.0-only parts; phob.cpp itself is compiled from the fetched tree under its GPL-3.0-only notice).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phob_adapter.h"
#ifdef MNX_PHOB_CHECK
#include <cstdarg>
#include <cstdio>
namespace fe { inline void say(const char* f, ...) { va_list a; va_start(a, f); std::vprintf(f, a); va_end(a); std::printf("\n"); } }
#else
#include "frontend.h"   // say
#endif
#include "phob_adapter_input.h"   // melee-unlocked port/runtime/host (the fetched tree): a report's port block as the firmware's input

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
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
  ph::HardwareInput raw;          // the latest report's reading (PhobScope, calibration)
  bool have_raw = false;
  host::GCScopeSample scope[4096] = {};
  uint32_t scope_head = 0;
};
constexpr int kScopeLen = 4096;
std::mutex g_mutex;
Port g_port[4];
std::string g_folder;
uint64_t g_last_us = 0;
uint64_t g_rate_start_us = 0;
uint32_t g_rate_count = 0;
double g_rate_hz = 0;

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
  // Reports per second over the last second (melee-unlocked measures incoming reports the same way).
  if (!g_rate_start_us) g_rate_start_us = time_us;
  ++g_rate_count;
  if (time_us - g_rate_start_us >= 1000000) { g_rate_hz = double(g_rate_count) * 1e6 / double(time_us - g_rate_start_us); g_rate_count = 0; g_rate_start_us = time_us; }
  uint8_t copy[37];
  std::copy(r, r + 37, copy);
  const uint32_t t = uint32_t(time_us);
  for (int port = 0; port < 4; ++port) {
    Port& p = g_port[port];
    ph::HardwareInput in;
    if (!ph::mirror_input(copy, port, p.origin, in)) { p.live = false; p.have_raw = false; p.st.primed = false; continue; }
    p.raw = in; p.have_raw = true;
    if (g_cal.active && g_cal.port == port) cal_report(in, steps);
    // PhobScope's sample (melee-unlocked gc_adapter.cpp phob_process): as read, and as the settings shaped it.
    host::GCScopeSample& s = p.scope[p.scope_head % kScopeLen];
    s.time_us = t;
    s.raw_ax = int8_t(std::clamp(int(in.ax) - in.ax0, -128, 127)); s.raw_ay = int8_t(std::clamp(int(in.ay) - in.ay0, -128, 127));
    s.raw_cx = int8_t(std::clamp(int(in.cx) - in.cx0, -128, 127)); s.raw_cy = int8_t(std::clamp(int(in.cy) - in.cy0, -128, 127));
    s.raw_l = in.la; s.raw_r = in.ra;
    s.raw_buttons = in.buttons; s.hw_ax = in.ax; s.hw_ay = in.ay; s.hw_cx = in.cx; s.hw_cy = in.cy;
    if (p.prep.s.enabled && !p.prep.s.phob_hardware) {
      p.out = ph::step(p.prep, p.st, in, steps); p.live = true;
      // The firmware's "unfiltered" stick: through the calibration, before the filters.
      auto unf = [](float v) { const long q = std::lround(v); return int8_t(q > 127 ? 127 : q < -128 ? -128 : q); };
      s.raw_ax = unf(p.st.ax_unfilt); s.raw_ay = unf(p.st.ay_unfilt); s.raw_cx = unf(p.st.cx_unfilt); s.raw_cy = unf(p.st.cy_unfilt);
      s.ax = p.out.stick_x; s.ay = p.out.stick_y; s.cx = p.out.sub_x; s.cy = p.out.sub_y;
      s.l = p.out.trig_l; s.r = p.out.trig_r; s.buttons = p.out.buttons;
    } else {
      p.live = false; p.st.primed = false;
      s.ax = s.raw_ax; s.ay = s.raw_ay; s.cx = s.raw_cx; s.cy = s.raw_cy; s.l = s.raw_l; s.r = s.raw_r; s.buttons = s.raw_buttons;
    }
    ++p.scope_head;
  }
}

int scope(int port, host::GCScopeSample* out, int max) {
  if (port < 0 || port > 3 || max <= 0) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Port& p = g_port[port];
  const int n = int(std::min<uint32_t>(uint32_t(std::min(max, kScopeLen)), p.scope_head));
  for (int i = 0; i < n; ++i) out[i] = p.scope[(p.scope_head - uint32_t(n) + uint32_t(i)) % kScopeLen];
  return n;
}
bool raw(int port, ph::HardwareInput& in) {
  if (port < 0 || port > 3) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_port[port].have_raw) return false;
  in = g_port[port].raw;
  return true;
}
double rate_hz() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_last_us) return 0;
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  return now - g_last_us > 1000000 ? 0 : g_rate_hz;
}

// ---- the scripted adapter (tests off the console; melee-unlocked gc_adapter.cpp test_script_thread, GPL-2.0-or-later) ----
namespace {
struct ScriptStep { int ms; uint8_t ax, ay, cx, cy, la, ra; uint16_t buttons; };
std::thread* g_test_thread = nullptr;   // never destroyed: a process that ends without test_script_stop must not abort
std::atomic<bool> g_test_run{false};
void test_script_thread(std::vector<ScriptStep> steps) {
  uint8_t rep[37] = {0x21};
  auto fill = [&](const ScriptStep& st) {
    uint8_t* c = rep + 1;
    c[0] = 0x10;
    const uint16_t b = st.buttons;   // GameCube bits as the game reads them -> the adapter's two button bytes
    c[1] = uint8_t((b & 0x0100 ? 0x01 : 0) | (b & 0x0200 ? 0x02 : 0) | (b & 0x0400 ? 0x04 : 0) | (b & 0x0800 ? 0x08 : 0) |
                   (b & 0x0001 ? 0x10 : 0) | (b & 0x0002 ? 0x20 : 0) | (b & 0x0004 ? 0x40 : 0) | (b & 0x0008 ? 0x80 : 0));
    c[2] = uint8_t((b & 0x1000 ? 0x01 : 0) | (b & 0x0010 ? 0x02 : 0) | (b & 0x0020 ? 0x04 : 0) | (b & 0x0040 ? 0x08 : 0));
    c[3] = st.ax; c[4] = st.ay; c[5] = st.cx; c[6] = st.cy; c[7] = st.la; c[8] = st.ra;
  };
  // The adapter's neutral point is taken from its first report (mirror_input); the first report is the centre.
  { uint8_t c0[37] = {0x21}; c0[1] = 0x10; c0[4] = c0[5] = c0[6] = c0[7] = 128; c0[8] = c0[9] = 0;
    report(c0, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())); }
  auto next = std::chrono::steady_clock::now();
  auto last = next;
  size_t i = 0;
  int left = steps.empty() ? 0 : steps[0].ms;
  if (!steps.empty()) fill(steps[0]);
  while (g_test_run.load()) {
    next += std::chrono::milliseconds(1);
    std::this_thread::sleep_until(next);
    const auto now = std::chrono::steady_clock::now();
    report(rep, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()));
    int elapsed = int(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
    if (elapsed > 0) last += std::chrono::milliseconds(elapsed);
    while (!steps.empty() && elapsed > 0) {
      const int used = std::min(elapsed, left);
      left -= used; elapsed -= used;
      if (left <= 0) { if (i + 1 >= steps.size()) break; left = steps[++i].ms; fill(steps[i]); }
    }
    if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(50)) next = std::chrono::steady_clock::now();
  }
}
}  // namespace

void test_script_start() {
#ifndef __SWITCH__
  static std::mutex start_mutex;
  std::lock_guard<std::mutex> lock(start_mutex);
  const char* path = std::getenv("MELEE_TEST_ADAPTER_SCRIPT");
  if (!path || !*path || g_test_thread) return;
  std::ifstream in(path);
  std::vector<ScriptStep> steps;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    int ms, v[6];
    if (!(ls >> ms >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5])) continue;
    ScriptStep st{ms, uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3]), uint8_t(v[4]), uint8_t(v[5]), 0};
    std::string b;
    static const std::pair<const char*, uint16_t> names[] = {{"A", 0x0100}, {"B", 0x0200}, {"X", 0x0400}, {"Y", 0x0800}, {"Z", 0x0010}, {"L", 0x0040},
                                                             {"R", 0x0020}, {"S", 0x1000}, {"DU", 0x0008}, {"DD", 0x0004}, {"DL", 0x0001}, {"DR", 0x0002}};
    while (ls >> b) for (const auto& [n, bit] : names) if (b == n) st.buttons |= bit;
    steps.push_back(st);
  }
  fe::say("gc adapter: test script %s, %zu steps (adapter socket 1, 1000 reports/s)", path, steps.size());
  g_test_run.store(true);
  g_test_thread = new std::thread(test_script_thread, std::move(steps));
#endif
}
void test_script_stop() {
  if (g_test_thread && g_test_thread->joinable()) { g_test_run.store(false); g_test_thread->join(); delete g_test_thread; g_test_thread = nullptr; }
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
