// PhobGCC settings on a stock GameCube controller (melee-unlocked's host/phob.cpp, GPL-3.0-only, compiled from the fetched tree):
// the firmware's processing on the adapter's raw reports, as melee-unlocked's gc_adapter.cpp runs it. The app's adapter reader
// (gc_adapter_usbhs.cpp, frontend/patches/0123) hands every 37-byte report to report() on its own thread (1 ms poll) and asks
// pad() for each port's result when the game reads the controllers.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

#include "phob.h"   // melee-unlocked port/runtime/host (the fetched tree)

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
bool cal_start(int port, host::phob::Stick which, bool notch_fix);
bool cal_active(int* port = nullptr);
std::string cal_line();     // what to do now, for the overlay
}  // namespace fe::phob_adapter
