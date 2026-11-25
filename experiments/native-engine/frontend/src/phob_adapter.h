// PhobGCC settings on a stock GameCube controller (melee-unlocked's host/phob.cpp, GPL-3.0-only, compiled from the fetched tree):
// the firmware's processing on the adapter's raw reports, as melee-unlocked's gc_adapter.cpp runs it. The app's adapter reader
// (gc_adapter_usbhs.cpp, frontend/patches/0123) hands every 37-byte report to report() on its own thread (1 ms poll) and asks
// pad() for each port's result when the game reads the controllers.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

#include "phob.h"   // melee-unlocked port/runtime/host (the fetched tree)

namespace host {
// One adapter report as read and as the Phob settings shaped it, for PhobScope and the PhobGCC menu (melee-unlocked's host.h
// GCScopeSample, the same fields in the same order). Stick values are relative to the neutral point.
struct GCScopeSample {
  uint32_t time_us;
  int8_t raw_ax, raw_ay, raw_cx, raw_cy, ax, ay, cx, cy;
  uint8_t raw_l, raw_r, l, r;
  uint16_t raw_buttons, buttons;
  uint8_t hw_ax, hw_ay, hw_cx, hw_cy;   // the stick bytes exactly as the adapter reported them (calibration reads these)
};
}  // namespace host

namespace fe::phob_adapter {
// <folder>/PhobGCC/port<N>.ini (upstream's file and text form, phob::serialize): read now, written by save().
void load(const std::string& app_folder);
host::phob::Settings settings(int port);
void set_settings(int port, const host::phob::Settings& s);   // takes effect at the next report
bool save(int port);
// The reader thread: one adapter report (0x21 + 9 bytes per port) at `time_us` (a steady clock).
void report(const uint8_t r[37], uint64_t time_us);
// The game's read: the port's processed controller, when its settings are on (and the adapter has sent reports).
struct Pad { uint16_t buttons; int8_t stick_x, stick_y, sub_x, sub_y; uint8_t trig_l, trig_r; };
bool pad(int port, Pad* out);

// The firmware's stick calibration, step by step (phob::CalSession) on a port's live raw readings: A, L or R records the
// reading (averaged over the last 50 ms) or moves to the next notch, Z steps back, X / Y held turn the notch being adjusted,
// B puts it back; the game gets a neutral controller from that port meanwhile. Start ends it without keeping anything.
// PhobScope's feed (melee-unlocked's gc_adapter.cpp gcadapter_scope / gcadapter_phob_raw): the latest `max` reports of a port,
// oldest first (4096 kept per port); the port's controller as the adapter reports it right now; reports per second measured
// from what arrived (0 when none came for a second).
int scope(int port, host::GCScopeSample* out, int max);
bool raw(int port, host::phob::HardwareInput& in);
double rate_hz();
// Test runs off the console (melee-unlocked's MELEE_TEST_ADAPTER_SCRIPT=<file>): a scripted controller in adapter socket 1, played
// once at 1 kHz through report() as a real adapter's reports; the last step then holds. One line per step:
// `<ms> <ax> <ay> <cx> <cy> <la> <ra> [A B X Y Z L R S DU DD DL DR]`, raw adapter bytes (128 centre). Started by the first call
// (the PhobGCC menu's, or the run's start with MU_TEST_ADAPTER_AT_START=1); nothing on the console or without the variable.
void test_script_start();
void test_script_stop();

bool cal_start(int port, host::phob::Stick which, bool notch_fix);
bool cal_active(int* port = nullptr);
std::string cal_line();     // what to do now, for the overlay
}  // namespace fe::phob_adapter
