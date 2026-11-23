// Stand-in for port/runtime/host/host.h when this repository's GX decoder (port/runtime/gx) is built on
// melee-unlocked's native host services instead of the recompiled runtime. Only what the decoder and
// the render queue use. Implemented in shim.cpp, except host::log (melee-unlocked's host_services.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "ppc.h"
#include "fatal_boundary.h"

namespace host {
extern uint8_t* ram;                       // 0x80000000
void log(const char* fmt, ...);
[[noreturn]] void die(const char* fmt, ...);   // throws; the frontend's FIFO sink catches it
uint8_t* ptr(uint32_t addr, uint32_t bytes = 1);
uint8_t rd8(uint32_t addr);
// The native game raises its own draw-done and token interrupts after it has flushed the pipe.
inline void set_pe_finish_pending() {}
inline void set_pe_token_pending(uint16_t) {}
// Scene controller state from the game's own state snapshot.
void native_scene(uint8_t* major, uint8_t* minor);

double now_seconds();
enum SimCost { SIM_DVD, SIM_AX, SIM_JUKEBOX, SIM_EXI, SIM_SNAPSHOT, SIM_QUEUE, SIM_OBSERVE, SIM_RECORD, SIM_RENDER, SIM_TEXTURE, SIM_PUMP, SIM_GPUWAIT, SIM_DRAWABLE, SIM_SAVESTATE, SIM_COST_COUNT };
void sim_cost_add(int slot, double seconds);
struct SimCostScope { int slot; double t0; explicit SimCostScope(int s) : slot(s), t0(now_seconds()) {} ~SimCostScope() { sim_cost_add(slot, now_seconds() - t0); } };
}  // namespace host
