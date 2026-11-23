// Where a frame's time goes: inside the game image, or in each host service it calls (and each
// game function the host calls back). See hostprof.S for the mechanism.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mu_host.h"
#include <cstdint>
#include <vector>

namespace hostprof {
constexpr int kSlots = 128;
struct Sample { uint64_t calls[kSlots]; uint64_t ticks[kSlots]; };   // ticks: exclusive time

// Call before linux_host::initialize: wraps both tables as the host hands them over.
void install();
bool installed();
// Totals so far.
void read(Sample* out);
// Ticks of the counter the wrappers use, and its frequency.
uint64_t now();
uint64_t frequency();
// Time the caller spent in something that must not count for whichever service is running
// (the runner's per-frame observer): removes it from that service's exclusive time.
void discount(uint64_t ticks);
// "host: poll", "game: retrace", ...; nullptr for an unused slot.
const char* name(int slot);
}
