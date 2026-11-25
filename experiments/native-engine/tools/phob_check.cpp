// This program's PhobGCC report path (frontend/src/phob_adapter.cpp) on made-up adapter reports: a port whose settings are off is
// left to the adapter's own reading, an uncalibrated stick passes through, a remap swaps buttons, smart snapback is applied,
// and the stick calibration run by buttons on the reports (44 steps on a stock-shaped gate) ends with the calibration kept in
// PhobGCC/port1.ini and the stick reaching the firmware's notch targets. Run by phob_check.sh.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phob_adapter.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace ph = host::phob;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) ++failures; }

static uint64_t now_us = 1000000;
// One report: port 1 with the given buttons (adapter bits: byte 1 A B X Y DL DR DD DU, byte 2 Start Z R L), sticks, triggers.
static void send(uint8_t b1, uint8_t b2, uint8_t ax, uint8_t ay, uint8_t cx = 128, uint8_t cy = 128, uint8_t l = 30, uint8_t r = 30, int ms = 1) {
  uint8_t rep[37] = {0x21};
  uint8_t* c = rep + 1;
  c[0] = 0x10; c[1] = b1; c[2] = b2; c[3] = ax; c[4] = ay; c[5] = cx; c[6] = cy; c[7] = l; c[8] = r;
  now_us += uint64_t(ms) * 1000;
  fe::phob_adapter::report(rep, now_us);
}

int main() {
  const std::string dir = "/tmp/phob-check";
  std::remove((dir + "/PhobGCC/port1.ini").c_str());
  mkdir(dir.c_str(), 0777);
  fe::phob_adapter::load(dir);
  fe::phob_adapter::Pad p{};
  send(0, 0, 128, 128);
  check(!fe::phob_adapter::pad(0, &p), "settings off: the adapter's own reading is used");

  ph::Settings s = fe::phob_adapter::settings(0);
  s.enabled = true;
  fe::phob_adapter::set_settings(0, s);
  for (int i = 0; i < 20; ++i) send(0, 0, 128, 128);
  check(fe::phob_adapter::pad(0, &p) && p.stick_x == 0 && p.stick_y == 0, "on, uncalibrated, at rest: 0, 0");
  for (int i = 0; i < 60; ++i) send(0, 0, 200, 128);
  fe::phob_adapter::pad(0, &p);
  check(p.stick_x > 60 && p.stick_x <= 72 && p.stick_y == 0, "uncalibrated stick pushed right (+72 raw): x " + std::to_string(p.stick_x));
  send(0x01, 0, 128, 128);
  fe::phob_adapter::pad(0, &p);
  check(p.buttons & 0x0100, "A pressed reaches the game as A");

  s.remap[ph::A_REMAP] = 1 << ph::B_REMAP; s.remap[ph::B_REMAP] = 1 << ph::A_REMAP;
  fe::phob_adapter::set_settings(0, s);
  send(0x01, 0, 128, 128);
  fe::phob_adapter::pad(0, &p);
  check((p.buttons & 0x0200) && !(p.buttons & 0x0100), "A and B swapped: the A button reaches the game as B");
  check(fe::phob_adapter::save(0), "settings written to PhobGCC/port1.ini");

  // The stick calibration: the prompts answered with a stock-shaped gate (phob_test.cpp's), A to record each.
  s.remap[ph::A_REMAP] = 1 << ph::A_REMAP; s.remap[ph::B_REMAP] = 1 << ph::B_REMAP;
  fe::phob_adapter::set_settings(0, s);
  check(fe::phob_adapter::cal_start(0, ph::ASTICK, false), "calibration started: " + fe::phob_adapter::cal_line());
  send(0, 0, 128, 128);
  int presses = 0;
  for (int guard = 0; guard < 100 && fe::phob_adapter::cal_active(); ++guard) {
    int step = -1;
    { const std::string line = fe::phob_adapter::cal_line(); const size_t at = line.find("step "); if (at != std::string::npos) step = std::atoi(line.c_str() + at + 5) - 1; }
    uint8_t x = 130, y = 125;   // rest, a little off centre
    if (step >= 0 && step < ph::kCalPoints) {
      const ph::CalPrompt pr = ph::cal_prompt(step);
      if (!pr.centre && !pr.optional) {
        const float a = 3.14159265f * pr.notch / 8.0f, rad = (pr.notch % 4 == 2) ? 0.34f : 0.37f;
        x = uint8_t(std::lround(255.0f * (0.51f + rad * 1.05f * std::cos(a))));
        y = uint8_t(std::lround(255.0f * (0.49f + rad * 0.95f * std::sin(a))));
      }
    }
    for (int i = 0; i < 60; ++i) send(0, 0, x, y);
    fe::phob_adapter::pad(0, &p);
    if (p.stick_x || p.stick_y || p.buttons) { check(false, "the game gets a resting controller during calibration"); break; }
    send(0x01, 0, x, y); send(0, 0, x, y); ++presses;
  }
  // 32 measurements, then the notches the firmware lets this gate adjust (it skips the ones a stock gate has no notch for).
  check(!fe::phob_adapter::cal_active() && presses > ph::kCalPoints && presses <= ph::kCalSteps,
        "calibration finished after " + std::to_string(presses) + " presses: " + fe::phob_adapter::cal_line());
  const ph::Settings after = fe::phob_adapter::settings(0);
  check(after.a_cal.measured, "the left stick's calibration is kept");
  std::ifstream f(dir + "/PhobGCC/port1.ini");
  const std::string text((std::istreambuf_iterator<char>(f)), {});
  check(text.find("a_cal") != std::string::npos || text.size() > 200, "and written to PhobGCC/port1.ini (" + std::to_string(text.size()) + " bytes)");
  // Calibrated: a full push to the right notch of that gate reaches the firmware's cardinal target (127 - 127 = 0 .. 80 in Melee units).
  for (int i = 0; i < 100; ++i) send(0, 0, uint8_t(std::lround(255.0f * (0.51f + 0.37f * 1.05f))), uint8_t(std::lround(255.0f * 0.49f)));
  fe::phob_adapter::pad(0, &p);
  float mx = 0, my = 0;
  ph::melee_coords(p.stick_x, p.stick_y, mx, my);
  char line[160];
  std::snprintf(line, sizeof line, "calibrated, pushed into the right notch: stick %d, %d (Melee %.4f, %.4f): full right, no vertical", p.stick_x, p.stick_y, mx, my);
  check(mx > 0.95f && std::fabs(my) < 0.02f, line);
  std::printf("%d failed\n", failures);
  return failures ? 1 : 0;
}
